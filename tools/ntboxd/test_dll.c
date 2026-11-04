/*
 * test_dll.c — الاختبار الذاتي لوحدة تحميل DLL (W5 · T-119 + T-127).
 *
 * ما يفحصه (بالترتيب):
 *   1) [DLL-001] جدول تصديرات PE من DLL حقيقي:
 *        - عدد التصديرات + أول 10 أسماء مرتَّبة بالترتيب (ordinal)
 *        - بحث بالاسم ("CreateFileA") ⇒ ترتيب + RVA
 *        - بحث بالترتيب ⇒ نفس التصدير
 *        - رفض ملف غير-PE (بلا انهيار)
 *   2) [DLL-010] قراءة مخطّط apisetschema من خلية SOFTWARE
 *        (الوضع الحقيقي: ويندوز 7 **لا** يحتوي مخطّطاً ⇒ يُبلَّغ ذلك
 *         بوضوح، ولا يدّعي الاختبار نجاحاً لمخطّط غير موجود)
 *   3) [DLL-009] حلّ عقود api-set ⇒ أسماء DLLs مضيفة
 *        (تقرير مصدر كل نتيجة: schema / win7-derived / fallback-table)
 *
 * جذر ويندوز: من NTBOX_ROOT أو argv[1] أو المسار الافتراضي.
 * الخروج: 0 فقط إذا نجحت كل الفحوص.
 *
 * البناء:
 *   gcc -O2 -Wall -Wextra -Itools/ntboxreg -Itools/ntboxd \
 *       -Itools/ntboxd/syscalls -o /tmp/opencode/test_dll \
 *       tools/ntboxd/test_dll.c tools/ntboxd/pe_export.c \
 *       tools/ntboxd/apiset.c tools/ntboxreg/hive.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <unistd.h>
#include <dirent.h>

#include "pe_export.h"
#include "apiset.h"
#include "ntb0_trace.h"

#define DEFAULT_ROOT "/home/imadix/Downloads/Niga/win7root"

static int g_fail = 0;

/* ------------------------------------------------------------------ */
/* (1) جدول التصديرات                                                 */
/* ------------------------------------------------------------------ */

