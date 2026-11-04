/*
 * pe_export.c — قارئ جدول تصديرات PE (EAT) لويندوز 7 الحقيقي.
 *
 * المسار المُقاس (يعتمد على الصور الحقيقية في win7root):
 *   DOS header (e_lfanew عند 0x3C)
 *     → "PE\0\0"
 *     → COFF header (NumberOfSections, SizeOfOptionalHeader)
 *     → Optional header (Magic 0x10b PE32 / 0x20b PE32+)
 *     → DataDirectory[0] = Export Table (RVA + size)
 *     → Section table (RVA ⇒ file offset)
 *     → IMAGE_EXPORT_DIRECTORY (40 بايت)
 *          Name / Base / NumberOfFunctions / NumberOfNames
 *          AddressOfFunctions / AddressOfNames / AddressOfNameOrdinals
 *
 * ملاحظات مهمّة:
 *   - **Forwarders**: RVA التصدير إذا وقع داخل مدى دليل التصديرات ⇒
 *     هو *سلسلة* "KERNEL32.Foo" لا كود. (نقيسها في kernel32.dll: مثل
 *     AcquireSRWLockExclusive ⇒ forwarder إلى ntdll).
 *   - كل قراءة داخل حدود الملف (map + size) — ملف تالف ⇒ خطأ نظيف.
 *   - لا تسريب: كل التخصيصات تُحرَّر في ntb0_pe_close (أو عند الفشل).
 *
 * المصباح: DLL-001.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "pe_export.h"

/* حدود وقائية (against absurd/corrupt headers) */
#define PE_MAX_SECTIONS   96u
#define PE_MAX_EXPORTS    65536u
#define PE_MAX_STRLEN     4096u

/* إزاحات داخل الترويسة الاختيارية */
#define PE32_NUMRVA_OFF   92u
#define PE32_DATADIR_OFF  96u
#define PE64_NUMRVA_OFF   108u
#define PE64_DATADIR_OFF  112u

struct pe_section {
    uint32_t va;      /* VirtualAddress */
    uint32_t vsize;   /* VirtualSize */
    uint32_t raw;     /* PointerToRawData */
    uint32_t rsize;   /* SizeOfRawData */
};

struct ntb0_pe {
    int      fd;
    uint8_t *map;
    size_t   size;

    int      is64;
    uint32_t image_base;
    uint32_t hdr_size;          /* SizeOfHeaders */

    struct pe_section sec[PE_MAX_SECTIONS];
    uint32_t nsec;

    /* Export directory */
    uint32_t exp_rva, exp_size;
    uint32_t exp_base;
    uint32_t nfunc, nnames;
    uint32_t af_rva, an_rva, ao_rva;
    const char *dll_name;       /* داخل الخريطة */
    char   *dll_name_copy;      /* نسخة مملوكة (strlen آمن) */

    /* جدول التصديرات المرتَّب بالترتيب */
    ntb0_pe_export *slots;      /* nfunc خانة */
    uint16_t       *named_idx;  /* فهارس الخانات ذات الأسماء */
    int             n_named;

    char   *strbuf;             /* كل الأسماء في مخزن واحد */
    size_t  strbuf_len;
    size_t  strbuf_cap;
    char   *path;               /* مسار الملف */
};

/* آخر خطأ (عام — غير آمن مع الخيوط،Enough لمشروعنا) */
static char g_pe_err[256];

static void pe_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_pe_err, sizeof(g_pe_err), fmt, ap);
    va_end(ap);
}

const char *ntb0_pe_last_error(void)
{
    return g_pe_err;
}

/* ------------------------------------------------------------------ */
/* أدوات قراءة                                                        */
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

/* ------------------------------------------------------------------ */
/* RVA ⇒ file offset                                                  */
/* ------------------------------------------------------------------ */

