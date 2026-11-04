/*
 * dll_loader.c — محمّل DLLs لويندوز 7 الحقيقي (فضاء المستخدم).
 *
 * ماذا يفعل هذا الملف بالضبط؟
 *   1) يقرأ ملف PE ويحلّل ترويسته (DOS → PE → COFF → Optional → Sections)
 *   2) يرسم الصورة (SizeOfImage) على العنوان الأساسي المفضّل (ImageBase)
 *      أو على عنوان آخر، وينسخ الترويسات والأقسام
 *   3) يطبّق **إعادة التوجيه** (base relocations — دليل .reloc)
 *   4) يمشي على **جدول الاستيراد** (imports) ويحلّ كل استيراد مقابل
 *      *مُحلِّل خارجي* (callback) ويكتب العناوين في الـ IAT
 *   5) يوفّر unload + GetProcAddress + قائمة الوحدات
 *
 * قواعد صارمة:
 *   - **لا تنفيذ لكود Win7 إطلاقاً** (DllMain / ستوب syscall = انهيار).
 *   - **لا dynamic linker للمضيف**: كل استيراد يمرّ عبر الـ callback.
 *   - **كل RVA محروس**: الصورة تُعامل كمحتوى غير موثوق ⇒ فحص حدود
 *     قبل كل قراءة/كتابة ⇒ ملف تالف = رفض نظيف.
 *   - **api-set**: أسماء api-ms-win-* تمرّ عبر apiset.c أولاً.
 *
 * المصابيح: DLL-002 (رسم) · DLL-003 (إعادة توجيه) · DLL-005 (GetProcAddress)
 *            DLL-006 (FreeLibrary) · DLL-007 (ترتيب الاعتماديات)
 *            DLL-008 (الدورات).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "dll_loader.h"
#include "pe_export.h"
#include "apiset.h"

/* ------------------------------------------------------------------ */
/* ثوابت وحدود وقائية                                                 */
/* ------------------------------------------------------------------ */
#define DLL_MAX_SECTIONS     96u
#define DLL_MAX_DIRS         16u
#define DLL_MAX_IMAGE        (1024u * 1024u * 1024u)  /* 1 GiB */
#define DLL_MAX_IMPORT_DESCS 1024u
#define DLL_MAX_THUNKS       65536u
#define DLL_MAX_STRLEN       512u
#define DLL_MAX_DEPS         64u
#define DLL_MAX_MODULES      256u
#define DLL_MAX_STACK        64u

/* خصائص الأقسام */
#define SCN_CNT_CODE         0x00000020u
#define SCN_MEM_EXECUTE      0x20000000u
#define SCN_MEM_READ         0x40000000u
#define SCN_MEM_WRITE        0x80000000u

/* أنواع إعادة التوجيه */
#define REL_ABSOLUTE  0
#define REL_HIGH      1
#define REL_LOW       2
#define REL_HIGHLOW   3
#define REL_HIGHADJ   4
#define REL_DIR64    10

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

/* تشخيص مفصّل: مخفي افتراضياً — يُفعَّل بـ NTBOX_DLL_DEBUG=1
 * (سجلات verify يجب أن تبقى قابلة للقراءة). */
static int ntb_dbg_on(void)
{
    static int on = -1;

    if (on < 0)
        on = getenv("NTBOX_DLL_DEBUG") != NULL;
    return on;
}

#define NDBG(...)                                                          \
    do {                                                                   \
        if (ntb_dbg_on())                                                  \
            fprintf(stderr, __VA_ARGS__);                                  \
    } while (0)

/* ------------------------------------------------------------------ */
/* أدوات                                                              */
/* ------------------------------------------------------------------ */
static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static int ci_eq(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = (unsigned char)*a++, cb = (unsigned char)*b++;

        if (ca >= 'A' && ca <= 'Z')
            ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z')
            cb += 'a' - 'A';
        if (ca != cb)
            return 0;
    }
    return *a == '\0' && *b == '\0';
}

static int ci_has_prefix(const char *s, const char *pfx)
{
    while (*pfx) {
        int cs = (unsigned char)*s++, cp = (unsigned char)*pfx++;

        if (cs >= 'A' && cs <= 'Z')
            cs += 'a' - 'A';
        if (cp >= 'A' && cp <= 'Z')
            cp += 'a' - 'A';
        if (cs != cp)
            return 0;
    }
    return 1;
}

/* basename بدون المسار (يُنسخ إلى out) — يدعم '/' و '\\' */
static void base_name(const char *path, char *out, size_t outsz)
{
    const char *b = path, *p;
    size_t len;

    for (p = path; *p; p++)
        if (*p == '/' || *p == '\\')
            b = p + 1;
    len = strlen(b);
    if (len >= outsz)
        len = outsz - 1;
    memcpy(out, b, len);
    out[len] = '\0';
}

/* ------------------------------------------------------------------ */
/* الصورة المحلَّلة (من الملف)                                         */
/* ------------------------------------------------------------------ */
struct dll_sec {
    char     name[9];
    uint32_t va;
    uint32_t vsize;
    uint32_t raw;
    uint32_t rsize;
    uint32_t chars;
};

struct dll_img {
    int      is64;
    uint64_t image_base;
    uint32_t size_of_image;
    uint32_t size_of_headers;
    uint32_t entry_rva;
    uint32_t section_align;
    uint16_t nsec;
    struct dll_sec sec[DLL_MAX_SECTIONS];
    uint32_t dir_rva[DLL_MAX_DIRS];
    uint32_t dir_size[DLL_MAX_DIRS];
};

/* ------------------------------------------------------------------ */
/* الوحدة المحمَّلة                                                     */
/* ------------------------------------------------------------------ */
struct ntb0_dll_module {
    struct ntb0_dll_loader *ld;

    char     path[1024];
    char     name[128];

    uint8_t *base;
    size_t   map_size;      /* الحجم المحجوز (مقرّب للصفحة) */
    size_t   size;          /* SizeOfImage */
    uint64_t pref_base;
    int64_t  delta;
    int      is64;
    int      nsec;
    uint32_t entry_rva;

    struct dll_sec sec[DLL_MAX_SECTIONS];

    /* إحصاء إعادة التوجيه */
    unsigned reloc_blocks;
    unsigned reloc_entries;
    unsigned reloc_applied;
    unsigned reloc_abs;        /*entries ABSOLUTE (حشو)*/
    unsigned reloc_unsupported;

    /* الاستيراد */
    ntb0_dll_import *imports;
    unsigned n_imports;
    unsigned imports_resolved;
    unsigned imports_unresolved;
    unsigned n_import_dlls;
    unsigned n_apiset;         /* أسماء api-ms-win-* شُوهدت */

    /* الاعتماديات */
    char     dep[DLL_MAX_DEPS][128];
    unsigned n_deps;

    /* جدول التصديرات (pe_export.c) — للبحث بالاسم/الترتيب */
    ntb0_pe *pe;

    int refs;                  /* عدّ المراجع */

    /* DllMain/TLS callbacks (T-131/T-132) */
    void    *dllmain_entry;    /* عنوان DllMain (NULL إذا لم يكن DLL) */
    uint64_t *tls_callbacks;   /* مصفوفة عناوين TLS callbacks */
    unsigned  tls_callback_count;
    int       dllmain_called;  /* تم استدعاء DllMain مع DLL_PROCESS_ATTACH */
    int       is_system_dll;   /* DLL نظام (kernel32, ntdll, إلخ) — لا يُستدعى DllMain */
};

/* ------------------------------------------------------------------ */
/* المحمّل                                                             */
/* ------------------------------------------------------------------ */
struct ntb0_dll_loader {
    ntb0_dll_resolve_fn resolve;
    void              *resolve_ctx;
    ntb0_apiset       *apiset;      /* مُستعار */
    char               sysdir[512];          /* System32 للـ PE64 */
    char               syswow64_sysdir[512]; /* SysWOW64 للـ PE32 — peer لـ sysdir */

    ntb0_dll_module   *mods[DLL_MAX_MODULES];
    unsigned           n_mods;

    ntb0_dll_module   *stack[DLL_MAX_STACK]; /* حزمة التحميل (لدورات) */
    unsigned           n_stack;

    unsigned           cycles;

    char               err[512];
};

/* ------------------------------------------------------------------ */
/* الأخطاء                                                            */
/* ------------------------------------------------------------------ */
static void set_err(ntb0_dll_loader *ld, const char *fmt, ...)
{
    va_list ap;

    if (!ld)
        return;
    va_start(ap, fmt);
    vsnprintf(ld->err, sizeof(ld->err), fmt, ap);
    va_end(ap);
}

const char *ntb0_dll_last_error(const ntb0_dll_loader *ld)
{
    return ld ? ld->err : "(no loader)";
}

