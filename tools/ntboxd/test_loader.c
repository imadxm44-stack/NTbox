/*
 * test_loader.c — الاختبار الذاتي لمحمّل DLLs الحقيقية (W5 · T-120…T-126).
 *
 * ما يفحصه (بالترتيب):
 *   1) [DLL-002] رسم kernel32.dll الحقيقي: العنوان الأساسي، عدد الأقسام،
 *      الحجم،.delta.
 *   2) [DLL-003] تطبيق إعادة التوجيه (.reloc): عدد الكتل والمدخلات.
 *   3) [DLL-003] الاستيرادات: كم حُلّ وكم لم يُحلّ + إثبات الكتابة في IAT.
 *   4) [DLL-009] api-set: أسماء api-ms-win-* تُمرَّر إلى apiset.c.
 *   5) [DLL-005] GetProcAddress: "CreateFileA" ⇒ عنوان + RVA (مُقارَن
 *      بجدول التصديرات — لا قيمة مكتوبة مسبقاً).
 *   6) [DLL-004] تحميل وحدة ثانية (user32.dll) ⇒ دعم عدة وحدات.
 *   7) [DLL-004] استيراد **بالترتيب** (ordinal) على PE مصطنع + IAT.
 *   8) [DLL-007/008] الاعتماديات تلقائياً + كشف الدورات (PE مصطنع).
 *   9) [DLL-002] رفض ملفات PE تالفة/مبتورة (بلا انهيار ولا تسريب).
 *  10) [DLL-006] FreeLibrary: تفكيك الوحدات + تفريغ المحمّل.
 *
 * جذر ويندوز: من NTBOX_ROOT أو argv[1] أو المسار الافتراضي.
 * الخروج: 0 فقط إذا نجحت كل الفحوص.
 *
 * البناء:
 *   gcc -O2 -Wall -Wextra -Itools/ntboxreg -Itools/ntboxd \
 *       -Itools/ntboxd/syscalls -o /tmp/opencode/test_loader \
 *       tools/ntboxd/test_loader.c tools/ntboxd/dll_loader.c \
 *       tools/ntboxd/pe_export.c tools/ntboxd/apiset.c tools/ntboxreg/hive.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "dll_loader.h"
#include "pe_export.h"
#include "apiset.h"

#define DEFAULT_ROOT "/home/imadix/Downloads/Niga/win7root"
#define FAKE_DIR     "/tmp/opencode/w5fake"
#define BAD_DIR      "/tmp/opencode/w5bad"

static int g_fail = 0;

/* نتيجة البحث عن CreateFileA (تُملأ في test_kernel32) */
static char     g_gp_name[64];
static uint64_t g_gp_addr;
static uint32_t g_gp_rva;
static unsigned g_gp_ordinal;

#define CHECK(cond, fmt, ...)                                                 \
    do {                                                                      \
        if (!(cond)) {                                                        \
            NTB0_DLL_LFAIL("TEST", fmt, ##__VA_ARGS__);                       \
            g_fail = 1;                                                       \
        }                                                                     \
    } while (0)

/* ------------------------------------------------------------------ */
/* مُحلِّل stub: يعيد عنواناً مصطنعاً (المضيف لا يستطيع تشغيل كود Win32) */
/* ------------------------------------------------------------------ */
struct stub_ctx {
    unsigned calls;
    unsigned n_distinct;
    char     last_dll[128];
    char     last_func[192];
};

/* عنوان stub حتمي (FNV-1a) ⇒ يمكن التحقّق منه في الاختبار */
static uint64_t stub_addr(const char *dll, const char *func)
{
    uint64_t h = 1469598103934665603ULL;

    while (dll && *dll) {
        h ^= (unsigned char)*dll++;
        h *= 1099511628211ULL;
    }
    h ^= 0xff;
    while (func && *func) {
        h ^= (unsigned char)*func++;
        h *= 1099511628211ULL;
    }
    h &= 0xffffffULL;
    return 0x00007f0000000000ULL + h * 16;
}

static int stub_resolve(void *ctx, const char *dll, const char *func,
                        uint64_t *out_addr)
{
    struct stub_ctx *c = (struct stub_ctx *)ctx;

    c->calls++;
    snprintf(c->last_dll, sizeof(c->last_dll), "%s", dll ? dll : "");
    snprintf(c->last_func, sizeof(c->last_func), "%s", func ? func : "");
    if (!out_addr)
        return -1;
    *out_addr = stub_addr(dll, func);
    return 0;
}

/* ------------------------------------------------------------------ */
/* بناء PE مصطنع (لاختبار الاستيراد بالترتيب والدورات)              */
/* ------------------------------------------------------------------ */
#define FAKE_SECT_RVA  0x1000u
#define FAKE_SECT_OFF  0x200u
#define FAKE_ILT_RVA   0x1030u
#define FAKE_IAT_RVA   0x1050u
#define FAKE_IBN_RVA   0x1070u
#define FAKE_NAME_RVA  0x1080u

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)(v & 0xffffffffu));
    put32(p + 4, (uint32_t)(v >> 32));
}

/*
 * PE32+ مصغّر: قسم واحد (.rdata) فيه واصف استيراد واحد يتضمّن
 * استيراداً بالاسم وآخر **بالترتيب**.
 *   imp_dll : اسم الـ DLL المستورد
 *   func    : اسم الدالة (يُستعمل في الاستيراد بالاسم)
 *   ordinal : ترتيب الدالة المستوردة بالترتيب
 */