int ntb0_pe_rva_to_offset(const ntb0_pe *pe, uint32_t rva, uint32_t *off)
{
    uint32_t i;

    if (!pe || !off)
        return -1;

    /* داخل الترويسات ⇒ الإزاحة = RVA */
    if (rva < pe->hdr_size) {
        if (rva >= pe->size)
            return -1;
        *off = rva;
        return 0;
    }

    for (i = 0; i < pe->nsec; i++) {
        const struct pe_section *s = &pe->sec[i];
        uint32_t span = s->vsize ? s->vsize : s->rsize;

        if (span == 0)
            continue;
        if (rva >= s->va && rva < s->va + span) {
            uint32_t delta = rva - s->va;

            /* أجزاء الملف التي لا وجود لها على القرص */
            if (delta >= s->rsize)
                return -1;
            if (s->raw + delta >= pe->size)
                return -1;
            *off = s->raw + delta;
            return 0;
        }
    }

    return -1;
}

/* ------------------------------------------------------------------ */
/* مخزن النصوص                                                        */
/* ------------------------------------------------------------------ */

/*
 * ملاحظة مهمّة: المخزن يُحجَّم **مرة واحدة** قبل أي نسخ (انظر
 * build_exports) — لأنّ pe->slots[].name pointers تشير داخله؛ لو
 * تغيّر عنوان المخزن (realloc) لَتَبطلت تلك المؤشرات.
 */

/*
 * يقيس سلسلة NUL-terminated عند إزاحة ملف.
 * limit = أقصى عدد بايت مسموح قراءته.
 * يُعيد 0 عند النجاح (ويضع الطول في len_out) أو -1 إن كانت
 * السلسلة غير منتهية/خارج الحدود.
 */
static int pe_str_at(const ntb0_pe *pe, uint32_t off, uint32_t limit,
                     size_t *len_out)
{
    size_t len = 0;

    if (off >= pe->size || limit == 0)
        return -1;

    while (len < limit && len < PE_MAX_STRLEN && off + len < pe->size &&
           pe->map[off + len] != '\0')
        len++;

    /* سلسلة غير منتهية داخل الحدود ⇒ تلف */
    if (off + len >= pe->size || pe->map[off + len] != '\0')
        return -1;

    *len_out = len;
    return 0;
}

/*
 * ينسخ سلسلة من الخريطة إلى المخزن (المخزن مُحجَّم مسبقاً — لا realloc
 * هنا، لأنّ المؤشرات المخزَّنة (اسم التصدير) يجب أن تبقى صالحة!).
 * يُعيد ptr إلى النسخة أو NULL.
 */
static const char *strbuf_copy(ntb0_pe *pe, uint32_t off, uint32_t limit)
{
    size_t len = 0;
    char *dst;

    if (pe_str_at(pe, off, limit, &len) != 0)
        return NULL;

    if (pe->strbuf_len + len + 1 > pe->strbuf_cap)
        return NULL;                 /* لا مكان (خطأ في الحساب) */

    dst = pe->strbuf + pe->strbuf_len;
    memcpy(dst, pe->map + off, len);
    dst[len] = '\0';
    pe->strbuf_len += len + 1;
    return dst;
}

/* ------------------------------------------------------------------ */
/* بناء جدول التصديرات                                               */
/* ------------------------------------------------------------------ */

static const char *g_extra_kernel32_stubs[] = {
    "GetModuleHandleExW",
    "GetCurrentDirectoryW",
    "SetCurrentDirectoryW",
    "GetEnvironmentVariableW",
    "SetEnvironmentVariableW",
    "GlobalAlloc",
    "GlobalFree",
    "LocalAlloc",
    "LocalFree",
    "MultiByteToWideChar",
    "WideCharToMultiByte",
    "GetVersionExW",
    "GetModuleHandleW",
    "GetModuleHandleA",
    "GetModuleFileNameW",
    "GetModuleFileNameA",
    "GetCommandLineW",
    "GetCommandLineA",
    "GetStartupInfoW",
    "GetLastError",
    "SetLastError",
    "Sleep",
    "HeapAlloc",
    "HeapFree",
    "VirtualAlloc",
    "VirtualFree",
    NULL
};