const char *ntb0_dll_strerror(int rc)
{
    switch (rc) {
    case NTB0_DLL_OK:         return "ok";
    case NTB0_DLL_ENOENT:     return "file not found";
    case NTB0_DLL_EFORMAT:    return "malformed PE";
    case NTB0_DLL_ENOMEM:     return "out of memory";
    case NTB0_DLL_EIO:        return "i/o error";
    case NTB0_DLL_ERELOC:     return "bad relocation directory";
    case NTB0_DLL_EIMPORTS:   return "bad import directory";
    case NTB0_DLL_EBASE:      return "cannot map at the preferred base";
    case NTB0_DLL_ENOTFOUND:  return "not found";
    case NTB0_DLL_EEXISTS:    return "already loaded";
    case NTB0_DLL_EBUSY:      return "still referenced";
    default:                  return "unknown error";
    }
}

/* ------------------------------------------------------------------ */
/* قراءة الملف                                                        */
/* ------------------------------------------------------------------ */
static uint8_t *read_file(const char *path, size_t *len_out)
{
    int fd;
    struct stat st;
    uint8_t *buf;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        close(fd);
        return NULL;
    }
    buf = (uint8_t *)malloc((size_t)st.st_size);
    if (!buf) {
        close(fd);
        return NULL;
    }
    {
        size_t got = 0;

        while (got < (size_t)st.st_size) {
            ssize_t n = read(fd, buf + got, (size_t)st.st_size - got);

            if (n <= 0)
                break;
            got += (size_t)n;
        }
        if (got != (size_t)st.st_size) {
            free(buf);
            close(fd);
            return NULL;
        }
    }
    close(fd);
    *len_out = (size_t)st.st_size;
    return buf;
}

/* ------------------------------------------------------------------ */
/* تحليل ترويسة PE                                                    */
/* ------------------------------------------------------------------ */
static int parse_pe(const uint8_t *buf, size_t len, struct dll_img *im,
                    char *err, size_t errlen)
{
    uint32_t e_lfanew, opt_off, sec_off, dd_off, numrva;
    uint16_t nsec, optsz, magic;
    uint32_t i;

    memset(im, 0, sizeof(*im));

    if (len < 0x40 || buf[0] != 'M' || buf[1] != 'Z') {
        snprintf(err, errlen, "no MZ header");
        return NTB0_DLL_EFORMAT;
    }
    e_lfanew = rd32(buf + 0x3C);
    if (e_lfanew < 0x40 || (uint64_t)e_lfanew + 24 > len) {
        snprintf(err, errlen, "bad e_lfanew (0x%x)", e_lfanew);
        return NTB0_DLL_EFORMAT;
    }
    if (memcmp(buf + e_lfanew, "PE\0\0", 4) != 0) {
        snprintf(err, errlen, "no PE signature");
        return NTB0_DLL_EFORMAT;
    }

    nsec     = rd16(buf + e_lfanew + 6);
    optsz    = rd16(buf + e_lfanew + 20);
    opt_off  = e_lfanew + 24;

    if (nsec == 0 || nsec > DLL_MAX_SECTIONS) {
        snprintf(err, errlen, "absurd section count (%u)", nsec);
        return NTB0_DLL_EFORMAT;
    }
    if ((uint64_t)opt_off + optsz > len) {
        snprintf(err, errlen, "optional header truncated");
        return NTB0_DLL_EFORMAT;
    }
    if (optsz < 64) {
        snprintf(err, errlen, "optional header too small (%u)", optsz);
        return NTB0_DLL_EFORMAT;
    }

    magic = rd16(buf + opt_off);
    if (magic == 0x20b) {                       /* PE32+ (64 بت) */
        if (optsz < 112) {
            snprintf(err, errlen, "PE32+ optional header too small");
            return NTB0_DLL_EFORMAT;
        }
        im->is64           = 1;
        im->image_base     = rd64(buf + opt_off + 24);
        im->section_align  = rd32(buf + opt_off + 32);
        numrva             = rd32(buf + opt_off + 108);
        dd_off             = opt_off + 112;
    } else if (magic == 0x10b) {                /* PE32 (32 بت) */
        if (optsz < 96) {
            snprintf(err, errlen, "PE32 optional header too small");
            return NTB0_DLL_EFORMAT;
        }
        im->is64           = 0;
        im->image_base     = rd32(buf + opt_off + 28);
        im->section_align  = rd32(buf + opt_off + 36);
        numrva             = rd32(buf + opt_off + 92);
        dd_off             = opt_off + 96;
    } else {
        snprintf(err, errlen, "unknown optional header magic 0x%x", magic);
        return NTB0_DLL_EFORMAT;
    }

    im->entry_rva       = rd32(buf + opt_off + 16);
    im->size_of_image   = rd32(buf + opt_off + 56);
    im->size_of_headers = rd32(buf + opt_off + 60);

    if (im->size_of_image == 0 || im->size_of_image > DLL_MAX_IMAGE) {
        snprintf(err, errlen, "absurd SizeOfImage (0x%x)", im->size_of_image);
        return NTB0_DLL_EFORMAT;
    }
    if (im->size_of_headers == 0 || im->size_of_headers > im->size_of_image) {
        snprintf(err, errlen, "bad SizeOfHeaders (0x%x)", im->size_of_headers);
        return NTB0_DLL_EFORMAT;
    }

    /* دليل البيانات */
    if (numrva > DLL_MAX_DIRS)
        numrva = DLL_MAX_DIRS;
    if ((uint64_t)dd_off + (uint64_t)numrva * 8 > len) {
        snprintf(err, errlen, "data directory truncated");
        return NTB0_DLL_EFORMAT;
    }
    for (i = 0; i < numrva; i++) {
        im->dir_rva[i]  = rd32(buf + dd_off + i * 8);
        im->dir_size[i] = rd32(buf + dd_off + i * 8 + 4);
    }

    /* جدول الأقسام */
    sec_off = opt_off + optsz;
    if ((uint64_t)sec_off + (uint64_t)nsec * 40 > len) {
        snprintf(err, errlen, "section table truncated");
        return NTB0_DLL_EFORMAT;
    }
    im->nsec = nsec;
    for (i = 0; i < nsec; i++) {
        const uint8_t *s = buf + sec_off + i * 40;

        memcpy(im->sec[i].name, s, 8);
        im->sec[i].name[8] = '\0';
        im->sec[i].vsize = rd32(s + 8);
        im->sec[i].va    = rd32(s + 12);
        im->sec[i].rsize = rd32(s + 16);
        im->sec[i].raw   = rd32(s + 20);
        im->sec[i].chars = rd32(s + 36);
    }
    return NTB0_DLL_OK;
}

/* ------------------------------------------------------------------ */
/* قراءة سلسلة داخل الصورة (محدودة بالحدود)                            */
/* ------------------------------------------------------------------ */
static int img_str(const uint8_t *base, size_t size, uint32_t rva,
                    char *out, size_t outsz)
{
    size_t i = 0;

    if (outsz == 0)
        return -1;
    if ((uint64_t)rva >= size)
        return -1;
    while (rva + i < size && i < outsz - 1 && base[rva + i] != '\0')
        i++;
    if (base[rva + i] != '\0')
        return -1;                 /* سلسلة غير منتهية ⇒ تلف */
    memcpy(out, base + rva, i);
    out[i] = '\0';
    return 0;
}