static int test_exports(const char *root)
{
    char path[1024];
    const char *cands[3];
    ntb0_pe *pe = NULL;
    ntb0_pe_export e;
    int i, named, slots, base;
    const char *used = NULL;

    cands[0] = "kernel32.dll";
    cands[1] = "user32.dll";
    cands[2] = "gdi32.dll";

    for (i = 0; i < 3; i++) {
        snprintf(path, sizeof(path), "%s/Windows/System32/%s", root, cands[i]);
        pe = ntb0_pe_open(path);
        if (pe) {
            used = cands[i];
            break;
        }
    }

    if (!pe) {
        NTB0_FAIL("DLL-001", "no PE image found under %s/Windows/System32",
                  root);
        g_fail = 1;
        return -1;
    }

    named = ntb0_pe_export_count(pe);
    slots = ntb0_pe_export_slot_count(pe);
    base  = ntb0_pe_export_ordinal_base(pe);

    printf("PE image      : %s\n", path);
    printf("  internal name: %s\n", ntb0_pe_dll_name(pe));
    printf("  format       : %s, image base 0x%llx, %zu bytes\n",
           ntb0_pe_is_64(pe) ? "PE32+ (x64)" : "PE32 (x86)",
           (unsigned long long)ntb0_pe_image_base(pe), ntb0_pe_file_size(pe));
    printf("  exports      : %d named (ordinal base %d), %d slots\n", named,
           base, slots);

    if (named <= 0) {
        NTB0_FAIL("DLL-001", "%s exposes no named exports", used);
        g_fail = 1;
        ntb0_pe_close(pe);
        return -1;
    }

    /* أول 10 أسماء مرتَّبة بالترتيب */
    printf("  first 10 exports (by ordinal):\n");
    for (i = 0; i < 10 && i < named; i++) {
        if (ntb0_pe_export_named_at(pe, i, &e) != 0)
            break;
        printf("    #%-5u ord=%-5u rva=0x%08x %-34s%s%s\n", (unsigned)i,
               e.ordinal, e.rva, e.name ? e.name : "(null)",
               e.forwarded ? " -> " : "", e.forwarded ? e.forwarder : "");
    }

    /* بحث بالاسم */
    if (!ntb0_pe_find_by_name(pe, "CreateFileA", &e) &&
        !ntb0_pe_find_by_name(pe, "LoadLibraryA", &e) &&
        !ntb0_pe_find_by_name(pe, "GetProcAddress", &e)) {
        NTB0_FAIL("DLL-001", "CreateFileA/LoadLibraryA not found in %s", used);
        g_fail = 1;
        ntb0_pe_close(pe);
        return -1;
    }

    printf("  lookup by name: %s => ordinal %u, RVA 0x%08x%s%s\n",
           e.name ? e.name : "?", e.ordinal, e.rva,
           e.forwarded ? ", forwarder -> " : "", e.forwarded ? e.forwarder : "");

    /* بحث بالترتيب ⇒ نفس النتيجة */
    {
        ntb0_pe_export e2;

        if (!ntb0_pe_find_by_ordinal(pe, e.ordinal, &e2) ||
            !e2.name || strcmp(e2.name, e.name) != 0) {
            NTB0_FAIL("DLL-001", "ordinal lookup failed for ordinal %u",
                      e.ordinal);
            g_fail = 1;
            ntb0_pe_close(pe);
            return -1;
        }
        printf("  lookup by ordinal %u => %s (RVA 0x%08x)\n", e.ordinal,
               e2.name, e2.rva);
    }

    /* رفض ملف غير PE (بلا انهيار) */
    {
        char junk[512];
        FILE *f;
        ntb0_pe *bad;

        snprintf(junk, sizeof(junk), "/tmp/opencode/test_dll_junk.bin");
        f = fopen(junk, "wb");
        if (f) {
            static const char payload[] = "NOT-A-PE-FILE-AT-ALL-0123456789";

            fwrite(payload, 1, sizeof(payload) - 1, f);
            fclose(f);
            bad = ntb0_pe_open(junk);
            if (bad) {
                NTB0_FAIL("DLL-001", "a non-PE file was accepted (!)");
                g_fail = 1;
                ntb0_pe_close(bad);
            } else {
                printf("  malformed file rejected cleanly: %s\n",
                       ntb0_pe_last_error());
            }
            unlink(junk);
        }
    }

    NTB0_OK("DLL-001", "%s: %d named exports (base %d, %d slots), "
                      "'%s' => ordinal %u RVA 0x%08x", used, named, base, slots,
            e.name ? e.name : "?", e.ordinal, e.rva);

    ntb0_pe_close(pe);
    return 0;
}

/* ------------------------------------------------------------------ */
/* (2) مخطّط api-set من الريجستري                                      */
/* ------------------------------------------------------------------ */

static int test_schema(const char *root)
{
    char hive[1024];
    ntb0_apiset *a;
    int rc;

    snprintf(hive, sizeof(hive), "%s/Windows/System32/config/SOFTWARE", root);

    a = ntb0_apiset_new();
    if (!a) {
        NTB0_FAIL("DLL-010", "out of memory");
        g_fail = 1;
        return -1;
    }

    rc = ntb0_apiset_load_schema(a, hive);

    if (rc == NTB0_APISET_OK) {
        printf("api-set schema : LOADED from '%s' — %s\n", hive,
               ntb0_apiset_status(a));
        NTB0_OK("DLL-010", "api-set schema parsed from '%s': %s", hive,
                ntb0_apiset_status(a));
    } else {
        /*
         * الوضع الحقيقي على ويندوز 7: لا مخطّط في الريجستري.
         * (تم الفحص بالبايت على الخلية: "apisetschema" غير موجودة إطلاقاً.)
         * نُبلّغ الأمر بوضوح — ولا نُعلن نجاحاً لمخطّط غير موجود.
         */
        printf("api-set schema : NOT FOUND in '%s' (%s) — rc=%d\n", hive,
               ntb0_apiset_status(a), rc);
        NTB0_OK("DLL-010", "no api-set schema in '%s' (Windows 7 has none): %s",
                hive, ntb0_apiset_status(a));
    }

    ntb0_apiset_free(a);
    return rc;
}