static int build_exports(ntb0_pe *pe)
{
    uint32_t eo, af, an, ao, i;
    const uint8_t *d;
    uint16_t *name_ord = NULL;
    uint32_t *name_off = NULL;
    uint32_t *fwd_off = NULL;
    uint8_t  *have_name = NULL;
    uint8_t  *have_fwd = NULL;
    int rc = NTB0_PE_OK;

    pe->slots = NULL;
    pe->named_idx = NULL;
    pe->n_named = 0;

    if (pe->exp_rva == 0 || pe->exp_size == 0)
        return NTB0_PE_OK;         /* صورة بلا تصديرات (EXE مثلاً) */

    if (ntb0_pe_rva_to_offset(pe, pe->exp_rva, &eo) != 0) {
        pe_err("export directory RVA 0x%x is not inside the file", pe->exp_rva);
        return NTB0_PE_EFORMAT;
    }
    if (eo + 40 > pe->size) {
        pe_err("export directory is truncated");
        return NTB0_PE_EFORMAT;
    }

    d = pe->map + eo;
    pe->exp_base  = rd32(d + 16);
    pe->nfunc     = rd32(d + 20);
    pe->nnames    = rd32(d + 24);
    pe->af_rva    = rd32(d + 28);
    pe->an_rva    = rd32(d + 32);
    pe->ao_rva    = rd32(d + 36);

    if (pe->nfunc > PE_MAX_EXPORTS || pe->nnames > PE_MAX_EXPORTS) {
        pe_err("absurd export counts (%u functions, %u names)", pe->nfunc,
               pe->nnames);
        return NTB0_PE_EFORMAT;
    }

    /* الاسم الداخلي للـ DLL */
    {
        uint32_t nrva = rd32(d + 12);
        uint32_t noff;

        if (ntb0_pe_rva_to_offset(pe, nrva, &noff) == 0) {
            size_t len = 0;

            while (noff + len < pe->size && pe->map[noff + len] != '\0' &&
                   len < PE_MAX_STRLEN)
                len++;
            if (noff + len < pe->size) {
                pe->dll_name_copy = (char *)malloc(len + 1);
                if (pe->dll_name_copy) {
                    memcpy(pe->dll_name_copy, pe->map + noff, len);
                    pe->dll_name_copy[len] = '\0';
                    pe->dll_name = pe->dll_name_copy;
                }
            }
        }
    }

    if (pe->nfunc == 0)
        return NTB0_PE_OK;

    if (ntb0_pe_rva_to_offset(pe, pe->af_rva, &af) != 0 ||
        (uint64_t)af + (uint64_t)pe->nfunc * 4 > pe->size) {
        pe_err("export address table is not readable");
        return NTB0_PE_EFORMAT;
    }

    pe->slots = (ntb0_pe_export *)calloc(pe->nfunc, sizeof(*pe->slots));
    if (!pe->slots) {
        pe_err("out of memory for %u export slots", pe->nfunc);
        return NTB0_PE_ENOMEM;
    }

    /* جدول الدوال */
    for (i = 0; i < pe->nfunc; i++) {
        pe->slots[i].ordinal = (uint16_t)(pe->exp_base + i);
        pe->slots[i].rva = rd32(pe->map + af + i * 4);
    }

    /* جدول الأسماء + جدول الترتيبات: نتحقّق من قابلية القراءة أولاً */
    if (pe->nnames > 0) {
        if (ntb0_pe_rva_to_offset(pe, pe->an_rva, &an) != 0 ||
            ntb0_pe_rva_to_offset(pe, pe->ao_rva, &ao) != 0 ||
            (uint64_t)an + (uint64_t)pe->nnames * 4 > pe->size ||
            (uint64_t)ao + (uint64_t)pe->nnames * 2 > pe->size) {
            pe_err("export name/ordinal tables are not readable");
            rc = NTB0_PE_EFORMAT;
            goto fail_tmp;
        }

        pe->named_idx = (uint16_t *)calloc(pe->nnames, sizeof(uint16_t));
        if (!pe->named_idx) {
            pe_err("out of memory for the named export index");
            rc = NTB0_PE_ENOMEM;
            goto fail_tmp;
        }
    }

    /* مصفوفات مؤقتة للنسختين (قياس ثم نسخ) */
    if (pe->nnames) {
        name_ord = (uint16_t *)calloc(pe->nnames, sizeof(uint16_t));
        name_off = (uint32_t *)calloc(pe->nnames, sizeof(uint32_t));
        have_name = (uint8_t *)calloc(pe->nnames, 1);
    }
    fwd_off = (uint32_t *)calloc(pe->nfunc ? pe->nfunc : 1, sizeof(uint32_t));
    have_fwd = (uint8_t *)calloc(pe->nfunc ? pe->nfunc : 1, 1);
    if (!fwd_off || !have_fwd ||
        (pe->nnames && (!name_ord || !name_off || !have_name))) {
        pe_err("out of memory for the export name tables");
        rc = NTB0_PE_ENOMEM;
        goto fail_tmp;
    }

    /* مرحلة القياس: نجمع (ترتيب، إزاحة) لكل اسم */
    for (i = 0; i < pe->nnames; i++) {
        uint32_t nrva = rd32(pe->map + an + i * 4);
        uint16_t ord = rd16(pe->map + ao + i * 2);
        uint32_t noff;
        size_t len;

        if (ord >= pe->nfunc)
            continue;                   /* ترتيب خارج المدى — تالف */

        if (ntb0_pe_rva_to_offset(pe, nrva, &noff) != 0)
            continue;
        if (pe_str_at(pe, noff, pe->size - noff, &len) != 0)
            continue;

        name_ord[i] = ord;
        name_off[i] = noff;
        have_name[i] = 1;
    }

    /* فحص الستوبات الإضافية عالية القيمة (kernel32/kernelbase stubs) */
    /* NOTE: Extra stub injection is DISABLED. The feature was causing memory corruption */
    /* through unsafe reallocation of pe->slots and pe->named_idx without proper cleanup. */
    /* Specifically:*/
    /*  1. realloc(pe->slots) could succeed while realloc(pe->named_idx) fails */
    /*  2. Incomplete cleanup led to corrupted export table pointers */
    /*  3. ntdll.dll must NEVER have stubs injected (syscall table corruption) */
    /* Solution: Skip injection entirely. Exports from real DLLs are complete. */
    int missing_extra_mask[32] = {0};
    int n_missing_extra = 0;
    int k;
    int allow_extra_stubs = 0;  /* ALWAYS 0 - injection disabled for safety */
    (void)allow_extra_stubs;

    /* Initialize to safe defaults */
    n_missing_extra = 0;
    for (k = 0; k < 32; k++)
        missing_extra_mask[k] = 0;

    /* Forwarders: RVA داخل مدى دليل التصديرات ⇒ سلسلة "DLL.Name" */
    for (i = 0; i < pe->nfunc; i++) {
        uint32_t rva = pe->slots[i].rva;
        uint32_t foff;
        size_t len;

        if (rva == 0)
            continue;
        if (rva < pe->exp_rva || rva >= pe->exp_rva + pe->exp_size)
            continue;
        if (ntb0_pe_rva_to_offset(pe, rva, &foff) != 0)
            continue;
        if (pe_str_at(pe, foff, pe->size - foff, &len) != 0)
            continue;

        fwd_off[i] = foff;
        have_fwd[i] = 1;
    }

    /* ---- قياس المخزن ثم تخصيصه مرة واحدة (لا realloc بعد ذلك) ---- */
    {
        size_t need = 1;

        for (i = 0; i < pe->nnames; i++) {
            size_t len;

            if (!have_name[i])
                continue;
            if (pe_str_at(pe, name_off[i], pe->size - name_off[i], &len) == 0)
                need += len + 1;
        }
        for (i = 0; i < pe->nfunc; i++) {
            size_t len;

            if (!have_fwd[i])
                continue;
            if (pe_str_at(pe, fwd_off[i], pe->size - fwd_off[i], &len) == 0)
                need += len + 1;
        }
        if (allow_extra_stubs) {
            for (k = 0; g_extra_kernel32_stubs[k] && k < 32; k++) {
                if (missing_extra_mask[k]) {
                    need += strlen(g_extra_kernel32_stubs[k]) + 1;
                }
            }
        }

        pe->strbuf = (char *)malloc(need);
        if (!pe->strbuf) {
            pe_err("out of memory for the export name buffer (%zu bytes)",
                   need);
            rc = NTB0_PE_ENOMEM;
            goto fail_tmp;
        }
        pe->strbuf_cap = need;
        pe->strbuf_len = 0;
    }

    /* ---- النسخة الثانية: نسخ الأسماء ---- */
    for (i = 0; i < pe->nnames; i++) {
        const char *nm;

        if (!have_name[i])
            continue;

        nm = strbuf_copy(pe, name_off[i], pe->size - name_off[i]);
        if (!nm)
            continue;

        if (!pe->slots || !pe->named_idx) {
            pe_err("slots or named_idx allocation missing");
            rc = NTB0_PE_EFORMAT;
            goto fail_tmp;
        }

        pe->slots[name_ord[i]].name = nm;
        pe->named_idx[pe->n_named++] = name_ord[i];
    }

    /* ---- النسخة الثانية: نسخ سلاسل الـ forwarders ---- */
    for (i = 0; i < pe->nfunc; i++) {
        const char *fw;

        if (!have_fwd[i])
            continue;

        fw = strbuf_copy(pe, fwd_off[i], pe->size - fwd_off[i]);
        if (!fw)
            continue;

        pe->slots[i].forwarded = 1;
        pe->slots[i].forwarder = fw;
    }

     /* ---- إضافة الستوبات الإضافية المفقودة ---- */
    /* INJECTION DISABLED - not needed for core functionality */
    if (0) {  /* Always false - injection code kept for reference but disabled */
        uint32_t base_rva = 0x1000;
        uint32_t orig_nfunc = pe->nfunc;
        for (i = 0; i < orig_nfunc; i++) {
            if (pe->slots[i].rva != 0) {
                base_rva = pe->slots[i].rva;
                break;
            }
        }
        uint32_t added = 0;
        int max_named_slots = (int)((size_t)pe->nnames + (size_t)n_missing_extra);
        for (k = 0; g_extra_kernel32_stubs[k] && k < 32 && added < (uint32_t)n_missing_extra; k++) {
            if (!missing_extra_mask[k])
                continue;
            const char *ex = g_extra_kernel32_stubs[k];
            uint32_t slot_idx = orig_nfunc + added;
            char *dst;
            size_t len = strlen(ex);

            if (slot_idx >= orig_nfunc + (uint32_t)n_missing_extra)
                break;
            if (pe->strbuf_len + len + 1 > pe->strbuf_cap)
                break;
            if (pe->n_named >= max_named_slots)
                break;

            dst = pe->strbuf + pe->strbuf_len;
            memcpy(dst, ex, len);
            dst[len] = '\0';
            pe->strbuf_len += len + 1;

            pe->slots[slot_idx].ordinal = (uint16_t)(pe->exp_base + slot_idx);
            pe->slots[slot_idx].rva = base_rva;
            pe->slots[slot_idx].name = dst;
            pe->named_idx[pe->n_named++] = (uint16_t)slot_idx;
            added++;
        }
        pe->nfunc = orig_nfunc + added;
    }

    free(name_ord);
    free(name_off);
    free(have_name);
    free(fwd_off);
    free(have_fwd);

    return NTB0_PE_OK;

fail_tmp:
    free(name_ord);
    free(name_off);
    free(have_name);
    free(fwd_off);
    free(have_fwd);
    return rc;
}

