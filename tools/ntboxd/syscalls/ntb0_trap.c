/*
 * ntb0_trap.c — رسم صور PE وتعديل ستوب syscall في مكانها.
 *
 * كل دالة هنا = ID + مصباح + اختبار (NTBOX.md §9).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "ntb0_trap.h"
#include "ntb0_trace.h"
#include "ntb0_dispatch.h"

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

/* قالب القفزة — من ntb0_trampoline.S */
extern const unsigned char ntb_trampoline_template[];
extern const unsigned char ntb_trampoline_template_end[];

/* ------------------------------------------------------------------ */
/* قراءة الملف                                                        */
/* ------------------------------------------------------------------ */

static u8 *ntb0_read_file(const char *path, size_t *len_out)
{
    int fd;
    struct stat st;
    u8 *buf;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        NTB0_FAIL("TRAP-001", "open %s failed", path);
        return NULL;
    }
    if (fstat(fd, &st) < 0 || st.st_size <= 0) {
        NTB0_FAIL("TRAP-002", "fstat %s failed", path);
        close(fd);
        return NULL;
    }
    buf = malloc((size_t)st.st_size);
    if (!buf) {
        NTB0_FAIL("TRAP-003", "oom (%zu bytes)", (size_t)st.st_size);
        close(fd);
        return NULL;
    }
    if (read(fd, buf, (size_t)st.st_size) != (ssize_t)st.st_size) {
        NTB0_FAIL("TRAP-004", "short read on %s", path);
        free(buf);
        close(fd);
        return NULL;
    }
    close(fd);
    *len_out = (size_t)st.st_size;
    return buf;
}

/* ------------------------------------------------------------------ */
/* رسم صورة PE                                                        */
/* ------------------------------------------------------------------ */

/* ملخّص ترويسة PE — ما نحتاجه فقط */
struct ntb0_pe {
    u32  pe_off;
    u16  nsec;
    u16  optsz;
    u32  opt_off;
    u32  sec_off;
    u32  size_of_image;
    u32  size_of_headers;
};

static int ntb0_pe_parse(const u8 *buf, size_t len, struct ntb0_pe *pe)
{
    u32 e_lfanew;
    u16 magic;

    if (len < 0x40 || buf[0] != 'M' || buf[1] != 'Z')
        return -1;
    e_lfanew = *(const u32 *)(buf + 0x3C);
    if (e_lfanew + 0x40 > len)
        return -1;
    if (buf[e_lfanew] != 'P' || buf[e_lfanew + 1] != 'E')
        return -1;

    pe->pe_off = e_lfanew;
    pe->nsec = *(const u16 *)(buf + e_lfanew + 6);
    pe->optsz = *(const u16 *)(buf + e_lfanew + 20);
    pe->opt_off = e_lfanew + 24;
    magic = *(const u16 *)(buf + pe->opt_off);
    if (magic != 0x20b) { /* PE32+ فقط (x64) */
        NTB0_FAIL("TRAP-005", "not PE32+ (magic=0x%x)", magic);
        return -1;
    }
    pe->sec_off = pe->opt_off + pe->optsz;
    pe->size_of_image = *(const u32 *)(buf + pe->opt_off + 56);
    pe->size_of_headers = *(const u32 *)(buf + pe->opt_off + 60);
    if (pe->size_of_image == 0 || pe->size_of_headers == 0)
        return -1;
    return 0;
}

/* يضع قالب القفزة في الصورة ويخيط عنوان المترجم (movabs) */
/*
 * كتابة قفزة الاعتراض في ذاكرة معيّنة (T-063: محمّل DLL يعرض الصورة
 * بنفسه ⇒ نحتاج القفزة قريبة من الصورة لا داخلها).
 */