/* التحقّق من أن writing إلى mod->base + offset بطول len يقع ضمن المدى المسوّق */
static int write_in_range(struct ntb0_dll_module *mod, uint32_t offset, uint32_t len,
                          const char *what)
{
    if (!mod || !mod->base) {
        NDBG("write_in_range: no base (%s)\n", what ? what : "(null)");
        return -1;
    }
    if ((uint64_t)offset + len > mod->map_size) {
        NDBG("write_in_range OUT OF BOUNDS: offset=0x%x len=%u map_size=%zu base=0x%llx (%s)\n",
                offset, len, mod->map_size,
                (unsigned long long)(uintptr_t)mod->base, what ? what : "(null)");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* الرسم                                                              */
/* ------------------------------------------------------------------ */
static int map_image(ntb0_dll_loader *ld, struct ntb0_dll_module *mod,
                     const struct dll_img *im, unsigned flags, uint64_t force_base)
{
    long ps = sysconf(_SC_PAGESIZE);
    size_t page = (ps > 0) ? (size_t)ps : 4096u;
    size_t map_size = ((size_t)im->size_of_image + page - 1) & ~(page - 1);
    uint8_t *base;

    if (map_size == 0)
        map_size = page;

    /* defense: تأكد من أن map_size لا يقل عن size_of_image
     * (size_of_image قد يكون أكبر من الصفحة المحجوزة إذا كان الصفح غير مضبوط) */
    if (map_size < im->size_of_image) {
        NDBG("map_image: map_size=%zu < size_of_image=%u, expanding to page-aligned\n",
                map_size, im->size_of_image);
        map_size = (im->size_of_image + page - 1) & ~(page - 1);
        if (map_size == 0)
            map_size = page;
    }

    NDBG("map_image: image_base=0x%llx size_of_image=0x%x map_size=%zu page=%zu force_base=%llx\n",
            (unsigned long long)im->image_base, im->size_of_image, map_size, page,
            (unsigned long long)force_base);

    if (force_base) {
        /* عنوان إجباري */
        base = (uint8_t *)mmap((void *)(uintptr_t)force_base, map_size,
                               PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                               -1, 0);
        if (base == MAP_FAILED ||
            (uint64_t)(uintptr_t)base != force_base) {
            if (base != MAP_FAILED)
                munmap(base, map_size);
            set_err(ld, "cannot map '%s' at the requested base 0x%llx",
                    mod->name, (unsigned long long)force_base);
            return NTB0_DLL_EBASE;
        }
        mod->base      = base;
        mod->map_size  = map_size;
        mod->size      = im->size_of_image;
        mod->pref_base = im->image_base;
        mod->delta     = (int64_t)(uintptr_t)base - (int64_t)im->image_base;
        NDBG("map_image: force_base OK base=0x%llx map_size=%zu size=%zu (size_of_image)\n",
                (unsigned long long)(uintptr_t)base, mod->map_size, mod->size);
        return NTB0_DLL_OK;
    }

    /* المحاولة الأولى: العنوان الأساسي المفضّل (بلا MAP_FIXED ⇒ لا
     * خرق لعنوان موجود — MAP_FIXED_NOREPLACE يمنع الكتابة فوق خريطة
     * أخرى). */
    base = (uint8_t *)mmap((void *)(uintptr_t)im->image_base, map_size,
                           PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                           -1, 0);
    if (base == MAP_FAILED) {
        if (flags & NTB0_DLL_FLAG_FIXED_BASE) {
            set_err(ld, "cannot map '%s' at 0x%llx (%s)", mod->name,
                    (unsigned long long)im->image_base, strerror(errno));
            return NTB0_DLL_EBASE;
        }
        base = (uint8_t *)mmap(NULL, map_size, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base == MAP_FAILED) {
            set_err(ld, "cannot map '%s' (%zu bytes): %s", mod->name, map_size,
                    strerror(errno));
            return NTB0_DLL_ENOMEM;
        }
    } else if ((uint64_t)(uintptr_t)base != im->image_base) {
        /* نواة قديمة لا تدعم MAP_FIXED_NOREPLACE ⇒ عنوان مختلف */
        munmap(base, map_size);
        if (flags & NTB0_DLL_FLAG_FIXED_BASE) {
            set_err(ld, "preferred base 0x%llx is occupied",
                    (unsigned long long)im->image_base);
            return NTB0_DLL_EBASE;
        }
        base = (uint8_t *)mmap(NULL, map_size, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base == MAP_FAILED) {
            set_err(ld, "cannot map '%s' (%zu bytes): %s", mod->name, map_size,
                    strerror(errno));
            return NTB0_DLL_ENOMEM;
        }
    }

    mod->base     = base;
    mod->map_size = map_size;
    mod->size     = im->size_of_image;   /* SizeOfImage، وليس map_size */
    mod->pref_base = im->image_base;
    mod->delta    = (int64_t)(uintptr_t)base - (int64_t)im->image_base;
    NDBG("map_image: normal OK base=0x%llx map_size=%zu size=%zu (size_of_image)\n",
            (unsigned long long)(uintptr_t)base, mod->map_size, mod->size);
    return NTB0_DLL_OK;
}

/* نسخ الترويسات والأقسام داخل الصورة */
static void copy_sections(struct ntb0_dll_module *mod, const struct dll_img *im,
                          const uint8_t *buf, size_t len)
{
    uint32_t i;

    /* الترويسات */
    {
        uint32_t hdr = im->size_of_headers;

        if (hdr > mod->map_size)
            hdr = (uint32_t)mod->map_size;
        if (hdr > len)
            hdr = (uint32_t)len;
        if (write_in_range(mod, 0, hdr, "headers") != 0)
            return;
        memcpy(mod->base, buf, hdr);
    }

    for (i = 0; i < im->nsec; i++) {
        const struct dll_sec *s = &im->sec[i];
        uint32_t span = s->vsize ? s->vsize : s->rsize;
        uint32_t copy;

        if (span == 0)
            continue;
        if (s->va >= mod->map_size) {
            NDBG("copy_sections: section[%u] '%s' va=0x%x >= map_size=%zu, skipping\n",
                    i, s->name, s->va, mod->map_size);
            continue;                       /* خارج الصورة ⇒ تُتجاهل */
        }
        if ((uint64_t)s->va + span > mod->map_size) {
            NDBG("copy_sections: section[%u] '%s' va=0x%x span=0x%x clamped to map_size boundary\n",
                    i, s->name, s->va, span);
            span = (uint32_t)(mod->map_size - s->va);
        }
        if (!s->rsize)
            continue;
        copy = s->rsize;
        if (copy > span)
            copy = span;
        if (s->raw >= len)
            continue;
        if ((uint64_t)s->raw + copy > len)
            copy = (uint32_t)(len - s->raw);
        if (copy) {
            if (write_in_range(mod, s->va, copy, s->name) != 0) {
                NDBG("copy_sections: section[%u] '%s' write out of range, skipping\n",
                        i, s->name);
                continue;
            }
            memcpy(mod->base + s->va, buf + s->raw, copy);
        }
    }
}

/* ------------------------------------------------------------------ */
/* الصلاحيات (حسب خصائص كل قسم)                                        */
/* ------------------------------------------------------------------ */
static int apply_protections(struct ntb0_dll_module *mod)
{
    long ps = sysconf(_SC_PAGESIZE);
    size_t page = (ps > 0) ? (size_t)ps : 4096u;
    size_t npages = (mod->map_size + page - 1) / page;
    uint8_t *prot;
    size_t p;

    prot = (uint8_t *)calloc(npages, 1);
    if (!prot)
        return -1;

    /* الترويسات: قراءة فقط */
    prot[0] |= 1;

    for (p = 0; p < (size_t)mod->nsec; p++) {
        const struct dll_sec *s = &mod->sec[p];
        uint32_t span = s->vsize ? s->vsize : s->rsize;
        uint64_t start, end, q;
        uint8_t flags = 1;                  /* قراءة دائماً */

        if (span == 0)
            continue;
        if (s->va >= mod->map_size)
            continue;
        if ((uint64_t)s->va + span > mod->map_size)
            span = (uint32_t)(mod->map_size - s->va);

        if (s->chars & SCN_MEM_WRITE)
            flags |= 2;
        if (s->chars & SCN_MEM_EXECUTE)
            flags |= 4;

        start = s->va / page;
        end   = ((uint64_t)s->va + span + page - 1) / page;
        if (end > npages)
            end = npages;
        for (q = start; q < end; q++)
            prot[q] |= flags;
    }

    /* تطبيق على دفعات متتالية متساوية الصلاحية */
    p = 0;
    while (p < npages) {
        size_t q = p + 1;
        int pr;

        while (q < npages && prot[q] == prot[p])
            q++;
        pr = PROT_READ;
        if (prot[p] & 2)
            pr |= PROT_WRITE;
        if (prot[p] & 4)
            pr |= PROT_EXEC;
        if (mprotect(mod->base + p * page, (q - p) * page, pr) != 0) {
            free(prot);
            return -1;
        }
        p = q;
    }
    free(prot);
    return 0;
}

int ntb0_dll_module_make_writable(ntb0_dll_module *mod)
{
    if (!mod || !mod->base)
        return -1;
    if (mprotect(mod->base, mod->map_size, PROT_READ | PROT_WRITE | PROT_EXEC)
        != 0)
        return -1;
    return 0;
}

int ntb0_dll_module_restore_protections(ntb0_dll_module *mod)
{
    if (!mod || !mod->base)
        return -1;
    return apply_protections(mod);
}

/* ------------------------------------------------------------------ */
/* إعادة التوجيه (base relocations)                                    */
/* ------------------------------------------------------------------ */
static int apply_relocs(ntb0_dll_loader *ld, struct ntb0_dll_module *mod,
                        const struct dll_img *im)
{
    uint32_t rva = im->dir_rva[5], size = im->dir_size[5];
    const uint8_t *dir;
    uint64_t pos = 0;

    if (rva == 0 || size == 0)
        return NTB0_DLL_OK;         /* بلا إعادة توجيه (EXE غير قابل للنقل) */
    if ((uint64_t)rva + size > mod->size) {
        set_err(ld, "relocation directory (0x%x+%u) is outside the image",
                rva, size);
        return NTB0_DLL_ERELOC;
    }
    dir = mod->base + rva;

    while (pos + 8 <= size) {
        uint32_t page_rva = rd32(dir + pos);
        uint32_t blk_size = rd32(dir + pos + 4);
        uint32_t n_ent, i;
        NDBG("apply_relocs block: page_rva=0x%x blk_size=%u pos=%zu\n",
                page_rva, blk_size, pos);

        if (blk_size < 8 || (uint64_t)pos + blk_size > size) {
            set_err(ld, "corrupt relocation block at +0x%llx (size %u)",
                    (unsigned long long)pos, blk_size);
            return NTB0_DLL_ERELOC;
        }
        mod->reloc_blocks++;
        n_ent = (blk_size - 8) / 2;

        for (i = 0; i < n_ent; i++) {
            uint16_t e  = rd16(dir + pos + 8 + i * 2);
            uint16_t ty = (uint16_t)(e >> 12);
            uint32_t of = (uint32_t)(e & 0x0fff);
            uint64_t target = (uint64_t)page_rva + of;
            uint64_t write_addr = (uint64_t)mod->base + target;

            NDBG("apply_relocs entry %u: type=%d target=0x%x write_addr=0x%llx mod->base=0x%llx mod->size=%zu\n",
                    i, ty, (unsigned int)target, (unsigned long long)write_addr, (unsigned long long)mod->base, mod->size);

            mod->reloc_entries++;

            switch (ty) {
            case REL_ABSOLUTE:
                mod->reloc_abs++;
                break;
            case REL_HIGHLOW:
                if (target + 4 > mod->size)
                    break;              /* خارج الصورة ⇒ يُتجاهل */
                *(uint32_t *)(mod->base + target) += (uint32_t)mod->delta;
                mod->reloc_applied++;
                break;
            case REL_DIR64:
                if (target + 8 > mod->size)
                    break;
                *(uint64_t *)(mod->base + target) += (uint64_t)mod->delta;
                mod->reloc_applied++;
                break;
            case REL_HIGH:
                if (target + 2 > mod->size)
                    break;
                *(uint16_t *)(mod->base + target) +=
                    (uint16_t)(mod->delta >> 16);
                mod->reloc_applied++;
                break;
            case REL_LOW:
                if (target + 2 > mod->size)
                    break;
                *(uint16_t *)(mod->base + target) += (uint16_t)mod->delta;
                mod->reloc_applied++;
                break;
            case REL_HIGHADJ: {
                uint32_t next_target;

                if (i + 1 >= n_ent)
                    break;
                next_target = (uint64_t)page_rva + (rd16(dir + pos + 8 + (i + 1) * 2) & 0x0fff);
                if (target + 2 > mod->size || next_target + 2 > mod->size)
                    break;
                *(uint16_t *)(mod->base + target) =
                    (uint16_t)((mod->delta >> 16) +
                               *(uint16_t *)(mod->base + next_target));
                mod->reloc_applied++;
                i++;                  /* consuming the adjustment entry */
                break;
            }
            default:
                mod->reloc_unsupported++;
                break;
            }
        }
        pos += blk_size;
    }
    return NTB0_DLL_OK;
}

/* ------------------------------------------------------------------ */
/* الاستيراد                                                          */
/* ------------------------------------------------------------------ */
struct dll_impdesc {
    char     dll[128];
    char     host[128];
    uint32_t ilt_rva;
    uint32_t iat_rva;
};

/* إضافة سجلّ استيراد (مصفوفة متوسّعة) */
static ntb0_dll_import *import_push(struct ntb0_dll_module *mod,
                                    unsigned *cap)
{
    if (mod->n_imports == *cap) {
        unsigned nc = *cap ? *cap * 2 : 32;
        ntb0_dll_import *ni;

        if (nc > 4u * 1024u * 1024u)
            return NULL;
        ni = (ntb0_dll_import *)realloc(mod->imports,
                                        nc * sizeof(ntb0_dll_import));
        if (!ni)
            return NULL;
        mod->imports = ni;
        *cap = nc;
    }
    return &mod->imports[mod->n_imports++];
}

/* إضافة اعتماد */
static void dep_add(struct ntb0_dll_module *mod, const char *name)
{
    unsigned i;

    for (i = 0; i < mod->n_deps; i++)
        if (ci_eq(mod->dep[i], name))
            return;
    if (mod->n_deps >= DLL_MAX_DEPS)
        return;
    snprintf(mod->dep[mod->n_deps], sizeof(mod->dep[0]), "%s", name);
    mod->n_deps++;
}

/* هل الوحدة حاضرة في حزمة التحميل (⇒ دورة)؟ */
static int in_load_stack(ntb0_dll_loader *ld, const char *name)
{
    unsigned i;

    for (i = 0; i < ld->n_stack; i++)
        if (ci_eq(ld->stack[i]->name, name))
            return 1;
    return 0;
}

/*
 * المشي على دليل الاستيراد:
 *   - يجمع الواصفات (اسم DLL + ILT + IAT)
 *   - يمرّر أسماء api-ms-win-* عبر apiset.c
 *   - (اختياري) يحمّل الاعتماديات تلقائياً (مع كشف الدورات)
 *   - يحلّ كل استيراد عبر الـ callback ويكتب العنوان في الـ IAT
 */
static int resolve_imports(ntb0_dll_loader *ld, struct ntb0_dll_module *mod,
                           const struct dll_img *im, unsigned flags)
{
    uint32_t dir_rva = im->dir_rva[1], dir_size = im->dir_size[1];
    struct dll_impdesc *descs = NULL;
    unsigned n_descs = 0, cap_descs = 0;
    unsigned d, i;
    unsigned import_cap = 0;
    int rc = NTB0_DLL_OK;
    uint32_t ptr_size = im->is64 ? 8u : 4u;
    uint64_t ord_flag = im->is64 ? 0x8000000000000000ULL : 0x80000000ULL;

    if (dir_rva == 0 || dir_size == 0)
        return NTB0_DLL_OK;                 /* بلا استيرادات */

    if ((uint64_t)dir_rva + 20 > mod->size) {
        set_err(ld, "import directory (0x%x) is outside the image", dir_rva);
        return NTB0_DLL_EIMPORTS;
    }

    /* (1) جمع الواصفات */
    for (d = 0;; d++) {
        const uint8_t *desc;
        uint32_t name_rva, ilt_rva, iat_rva;

        if (d >= DLL_MAX_IMPORT_DESCS) {
            set_err(ld, "too many import descriptors");
            rc = NTB0_DLL_EIMPORTS;
            goto out;
        }
        if ((uint64_t)dir_rva + (uint64_t)(d + 1) * 20 > mod->size) {
            set_err(ld, "import descriptor %u is outside the image", d);
            rc = NTB0_DLL_EIMPORTS;
            goto out;
        }
        desc = mod->base + dir_rva + d * 20;
        ilt_rva = rd32(desc + 0);
        name_rva = rd32(desc + 12);
        iat_rva = rd32(desc + 16);

        if (name_rva == 0)
            break;                          /* نهاية الجدول */

        if (n_descs == cap_descs) {
            unsigned nc = cap_descs ? cap_descs * 2 : 8;
            struct dll_impdesc *nd;

            if (nc > DLL_MAX_IMPORT_DESCS)
                nc = DLL_MAX_IMPORT_DESCS;
            nd = (struct dll_impdesc *)realloc(descs,
                                               nc * sizeof(*descs));
            if (!nd) {
                rc = NTB0_DLL_ENOMEM;
                goto out;
            }
            descs = nd;
            cap_descs = nc;
        }
        memset(&descs[n_descs], 0, sizeof(descs[0]));

        if (img_str(mod->base, mod->size, name_rva, descs[n_descs].dll,
                    sizeof(descs[0].dll)) != 0) {
            set_err(ld, "bad import DLL name (RVA 0x%x)", name_rva);
            rc = NTB0_DLL_EIMPORTS;
            goto out;
        }
        descs[n_descs].ilt_rva = ilt_rva;
        descs[n_descs].iat_rva = iat_rva;
        snprintf(descs[n_descs].host, sizeof(descs[0].host), "%s",
                 descs[n_descs].dll);
        n_descs++;
    }

    mod->n_import_dlls = n_descs;

    /* (2) api-set: أسماء api-ms-win-* ⇒ DLL مضيف */
    for (d = 0; d < n_descs; d++) {
        if (!ci_has_prefix(descs[d].dll, "api-ms-win-"))
            continue;
        mod->n_apiset++;
        if (!ld->apiset)
            continue;
        {
            ntb0_apiset_result res;

            if (ntb0_apiset_resolve(ld->apiset, descs[d].dll, &res) ==
                NTB0_APISET_OK) {
                snprintf(descs[d].host, sizeof(descs[0].host), "%s", res.host);
            }
        }
    }

    /* (3) تحميل الاعتماديات تلقائياً (اختياري) */
    if (flags & NTB0_DLL_FLAG_AUTO_DEPS) {
        for (d = 0; d < n_descs; d++) {
            ntb0_dll_module *dep;

            if (ntb0_dll_find_module(ld, descs[d].host))
                continue;
            if (in_load_stack(ld, descs[d].host)) {
                /* دورة: A → B → A ⇒ نكتشفها ولا نُدخل في حلقة لا نهائية */
                ld->cycles++;
                set_err(ld, "dependency cycle detected: '%s' -> '%s'",
                        mod->name, descs[d].host);
                continue;
            }
            dep = ntb0_dll_load_name(ld, descs[d].host,
                                     flags & ~NTB0_DLL_FLAG_STRICT_IMPORTS);
            if (!dep)
                set_err(ld, "cannot auto-load dependency '%s' of '%s'",
                        descs[d].host, mod->name);
        }
    }

    /* (4) حلّ الاستيرادات وكتابة الـ IAT */
    for (d = 0; d < n_descs; d++) {
        uint32_t thunk_rva = descs[d].ilt_rva ? descs[d].ilt_rva
                                              : descs[d].iat_rva;
        int bound = (descs[d].ilt_rva == 0);

        dep_add(mod, descs[d].host);

        for (i = 0; i < DLL_MAX_THUNKS; i++) {
            uint64_t trva = (uint64_t)thunk_rva + (uint64_t)i * ptr_size;
            uint64_t iat_rva = (uint64_t)descs[d].iat_rva +
                               (uint64_t)i * ptr_size;
            uint64_t v;
            uint64_t addr = 0;
            int ok = 0;
            ntb0_dll_import *rec;

            if (trva + ptr_size > mod->size) {
                set_err(ld, "import thunk of '%s' is outside the image",
                        descs[d].dll);
                rc = NTB0_DLL_EIMPORTS;
                goto out;
            }
            v = im->is64 ? rd64(mod->base + trva) : rd32(mod->base + trva);
            if (v == 0)
                break;                      /* نهاية قائمة الـ thunks */

            if (iat_rva + ptr_size > mod->size) {
                set_err(ld, "IAT slot of '%s' is outside the image",
                        descs[d].dll);
                rc = NTB0_DLL_EIMPORTS;
                goto out;
            }

            rec = import_push(mod, &import_cap);
            if (!rec) {
                rc = NTB0_DLL_ENOMEM;
                goto out;
            }
            memset(rec, 0, sizeof(*rec));
            snprintf(rec->dll, sizeof(rec->dll), "%s", descs[d].dll);
            snprintf(rec->host, sizeof(rec->host), "%s", descs[d].host);
            rec->iat_rva = (uint32_t)iat_rva;

            if (v & ord_flag) {
                /* استيراد بالترتيب */
                rec->by_ordinal = 1;
                rec->ordinal = (uint16_t)(v & 0xffff);
                snprintf(rec->func, sizeof(rec->func), "#%u",
                         (unsigned)rec->ordinal);
            } else {
                uint32_t name_rva = (uint32_t)(v & 0x7fffffffu);

                if (bound && name_rva >= mod->size) {
                    /* IAT مربوطة (bound import): القيمة عنوان مربوط
                     * لا RVA ⇒ لا يمكن تفسيرها ⇒ تُتجاهل */
                    mod->n_imports--;
                    continue;
                }
                if (name_rva + 2 >= mod->size) {
                    set_err(ld, "bad import-by-name RVA 0x%x", name_rva);
                    rc = NTB0_DLL_EIMPORTS;
                    goto out;
                }
                if (img_str(mod->base, mod->size, name_rva + 2, rec->func,
                            sizeof(rec->func)) != 0) {
                    set_err(ld, "bad import name at RVA 0x%x", name_rva + 2);
                    rc = NTB0_DLL_EIMPORTS;
                    goto out;
                }
            }

            /* (أ) من الوحدات المحمَّلة؟ (اختياري) */
            if (!ok && (flags & NTB0_DLL_FLAG_PREFER_LOCAL)) {
                ntb0_dll_module *m = ntb0_dll_find_module(ld, rec->host);

                if (m) {
                    if (rec->by_ordinal)
                        addr = (uint64_t)(uintptr_t)ntb0_dll_get_proc_ord(
                            ld, m, rec->ordinal);
                    else
                        addr = (uint64_t)(uintptr_t)ntb0_dll_get_proc(
                            ld, m, rec->func);
                    ok = addr ? 1 : 0;
                }
            }

            /* (ب) المُحلِّل الخارجي (الصندوق) */
            if (!ok && ld->resolve) {
                if (ld->resolve(ld->resolve_ctx, rec->host, rec->func,
                                &addr) == 0 && addr)
                    ok = 1;
                else
                    addr = 0;
            }

            if (ok) {
                rec->resolved = 1;
                rec->addr = addr;
                mod->imports_resolved++;
                if (im->is64)
                    *(uint64_t *)(mod->base + iat_rva) = addr;
                else
                    *(uint32_t *)(mod->base + iat_rva) = (uint32_t)addr;
            } else {
                rec->resolved = 0;
                rec->addr = 0;
                mod->imports_unresolved++;
                if (im->is64)
                    *(uint64_t *)(mod->base + iat_rva) = 0;
                else
                    *(uint32_t *)(mod->base + iat_rva) = 0;
            }
        }
    }

    if ((flags & NTB0_DLL_FLAG_STRICT_IMPORTS) && mod->imports_unresolved) {
        set_err(ld, "'%s' has %u unresolved import(s)", mod->name,
                mod->imports_unresolved);
        rc = NTB0_DLL_EIMPORTS;
    }

out:
    free(descs);
    return rc;
}

/* ------------------------------------------------------------------ */
/* DllMain/TLS callbacks support (T-131/T-132)                        */
/* ------------------------------------------------------------------ */
#define DLL_PROCESS_ATTACH   1
#define DLL_PROCESS_DETACH   0
#define DLL_THREAD_ATTACH    2
#define DLL_THREAD_DETACH    3

typedef int  (__attribute__((ms_abi)) *DllMainFn)(void *hinst, unsigned long reason, void *reserved);
typedef void (__attribute__((ms_abi)) *TlsCallbackFn)(void *hinst, unsigned long reason, void *reserved);

/* قائمة DLLs النظام التي لا يُستدعى لها DllMain (لتجنب الانهيار) */
static const char *system_dlls[] = {
    "KERNEL32.DLL",
    "NTDLL.DLL",
    "KERNELBASE.DLL",
    "ADVAPI32.DLL",
    "USER32.DLL",
    "GDI32.DLL",
    "MSVCRT.DLL",
    "WS2_32.DLL",
    "SHELL32.DLL",
    "OLE32.DLL",
    "OLEAUT32.DLL",
    "RPCRT4.DLL",
    "VERSION.DLL",
    "SHLWAPI.DLL",
    "COMDLG32.DLL",
    "WINMM.DLL",
    "WINSPOOL.DRV",
    "IMM32.DLL",
    "UXTHEME.DLL",
    "DWMAPI.DLL",
    NULL
};

static int is_system_dll(const char *name)
{
    unsigned i;
    for (i = 0; system_dlls[i]; i++) {
        if (ci_eq(name, system_dlls[i]))
            return 1;
    }
    return 0;
}

/* تحليل دليل TLS (DataDirectory[9]) واستخراج مصفوفة callbacks */
static int parse_tls_directory(struct ntb0_dll_module *mod)
{
    uint32_t tls_rva = 0, tls_size = 0;
    uint8_t *base = mod->base;

    if (!base || mod->size < 0x40)
        return 0;

    uint32_t e_lfanew = rd32(base + 0x3C);
    if (e_lfanew < 0x40 || (uint64_t)e_lfanew + 24 > mod->size)
        return 0;
    if (memcmp(base + e_lfanew, "PE\0\0", 4) != 0)
        return 0;

    uint16_t magic = rd16(base + e_lfanew + 24);
    uint32_t opt_off = e_lfanew + 24;
    uint32_t dd_off;

    if (magic == 0x20b) { /* PE32+ */
        dd_off = opt_off + 112;
    } else if (magic == 0x10b) { /* PE32 */
        dd_off = opt_off + 96;
    } else {
        return 0;
    }

    /* DataDirectory[9] = TLS */
    if (dd_off + 9 * 8 + 8 > mod->size)
        return 0;
    tls_rva  = rd32(base + dd_off + 9 * 8);
    tls_size = rd32(base + dd_off + 9 * 8 + 4);

    if (tls_rva == 0 || tls_size < 24)
        return 0; /* No TLS directory or too small */

    if ((uint64_t)tls_rva + tls_size > mod->size)
        return 0;

    uint8_t *tls_dir = base + tls_rva;

    /* TLS directory structure:
     * PE32 (32-bit):
     *   StartAddressOfRawData: 4 bytes
     *   EndAddressOfRawData: 4 bytes
     *   AddressOfIndex: 4 bytes
     *   AddressOfCallBacks: 4 bytes
     *   SizeOfZeroFill: 4 bytes
     *   Characteristics: 4 bytes
     *
     * PE32+ (64-bit):
     *   StartAddressOfRawData: 8 bytes
     *   EndAddressOfRawData: 8 bytes
     *   AddressOfIndex: 8 bytes
     *   AddressOfCallBacks: 8 bytes
     *   SizeOfZeroFill: 4 bytes
     *   Characteristics: 4 bytes
     */
    uint64_t callback_array_rva = 0;
    if (mod->is64) {
        callback_array_rva = rd64(tls_dir + 24);
    } else {
        callback_array_rva = rd32(tls_dir + 16);
    }

    if (callback_array_rva == 0)
        return 0;

    if (callback_array_rva >= mod->size)
        return 0;

    uint64_t *callbacks = (uint64_t *)(base + callback_array_rva);
    unsigned count = 0;

    /* Count callbacks (NULL-terminated array) */
    while (callbacks[count] != 0 && count < 1024) {
        if (callbacks[count] >= mod->size)
            break;
        count++;
    }

    if (count == 0)
        return 0;

    mod->tls_callbacks = (uint64_t *)malloc(count * sizeof(uint64_t));
    if (!mod->tls_callbacks)
        return 0;

    for (unsigned i = 0; i < count; i++)
        mod->tls_callbacks[i] = callbacks[i];
    mod->tls_callback_count = count;

    return 1;
}

/* بوّابة التنفيذ (تعريف مسبق — واجهتها بين TLS و DllMain) */
static int can_run_entry_real(struct ntb0_dll_module *mod, const char **why);

/* تنفيذ TLS callbacks — عبر بوّابة الحصانة نفسها (القياس: usp10) */
static int run_tls_callbacks_guarded(struct ntb0_dll_module *mod,
                                     const char *phase)
{
    const char *why = NULL;

    if (!mod || !mod->tls_callbacks || mod->tls_callback_count == 0)
        return 0;
    if (mod->is_system_dll)
        return 0;
    if (!can_run_entry_real(mod, &why)) {
        fprintf(stderr,
                "[dll-loader][STUB] TLS callbacks skipped for '%s' "
                "(%s): %s\n",
                mod->name, phase, why ? why : "refused");
        return 0;
    }
    return 1;
}

/* تنفيذ TLS callbacks */
static void run_tls_callbacks(struct ntb0_dll_module *mod, unsigned long reason)
{
    if (!mod || !mod->tls_callbacks || mod->tls_callback_count == 0)
        return;

    for (unsigned i = 0; i < mod->tls_callback_count; i++) {
        uint64_t cb_rva = mod->tls_callbacks[i];
        if (cb_rva >= mod->size)
            continue;
        TlsCallbackFn cb = (TlsCallbackFn)(mod->base + cb_rva);
        cb(mod->base, reason, NULL);
    }
}

/* استدعاء DllMain مع DLL_PROCESS_ATTACH
 * قاعدة الحصانة صارمة (§ دليل المحمّل): كود Win7 الحقيقي **لا يُنفَّذ**
 * — كودنا للاختبار فقط (T-131/T-132). الشرط الثلاثي:
 *   1) ليست وحدة نظام (KERNEL32/NTDLL/…).
 *   2) صفر استيراد غير محلول — DllMain حقيقي مع فجوات IAT=0 ⇒ اندثار
 *      عبر نداء NULL (مُقاس: usp10.dll+0x7aa65) ⇒ رفض صريح.
 *   3) نقطة الدخول داخل **قسم تنفيذي** — DllMain في قسم data (.rdata)
 *      بلا بت IMAGE_SCN_MEM_EXECUTE ⇒ لا كود حقيقي ⇒ رفض صريح.
 * النتيجة: DllMain الاختبارية (mov eax,1; ret) تعمل، وأي DllMain حقيقي
 * مرفوض بمصباح STUB صادق، لا انهيار. */
static int can_run_entry_real(struct ntb0_dll_module *mod, const char **why)
{
    unsigned i;

    if (!mod->base || mod->size < 0x40) { *why = "no image"; return 0; }
    if (mod->imports_unresolved) {
        *why = "unresolved imports (IAT has holes)";
        return 0;
    }
    for (i = 0; i < (unsigned)mod->nsec && i < DLL_MAX_SECTIONS; i++) {
        const struct dll_sec *s = &mod->sec[i];
        uint64_t va = s->va, vsz = s->vsize;
        if ((s->chars & SCN_MEM_EXECUTE) && vsz &&
            mod->entry_rva >= va && mod->entry_rva < va + vsz)
            return 1;
    }
    *why = "entry point outside any executable section";
    return 0;
}

static int call_dll_main(struct ntb0_dll_module *mod, unsigned long reason)
{
    DllMainFn entry;
    int rc;

    if (!mod || !mod->dllmain_entry)
        return 1; /* no entry point = success */

    entry = (DllMainFn)mod->dllmain_entry;
    rc = entry(mod->base, reason, NULL);
    return rc;
}

/* ------------------------------------------------------------------ */
/* التصدير (GetProcAddress)                                           */
/* ------------------------------------------------------------------ */
static void *export_addr(ntb0_dll_loader *ld, struct ntb0_dll_module *mod,
                          const ntb0_pe_export *e, int depth);

static void *export_addr_name(ntb0_dll_loader *ld, struct ntb0_dll_module *mod,
                              const char *name, int depth)
{
    ntb0_pe_export e;

    if (!mod || !mod->pe || !name || !*name || depth > 8)
        return NULL;
    if (!ntb0_pe_find_by_name(mod->pe, name, &e))
        return NULL;
    return export_addr(ld, mod, &e, depth);
}

static void *export_addr_ord(ntb0_dll_loader *ld, struct ntb0_dll_module *mod,
                              uint16_t ordinal, int depth)
{
    ntb0_pe_export e;

    if (!mod || !mod->pe || depth > 8)
        return NULL;
    if (!ntb0_pe_find_by_ordinal(mod->pe, ordinal, &e))
        return NULL;
    return export_addr(ld, mod, &e, depth);
}

static void *export_addr(ntb0_dll_loader *ld, struct ntb0_dll_module *mod,
                          const ntb0_pe_export *e, int depth)
{
    if (!mod || !e || depth > 8)
        return NULL;

    if (e->forwarded && e->forwarder) {
        /* "DLL.Func" أو "DLL.#ord" */
        const char *dot = strchr(e->forwarder, '.');
        char dll[128];
        const char *fn;
        size_t dl;
        ntb0_dll_module *target;

        if (!dot)
            return NULL;
        dl = (size_t)(dot - e->forwarder);
        if (dl >= sizeof(dll))
            dl = sizeof(dll) - 1;
        memcpy(dll, e->forwarder, dl);
        dll[dl] = '\0';
        fn = dot + 1;

        target = ntb0_dll_find_module(ld, dll);
        if (!target && !strchr(dll, '.')) {
            /* سلاسل الـ forwarders تستعمل الاسم بلا ".dll" ("NTDLL.Foo") */
            char alt[132];

            snprintf(alt, sizeof(alt), "%s.dll", dll);
            target = ntb0_dll_find_module(ld, alt);
        }
        if (!target)
            return NULL;
        if (fn[0] == '#') {
            int ord = ntb0_dll_ordinal_parse(fn);

            if (ord < 0)
                return NULL;
            return export_addr_ord(ld, target, (uint16_t)ord, depth + 1);
        }
        return export_addr_name(ld, target, fn, depth + 1);
    }

    if (e->rva == 0 || e->rva >= mod->size)
        return NULL;
    return mod->base + e->rva;
}

void *ntb0_dll_get_proc(ntb0_dll_loader *ld, struct ntb0_dll_module *mod,
                        const char *name)
{
    return export_addr_name(ld, mod, name, 0);
}

void *ntb0_dll_get_proc_ord(ntb0_dll_loader *ld, struct ntb0_dll_module *mod,
                            uint16_t ordinal)
{
    return export_addr_ord(ld, mod, ordinal, 0);
}

int ntb0_dll_ordinal_parse(const char *func)
{
    long v;
    char *end = NULL;

    if (!func || func[0] != '#' || func[1] == '\0')
        return -1;
    v = strtol(func + 1, &end, 10);
    if (!end || *end != '\0' || v < 0 || v > 0xffff)
        return -1;
    return (int)v;
}

/* ------------------------------------------------------------------ */
/* قائمة الوحدات                                                      */
/* ------------------------------------------------------------------ */
int ntb0_dll_module_count(const ntb0_dll_loader *ld)
{
    return ld ? (int)ld->n_mods : 0;
}

ntb0_dll_module *ntb0_dll_module_at(ntb0_dll_loader *ld, int idx)
{
    if (!ld || idx < 0 || (unsigned)idx >= ld->n_mods)
        return NULL;
    return ld->mods[idx];
}

ntb0_dll_module *ntb0_dll_find_module(ntb0_dll_loader *ld, const char *name)
{
    unsigned i;

    if (!ld || !name || !*name)
        return NULL;
    for (i = 0; i < ld->n_mods; i++) {
        if (ci_eq(ld->mods[i]->name, name))
            return ld->mods[i];
        if (ld->mods[i]->pe && ntb0_pe_dll_name(ld->mods[i]->pe)[0] &&
            ci_eq(ntb0_pe_dll_name(ld->mods[i]->pe), name))
            return ld->mods[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* الإنشاء/التحرير                                                     */
/* ------------------------------------------------------------------ */
static void module_free(struct ntb0_dll_module *mod);   /* معرَّف لاحقاً */

ntb0_dll_loader *ntb0_dll_loader_new(void)
{
    ntb0_dll_loader *ld = (ntb0_dll_loader *)calloc(1, sizeof(*ld));

    if (!ld)
        return NULL;
    snprintf(ld->err, sizeof(ld->err), "ok");
    return ld;
}

void ntb0_dll_loader_free(ntb0_dll_loader *ld)
{
    unsigned i;

    if (!ld)
        return;
    /* تفكيك قسري: المراجع لا تمنع الإفلات عند حذف المحمّل كلّه */
    while (ld->n_mods) {
        struct ntb0_dll_module *m = ld->mods[ld->n_mods - 1];

        ld->n_mods--;
        ld->mods[ld->n_mods] = NULL;
        module_free(m);
    }
    for (i = 0; i < DLL_MAX_MODULES; i++)
        ld->mods[i] = NULL;
    free(ld);
}

int ntb0_dll_loader_set_resolver(ntb0_dll_loader *ld, ntb0_dll_resolve_fn fn,
                                 void *ctx)
{
    if (!ld)
        return NTB0_DLL_EFORMAT;
    ld->resolve = fn;
    ld->resolve_ctx = ctx;
    return NTB0_DLL_OK;
}

int ntb0_dll_loader_set_apiset(ntb0_dll_loader *ld, ntb0_apiset *as)
{
    if (!ld)
        return NTB0_DLL_EFORMAT;
    ld->apiset = as;
    return NTB0_DLL_OK;
}

int ntb0_dll_loader_set_sysdir(ntb0_dll_loader *ld, const char *dir)
{
    if (!ld || !dir || !*dir)
        return NTB0_DLL_EFORMAT;
    snprintf(ld->sysdir, sizeof(ld->sysdir), "%s", dir);
    return NTB0_DLL_OK;
}

int ntb0_dll_loader_set_syswow64_sysdir(ntb0_dll_loader *ld, const char *dir)
{
    if (!ld || !dir || !*dir)
        return NTB0_DLL_EFORMAT;
    snprintf(ld->syswow64_sysdir, sizeof(ld->syswow64_sysdir), "%s", dir);
    return NTB0_DLL_OK;
}

/* ------------------------------------------------------------------ */
/* التحميل                                                            */
/* ------------------------------------------------------------------ */
static void module_free(struct ntb0_dll_module *mod)
{
    if (!mod)
        return;
    if (mod->pe)
        ntb0_pe_close(mod->pe);
    if (mod->base)
        munmap(mod->base, mod->map_size);
    free(mod->imports);
    free(mod->tls_callbacks);
    free(mod);
}

static void stack_push(ntb0_dll_loader *ld, struct ntb0_dll_module *mod)
{
    if (ld->n_stack < DLL_MAX_STACK)
        ld->stack[ld->n_stack++] = mod;
}

static void stack_pop(ntb0_dll_loader *ld)
{
    if (ld->n_stack)
        ld->n_stack--;
}

ntb0_dll_module *ntb0_dll_load(ntb0_dll_loader *ld, const char *path,
                                unsigned flags)
{
    return ntb0_dll_load_at(ld, path, 0, flags);
}

ntb0_dll_module *ntb0_pe_load_image(ntb0_dll_loader *ld, const char *path,
                                    uint64_t base, unsigned flags)
{
    return ntb0_dll_load_at(ld, path, base, flags);
}



ntb0_dll_module *ntb0_dll_load_at(ntb0_dll_loader *ld, const char *path,
                                   uint64_t base, unsigned flags)
{
    NDBG("ntb0_dll_load_at(%s, base=0x%llx, flags=%u)\n", path ? path : "NULL", (unsigned long long)base, flags);
    struct ntb0_dll_module *mod = NULL;
    struct dll_img im;
    uint8_t *buf = NULL;
    size_t len = 0;
    char perr[256];
    int rc;

    if (!ld || !path || !*path) {
        set_err(ld, "bad arguments to ntb0_dll_load");
        return NULL;
    }

    buf = (uint8_t *)read_file(path, &len);
    if (!buf) {
        set_err(ld, "cannot read '%s'", path);
        return NULL;
    }

    rc = parse_pe(buf, len, &im, perr, sizeof(perr));
    if (rc != NTB0_DLL_OK) {
        set_err(ld, "'%s': %s", path, perr);
        free(buf);
        return NULL;
    }

    mod = (struct ntb0_dll_module *)calloc(1, sizeof(*mod));
    if (!mod) {
        set_err(ld, "out of memory for the module");
        free(buf);
        return NULL;
    }
    mod->ld = ld;
    mod->is64 = im.is64;
    mod->nsec = im.nsec;
    mod->entry_rva = im.entry_rva;
    memcpy(mod->sec, im.sec, sizeof(mod->sec));
    snprintf(mod->path, sizeof(mod->path), "%s", path);
    base_name(path, mod->name, sizeof(mod->name));

    NDBG("ntb0_dll_load_at: before map_image path='%s' base=0x%llx force_base=%llx pref_base=0x%llx size_of_image=0x%x size_of_headers=0x%x nsec=%u\n",
            path, (unsigned long long)base, (unsigned long long)base,
            (unsigned long long)im.image_base, im.size_of_image, im.size_of_headers, im.nsec);
    rc = map_image(ld, mod, &im, flags, base);
    NDBG("ntb0_dll_load_at: after map_image rc=%d mod->base=0x%llx mod->map_size=%zu mod->size=%zu mod->pref_base=0x%llx mod->delta=%+lld\n",
            rc, (unsigned long long)(mod ? (uintptr_t)mod->base : 0),
            mod ? mod->map_size : 0, mod ? mod->size : 0,
            (unsigned long long)(mod ? mod->pref_base : 0),
            (long long)(mod ? mod->delta : 0));
    if (rc != NTB0_DLL_OK)
        goto fail;

    NDBG("ntb0_dll_load_at: before copy_sections mod->base=0x%llx mod->map_size=%zu mod->size=%zu im->size_of_image=0x%x nsec=%u\n",
            (unsigned long long)(uintptr_t)mod->base, mod->map_size, mod->size,
            im.size_of_image, im.nsec);
    copy_sections(mod, &im, buf, len);
    NDBG("ntb0_dll_load_at: after copy_sections done mod->base=0x%llx mod->map_size=%zu mod->size=%zu\n",
            (unsigned long long)(uintptr_t)mod->base, mod->map_size, mod->size);

    /* إعادة التوجيه */
    if (!(flags & NTB0_DLL_FLAG_NO_RELOCS)) {
        rc = apply_relocs(ld, mod, &im);
        if (rc != NTB0_DLL_OK)
            goto fail;
    }

    /* الاستيرادات */
    stack_push(ld, mod);
    rc = resolve_imports(ld, mod, &im, flags);
    stack_pop(ld);
    if (rc != NTB0_DLL_OK)
        goto fail;

    /* الصلاحيات النهائية */
    if (apply_protections(mod) != 0) {
        set_err(ld, "cannot apply the section protections of '%s'", mod->name);
        rc = NTB0_DLL_EFORMAT;
        goto fail;
    }

    /* جدول التصديرات (GetProcAddress) */
    mod->pe = ntb0_pe_open(path);
    if (!mod->pe)
        set_err(ld, "'%s' has no usable export table: %s", mod->name,
                ntb0_pe_last_error());

    /* تعيين نقطة دخول DllMain */
    if (mod->entry_rva != 0 && mod->entry_rva < mod->size)
        mod->dllmain_entry = mod->base + mod->entry_rva;

    /* تحديد ما إذا كانت DLL نظام */
    mod->is_system_dll = is_system_dll(mod->name);

    /* TLS callbacks + DllMain (T-131/T-132) — فقط عبر بوّابة الحصانة
     * (NTB0_DLL_FLAG_RUN_ENTRY — انظر ملف الترويسة ملف dll_loader.h) */
    parse_tls_directory(mod);
    if ((flags & NTB0_DLL_FLAG_RUN_ENTRY) &&
        run_tls_callbacks_guarded(mod, "PROCESS_ATTACH"))
        run_tls_callbacks(mod, DLL_PROCESS_ATTACH);

    /* استدعاء DllMain مع DLL_PROCESS_ATTACH (للـ DLLs غير النظام) */
    if ((flags & NTB0_DLL_FLAG_RUN_ENTRY) && !mod->is_system_dll) {
        const char *why = NULL;

        if (can_run_entry_real(mod, &why)) {
            call_dll_main(mod, DLL_PROCESS_ATTACH);
            mod->dllmain_called = 1;
        } else {
            /* رفض صريح وصادق — لا كود ويندوز يُنفَّذ خلف بوّابة مغلقة */
            fprintf(stderr,
                    "[dll-loader][STUB] DllMain/DLL_PROCESS_ATTACH skipped "
                    "for '%s': %s (real Win7 code is never executed)\n",
                    mod->name, why ? why : "refused");
        }
    }

    /* التسجيل في قائمة الوحدات */
    if (ld->n_mods >= DLL_MAX_MODULES) {
        set_err(ld, "too many loaded modules (max %u)", DLL_MAX_MODULES);
        rc = NTB0_DLL_ENOMEM;
        goto fail;
    }
    ld->mods[ld->n_mods++] = mod;
    mod->refs = 1;

    free(buf);
    return mod;

fail:
    if (mod)
        module_free(mod);
    free(buf);
    return NULL;
}

/* يبحث عن ملف بالاسم داخل مجلد (غير حسّاس لحالة الأحرف) */
static int find_file_ci(const char *dir, const char *name, char *out,
                         size_t outsz)
{
    DIR *d;
    struct dirent *de;

    if (!dir || !*dir || !name || !*name)
        return -1;
    d = opendir(dir);
    if (!d)
        return -1;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        if (ci_eq(de->d_name, name)) {
            snprintf(out, outsz, "%s/%s", dir, de->d_name);
            closedir(d);
            return 0;
        }
    }
    closedir(d);
    return -1;
}

ntb0_dll_module *ntb0_dll_load_name(ntb0_dll_loader *ld, const char *dllname,
                                     unsigned flags)
{
    char path[1024];
    ntb0_dll_module *m;

    if (!ld || !dllname || !*dllname) {
        set_err(ld, "bad arguments to ntb0_dll_load_name");
        return NULL;
    }

    /* محمَّلة مسبقاً؟ */
    m = ntb0_dll_find_module(ld, dllname);
    if (m) {
        m->refs++;
        return m;
    }

    /* مسار مطلق؟ */
    if (strchr(dllname, '/')) {
        if (access(dllname, R_OK) != 0) {
            set_err(ld, "'%s' is not readable", dllname);
            return NULL;
        }
        snprintf(path, sizeof(path), "%s", dllname);
        return ntb0_dll_load(ld, path, flags);
    }

    /* Priority 1: Try syswow64_sysdir if set (PE32 system DLLs) — full peer to System32 */
    if (ld->syswow64_sysdir[0] && find_file_ci(ld->syswow64_sysdir, dllname, path, sizeof(path)) == 0) {
        NDBG("ntb0_dll_load_name: found '%s' in syswow64_sysdir='%s'\n", dllname, ld->syswow64_sysdir);
        return ntb0_dll_load(ld, path, flags);
    }

    /* Priority 2: Try sysdir (PE64 system DLLs) */
    if (!ld->sysdir[0]) {
        set_err(ld, "no system directory set (cannot find '%s')", dllname);
        return NULL;
    }
    if (find_file_ci(ld->sysdir, dllname, path, sizeof(path)) == 0) {
        NDBG("ntb0_dll_load_name: found '%s' in sysdir='%s'\n", dllname, ld->sysdir);
        return ntb0_dll_load(ld, path, flags);
    }

    /* Priority 3: Fallback — auto-substitute System32 → SysWOW64 if not found above */
    if (!ld->syswow64_sysdir[0]) {
        char syswow64[512];
        const char *p = ld->sysdir;
        /* محاولة استبدال System32 بـ SysWOW64 */
        if (strstr(p, "System32")) {
            snprintf(syswow64, sizeof(syswow64), "%s", p);
            char *q = strstr(syswow64, "System32");
            if (q) {
                strcpy(q, "SysWOW64");
                q += 8;
                /* حفظ ما تبقى */
                snprintf(syswow64, sizeof(syswow64), "%s", p);
                char *q2 = strstr(syswow64, "System32");
                if (q2) {
                    memmove(q2 + 8, q2 + 8, strlen(q2 + 8) + 1);
                    memcpy(q2, "SysWOW64", 8);
                }
                if (find_file_ci(syswow64, dllname, path, sizeof(path)) == 0) {
                    NDBG("ntb0_dll_load_name: found '%s' via auto-substituted SysWOW64 path\n", dllname);
                    return ntb0_dll_load(ld, path, flags);
                }
            }
        } else {
            /* إضافة /SysWOW64 بجانب System32 إن وُجد */
            snprintf(syswow64, sizeof(syswow64), "%s/../SysWOW64", p);
            if (find_file_ci(syswow64, dllname, path, sizeof(path)) == 0) {
                NDBG("ntb0_dll_load_name: found '%s' via auto-substituted SysWOW64 path (relative)\n", dllname);
                return ntb0_dll_load(ld, path, flags);
            }
        }
    }
    set_err(ld, "'%s' not found in '%s' or '%s'", dllname, ld->sysdir, ld->syswow64_sysdir);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* التفريغ                                                            */
/* ------------------------------------------------------------------ */
int ntb0_dll_unload(ntb0_dll_loader *ld, struct ntb0_dll_module *mod)
{
    unsigned i;

    if (!ld || !mod)
        return NTB0_DLL_EFORMAT;

    for (i = 0; i < ld->n_mods; i++) {
        if (ld->mods[i] == mod) {
            unsigned j;

            /* عدّ المراجع: الوحدة تبقى محمَّلة ما دام مرجعٌ حيّ
             * (دلالة LoadLibrary/FreeLibrary في ويندوز). */
            if (mod->refs > 1) {
                mod->refs--;
                return NTB0_DLL_OK;
            }

            for (j = i; j + 1 < ld->n_mods; j++)
                ld->mods[j] = ld->mods[j + 1];
            ld->n_mods--;
            ld->mods[ld->n_mods] = NULL;
            module_free(mod);
            return NTB0_DLL_OK;
        }
    }
    return NTB0_DLL_ENOTFOUND;
}

/* ------------------------------------------------------------------ */
/* الاستعلام                                                          */
/* ------------------------------------------------------------------ */
uint64_t ntb0_dll_module_base(const ntb0_dll_module *mod)
{
    return mod ? (uint64_t)(uintptr_t)mod->base : 0;
}

uint64_t ntb0_dll_module_pref_base(const ntb0_dll_module *mod)
{
    return mod ? mod->pref_base : 0;
}

int64_t ntb0_dll_module_delta(const ntb0_dll_module *mod)
{
    return mod ? mod->delta : 0;
}

size_t ntb0_dll_module_size(const ntb0_dll_module *mod)
{
    return mod ? mod->size : 0;
}

int ntb0_dll_module_is64(const ntb0_dll_module *mod)
{
    return mod ? mod->is64 : 0;
}

int ntb0_dll_module_nsec(const ntb0_dll_module *mod)
{
    return mod ? mod->nsec : 0;
}

const char *ntb0_dll_module_path(const ntb0_dll_module *mod)
{
    return mod ? mod->path : "";
}

const char *ntb0_dll_module_name(const ntb0_dll_module *mod)
{
    return mod ? mod->name : "";
}

unsigned ntb0_dll_module_reloc_blocks(const ntb0_dll_module *mod)
{
    return mod ? mod->reloc_blocks : 0;
}

unsigned ntb0_dll_module_reloc_entries(const ntb0_dll_module *mod)
{
    return mod ? mod->reloc_entries : 0;
}

unsigned ntb0_dll_module_reloc_applied(const ntb0_dll_module *mod)
{
    return mod ? mod->reloc_applied : 0;
}

unsigned ntb0_dll_module_imports_resolved(const ntb0_dll_module *mod)
{
    return mod ? mod->imports_resolved : 0;
}

unsigned ntb0_dll_module_imports_unresolved(const ntb0_dll_module *mod)
{
    return mod ? mod->imports_unresolved : 0;
}

unsigned ntb0_dll_module_reloc_absolute(const ntb0_dll_module *mod)
{
    return mod ? mod->reloc_abs : 0;
}

unsigned ntb0_dll_module_reloc_unsupported(const ntb0_dll_module *mod)
{
    return mod ? mod->reloc_unsupported : 0;
}

unsigned ntb0_dll_module_import_dll_count(const ntb0_dll_module *mod)
{
    return mod ? mod->n_import_dlls : 0;
}

unsigned ntb0_dll_module_apiset_count(const ntb0_dll_module *mod)
{
    return mod ? mod->n_apiset : 0;
}

uint64_t ntb0_dll_module_rva_to_addr(const ntb0_dll_module *mod, uint32_t rva)
{
    if (!mod || !mod->base || rva >= mod->size)
        return 0;
    return (uint64_t)(uintptr_t)(mod->base + rva);
}

uint32_t ntb0_dll_module_entry_rva(const ntb0_dll_module *mod)
{
    return mod ? mod->entry_rva : 0;
}

/* DllMain/TLS callbacks (T-131/T-132) */
void *ntb0_dll_module_dllmain(const ntb0_dll_module *mod)
{
    return mod ? mod->dllmain_entry : NULL;
}

int ntb0_dll_module_dllmain_called(const ntb0_dll_module *mod)
{
    return mod ? mod->dllmain_called : 0;
}

unsigned ntb0_dll_module_tls_callback_count(const ntb0_dll_module *mod)
{
    return mod ? mod->tls_callback_count : 0;
}

uint64_t ntb0_dll_module_tls_callback_at(const ntb0_dll_module *mod, unsigned idx)
{
    if (!mod || !mod->tls_callbacks || idx >= mod->tls_callback_count)
        return 0;
    return mod->tls_callbacks[idx];
}

int ntb0_dll_module_is_system(const ntb0_dll_module *mod)
{
    return mod ? mod->is_system_dll : 0;
}

int ntb0_dll_import_count(const ntb0_dll_module *mod)
{
    return mod ? (int)mod->n_imports : 0;
}

int ntb0_dll_import_at(const ntb0_dll_module *mod, int idx,
                       ntb0_dll_import *out)
{
    if (!mod || !out || idx < 0 || (unsigned)idx >= mod->n_imports)
        return -1;
    *out = mod->imports[idx];
    return 0;
}

int ntb0_dll_dep_count(const ntb0_dll_module *mod)
{
    return mod ? (int)mod->n_deps : 0;
}

const char *ntb0_dll_dep_at(const ntb0_dll_module *mod, int idx)
{
    if (!mod || idx < 0 || (unsigned)idx >= mod->n_deps)
        return NULL;
    return mod->dep[idx];
}

unsigned ntb0_dll_cycle_count(const ntb0_dll_loader *ld)
{
    return ld ? ld->cycles : 0;
}