static int build_fake_dll(const char *path, uint64_t image_base,
                          const char *imp_dll, const char *func,
                          uint16_t ordinal)
{
    uint8_t hdr[FAKE_SECT_OFF];
    uint8_t sec[0x200];
    uint32_t opt_off = 0x40 + 4 + 20;
    uint32_t dd_off = opt_off + 112;
    size_t fn_len = strlen(func) + 1;
    size_t dn_len = strlen(imp_dll) + 1;
    FILE *f;

    if (fn_len + 2 > 64 || dn_len > 96)
        return -1;

    memset(hdr, 0, sizeof(hdr));
    memset(sec, 0, sizeof(sec));

    /* DOS header */
    hdr[0] = 'M';
    hdr[1] = 'Z';
    put32(hdr + 0x3C, 0x40);

    /* PE signature + COFF header */
    memcpy(hdr + 0x40, "PE\0\0", 4);
    put16(hdr + 0x44, 0x8664);            /* Machine = x64 */
    put16(hdr + 0x46, 1);                 /* NumberOfSections */
    put16(hdr + 0x54, 0xF0);              /* SizeOfOptionalHeader */
    put16(hdr + 0x56, 0x2022);            /* DLL | EXECUTABLE | LARGE_ADDRESS */

    /* Optional header (PE32+) */
    put16(hdr + opt_off + 0, 0x20b);
    put32(hdr + opt_off + 16, FAKE_SECT_RVA);   /* AddressOfEntryPoint */
    put32(hdr + opt_off + 20, FAKE_SECT_RVA);   /* BaseOfCode */
    put64(hdr + opt_off + 24, image_base);      /* ImageBase */
    put32(hdr + opt_off + 32, 0x1000);           /* SectionAlignment */
    put32(hdr + opt_off + 36, 0x200);            /* FileAlignment */
    put32(hdr + opt_off + 56, 0x2000);           /* SizeOfImage */
    put32(hdr + opt_off + 60, FAKE_SECT_OFF);    /* SizeOfHeaders */
    put16(hdr + opt_off + 68, 3);                /* Subsystem = console */
    put64(hdr + opt_off + 72, 0x100000);         /* SizeOfStackReserve */
    put64(hdr + opt_off + 80, 0x1000);           /* SizeOfStackCommit */
    put64(hdr + opt_off + 88, 0x100000);         /* SizeOfHeapReserve */
    put64(hdr + opt_off + 96, 0x1000);           /* SizeOfHeapCommit */
    put32(hdr + opt_off + 108, 16);              /* NumberOfRvaAndSizes */
    /* DataDirectory[1] = Import */
    put32(hdr + dd_off + 8, FAKE_SECT_RVA);
    put32(hdr + dd_off + 12, 0x100);

    /* جدول القسم */
    {
        uint8_t *s = hdr + opt_off + 0xF0;

        memcpy(s, ".rdata\0", 8);
        put32(s + 8, 0x200);              /* VirtualSize */
        put32(s + 12, FAKE_SECT_RVA);     /* VirtualAddress */
        put32(s + 16, 0x200);             /* SizeOfRawData */
        put32(s + 20, FAKE_SECT_OFF);     /* PointerToRawData */
        put32(s + 36, 0x40000040);        /* INITIALIZED_DATA | MEM_READ */
    }

    /* محتوى القسم: واصف الاستيراد */
    put32(sec + 0x00, FAKE_ILT_RVA);      /* OriginalFirstThunk */
    put32(sec + 0x0C, FAKE_NAME_RVA);     /* Name */
    put32(sec + 0x10, FAKE_IAT_RVA);      /* FirstThunk */

    /* ILT: بالاسم ثم بالترتيب ثم NULL */
    put64(sec + (FAKE_ILT_RVA - FAKE_SECT_RVA) + 0, FAKE_IBN_RVA);
    put64(sec + (FAKE_ILT_RVA - FAKE_SECT_RVA) + 8, 0x8000000000000000ULL | ordinal);

    /* IAT: مكانها (يكتبها المحمّل) */
    put64(sec + (FAKE_IAT_RVA - FAKE_SECT_RVA) + 0, 0);
    put64(sec + (FAKE_IAT_RVA - FAKE_SECT_RVA) + 8, 0);

    /* IMAGE_IMPORT_BY_NAME */
    put16(sec + (FAKE_IBN_RVA - FAKE_SECT_RVA), 0);
    memcpy(sec + (FAKE_IBN_RVA - FAKE_SECT_RVA) + 2, func, fn_len);

    /* اسم الـ DLL المستورد */
    memcpy(sec + (FAKE_NAME_RVA - FAKE_SECT_RVA), imp_dll, dn_len);

    f = fopen(path, "wb");
    if (!f)
        return -1;
    fwrite(hdr, 1, sizeof(hdr), f);
    fwrite(sec, 1, sizeof(sec), f);
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/* نسخ ملف + إتلافه عند إزاحة معيّنة                                 */
/* ------------------------------------------------------------------ */
static uint8_t *slurp(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long sz;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return NULL;
    }
    buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len_out = (size_t)sz;
    return buf;
}