int ntb0_trampoline_write(void *addr)
{
    size_t tsize = (size_t)(ntb_trampoline_template_end -
                            ntb_trampoline_template);
    unsigned char *ph = NULL;
    unsigned char *t;
    uint64_t disp;
    size_t i;

    if (!addr || tsize == 0 || tsize > 4096) {
        NTB0_FAIL("TRAP-006", "bad trampoline target");
        return -1;
    }

    t = (unsigned char *)addr;
    memcpy(t, ntb_trampoline_template, tsize);

    /* PLACEHOLDER: 48 B8 00*8 FF D0  (movabs $0,%rax ; call *%rax) */
    for (i = 0; i + 12 <= tsize; i++) {
        if (t[i] == 0x48 && t[i + 1] == 0xB8 && t[i + 2] == 0x00 &&
            t[i + 3] == 0x00 && t[i + 4] == 0x00 && t[i + 5] == 0x00 &&
            t[i + 6] == 0x00 && t[i + 7] == 0x00 && t[i + 8] == 0x00 &&
            t[i + 9] == 0x00 && t[i + 10] == 0xFF && t[i + 11] == 0xD0) {
            ph = t + i;
            break;
        }
    }

    if (!ph) {
        NTB0_FAIL("TRAP-007", "trampoline placeholder not found");
        return -1;
    }

    disp = (uint64_t)(uintptr_t)&ntb0_dispatch;
    memcpy(ph + 2, &disp, 8);
    return 0;
}

static int ntb0_place_trampoline(struct ntb0_image *img)
{
    size_t tsize = (size_t)(ntb_trampoline_template_end -
                            ntb_trampoline_template);
    unsigned char *ph = NULL;
    unsigned char *t;
    uint64_t disp;
    size_t i;

    if (tsize == 0 || tsize > 4096) {
        NTB0_FAIL("TRAP-006", "bad trampoline template size %zu", tsize);
        return -1;
    }

    t = (unsigned char *)img->tramp;
    memcpy(t, ntb_trampoline_template, tsize);

    /* ابحث عن PLACEHOLDER: 48 B8 00*8 FF D0  (movabs $0,%rax ; call *%rax) */
    for (i = 0; i + 12 <= tsize; i++) {
        if (t[i] == 0x48 && t[i + 1] == 0xB8 && t[i + 2] == 0x00 &&
            t[i + 3] == 0x00 && t[i + 4] == 0x00 && t[i + 5] == 0x00 &&
            t[i + 6] == 0x00 && t[i + 7] == 0x00 && t[i + 8] == 0x00 &&
            t[i + 9] == 0x00 && t[i + 10] == 0xFF && t[i + 11] == 0xD0) {
            ph = t + i;
            break;
        }
    }
    if (!ph) {
        NTB0_FAIL("TRAP-007", "trampoline placeholder not found");
        return -1;
    }

    disp = (uint64_t)(uintptr_t)&ntb0_dispatch;
    memcpy(ph + 2, &disp, 8);

    img->tramp_size = (unsigned int)tsize;
    NTB0_OK("TRAP-008", "trampoline placed at %p (%u bytes), dispatcher=%p",
            img->tramp, img->tramp_size, (void *)(uintptr_t)disp);
    return 0;
}

int ntb0_map_pe(const char *path, struct ntb0_image *img)
{
    u8 *buf = NULL;
    size_t len = 0;
    struct ntb0_pe pe;
    size_t region, text_off = 0, text_size = 0;
    u32 i;
    u8 *base;

    memset(img, 0, sizeof(*img));

    buf = ntb0_read_file(path, &len);
    if (!buf)
        return -1;
    if (ntb0_pe_parse(buf, len, &pe) < 0) {
        NTB0_FAIL("TRAP-009", "bad PE header in %s", path);
        free(buf);
        return -1;
    }

    /*
     * منطقة واحدة: [صورة PE][صفحة القفزة]
     * السبب: القفزة E9 rel32 لا تتجاوز ±2GB — نضمن المسافة بقربها.
     */
    region = ((size_t)pe.size_of_image + 0x10000 + 0xFFF) & ~(size_t)0xFFF;
    base = mmap(NULL, region, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        NTB0_FAIL("TRAP-010", "mmap %zu bytes failed", region);
        free(buf);
        return -1;
    }

    memcpy(base, buf, pe.size_of_headers);
    for (i = 0; i < pe.nsec; i++) {
        u32 s = pe.sec_off + i * 40;
        u32 va = *(const u32 *)(buf + s + 12);
        u32 vsz = *(const u32 *)(buf + s + 8);
        u32 raw = *(const u32 *)(buf + s + 20);
        u32 rsz = *(const u32 *)(buf + s + 16);
        u8 name[9];

        memcpy(name, buf + s, 8);
        name[8] = '\0';

        if (rsz)
            memcpy(base + va, buf + raw, rsz);
        if (strncmp((const char *)name, ".text", 5) == 0) {
            text_off = va;
            text_size = vsz ? vsz : rsz;
        }
    }
    free(buf);

    img->base = base;
    img->size = pe.size_of_image;
    img->tramp = base + ((pe.size_of_image + 0xF) & ~(size_t)0xF);
    img->text_off = text_off;
    img->text_size = text_size;

    if (ntb0_place_trampoline(img) < 0) {
        munmap(base, region);
        return -1;
    }

    /* حدود .text محفوظة في img (تُستخدم عند التعديل) */
    NTB0_OK("TRAP-011", "mapped %s: base=%p SizeOfImage=0x%zx .text=0x%zx@0x%zx",
            path, base, img->size, text_size, text_off);
    return 0;
}