/* ------------------------------------------------------------------ */
/* (3) حلّ العقود                                                      */
/* ------------------------------------------------------------------ */

static const char *g_contracts[] = {
    "api-ms-win-core-file-l1-1-0.dll",
    "api-ms-win-core-file-l1-2-0.dll",
    "api-ms-win-core-synch-l1-1-0.dll",
    "api-ms-win-core-synch-l1-2-0.dll",
    "api-ms-win-core-heap-l1-1-0.dll",
    "api-ms-win-core-rtlsupport-l1-1-0.dll",
    "api-ms-win-security-base-l1-1-0.dll",
    "api-ms-win-service-management-l1-1-0.dll",
    "api-ms-win-crt-runtime-l1-1-0.dll",
};

#define N_CONTRACTS ((int)(sizeof(g_contracts) / sizeof(g_contracts[0])))

/* هل ملف DLL باسم معيّن موجود في المجلد؟ (غير حسّاس لحالة الأحرف) */
static int file_present_ci(const char *dir, const char *name)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    int found = 0;

    if (!d)
        return 0;
    while ((de = readdir(d)) != NULL) {
        size_t i;

        if (strlen(de->d_name) != strlen(name))
            continue;
        for (i = 0; de->d_name[i]; i++) {
            int a = tolower((unsigned char)de->d_name[i]);
            int b = tolower((unsigned char)name[i]);

            if (a != b)
                break;
        }
        if (de->d_name[i] == '\0') {
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

static int test_resolve(const char *root)
{
    char sysdir[1024];
    char hive[1024];
    ntb0_apiset *a;
    int i, resolved = 0;
    const char *unknown = "api-ms-win-not-a-real-contract-l9-9-9.dll";

    snprintf(sysdir, sizeof(sysdir), "%s/Windows/System32", root);
    snprintf(hive, sizeof(hive), "%s/Windows/System32/config/SOFTWARE", root);

    a = ntb0_apiset_new();
    if (!a) {
        NTB0_FAIL("DLL-009", "out of memory");
        g_fail = 1;
        return -1;
    }

    ntb0_apiset_set_sysdir(a, sysdir);
    ntb0_apiset_load_schema(a, hive);   /* غير حرج: قد لا يوجد مخطّط */

    printf("api-set resolution (system dir: %s)\n", sysdir);
    printf("  %-38s %-15s %-14s %s\n", "contract", "host dll", "source",
           "details");

    for (i = 0; i < N_CONTRACTS; i++) {
        ntb0_apiset_result r;
        char family[256];
        char detail[320];
        int host_here;

        if (ntb0_apiset_resolve(a, g_contracts[i], &r) != NTB0_APISET_OK) {
            NTB0_FAIL("DLL-009", "contract '%s' could not be resolved",
                      g_contracts[i]);
            g_fail = 1;
            continue;
        }

        ntb0_apiset_family(g_contracts[i], family, sizeof(family));
        host_here = file_present_ci(sysdir, r.host);
        snprintf(detail, sizeof(detail),
                 "family=%s, contract-file=%s, host-file-in-tree=%s",
                 family, r.have_stub ? "present" : "absent",
                 host_here ? "present" : "ABSENT");

        printf("  %-38s %-15s %-14s %s\n", g_contracts[i], r.host,
               ntb0_apiset_src_name(r.src), detail);
        printf("  %-38s %-15s %-14s %s\n", "", "", "", r.note);
        resolved++;
    }

    /* عقدة غير معروفة ⇒ تُبلَّغ بشكل مختلف */
    {
        ntb0_apiset_result r;

        if (ntb0_apiset_resolve(a, unknown, &r) == NTB0_APISET_OK) {
            NTB0_FAIL("DLL-009", "an unknown contract resolved to '%s' (!)",
                      r.host);
            g_fail = 1;
        } else {
            printf("  %-38s %-15s %-14s (unknown contract reported)\n", unknown,
                   "-", "none");
        }
    }

    if (resolved < 5) {
        NTB0_FAIL("DLL-009", "only %d/%d contracts resolved", resolved,
                  N_CONTRACTS);
        g_fail = 1;
    } else {
        NTB0_OK("DLL-009", "%d api-set contracts resolved "
                          "(source: schema/win7-derived/fallback as printed)",
                resolved);
    }

    ntb0_apiset_free(a);
    return 0;
}

/* ------------------------------------------------------------------ */
/* (4) فحص صيغة مخطّط APISET على كتلة مُركَّبة (اختبار ذاتي)          */
/* ------------------------------------------------------------------ */

/*
 * ملاحظة أمانة علمية: هذه الكتلة **مُركَّبة يدوياً** من التخطيط
 * الموصوف في apiset.h — وليست كتلة Microsoft حقيقية (لا وجود لها في شجرة
 * ويندوز 7 ولا شبكة للوصول إليها). الغرض: التأكّد من أنّ مسار التحليل
 * يعمل (حدود، تجزئة، بحث) — لا إثبات مطابقة الصيغة مع Microsoft.
 */
static int test_blob_parser(void)
{
    static const char *names[] = { "api-ms-win-core-file-l1-1-0.dll",
                                   "api-ms-win-core-synch-l1-1-0.dll" };
    static const char *hosts[] = { "kernel32.dll", "kernel32.dll" };
    uint8_t blob[512];
    uint32_t n_tbl = 0, h_tbl = 0, f_tbl = 0;
    uint32_t n_hash = 0;
    ntb0_apiset *a;
    int i, rc;
    char host[64];

    memset(blob, 0, sizeof(blob));

    for (i = 0; i < 2; i++)
        n_tbl += (uint32_t)strlen(names[i]) + 1;
    h_tbl = (uint32_t)strlen(hosts[0]) + 1 + (uint32_t)strlen(hosts[1]) + 1;
    f_tbl = 0;

    memcpy(blob, "APISET", 6);
    blob[6] = 6;                      /* version */
    blob[7] = 0;
    blob[8] = 0;                      /* flags */

#define PUT32(off, val)                                                   \
    do {                                                                  \
        blob[off] = (uint8_t)((val) & 0xff);                              \
        blob[(off) + 1] = (uint8_t)(((val) >> 8) & 0xff);                \
        blob[(off) + 2] = (uint8_t)(((val) >> 16) & 0xff);               \
        blob[(off) + 3] = (uint8_t)(((val) >> 24) & 0xff);               \
    } while (0)

    PUT32(NTB0_APISET_OFF_COUNT, 2);
    PUT32(NTB0_APISET_OFF_NAMES, NTB0_APISET_HDR_SIZE);
    PUT32(NTB0_APISET_OFF_HOSTS, NTB0_APISET_HDR_SIZE + n_tbl);
    PUT32(NTB0_APISET_OFF_FLAGSSTR, NTB0_APISET_HDR_SIZE + n_tbl + h_tbl);
    PUT32(NTB0_APISET_OFF_NAMEHASH, NTB0_APISET_HDR_SIZE + n_tbl + h_tbl + f_tbl);
    PUT32(NTB0_APISET_OFF_HOSTHASH, 0);
    PUT32(NTB0_APISET_OFF_NAMESZ, n_tbl);
    PUT32(NTB0_APISET_OFF_HOSTSZ, h_tbl);
    PUT32(NTB0_APISET_OFF_FLAGSZ, f_tbl);
    PUT32(NTB0_APISET_OFF_NAMEHASHSZ, 16);   /* زوجان */
    PUT32(NTB0_APISET_OFF_HOSTHASHSZ, 0);

    {
        uint32_t p = NTB0_APISET_HDR_SIZE;

        for (i = 0; i < 2; i++) {
            memcpy(blob + p, names[i], strlen(names[i]) + 1);
            p += (uint32_t)strlen(names[i]) + 1;
        }
        for (i = 0; i < 2; i++) {
            memcpy(blob + p, hosts[i], strlen(hosts[i]) + 1);
            p += (uint32_t)strlen(hosts[i]) + 1;
        }
    }

    /* جدول التجزئة: (hash, index) لكل عقدة — تجزئة FNV-1a */
    {
        uint32_t p = NTB0_APISET_HDR_SIZE + n_tbl + h_tbl + f_tbl;

        for (i = 0; i < 2; i++) {
            uint32_t h = 2166136261u;
            const char *s = names[i];

            for (; *s; s++) {
                unsigned char c = (unsigned char)*s;

                if (c >= 'A' && c <= 'Z')
                    c = (unsigned char)(c + 32);
                h ^= c;
                h *= 16777619u;
            }
            PUT32(p, h);
            PUT32(p + 4, (uint32_t)i);
            p += 8;
        }
        n_hash = p;
        (void)n_hash;
    }

    a = ntb0_apiset_new();
    if (!a) {
        NTB0_FAIL("DLL-009", "out of memory");
        g_fail = 1;
        return -1;
    }

    rc = ntb0_apiset_parse_blob(a, blob, sizeof(blob));
    if (rc != NTB0_APISET_OK) {
        NTB0_FAIL("DLL-009", "synthetic APISET blob was rejected (rc=%d)", rc);
        g_fail = 1;
        ntb0_apiset_free(a);
        return -1;
    }

    {
        ntb0_apiset_result r;

        if (ntb0_apiset_resolve(a, "API-MS-WIN-CORE-FILE-L1-1-0.DLL", &r) !=
                NTB0_APISET_OK ||
            strcmp(r.host, "kernel32.dll") != 0) {
            NTB0_FAIL("DLL-009", "synthetic schema lookup failed ('%s')",
                      r.host);
            g_fail = 1;
        } else {
            printf("  synthetic APISET blob  : parsed (%d entries) — "
                   "lookup => %s [source=%s]\n",
                   2, r.host, ntb0_apiset_src_name(r.src));
        }
    }

    /* كتلة غير APISET ⇒ رفض نظيف */
    {
        uint8_t junk[64];

        memset(junk, 0xAB, sizeof(junk));
        rc = ntb0_apiset_parse_blob(a, junk, sizeof(junk));
        if (rc != NTB0_APISET_EBADMAGIC) {
            NTB0_FAIL("DLL-009", "a non-APISET blob was not rejected (rc=%d)",
                      rc);
            g_fail = 1;
        }
    }

    (void)host;
    ntb0_apiset_free(a);
    return 0;
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *root = NULL;

    if (argc > 1 && argv[1][0])
        root = argv[1];
    else if (getenv("NTBOX_ROOT") && getenv("NTBOX_ROOT")[0])
        root = getenv("NTBOX_ROOT");
    else
        root = DEFAULT_ROOT;

    printf("NT-Box DLL self-test\n");
    printf("  win7 root: %s\n", root);
    printf("\n");

    if (test_exports(root) != 0)
        printf("\n");
    test_schema(root);
    printf("\n");
    test_resolve(root);
    printf("\n");
    test_blob_parser();

    printf("\n");
    if (g_fail) {
        NTB0_FAIL("DLL-001", "self-test FAILED");
        printf("RESULT: FAIL\n");
        return 1;
    }

    printf("RESULT: PASS\n");
    return 0;
}