/* ------------------------------------------------------------------ */
/* الفتح                                                               */
/* ------------------------------------------------------------------ */

ntb0_pe *ntb0_pe_open(const char *path)
{
    ntb0_pe *pe;
    struct stat st;
    uint32_t e_lfanew, opt_off, sec_off;
    uint16_t nsec, opt_size, magic;
    uint32_t dd_off, numrva;
    int fd;
    uint8_t *map;
    uint32_t i;

    if (!path || !*path) {
        pe_err("empty PE path");
        return NULL;
    }

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        pe_err("cannot open '%s'", path);
        return NULL;
    }

    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        pe_err("'%s' is not a regular non-empty file", path);
        close(fd);
        return NULL;
    }

    map = (uint8_t *)mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        pe_err("cannot mmap '%s'", path);
        close(fd);
        return NULL;
    }

    pe = (ntb0_pe *)calloc(1, sizeof(*pe));
    if (!pe) {
        pe_err("out of memory for the PE struct");
        munmap(map, (size_t)st.st_size);
        close(fd);
        return NULL;
    }

    pe->fd = fd;
    pe->map = map;
    pe->size = (size_t)st.st_size;
    pe->path = strdup(path);
    if (!pe->path) {
        pe_err("out of memory for the PE path");
        goto fail;
    }

    /* ---- DOS header ---- */
    if (pe->size < 0x40 || map[0] != 'M' || map[1] != 'Z') {
        pe_err("'%s' has no MZ header", path);
        goto fail;
    }

    e_lfanew = rd32(map + 0x3C);
    if (e_lfanew < 0x40 || (uint64_t)e_lfanew + 24 > pe->size) {
        pe_err("'%s' has a bad e_lfanew (0x%x)", path, e_lfanew);
        goto fail;
    }

    /* ---- PE signature ---- */
    if (memcmp(map + e_lfanew, "PE\0\0", 4) != 0) {
        pe_err("'%s' has no PE signature", path);
        goto fail;
    }

    /* ---- COFF header ---- */
    nsec     = rd16(map + e_lfanew + 6);
    opt_size = rd16(map + e_lfanew + 20);
    opt_off  = e_lfanew + 24;

    if (nsec == 0 || nsec > PE_MAX_SECTIONS) {
        pe_err("'%s' has %u sections (invalid)", path, nsec);
        goto fail;
    }
    if ((uint64_t)opt_off + opt_size > pe->size) {
        pe_err("'%s' optional header is truncated", path);
        goto fail;
    }
    if (opt_size < 2) {
        pe_err("'%s' has no optional header", path);
        goto fail;
    }

    /* ---- Optional header ---- */
    magic = rd16(map + opt_off);
    if (magic == 0x20b) {
        pe->is64 = 1;
        numrva = rd32(map + opt_off + PE64_NUMRVA_OFF);
        dd_off = opt_off + PE64_DATADIR_OFF;
        if (opt_size >= 32)
            pe->image_base = (uint32_t)rd64(map + opt_off + 24);
        if (opt_size >= 60)
            pe->hdr_size = rd32(map + opt_off + 60);
    } else if (magic == 0x10b) {
        pe->is64 = 0;
        numrva = rd32(map + opt_off + PE32_NUMRVA_OFF);
        dd_off = opt_off + PE32_DATADIR_OFF;
        if (opt_size >= 32)
            pe->image_base = rd32(map + opt_off + 28);
        if (opt_size >= 60)
            pe->hdr_size = rd32(map + opt_off + 60);
    } else {
        pe_err("'%s' has an unknown optional header magic 0x%x", path, magic);
        goto fail;
    }

    if (pe->hdr_size == 0 || pe->hdr_size > pe->size)
        pe->hdr_size = 0x400;

    /* ---- Data directory[0] = Export ---- */
    if (numrva < 1 || (uint64_t)dd_off + 8 > pe->size) {
        pe_err("'%s' has no data directory", path);
        goto fail;
    }
    pe->exp_rva  = rd32(map + dd_off);
    pe->exp_size = rd32(map + dd_off + 4);

    /* ---- Section table ---- */
    sec_off = opt_off + opt_size;
    if ((uint64_t)sec_off + (uint64_t)nsec * 40 > pe->size) {
        pe_err("'%s' section table is truncated", path);
        goto fail;
    }

    pe->nsec = nsec;
    for (i = 0; i < nsec; i++) {
        const uint8_t *s = map + sec_off + i * 40;

        pe->sec[i].vsize = rd32(s + 8);
        pe->sec[i].va    = rd32(s + 12);
        pe->sec[i].rsize = rd32(s + 16);
        pe->sec[i].raw   = rd32(s + 20);
    }

    /* ---- Export table ---- */
    if (build_exports(pe) != NTB0_PE_OK)
        goto fail;

    return pe;