static int spit(const char *path, const uint8_t *buf, size_t len)
{
    FILE *f = fopen(path, "wb");

    if (!f)
        return -1;
    if (fwrite(buf, 1, len, f) != len) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

/* إزاحة دليل البيانات رقم idx داخل صورة PE (أو -1) */
static long dd_offset(const uint8_t *buf, size_t len, int idx)
{
    uint32_t e_lfanew, opt_off;

    if (len < 0x100 || buf[0] != 'M' || buf[1] != 'Z')
        return -1;
    e_lfanew = (uint32_t)buf[0x3C] | ((uint32_t)buf[0x3D] << 8) |
               ((uint32_t)buf[0x3E] << 16) | ((uint32_t)buf[0x3F] << 24);
    if (e_lfanew + 24 + 112 > len)
        return -1;
    opt_off = e_lfanew + 24;
    if (buf[opt_off] != 0x0b || buf[opt_off + 1] != 0x02)
        return -1;                      /* PE32+ */
    return (long)(opt_off + 112 + idx * 8);
}

/* معلومات PE الدنيا (للتحقّق من إعادة التوجيه) */
struct pe_min {
    uint64_t image_base;
    uint32_t size_of_image;
    uint32_t sec_off;
    uint16_t nsec;
};

static int pe_min_parse(const uint8_t *buf, size_t len, struct pe_min *pm)
{
    uint32_t e_lfanew, opt_off, optsz, i;
    uint16_t nsec;

    if (len < 0x100 || buf[0] != 'M' || buf[1] != 'Z')
        return -1;
    e_lfanew = (uint32_t)buf[0x3C] | ((uint32_t)buf[0x3D] << 8) |
               ((uint32_t)buf[0x3E] << 16) | ((uint32_t)buf[0x3F] << 24);
    if (e_lfanew + 24 + 112 > len)
        return -1;
    opt_off = e_lfanew + 24;
    optsz = (uint32_t)(buf[e_lfanew + 20] | (buf[e_lfanew + 21] << 8));
    if (buf[opt_off] != 0x0b || buf[opt_off + 1] != 0x02)
        return -1;                      /* PE32+ فقط */
    nsec = (uint16_t)(buf[e_lfanew + 6] | (buf[e_lfanew + 7] << 8));

    pm->image_base    = 0;
    for (i = 0; i < 8; i++)
        pm->image_base |= (uint64_t)buf[opt_off + 24 + i] << (i * 8);
    pm->size_of_image = (uint32_t)buf[opt_off + 56] |
                        ((uint32_t)buf[opt_off + 57] << 8) |
                        ((uint32_t)buf[opt_off + 58] << 16) |
                        ((uint32_t)buf[opt_off + 59] << 24);
    pm->sec_off = opt_off + optsz;
    pm->nsec = nsec;
    if ((uint64_t)pm->sec_off + (uint64_t)nsec * 40 > len)
        return -1;
    return 0;
}

/* RVA ⇒ إزاحة في الملف (أو -1) */
static long rva_to_off(const struct pe_min *pm, const uint8_t *buf, uint32_t rva)
{
    uint16_t i;

    for (i = 0; i < pm->nsec; i++) {
        const uint8_t *s = buf + pm->sec_off + i * 40;
        uint32_t va    = (uint32_t)s[12] | ((uint32_t)s[13] << 8) |
                         ((uint32_t)s[14] << 16) | ((uint32_t)s[15] << 24);
        uint32_t rsize = (uint32_t)s[16] | ((uint32_t)s[17] << 8) |
                         ((uint32_t)s[18] << 16) | ((uint32_t)s[19] << 24);
        uint32_t raw   = (uint32_t)s[20] | ((uint32_t)s[21] << 8) |
                         ((uint32_t)s[22] << 16) | ((uint32_t)s[23] << 24);

        if (rva >= va && rva < (uint64_t)va + rsize)
            return (long)(raw + (rva - va));
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* (1..6) الفحوص الرئيسية                                              */
/* ------------------------------------------------------------------ */
static int test_kernel32(const char *root, ntb0_dll_loader *ld,
                          struct stub_ctx *ctx, ntb0_dll_module **k32_out)
{
    char path[1024];
    ntb0_dll_module *k32;
    unsigned i, n_imp;
    int n_apiset_rec = 0, apiset_ok = 0;

    snprintf(path, sizeof(path), "%s/Windows/System32/kernel32.dll", root);

    k32 = ntb0_dll_load(ld, path, 0);
    if (!k32) {
        NTB0_DLL_LFAIL("DLL-002", "cannot load %s: %s", path,
                       ntb0_dll_last_error(ld));
        g_fail = 1;
        return -1;
    }
    *k32_out = k32;

    printf("kernel32.dll  : %s\n", path);
    printf("  mapped base : 0x%llx (preferred 0x%llx, delta %+lld)\n",
           (unsigned long long)ntb0_dll_module_base(k32),
           (unsigned long long)ntb0_dll_module_pref_base(k32),
           (long long)ntb0_dll_module_delta(k32));
    printf("  SizeOfImage : 0x%zx bytes, %d section(s), %s\n",
           ntb0_dll_module_size(k32), ntb0_dll_module_nsec(k32),
           ntb0_dll_module_is64(k32) ? "PE32+" : "PE32");
    printf("  relocations : %u block(s), %u entr(ies), %u applied "
           "(%u absolute, %u unsupported)\n",
           ntb0_dll_module_reloc_blocks(k32),
           ntb0_dll_module_reloc_entries(k32),
           ntb0_dll_module_reloc_applied(k32),
           ntb0_dll_module_reloc_absolute(k32),
           ntb0_dll_module_reloc_unsupported(k32));
    printf("  imports     : %d resolved / %d unresolved, %u imported DLL(s)\n",
           ntb0_dll_module_imports_resolved(k32),
           ntb0_dll_module_imports_unresolved(k32),
           ntb0_dll_module_import_dll_count(k32));

    CHECK(ntb0_dll_module_base(k32) != 0, "kernel32 was not mapped");
    CHECK(ntb0_dll_module_nsec(k32) > 0, "kernel32 has no section");
    CHECK(ntb0_dll_module_reloc_blocks(k32) > 0,
          "kernel32 has no relocation block");
    CHECK(ntb0_dll_module_reloc_applied(k32) > 0,
          "no relocation was applied");
    CHECK(ntb0_dll_module_imports_unresolved(k32) == 0,
          "%u import(s) of kernel32 stayed unresolved",
          ntb0_dll_module_imports_unresolved(k32));
    CHECK(ntb0_dll_module_imports_resolved(k32) > 0,
          "kernel32 has no resolved import");

    /* ---- (3) إثبات الكتابة في الـ IAT ---- */
    n_imp = (unsigned)ntb0_dll_import_count(k32);
    {
        unsigned bad = 0, iat_bad = 0;

        for (i = 0; i < n_imp; i++) {
            ntb0_dll_import rec;
            uint64_t expect;

            if (ntb0_dll_import_at(k32, (int)i, &rec) != 0)
                continue;
            expect = stub_addr(rec.host, rec.func);
            if (!rec.resolved || rec.addr != expect)
                bad++;
            /* القيمة داخل الصورة نفسها */
            {
                uint64_t a = ntb0_dll_module_rva_to_addr(k32, rec.iat_rva);

                if (!a) {
                    iat_bad++;
                } else {
                    uint64_t v;

                    memcpy(&v, (const void *)(uintptr_t)a, sizeof(v));
                    if (v != rec.addr)
                        iat_bad++;
                }
            }
            /* api-set */
            if (strncasecmp(rec.dll, "api-ms-win-", 11) == 0) {
                n_apiset_rec++;
                if (strcmp(rec.dll, rec.host) != 0)
                    apiset_ok++;
            }
        }
        CHECK(bad == 0, "%u import(s) of kernel32 hold a wrong address", bad);
        CHECK(iat_bad == 0, "%u IAT slot(s) of kernel32 hold a wrong value",
              iat_bad);
        CHECK(n_imp > 0, "kernel32 has no import record");
    }

    if (n_apiset_rec)
        printf("  api-set     : %d api-ms-win-* import(s), %d re-routed "
               "to a host DLL\n", n_apiset_rec, apiset_ok);

    /* جدول الاستيراد: DLL ⇒ عدد الدوال + المضيف بعد api-set */
    {
        int d, nd = (int)ntb0_dll_module_import_dll_count(k32);
        int idx = 0;

        printf("  import table:\n");
        for (d = 0; d < nd; d++) {            char dll[128] = "";
            char host[128] = "";
            int cnt = 0;

            while (idx < (int)n_imp) {
                ntb0_dll_import rec;

                ntb0_dll_import_at(k32, idx, &rec);
                if (!dll[0]) {
                    snprintf(dll, sizeof(dll), "%s", rec.dll);
                    snprintf(host, sizeof(host), "%s", rec.host);
                }
                if (strcmp(dll, rec.dll) != 0)
                    break;
                cnt++;
                idx++;
            }
            printf("    %-44s %3d func(s) -> %s\n", dll, cnt,
                   strcmp(host, dll) ? host : "(as is)");
        }
    }

    /* ---- (5) GetProcAddress ---- */
    {
        ntb0_pe *pe = ntb0_pe_open(path);
        ntb0_pe_export e;
        void *addr;
        uint32_t rva;
        int found = 0;

        if (!pe) {
            NTB0_DLL_LFAIL("DLL-005", "cannot re-open the export table: %s",
                           ntb0_pe_last_error());
            g_fail = 1;
            return -1;
        }
        if (ntb0_pe_find_by_name(pe, "CreateFileA", &e))
            found = 1;
        else if (ntb0_pe_find_by_name(pe, "LoadLibraryA", &e))
            found = 1;
        else if (ntb0_pe_find_by_name(pe, "GetProcAddress", &e))
            found = 1;

        if (!found) {
            NTB0_DLL_LFAIL("DLL-005", "CreateFileA not found in the export "
                                      "table");
            g_fail = 1;
            ntb0_pe_close(pe);
            return -1;
        }

        addr = ntb0_dll_get_proc(ld, k32, e.name);
        rva = e.rva;

        snprintf(g_gp_name, sizeof(g_gp_name), "%s", e.name);
        g_gp_addr = (uint64_t)(uintptr_t)addr;
        g_gp_rva = rva;
        g_gp_ordinal = e.ordinal;

        printf("  GetProcAddress(\"%s\") = 0x%llx (RVA 0x%08x, ordinal %u)%s\n",
               e.name, (unsigned long long)(uintptr_t)addr, rva,
               (unsigned)e.ordinal, e.forwarded ? " [forwarder]" : "");

        CHECK(addr != NULL, "GetProcAddress(%s) failed", e.name);
        if (addr && !e.forwarded) {
            CHECK((uint64_t)(uintptr_t)addr ==
                      ntb0_dll_module_base(k32) + rva,
                  "GetProcAddress(%s) = %p does not match base+RVA (%p)",
                  e.name, addr,
                  (void *)(uintptr_t)(ntb0_dll_module_base(k32) + rva));
        }
        /* البحث بالترتيب يجب أن يعطي نفس العنوان */
        {
            void *addr2 = ntb0_dll_get_proc_ord(ld, k32, e.ordinal);

            CHECK(addr2 == addr,
                  "GetProcAddress by ordinal %u disagrees with the name "
                  "lookup", (unsigned)e.ordinal);
        }
        ntb0_pe_close(pe);
    }

    NTB0_DLL_LOK("DLL-002", "kernel32.dll mapped at 0x%llx (%d sections, "
                            "%zu bytes)", (unsigned long long)
                  ntb0_dll_module_base(k32), ntb0_dll_module_nsec(k32),
                  ntb0_dll_module_size(k32));
    NTB0_DLL_LOK("DLL-003", "relocations applied: %u entry(ies) in %u "
                            "block(s), delta %+lld",
                 ntb0_dll_module_reloc_applied(k32),
                 ntb0_dll_module_reloc_blocks(k32),
                 (long long)ntb0_dll_module_delta(k32));
    NTB0_DLL_LOK("DLL-004", "imports: %u resolved, %u unresolved (IAT "
                            "verified)", ntb0_dll_module_imports_resolved(k32),
                 ntb0_dll_module_imports_unresolved(k32));
    NTB0_DLL_LOK("DLL-005", "GetProcAddress(\"%s\") = 0x%llx (RVA 0x%08x, "
                            "ordinal %u)", g_gp_name,
                 (unsigned long long)g_gp_addr, g_gp_rva, g_gp_ordinal);
    (void)ctx;
    return 0;
}

/* (6) الوحدة الثانية + قائمة الوحدات */
static int test_second_module(const char *root, ntb0_dll_loader *ld,
                               ntb0_dll_module **u32_out)
{
    char path[1024];
    ntb0_dll_module *u32;
    int i, n;

    snprintf(path, sizeof(path), "%s/Windows/System32/user32.dll", root);
    u32 = ntb0_dll_load(ld, path, 0);
    if (!u32) {
        NTB0_DLL_LFAIL("DLL-004", "cannot load user32.dll: %s",
                       ntb0_dll_last_error(ld));
        g_fail = 1;
        return -1;
    }
    *u32_out = u32;

    printf("user32.dll   : mapped at 0x%llx, %d section(s), %u reloc(s), "
           "%d imports resolved / %d unresolved (%d records)\n",
           (unsigned long long)ntb0_dll_module_base(u32),
           ntb0_dll_module_nsec(u32), ntb0_dll_module_reloc_applied(u32),
           ntb0_dll_module_imports_resolved(u32),
           ntb0_dll_module_imports_unresolved(u32),
           ntb0_dll_import_count(u32));

    n = ntb0_dll_module_count(ld);
    CHECK(n >= 2, "the loader lists %d module(s), expected >= 2", n);
    printf("  loaded modules (%d):\n", n);
    for (i = 0; i < n; i++) {
        ntb0_dll_module *m = ntb0_dll_module_at(ld, i);

        if (m)
            printf("    [%d] %-16s 0x%llx  %s\n", i,
                   ntb0_dll_module_name(m),
                   (unsigned long long)ntb0_dll_module_base(m),
                   ntb0_dll_module_path(m));
    }
    CHECK(ntb0_dll_find_module(ld, "KERNEL32.DLL") != NULL,
          "the module list is not searchable (case-insensitive)");

    /* انظر: هل انتهى الـ stub في حساب العناوين؟ */
    {
        void *p = ntb0_dll_get_proc(ld, u32, "MessageBoxA");

        CHECK(p != NULL, "MessageBoxA not found in user32");
        if (p)
            printf("  user32!MessageBoxA = 0x%llx\n",
                   (unsigned long long)(uintptr_t)p);
    }

    NTB0_DLL_LOK("DLL-004", "second module loaded: user32.dll at 0x%llx "
                            "(%d modules in the list)",
                 (unsigned long long)ntb0_dll_module_base(u32),
                 ntb0_dll_module_count(ld));
    return 0;
}

/* (7) الاستيراد بالترتيب على PE مصطنع */
static int test_ordinal_import(ntb0_dll_loader *ld)
{
    char path[512];
    ntb0_dll_module *m;
    ntb0_dll_import rec;
    int n;

    mkdir(FAKE_DIR, 0755);
    snprintf(path, sizeof(path), "%s/fakeord.dll", FAKE_DIR);
    if (build_fake_dll(path, 0x22000000ULL, "fakedep.dll", "FakeFuncA",
                       5) != 0) {
        NTB0_DLL_LFAIL("DLL-002", "cannot build the synthetic PE");
        g_fail = 1;
        return -1;
    }

    m = ntb0_dll_load(ld, path, 0);
    if (!m) {
        NTB0_DLL_LFAIL("DLL-002", "cannot load the synthetic PE: %s",
                       ntb0_dll_last_error(ld));
        g_fail = 1;
        return -1;
    }

    n = ntb0_dll_import_count(m);
    CHECK(n == 2, "the synthetic PE reports %d import(s), expected 2", n);

    if (ntb0_dll_import_at(m, 0, &rec) == 0) {
        printf("synthetic PE : import[0] %s!%s by name => 0x%llx\n",
               rec.dll, rec.func, (unsigned long long)rec.addr);
        CHECK(!rec.by_ordinal, "import[0] was taken as an ordinal");
        CHECK(strcmp(rec.func, "FakeFuncA") == 0,
              "import[0] name is '%s'", rec.func);
        CHECK(rec.resolved && rec.addr == stub_addr(rec.host, rec.func),
              "import[0] was not resolved to the stub address");
    }
    if (ntb0_dll_import_at(m, 1, &rec) == 0) {
        printf("synthetic PE : import[1] %s!%s by ordinal => 0x%llx\n",
               rec.dll, rec.func, (unsigned long long)rec.addr);
        CHECK(rec.by_ordinal, "import[1] was not taken as an ordinal");
        CHECK(rec.ordinal == 5, "import[1] ordinal is %u, expected 5",
              (unsigned)rec.ordinal);
        CHECK(strcmp(rec.func, "#5") == 0, "import[1] func is '%s'",
              rec.func);
        CHECK(rec.resolved && rec.addr == stub_addr(rec.host, rec.func),
              "import[1] was not resolved to the stub address");
        /* الـ IAT كُتب فعلاً */
        {
            uint64_t a = ntb0_dll_module_rva_to_addr(m, rec.iat_rva);
            uint64_t v = 0;

            if (a)
                memcpy(&v, (const void *)(uintptr_t)a, sizeof(v));
            CHECK(a && v == rec.addr,
                  "the ordinal import did not reach the IAT");
        }
    }

    /* تفريغ الوحدة */
    CHECK(ntb0_dll_unload(ld, m) == NTB0_DLL_OK,
          "cannot unload the synthetic module");
    NTB0_DLL_LOK("DLL-004", "ordinal import resolved (#5) and written to "
                            "the IAT");
    return 0;
}

/* (7b) التصديرات المُعاد توجيهها (forwarders) + ntdll الحقيقي */
static int test_forwarders(const char *root, ntb0_dll_loader *ld,
                           ntb0_dll_module *k32)
{
    char path[1024];
    ntb0_dll_module *ntdll;
    void *p;
    unsigned stubs = 0;

    snprintf(path, sizeof(path), "%s/Windows/System32/ntdll.dll", root);
    ntdll = ntb0_dll_load(ld, path, 0);
    if (!ntdll) {
        NTB0_DLL_LFAIL("DLL-005", "cannot load the real ntdll.dll: %s",
                       ntb0_dll_last_error(ld));
        g_fail = 1;
        return -1;
    }

    printf("ntdll.dll    : mapped at 0x%llx (%d sections, %u reloc(s), "
           "%d imports resolved)\n",
           (unsigned long long)ntb0_dll_module_base(ntdll),
           ntb0_dll_module_nsec(ntdll), ntb0_dll_module_reloc_applied(ntdll),
           ntb0_dll_module_imports_resolved(ntdll));

    /* تصدير مُعاد توجيهه من kernel32 ⇒ ntdll (مثل AcquireSRWLockExclusive) */
    p = ntb0_dll_get_proc(ld, k32, "AcquireSRWLockExclusive");
    CHECK(p != NULL, "the forwarded export AcquireSRWLockExclusive was not "
                     "resolved");
    if (p) {
        uint64_t a = ntb0_dll_module_base(ntdll);
        uint64_t s = ntb0_dll_module_size(ntdll);

        printf("  forwarder   : kernel32!AcquireSRWLockExclusive -> "
               "ntdll 0x%llx (inside ntdll: %s)\n",
               (unsigned long long)(uintptr_t)p,
               ((uint64_t)(uintptr_t)p >= a &&
                (uint64_t)(uintptr_t)p < a + s) ? "yes" : "NO");
        CHECK((uint64_t)(uintptr_t)p >= a && (uint64_t)(uintptr_t)p < a + s,
              "the forwarded address is not inside the ntdll image");
    }

    /*
     * ستوبات الـ syscall داخل الصورة المرسومة (نمط ntb0_trap.c):
     *   4C 8B D1 | B8 <num32> | 0F 05
     * ⇒ الصورة الجاهزة ليُصحّحها المترجم "في مكانها".
     */
    {
        const uint8_t *img = (const uint8_t *)(uintptr_t)
            ntb0_dll_module_base(ntdll);
        size_t n = ntb0_dll_module_size(ntdll);
        size_t i;

        for (i = 0; i + 10 <= n; i++) {
            if (img[i] == 0x4c && img[i + 1] == 0x8b && img[i + 2] == 0xd1 &&
                img[i + 3] == 0xb8 && img[i + 8] == 0x0f && img[i + 9] == 0x05)
                stubs++;
        }
    }
    printf("  syscalls    : %u syscall stub(s) found in the mapped ntdll "
           "(ready to be patched in place)\n", stubs);
    CHECK(stubs > 0, "no syscall stub found in the mapped ntdll");

    ntb0_dll_unload(ld, ntdll);
    NTB0_DLL_LOK("DLL-005", "forwarded exports resolved into the loaded "
                            "ntdll image (%u syscall stub(s) visible)",
                 stubs);
    return 0;
}

/* (8) الاعتماديات + الدورات */
static int test_deps_and_cycles(ntb0_dll_loader *ld)
{
    char pa[512], pb[512];
    ntb0_dll_module *a;
    unsigned before;

    mkdir(FAKE_DIR, 0755);
    snprintf(pa, sizeof(pa), "%s/fakea.dll", FAKE_DIR);
    snprintf(pb, sizeof(pb), "%s/fakeb.dll", FAKE_DIR);

    if (build_fake_dll(pa, 0x23000000ULL, "fakeb.dll", "FakeFuncB", 7) != 0 ||
        build_fake_dll(pb, 0x24000000ULL, "fakea.dll", "FakeFuncA", 9) != 0) {
        NTB0_DLL_LFAIL("DLL-007", "cannot build the synthetic dependency "
                                   "pair");
        g_fail = 1;
        return -1;
    }

    ntb0_dll_loader_set_sysdir(ld, FAKE_DIR);
    before = ntb0_dll_cycle_count(ld);

    a = ntb0_dll_load_name(ld, "fakea.dll", NTB0_DLL_FLAG_AUTO_DEPS);
    if (!a) {
        NTB0_DLL_LFAIL("DLL-007", "cannot load fakea.dll: %s",
                       ntb0_dll_last_error(ld));
        g_fail = 1;
        return -1;
    }

    printf("dependencies  : fakea.dll imports %d DLL(s):",
           ntb0_dll_dep_count(a));
    {
        int i, n = ntb0_dll_dep_count(a);

        for (i = 0; i < n; i++)
            printf(" %s", ntb0_dll_dep_at(a, i));
        printf("\n");
    }
    CHECK(ntb0_dll_dep_count(a) == 1, "fakea.dll should import 1 DLL");
    CHECK(ntb0_dll_find_module(ld, "fakeb.dll") != NULL,
          "the dependency fakeb.dll was not auto-loaded");
    CHECK(ntb0_dll_cycle_count(ld) > before,
          "the cycle fakea <-> fakeb was not detected");

    printf("cycles        : %u detected (fakea.dll -> fakeb.dll -> "
           "fakea.dll)\n", ntb0_dll_cycle_count(ld));

    NTB0_DLL_LOK("DLL-007", "dependency auto-load works: fakeb.dll pulled "
                            "in by fakea.dll");
    NTB0_DLL_LOK("DLL-008", "dependency cycle detected and broken "
                            "(%u cycle(s))", ntb0_dll_cycle_count(ld));
    return 0;
}

/* (8b) التحميل على عنوان مختلف ⇒ delta != 0 ⇒ إعادة توجيه حقيقية */
/* عناوين مرشّحة (خارج "فجوة" shadow الخاصة بـ ASan) */
static const uint64_t g_reloc_bases[] = {
    0x0000700000000000ULL, 0x0000600000000000ULL, 0x0000400000000000ULL,
    0x0000300000000000ULL, 0x0000210000000000ULL
};

static int test_relocated(const char *root)
{
    char path[1024];
    ntb0_dll_loader *ld;
    ntb0_dll_module *m;
    struct pe_min pm;
    uint8_t *file;
    size_t flen;

    snprintf(path, sizeof(path), "%s/Windows/System32/gdi32.dll", root);

    file = slurp(path, &flen);
    if (!file) {
        NTB0_DLL_LFAIL("DLL-003", "cannot read gdi32.dll");
        g_fail = 1;
        return -1;
    }
    if (pe_min_parse(file, flen, &pm) != 0) {
        NTB0_DLL_LFAIL("DLL-003", "cannot parse gdi32.dll");
        g_fail = 1;
        free(file);
        return -1;
    }

    /*
     * الحالة (أ): عنوان إجباري مختلف عن المفضّل ⇒ delta != 0.
     * (نجرّب عدّة عناوين: تحت ASan تكون بعض المناطق محجوزة.)
     */
    ld = ntb0_dll_loader_new();
    if (!ld) {
        free(file);
        g_fail = 1;
        return -1;
    }
    {
        unsigned bi;

        for (bi = 0; bi < sizeof(g_reloc_bases) / sizeof(g_reloc_bases[0]);
             bi++) {
            m = ntb0_dll_load_at(ld, path, g_reloc_bases[bi], 0);
            if (m)
                break;
        }
    }
    if (!m) {
        printf("moved base   : SKIPPED (no free address for a forced "
               "base): %s\n", ntb0_dll_last_error(ld));
        ntb0_dll_loader_free(ld);
        free(file);
        return 0;
    }

    printf("moved base   : gdi32.dll at 0x%llx (preferred 0x%llx, "
           "delta %+lld), %u reloc(s) applied\n",
           (unsigned long long)ntb0_dll_module_base(m),
           (unsigned long long)ntb0_dll_module_pref_base(m),
           (long long)ntb0_dll_module_delta(m),
           ntb0_dll_module_reloc_applied(m));

    CHECK(ntb0_dll_module_base(m) != ntb0_dll_module_pref_base(m),
          "the module was mapped at its preferred base anyway");
    CHECK(ntb0_dll_module_delta(m) != 0,
          "the loader did not move gdi32.dll off its preferred base");
    CHECK(ntb0_dll_module_reloc_applied(m) > 0,
          "no relocation was applied on the moved base");

    /*
     * تحقّق مستقل: نقرأ دليل .reloc من الملف بأنفسنا ونقارن الأرقام
     * مع ما يبلّغه المحمّل، ثم نتحقّق من قيمة مُصحَّحة فعلياً.
     */
    {
        long dd = dd_offset(file, flen, 5);
        uint32_t rel_rva = 0, rel_sz = 0;
        long off;
        uint64_t pos = 0;
        unsigned entries = 0, blocks = 0;
        long sample_rva = -1;
        uint64_t sample_old = 0;

        if (dd > 0) {
            rel_rva = (uint32_t)file[dd] | ((uint32_t)file[dd + 1] << 8) |
                      ((uint32_t)file[dd + 2] << 16) |
                      ((uint32_t)file[dd + 3] << 24);
            rel_sz = (uint32_t)file[dd + 4] | ((uint32_t)file[dd + 5] << 8) |
                     ((uint32_t)file[dd + 6] << 16) |
                     ((uint32_t)file[dd + 7] << 24);
        }
        off = rva_to_off(&pm, file, rel_rva);
        if (off >= 0) {
            while (pos + 8 <= rel_sz) {
                const uint8_t *blk = file + off + pos;
                uint32_t page_rva = (uint32_t)blk[0] | ((uint32_t)blk[1] << 8) |
                                    ((uint32_t)blk[2] << 16) |
                                    ((uint32_t)blk[3] << 24);
                uint32_t blk_size = (uint32_t)blk[4] | ((uint32_t)blk[5] << 8) |
                                    ((uint32_t)blk[6] << 16) |
                                    ((uint32_t)blk[7] << 24);
                uint32_t n_ent, k;

                if (blk_size < 8 || pos + blk_size > rel_sz)
                    break;
                blocks++;
                n_ent = (blk_size - 8) / 2;
                for (k = 0; k < n_ent; k++) {
                    const uint8_t *e = blk + 8 + k * 2;
                    uint16_t ev = (uint16_t)(e[0] | (e[1] << 8));
                    uint16_t ty = (uint16_t)(ev >> 12);
                    uint32_t of = (uint32_t)(ev & 0xfff);
                    long toff;

                    entries++;
                    if (ty != 10 || sample_rva >= 0)
                        continue;
                    toff = rva_to_off(&pm, file, page_rva + of);
                    if (toff < 0 || toff + 8 > (long)flen)
                        continue;
                    sample_old = 0;
                    {
                        int b;

                        for (b = 0; b < 8; b++)
                            sample_old |= (uint64_t)file[toff + b] << (b * 8);
                    }
                    if (sample_old)
                        sample_rva = (long)(page_rva + of);
                }
                pos += blk_size;
            }
        }

        printf("  .reloc      : independent parse => %u block(s), %u "
               "entr(ies) (loader: %u / %u)\n", blocks, entries,
               ntb0_dll_module_reloc_blocks(m),
               ntb0_dll_module_reloc_entries(m));
        CHECK(blocks == ntb0_dll_module_reloc_blocks(m),
              "the loader counted %u relocation block(s), the file has %u",
              ntb0_dll_module_reloc_blocks(m), blocks);
        CHECK(entries == ntb0_dll_module_reloc_entries(m),
              "the loader counted %u relocation entr(ies), the file has %u",
              ntb0_dll_module_reloc_entries(m), entries);

        if (sample_rva >= 0) {
            uint64_t a = ntb0_dll_module_rva_to_addr(m, (uint32_t)sample_rva);
            uint64_t now = 0;
            int b;

            for (b = 0; b < 8; b++)
                now |= (uint64_t)((const uint8_t *)(uintptr_t)a)[b]
                       << (b * 8);

            printf("  patched     : RVA 0x%lx: 0x%llx -> 0x%llx "
                   "(delta %+lld)\n", (unsigned long)sample_rva,
                   (unsigned long long)sample_old, (unsigned long long)now,
                   (long long)ntb0_dll_module_delta(m));
            CHECK(now == (uint64_t)(sample_old +
                                    (uint64_t)ntb0_dll_module_delta(m)),
                  "the relocated value at RVA 0x%lx is wrong",
                  (unsigned long)sample_rva);
        } else {
            printf("  patched     : no DIR64 sample with a non-zero value\n");
        }
    }

    {
        int64_t dl = ntb0_dll_module_delta(m);
        unsigned ap = ntb0_dll_module_reloc_applied(m);

        ntb0_dll_unload(ld, m);
        NTB0_DLL_LOK("DLL-003", "relocations applied on a moved base: delta "
                                "%+lld, %u entry(ies)", (long long)dl, ap);
    }

    /*
     * الحالة (ب): العنوان المفضّل مشغول ⇒ المحمّل يختار عنواناً آخر
     * (نحمّل gdi32.dll مرّتين: الثانية لا تجد العنوان المفضّل حرّاً).
     */
    {
        ntb0_dll_module *m1 = ntb0_dll_load(ld, path, 0);
        ntb0_dll_module *m2 = m1 ? ntb0_dll_load(ld, path, 0) : NULL;

        if (m1 && m2) {
            printf("  preferred busy: 1st at 0x%llx, 2nd at 0x%llx "
                   "(delta %+lld, %u reloc(s))\n",
                   (unsigned long long)ntb0_dll_module_base(m1),
                   (unsigned long long)ntb0_dll_module_base(m2),
                   (long long)ntb0_dll_module_delta(m2),
                   ntb0_dll_module_reloc_applied(m2));
            CHECK(ntb0_dll_module_base(m2) != ntb0_dll_module_base(m1),
                  "the second copy was mapped over the first one");
            CHECK(ntb0_dll_module_delta(m2) != 0,
                  "the second copy did not move");
            CHECK(ntb0_dll_module_reloc_applied(m2) > 0,
                  "no relocation was applied on the fallback base");
            ntb0_dll_unload(ld, m2);
        }
        if (m1)
            ntb0_dll_unload(ld, m1);
    }

    ntb0_dll_loader_free(ld);
    free(file);
    return 0;
}

/* (9) رفض الملفات التالفة */
static int test_bad_files(const char *root)
{
    char src[1024], dst[512];
    uint8_t *buf;
    size_t len;
    ntb0_dll_loader *ld;
    int bad = 0;

    mkdir(BAD_DIR, 0755);
    snprintf(src, sizeof(src), "%s/Windows/System32/kernel32.dll", root);
    snprintf(dst, sizeof(dst), "%s/bad.dll", BAD_DIR);

    buf = slurp(src, &len);
    if (!buf) {
        NTB0_DLL_LFAIL("DLL-002", "cannot read %s", src);
        g_fail = 1;
        return -1;
    }

    ld = ntb0_dll_loader_new();
    if (!ld) {
        free(buf);
        g_fail = 1;
        return -1;
    }

    /* (أ) توقيع MZ مكسور */
    {
        uint8_t *b = (uint8_t *)malloc(len);

        memcpy(b, buf, len);
        b[0] = 'X';
        spit(dst, b, len);
        free(b);
        if (ntb0_dll_load(ld, dst, 0) != NULL) {
            NTB0_DLL_LFAIL("DLL-002", "a corrupted MZ header was accepted");
            bad++;
            g_fail = 1;
        } else {
            printf("rejected      : %s (bad MZ): %s\n", dst,
                   ntb0_dll_last_error(ld));
        }
    }

    /* (ب) ملف مبتور */
    {
        spit(dst, buf, 0x80);
        if (ntb0_dll_load(ld, dst, 0) != NULL) {
            NTB0_DLL_LFAIL("DLL-002", "a truncated file was accepted");
            bad++;
            g_fail = 1;
        } else {
            printf("rejected      : %s (truncated): %s\n", dst,
                   ntb0_dll_last_error(ld));
        }
    }

    /* (ج) دليل إعادة التوجيه خارج الصورة */
    {
        uint8_t *b = (uint8_t *)malloc(len);
        long off = dd_offset(buf, len, 5);

        memcpy(b, buf, len);
        if (off > 0) {
            put32(b + off, 0x7f000000u);   /* RVA خارج الصورة */
            spit(dst, b, len);
            if (ntb0_dll_load(ld, dst, 0) != NULL) {
                NTB0_DLL_LFAIL("DLL-002", "a bad relocation directory was "
                                          "accepted");
                bad++;
                g_fail = 1;
            } else {
                printf("rejected      : %s (bad .reloc): %s\n", dst,
                       ntb0_dll_last_error(ld));
            }
        }
        free(b);
    }

    /* (د) دليل الاستيراد خارج الصورة */
    {
        uint8_t *b = (uint8_t *)malloc(len);
        long off = dd_offset(buf, len, 1);

        memcpy(b, buf, len);
        if (off > 0) {
            put32(b + off, 0x7f000000u);
            spit(dst, b, len);
            if (ntb0_dll_load(ld, dst, 0) != NULL) {
                NTB0_DLL_LFAIL("DLL-002", "a bad import directory was "
                                          "accepted");
                bad++;
                g_fail = 1;
            } else {
                printf("rejected      : %s (bad imports): %s\n", dst,
                       ntb0_dll_last_error(ld));
            }
        }
        free(b);
    }

    /* (هـ) عدد أقسامAbsurd */
    {
        uint8_t *b = (uint8_t *)malloc(len);
        uint32_t e_lfanew = (uint32_t)buf[0x3C] | ((uint32_t)buf[0x3D] << 8) |
                            ((uint32_t)buf[0x3E] << 16) |
                            ((uint32_t)buf[0x3F] << 24);

        memcpy(b, buf, len);
        put16(b + e_lfanew + 6, 0xFFFF);
        spit(dst, b, len);
        if (ntb0_dll_load(ld, dst, 0) != NULL) {
            NTB0_DLL_LFAIL("DLL-002", "an absurd section count was "
                                      "accepted");
            bad++;
            g_fail = 1;
        } else {
            printf("rejected      : %s (absurd section count): %s\n", dst,
                   ntb0_dll_last_error(ld));
        }
        free(b);
    }

    /* (و) ملف عشوائي */
    {
        uint8_t junk[256];
        size_t i;

        for (i = 0; i < sizeof(junk); i++)
            junk[i] = (uint8_t)(i * 7 + 3);
        spit(dst, junk, sizeof(junk));
        if (ntb0_dll_load(ld, dst, 0) != NULL) {
            NTB0_DLL_LFAIL("DLL-002", "a random file was accepted");
            bad++;
            g_fail = 1;
        } else {
            printf("rejected      : %s (random bytes): %s\n", dst,
                   ntb0_dll_last_error(ld));
        }
    }

    free(buf);
    ntb0_dll_loader_free(ld);

    CHECK(bad == 0, "%d corrupted file(s) were accepted", bad);
    if (bad == 0)
        NTB0_DLL_LOK("DLL-002", "malformed PE images are rejected cleanly "
                                "(no crash, no leak)");
    return 0;
}

/* (9b) اختبار تحمّل: تحويلات عشوائية في الترويسة ⇒ لا انهيار أبداً */
static int test_fuzz(const char *root)
{
    char src[1024], dst[512];
    uint8_t *base;
    size_t len;
    ntb0_dll_loader *ld;
    unsigned iter, loaded = 0, rejected = 0;
    unsigned seed = 0x5eed1234u;

    snprintf(src, sizeof(src), "%s/Windows/System32/kernel32.dll", root);
    snprintf(dst, sizeof(dst), "%s/fuzz.dll", BAD_DIR);

    base = slurp(src, &len);
    if (!base) {
        NTB0_DLL_LFAIL("DLL-002", "cannot read %s", src);
        g_fail = 1;
        return -1;
    }

    ld = ntb0_dll_loader_new();
    if (!ld) {
        free(base);
        g_fail = 1;
        return -1;
    }

    for (iter = 0; iter < 400; iter++) {
        uint8_t *copy = (uint8_t *)malloc(len);
        unsigned k;

        if (!copy)
            break;
        memcpy(copy, base, len);

        /* 1..8 بايتات عشوائية في الترويسة + جدول الأقسام */
        for (k = 0; k < 4; k++) {
            seed = seed * 1103515245u + 12345u;
            copy[(seed >> 8) % 0x400] = (uint8_t)(seed >> 16);
        }
        spit(dst, copy, len);
        free(copy);

        {
            ntb0_dll_module *m = ntb0_dll_load(ld, dst, 0);

            if (m) {
                loaded++;
                ntb0_dll_unload(ld, m);
            } else {
                rejected++;
            }
        }
    }

    free(base);
    ntb0_dll_loader_free(ld);

    printf("fuzz          : 400 mutated header(s) => %u loaded, %u rejected "
           "(no crash)\n", loaded, rejected);
    CHECK(loaded + rejected == 400, "the fuzz loop lost an iteration");
    NTB0_DLL_LOK("DLL-002", "fuzz: 400 mutated PE headers handled without a "
                            "crash (%u accepted, %u rejected)", loaded,
                 rejected);
    return 0;
}

/* ------------------------------------------------------------------ */
/* (10) التفريغ                                                        */
/* ------------------------------------------------------------------ */
static int test_unload(ntb0_dll_loader *ld, ntb0_dll_module *k32,
                       ntb0_dll_module *u32)
{
    int n0 = ntb0_dll_module_count(ld);

    CHECK(n0 >= 2, "expected at least 2 modules, got %d", n0);
    CHECK(ntb0_dll_unload(ld, u32) == NTB0_DLL_OK, "cannot unload user32");
    CHECK(ntb0_dll_module_count(ld) == n0 - 1,
          "the module list was not updated after the unload");
    CHECK(ntb0_dll_unload(ld, k32) == NTB0_DLL_OK, "cannot unload kernel32");
    NTB0_DLL_LOK("DLL-006", "FreeLibrary: %d module(s) unloaded",
                 n0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    const char *root = getenv("NTBOX_ROOT");
    char sysdir[1024];
    struct stub_ctx ctx;
    ntb0_dll_loader *ld;
    ntb0_apiset *as;
    ntb0_dll_module *k32 = NULL, *u32 = NULL;

    if (!root || !*root)
        root = (argc > 1) ? argv[1] : DEFAULT_ROOT;

    snprintf(sysdir, sizeof(sysdir), "%s/Windows/System32", root);

    memset(&ctx, 0, sizeof(ctx));
    setvbuf(stdout, NULL, _IOLBF, 0);   /* ترتيب المخرجات مع stderr */

    ld = ntb0_dll_loader_new();
    if (!ld) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    ntb0_dll_loader_set_resolver(ld, stub_resolve, &ctx);
    ntb0_dll_loader_set_sysdir(ld, sysdir);

    /* api-set: أسماء api-ms-win-* ⇒ DLL مضيف */
    as = ntb0_apiset_new();
    if (as) {
        ntb0_apiset_set_sysdir(as, sysdir);
        ntb0_dll_loader_set_apiset(ld, as);
    }

    printf("NTBOX_ROOT    : %s\n\n", root);

    if (test_kernel32(root, ld, &ctx, &k32) != 0)
        goto done;
    if (test_second_module(root, ld, &u32) != 0)
        goto done;
    test_ordinal_import(ld);
    test_forwarders(root, ld, k32);
    test_deps_and_cycles(ld);
    test_relocated(root);
    test_unload(ld, k32, u32);
    test_bad_files(root);
    test_fuzz(root);

    printf("\nresolver calls: %u\n", ctx.calls);

done:
    ntb0_apiset_free(as);
    ntb0_dll_loader_free(ld);

    if (g_fail) {
        NTB0_DLL_LFAIL("TEST", "the loader self-test FAILED");
        return 1;
    }
    printf("\n[ntbox][TEST][OK] the DLL loader self-test passed\n");
    return 0;
}