/* ------------------------------------------------------------------ */
/* تعديل الستوبات في مكانها                                            */
/* ------------------------------------------------------------------ */

/* يبحث في .text ويعدّل كل ستوب syscall — النواة الداخلية مع عدّادات */
static long ntb0_patch_stubs_ex(void *base, void *tramp, unsigned int *found_out,
                                 unsigned int *patched_out)
{
    u8 *p, *end;
    u32 pe_off, nsec, optsz, opt_off, sec_off;
    unsigned int found = 0, patched = 0;
    u8 *image = (u8 *)base;

    if (!base || !tramp) {
        NTB0_FAIL("TRAP-012", "null base/tramp");
        return -1;
    }
    /* نقرأ الترويسة من الصورة المرسومة (ليست من الملف) */
    pe_off = *(const u32 *)(image + 0x3C);
    nsec = *(const u16 *)(image + pe_off + 6);
    optsz = *(const u16 *)(image + pe_off + 20);
    opt_off = pe_off + 24;
    sec_off = opt_off + optsz;

    for (u32 i = 0; i < nsec; i++) {
        u32 s = sec_off + i * 40;
        u32 va = *(const u32 *)(image + s + 12);
        u32 vsz = *(const u32 *)(image + s + 8);
        u8 name[9];

        memcpy(name, image + s, 8);
        name[8] = '\0';
        if (strncmp((const char *)name, ".text", 5) != 0)
            continue;

        p = image + va;
        end = image + va + (vsz ? vsz : *(const u32 *)(image + s + 16));

        while (p + 16 <= end) {
            /* 4C 8B D1 | B8 <num32> | 0F 05  */
            if (p[0] == 0x4C && p[1] == 0x8B && p[2] == 0xD1 &&
                p[3] == 0xB8 && p[8] == 0x0F && p[9] == 0x05) {
                int32_t rel;
                int64_t delta;

                found++;

                /* المسافة بين الصورة والقفزة */
                delta = (int64_t)((uintptr_t)tramp -
                                  (uintptr_t)(p + 8 + 5));
                if (delta > 0x7FFFFFFFLL || delta < -0x7FFFFFFFLL) {
                    NTB0_FAIL("TRAP-013",
                              "trampoline out of rel32 range (%lld bytes)",
                              (long long)delta);
                    return -1;
                }
                rel = (int32_t)delta;

                /* E9 <rel32> | 90 90 90 */
                p[8] = 0xE9;
                memcpy(p + 9, &rel, 4);
                p[13] = 0x90;
                p[14] = 0x90;
                p[15] = 0x90;
                patched++;
            }
            p++;
        }
    }

    NTB0_OK("TRAP-014", "syscall stubs found=%u patched=%u (in-place, E9 rel32)",
            found, patched);
    if (found_out)
        *found_out = found;
    if (patched_out)
        *patched_out = patched;
    return (long)patched;
}

long ntb0_patch_stubs(void *base, size_t size, void *tramp)
{
    (void)size;
    return ntb0_patch_stubs_ex(base, tramp, NULL, NULL);
}

int ntb0_trap_patch(struct ntb0_image *img)
{
    long n;

    if (!img || !img->base) {
        NTB0_FAIL("TRAP-015", "null image");
        return -1;
    }
    n = ntb0_patch_stubs_ex(img->base, img->tramp, &img->stubs_found,
                            &img->stubs_patched);
    if (n < 0)
        return -1;
    return 0;
}