fail:
    ntb0_pe_close(pe);
    return NULL;
}

void ntb0_pe_close(ntb0_pe *pe)
{
    if (!pe)
        return;

    if (pe->map && pe->map != MAP_FAILED)
        munmap(pe->map, pe->size);
    if (pe->fd >= 0)
        close(pe->fd);

    free(pe->slots);
    free(pe->named_idx);
    free(pe->strbuf);
    free(pe->dll_name_copy);
    free(pe->path);
    free(pe);
}

/* ------------------------------------------------------------------ */
/* الاستعلام                                                          */
/* ------------------------------------------------------------------ */

const char *ntb0_pe_dll_name(const ntb0_pe *pe)
{
    if (!pe || !pe->dll_name)
        return "";
    return pe->dll_name;
}

const char *ntb0_pe_path(const ntb0_pe *pe)
{
    if (!pe)
        return "";
    return pe->path ? pe->path : "";
}

int ntb0_pe_is_64(const ntb0_pe *pe)
{
    return pe ? pe->is64 : 0;
}

uint32_t ntb0_pe_image_base(const ntb0_pe *pe)
{
    return pe ? pe->image_base : 0;
}

int ntb0_pe_export_count(const ntb0_pe *pe)
{
    return pe ? pe->n_named : 0;
}

int ntb0_pe_export_slot_count(const ntb0_pe *pe)
{
    return pe ? (int)pe->nfunc : 0;
}

int ntb0_pe_export_ordinal_base(const ntb0_pe *pe)
{
    return pe ? (int)pe->exp_base : 0;
}

size_t ntb0_pe_file_size(const ntb0_pe *pe)
{
    return pe ? pe->size : 0;
}

int ntb0_pe_export_at(const ntb0_pe *pe, int index, ntb0_pe_export *out)
{
    if (!pe || !out || index < 0 || index >= (int)pe->nfunc)
        return -1;
    *out = pe->slots[index];
    return 0;
}

int ntb0_pe_export_named_at(const ntb0_pe *pe, int index, ntb0_pe_export *out)
{
    if (!pe || !out || index < 0 || index >= pe->n_named)
        return -1;
    *out = pe->slots[pe->named_idx[index]];
    return 0;
}

static int ci_equal(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = *a, cb = *b;

        if (ca >= 'A' && ca <= 'Z')
            ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z')
            cb += 'a' - 'A';
        if (ca != cb)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

int ntb0_pe_find_by_name(ntb0_pe *pe, const char *name, ntb0_pe_export *out)
{
    int i;

    if (!pe || !name || !*name)
        return 0;

    /* مطابقة حسّاسة لحالة الأحرف ((GetProcAddress semantics) */
    for (i = 0; i < pe->n_named; i++) {
        const ntb0_pe_export *e = &pe->slots[pe->named_idx[i]];

        if (e->name && strcmp(e->name, name) == 0) {
            if (out)
                *out = *e;
            return 1;
        }
    }

    /* احتياطي: غير حسّاس لحالة الأحرف (بعض DLLs القديمة) */
    for (i = 0; i < pe->n_named; i++) {
        const ntb0_pe_export *e = &pe->slots[pe->named_idx[i]];

        if (e->name && ci_equal(e->name, name)) {
            if (out)
                *out = *e;
            return 1;
        }
    }

    return 0;
}

int ntb0_pe_find_by_ordinal(ntb0_pe *pe, uint16_t ordinal, ntb0_pe_export *out)
{
    uint32_t i;

    if (!pe || pe->nfunc == 0)
        return 0;

    for (i = 0; i < pe->nfunc; i++) {
        if (pe->slots[i].ordinal == ordinal) {
            if (pe->slots[i].rva == 0)
                return 0;
            if (out)
                *out = pe->slots[i];
            return 1;
        }
    }

    return 0;
}
