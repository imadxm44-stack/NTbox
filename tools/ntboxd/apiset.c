/*
 * apiset.c — محلّل أسماء api-set إلى DLLs حقيقية (NT-Box / W5 · T-127).
 *
 * المراجع في هذا الملف (كلها مقيسة على شجرة ويندوز 7 الحقيقية):
 *   1) dlls حقيقية تستورد أسماء api-set:
 *        kernel32.dll ⇒ API-MS-Win-Core-RtlSupport-L1-1-0.dll
 *        shell32.dll  ⇒ API-MS-Win-Core-File-L1-1-0.dll
 *        msvcrt.dll   ⇒ API-MS-Win-Core-Console-L1-1-0.dll
 *      ⇒ بلا حلّ العقد لا تُحمَّل هذه DLLs إطلاقاً.
 *   2) في ويندوز 7 لا يوجد مخطّط api-set في الريجستري:
 *      فحص بالبايت على خلية SOFTWARE (37,748,736 بايت) لـ "apisetschema"
 *      (ASCII و UTF-16، بلا حساسية) ⇒ **0 نتيجة**. لذلك في هذه الشجرة
 *      المصدر الفعّال هو الاشتقاق من الملفات (انظر أدناه) ثم الجدول
 *      الاحتياطي. هذا مُعلن في apiset.h وفي تقرير العمل — لا ادّعاء
 *      بأن المخطّط مُحلَّل وهو ليس كذلك.
 *   3) الاشتقاق من الملفات الحقيقية:
 *      في System32 بـ 35 ملف stub باسم api-ms-win-*.dll (و276 في الشجرة
 *      كلها). كل stub يُصدّر *أسماء* العقد (مثل api-ms-win-core-file:
 *      76 اسماً: CreateFileA/W, DeleteFileA/W, …) ⇒ نبحث عن الوحدة
 *      التي تُصدّر نفس الأسماء ⇒ المضيف الحقيقي.
 *      مثال مقيس: api-ms-win-core-file-l1-1-0.dll ⇒ 76/76 في
 *      kernel32.dll ⇒ المضيف = kernel32.dll.
 *      ملاحظة مهمّة: تصديرات الـ stub نفسها لا تصلح كدليل — كلها
 *      تشير إلى عناوين RVA واحدة (0x105c = "xor eax,eax; ret") أي أنها
 *      *أختام فارغة*؛ المضيف لا يُعرف منها بل من الوحدة الحقيقية.
 *
 * المسار (1) مخطّط الريجستري: الصيغة موثّقة في apiset.h (magic "APISET")
 * — وهو وصف مستند إلى ملف المهمة، **غير مُقارن** بكتلة Microsoft
 * حقيقية (لا وجود لها هنا). لذلك يفضّل المحلّل إرجاع خطأ "المخطّط غير
 * موجود" على ادّعاء نجاح.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#include "apiset.h"
#include "pe_export.h"
#include "hive.h"     /* قارئ الريجستري الموجود (tools/ntboxreg) — دون تعديل */

/* ------------------------------------------------------------------ */
/* بنى داخلية                                                         */
/* ------------------------------------------------------------------ */

struct apiset_entry {
    char *name;    /* اسم العقد ("api-ms-win-core-file-l1-2-0.dll") */
    char *host;    /* DLL المضيف ("kernel32.dll") */
    char *flags;   /* سلائم flags (قد تكون NULL) */
};

struct apiset_file {
    char *name;    /* اسم الملف كما هو على القرص */
    char *path;    /* المسار الكامل */
};

struct apiset_fam {
    char *family;
    char *host;
};

struct ntb0_apiset {
    /* --- المخطّط (من الريجستري) --- */
    int   schema_loaded;
    char  status[256];
    struct apiset_entry *ent;
    int   n_ent;
    uint32_t *name_hash;      /* أزواج: hash ثم index */
    int   n_name_hash;

    /* --- الاشتقاق من شجرة ويندوز 7 --- */
    char  sysdir[512];
    struct apiset_file *files; /* كل ملفات المجلد (بحث غير حسّاس لحالة الأحرف) */
    int   n_files;
    int   dir_scanned;
    struct apiset_fam *fams;   /* ذاكرة مؤقتة: عائلة ⇒ مضيف */
    int   n_fams;
};

/* ------------------------------------------------------------------ */
/* أدوات مساعدة                                                       */
/* ------------------------------------------------------------------ */

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int ci_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);

        if (ca != cb)
            return ca - cb;
        a++;
        b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

static int ci_eq(const char *a, const char *b)
{
    return ci_cmp(a, b) == 0;
}

/* بادئة (غير حسّاسة لحالة الأحرف) */
static int ci_prefix(const char *s, const char *prefix)
{
    while (*prefix) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix))
            return 0;
        s++;
        prefix++;
    }
    return 1;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);

    if (p)
        memcpy(p, s, n);
    return p;
}

/* FNV-1a (32 بت) على الاسم مُحوَّلاً لصغيرة — دالة تجزئة المخطّط */
static uint32_t fnv1a_lower(const char *s)
{
    uint32_t h = 2166136261u;

    for (; *s; s++) {
        unsigned char c = (unsigned char)tolower((unsigned char)*s);

        h ^= c;
        h *= 16777619u;
    }
    return h;
}

const char *ntb0_apiset_src_name(ntb0_apiset_src s)
{
    switch (s) {
    case NTB0_APISET_SRC_SCHEMA:   return "schema";
    case NTB0_APISET_SRC_WIN7:     return "win7-derived";
    case NTB0_APISET_SRC_FALLBACK: return "fallback-table";
    default:                       return "none";
    }
}

/* ------------------------------------------------------------------ */
/* عائلة العقد                                                        */
/* ------------------------------------------------------------------ */

/*
 * "api-ms-win-core-file-l1-2-0.dll" ⇒ "api-ms-win-core-file"
 * (حذف ".dll" ثم حذف اللاحقة -l<num>-<num>-<num>).
 */
const char *ntb0_apiset_family(const char *contract, char *buf, size_t len)
{
    size_t n;
    char *p;

    if (!contract || !buf || len == 0)
        return NULL;

    n = strlen(contract);
    if (n >= len)
        n = len - 1;
    memcpy(buf, contract, n);
    buf[n] = '\0';

    /* حذف ".dll" */
    n = strlen(buf);
    if (n > 4 && ci_eq(buf + n - 4, ".dll"))
        buf[n - 4] = '\0';

    /* حذف "-lX-Y-Z" — آخر "-l" يليه رقم-رقم-رقم */
    n = strlen(buf);
    for (p = buf + (n ? n - 1 : 0); p >= buf; p--) {
        char *q;
        int nums = 0;

        if (p[0] != 'l' || p == buf || p[-1] != '-')
            continue;

        /* pat = p+1 ⇒ "<num>-<num>-<num>" حتى نهاية السلسلة */
        q = p + 1;
        while (*q) {
            if (isdigit((unsigned char)*q)) {
                while (isdigit((unsigned char)*q))
                    q++;
                nums++;
                if (*q == '-') {
                    q++;
                    continue;
                }
                break;
            }
            break;
        }
        if (nums == 3 && *q == '\0') {
            p[-1] = '\0';       /* اقتطع اللاحقة */
            break;
        }
    }

    return buf;
}

/* ------------------------------------------------------------------ */
/* المخطّط (REG_BINARY "apisetschema")                                  */
/* ------------------------------------------------------------------ */

static void free_schema(ntb0_apiset *a)
{
    int i;

    for (i = 0; i < a->n_ent; i++) {
        free(a->ent[i].name);
        free(a->ent[i].host);
        free(a->ent[i].flags);
    }
    free(a->ent);
    a->ent = NULL;
    a->n_ent = 0;
    free(a->name_hash);
    a->name_hash = NULL;
    a->n_name_hash = 0;
    a->schema_loaded = 0;
}

/* يستخرج count سلسلة من جدول نصوص [off, off+sz) */
static int collect_strings(const uint8_t *blob, uint32_t size, uint32_t off,
                           uint32_t sz, uint32_t count, char ***out,
                           uint32_t *n_out)
{
    char **arr;
    uint32_t i = 0, p = off, end = off + sz;

    if ((uint64_t)off + sz > size)
        return -1;

    arr = (char **)calloc(count ? count : 1, sizeof(char *));
    if (!arr)
        return -1;

    while (i < count && p < end) {
        uint32_t start = p;

        while (p < end && blob[p] != '\0')
            p++;
        if (p >= end)
            break;                   /* سلسلة غير منتهية */
        arr[i] = xstrdup((const char *)blob + start);
        if (!arr[i]) {
            uint32_t k;

            for (k = 0; k < i; k++)
                free(arr[k]);
            free(arr);
            return -1;
        }
        i++;
        p++;                        /* تجاوز NUL */
    }

    *out = arr;
    *n_out = i;
    return 0;
}

int ntb0_apiset_parse_blob(ntb0_apiset *a, const uint8_t *blob, uint32_t size)
{
    uint32_t count, n_off, h_off, f_off, nh_off, hh_off;
    uint32_t n_sz, h_sz, f_sz, nh_sz, hh_sz;
    char **names = NULL, **hosts = NULL, **flags = NULL;
    uint32_t n_names = 0, n_hosts = 0, n_flags = 0;
    int rc = NTB0_APISET_OK;
    uint32_t i;

    if (!a)
        return NTB0_APISET_EFORMAT;
    if (!blob || size < NTB0_APISET_HDR_SIZE) {
        snprintf(a->status, sizeof(a->status),
                 "schema blob too small (%u bytes)", size);
        return NTB0_APISET_EFORMAT;
    }

    if (memcmp(blob, NTB0_APISET_MAGIC, NTB0_APISET_MAGIC_LEN) != 0) {
        snprintf(a->status, sizeof(a->status),
                 "schema blob has no 'APISET' magic");
        return NTB0_APISET_EBADMAGIC;
    }

    count  = rd32(blob + NTB0_APISET_OFF_COUNT);
    n_off  = rd32(blob + NTB0_APISET_OFF_NAMES);
    h_off  = rd32(blob + NTB0_APISET_OFF_HOSTS);
    f_off  = rd32(blob + NTB0_APISET_OFF_FLAGSSTR);
    nh_off = rd32(blob + NTB0_APISET_OFF_NAMEHASH);
    hh_off = rd32(blob + NTB0_APISET_OFF_HOSTHASH);
    n_sz   = rd32(blob + NTB0_APISET_OFF_NAMESZ);
    h_sz   = rd32(blob + NTB0_APISET_OFF_HOSTSZ);
    f_sz   = rd32(blob + NTB0_APISET_OFF_FLAGSZ);
    nh_sz  = rd32(blob + NTB0_APISET_OFF_NAMEHASHSZ);
    hh_sz  = rd32(blob + NTB0_APISET_OFF_HOSTHASHSZ);

    if (count > 65536u) {
        snprintf(a->status, sizeof(a->status),
                 "schema entry count is absurd (%u)", count);
        return NTB0_APISET_EFORMAT;
    }

    /* كل الجداول داخل الكتلة */
    if ((uint64_t)n_off + n_sz > size || (uint64_t)h_off + h_sz > size ||
        (uint64_t)f_off + f_sz > size || (uint64_t)nh_off + nh_sz > size ||
        (uint64_t)hh_off + hh_sz > size) {
        snprintf(a->status, sizeof(a->status),
                 "schema tables are out of the blob bounds");
        return NTB0_APISET_EFORMAT;
    }
    if (nh_sz % 8u || hh_sz % 8u) {
        snprintf(a->status, sizeof(a->status),
                 "schema hash tables are not (hash,index) pairs");
        return NTB0_APISET_EFORMAT;
    }

    if (collect_strings(blob, size, n_off, n_sz, count, &names, &n_names) != 0) {
        snprintf(a->status, sizeof(a->status), "schema name table is broken");
        return NTB0_APISET_EFORMAT;
    }
    if (collect_strings(blob, size, h_off, h_sz, count, &hosts, &n_hosts) != 0) {
        for (i = 0; i < n_names; i++)
            free(names[i]);
        free(names);
        snprintf(a->status, sizeof(a->status), "schema host table is broken");
        return NTB0_APISET_EFORMAT;
    }
    if (f_sz)
        (void)collect_strings(blob, size, f_off, f_sz, count, &flags, &n_flags);

    free_schema(a);

    a->ent = (struct apiset_entry *)calloc(n_names ? n_names : 1,
                                           sizeof(*a->ent));
    if (!a->ent) {
        rc = NTB0_APISET_ENOMEM;
        goto out;
    }

    for (i = 0; i < n_names; i++) {
        a->ent[i].name = names[i];
        names[i] = NULL;
        a->ent[i].host = (i < n_hosts) ? hosts[i] : NULL;
        if (i < n_hosts)
            hosts[i] = NULL;
        a->ent[i].flags = (i < n_flags) ? flags[i] : NULL;
        if (i < n_flags)
            flags[i] = NULL;
        a->n_ent++;
    }

    /* جدول تجزئة الأسماء (مُسرِّع البحث) */
    if (nh_sz) {
        uint32_t pairs = nh_sz / 8u;

        a->name_hash = (uint32_t *)calloc(pairs * 2u, sizeof(uint32_t));
        if (a->name_hash) {
            for (i = 0; i < pairs; i++) {
                a->name_hash[i * 2] = rd32(blob + nh_off + i * 8);
                a->name_hash[i * 2 + 1] = rd32(blob + nh_off + i * 8 + 4);
            }
            a->n_name_hash = (int)pairs;
        }
    }

    a->schema_loaded = 1;
    snprintf(a->status, sizeof(a->status),
             "schema parsed: %d entries (APISET v%u, flags 0x%x)", a->n_ent,
             (unsigned)(blob[NTB0_APISET_OFF_VERSION] |
                        (blob[NTB0_APISET_OFF_VERSION + 1] << 8)),
             rd32(blob + NTB0_APISET_OFF_FLAGS));

out:
    for (i = 0; i < n_names; i++)
        free(names[i]);
    for (i = 0; i < n_hosts; i++)
        free(hosts[i]);
    for (i = 0; i < n_flags; i++)
        free(flags[i]);
    free(names);
    free(hosts);
    free(flags);
    return rc;
}

int ntb0_apiset_load_schema(ntb0_apiset *a, const char *software_hive)
{
    ntb0_hive *h;
    uint32_t key, type = 0, size = 0;
    const uint8_t *data = NULL;
    struct stat st;
    int rc;

    if (!a)
        return NTB0_APISET_EIO;
    if (!software_hive || !*software_hive) {
        snprintf(a->status, sizeof(a->status), "no hive path given");
        return NTB0_APISET_EIO;
    }

    /* الملف يجب أن يكون ملفاً عادياً (قارئ hive يتوقّع خلية) */
    if (stat(software_hive, &st) != 0 || !S_ISREG(st.st_mode)) {
        snprintf(a->status, sizeof(a->status), "'%s' is not a registry hive file",
                 software_hive);
        return NTB0_APISET_EIO;
    }

    h = ntb0_hive_open(software_hive);
    if (!h) {
        snprintf(a->status, sizeof(a->status), "cannot open hive '%s'",
                 software_hive);
        return NTB0_APISET_EIO;
    }

    key = ntb0_hive_open_path(h, "Microsoft\\Windows NT\\CurrentVersion");
    if (key == 0xFFFFFFFFu) {
        snprintf(a->status, sizeof(a->status),
                 "no key 'Microsoft\\Windows NT\\CurrentVersion' in '%s'",
                 software_hive);
        ntb0_hive_close(h);
        return NTB0_APISET_ENOSCHEMA;
    }

    if (!ntb0_hive_find_value(h, key, "apisetschema", &type, &data, &size)) {
        /* ربّما كان مفتاحاً يحوي الكتلة */
        uint32_t k2 = ntb0_hive_open_path(
            h, "Microsoft\\Windows NT\\CurrentVersion\\apisetschema");

        if (k2 == 0xFFFFFFFFu ||
            (!ntb0_hive_find_value(h, k2, "apisetschema", &type, &data, &size) &&
             !ntb0_hive_find_value(h, k2, "", &type, &data, &size))) {
            snprintf(a->status, sizeof(a->status),
                     "no 'apisetschema' value under "
                     "Microsoft\\Windows NT\\CurrentVersion (Win7 has none)");
            ntb0_hive_close(h);
            return NTB0_APISET_ENOSCHEMA;
        }
    }

    if (type != NTB0_REG_BINARY) {
        snprintf(a->status, sizeof(a->status),
                 "'apisetschema' is type %u (expected REG_BINARY)", type);
        ntb0_hive_close(h);
        return NTB0_APISET_EFORMAT;
    }

    rc = ntb0_apiset_parse_blob(a, data, size);
    ntb0_hive_close(h);
    return rc;
}

int ntb0_apiset_schema_loaded(const ntb0_apiset *a)
{
    return a ? a->schema_loaded : 0;
}

const char *ntb0_apiset_status(const ntb0_apiset *a)
{
    if (!a)
        return "";
    return a->status;
}

/* بحث داخل المخطّط (بالتجزئة أولاً ثم مسح خطّي) */
static int schema_lookup(const ntb0_apiset *a, const char *contract, char *host,
                         size_t host_len)
{
    uint32_t h = fnv1a_lower(contract);
    int i;

    for (i = 0; i < a->n_name_hash; i++) {
        uint32_t hash = a->name_hash[i * 2];
        uint32_t idx = a->name_hash[i * 2 + 1];

        if (hash == h && idx < (uint32_t)a->n_ent && a->ent[idx].name &&
            ci_eq(a->ent[idx].name, contract)) {
            if (!a->ent[idx].host)
                return 0;
            snprintf(host, host_len, "%s", a->ent[idx].host);
            return 1;
        }
    }

    for (i = 0; i < a->n_ent; i++) {
        if (a->ent[i].name && ci_eq(a->ent[i].name, contract) && a->ent[i].host) {
            snprintf(host, host_len, "%s", a->ent[i].host);
            return 1;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* الاشتقاق من شجرة ويندوز 7 (الملفات الحقيقية)                        */
/* ------------------------------------------------------------------ */

/*
 * وحدات ويندوز المُرشَّحة كمرشّحين للمضيف — **مرتَّبة حسب التفضيل**.
 * ملاحظة: هذه **قائمة مرشّحين** (وحدات نظام معروفة) — وليست خريطة
 * عقد⇒مضيف؛ العلاقة تُشتق بالقياس من قوائم التصدير.
 * عند تساوي نسبة التغطية يُقدَّم المرشّح الأسبق في القائمة.
 * (kernel32/ntdll/advapi32 هي الوحدات "العلنية" التي تشير إليها خرائط
 *  api-set المنشورة من Microsoft؛ kernelbase.dll في آخر القائمة لأنها
 *  نسخة التنفيذ الداخلية في ويندوز 7 — kernel32 هو الواجهة العلنية.)
 */
static const char *g_candidates[] = {
    "kernel32.dll", "ntdll.dll",   "advapi32.dll", "secur32.dll",
    "user32.dll",   "gdi32.dll",   "msvcrt.dll",   "ole32.dll",
    "shell32.dll",  "shlwapi.dll", "rpcrt4.dll",   "comctl32.dll",
    "comdlg32.dll", "version.dll", "ws2_32.dll",   "winhttp.dll",
    "netapi32.dll", "crypt32.dll", "setupapi.dll", "wintrust.dll",
    "mpr.dll",      "iphlpapi.dll", "dnsapi.dll",  "psapi.dll",
    "imagehlp.dll", "userenv.dll", "wintypes.dll", "normaliz.dll",
    "uxtheme.dll",  "imm32.dll",   "winmm.dll",    "oleacc.dll",
    "shcore.dll",   "propsys.dll", "urlmon.dll",   "wininet.dll",
    "cabinet.dll",  "mswsock.dll", "ucrtbase.dll", "gdiplus.dll",
    "msimg32.dll",  "wtsapi32.dll",
    "kernelbase.dll",   /* آخراً: نسخة التنفيذ الداخلية في Win7 */
};

#define N_CANDIDATES ((int)(sizeof(g_candidates) / sizeof(g_candidates[0])))

/* مسح مجلد System32 (قائمة أسماء + مسارات) */
static void scan_dir(ntb0_apiset *a)
{
    DIR *d;
    struct dirent *de;

    if (a->dir_scanned)
        return;
    a->dir_scanned = 1;

    if (!a->sysdir[0])
        return;

    d = opendir(a->sysdir);
    if (!d)
        return;

    while ((de = readdir(d)) != NULL) {
        struct stat st;
        char path[1024];
        struct apiset_file *nf;

        if (de->d_name[0] == '.')
            continue;

        snprintf(path, sizeof(path), "%s/%s", a->sysdir, de->d_name);
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        nf = (struct apiset_file *)realloc(
            a->files, (size_t)(a->n_files + 1) * sizeof(*a->files));
        if (!nf)
            break;
        a->files = nf;
        a->files[a->n_files].name = xstrdup(de->d_name);
        a->files[a->n_files].path = xstrdup(path);
        if (!a->files[a->n_files].name || !a->files[a->n_files].path)
            break;
        a->n_files++;
    }

    closedir(d);
}

/* بحث عن ملف بالاسم (غير حسّاس لحالة الأحرف) */
static const char *find_file_ci(ntb0_apiset *a, const char *name)
{
    int i;

    scan_dir(a);
    for (i = 0; i < a->n_files; i++)
        if (ci_eq(a->files[i].name, name))
            return a->files[i].path;
    return NULL;
}

/* يقارن نسختين "-lX-Y-Z" ⇒ أكبر نسخة أحدث */
static int version_newer(const char *a, const char *b)
{
    int va = 0, vb = 0;
    const char *p;

    for (p = a; *p; p++)
        if (*p >= '0' && *p <= '9')
            va = va * 10 + (*p - '0');
    for (p = b; *p; p++)
        if (*p >= '0' && *p <= '9')
            vb = vb * 10 + (*p - '0');
    return va > vb;
}

/* يبحث عن stub العائلة داخل شجرة ويندوز 7 (أحدث نسخة) */
static const char *find_family_stub(ntb0_apiset *a, const char *family,
                                    char *ver_out, size_t ver_len)
{
    int i;
    const char *best = NULL;
    char best_ver[64] = "";

    scan_dir(a);

    for (i = 0; i < a->n_files; i++) {
        const char *name = a->files[i].name;
        size_t nl = strlen(name);
        char stem[256];
        const char *suffix;

        if (nl < 5 || !ci_eq(name + nl - 4, ".dll"))
            continue;
        if (nl - 4 >= sizeof(stem))
            continue;
        memcpy(stem, name, nl - 4);
        stem[nl - 4] = '\0';

        if (!ci_prefix(stem, family))
            continue;
        suffix = stem + strlen(family);
        if (*suffix != '-')
            continue;               /* ليست نسخة من نفس العائلة */

        if (!best || version_newer(suffix, best_ver)) {
            best = a->files[i].path;
            snprintf(best_ver, sizeof(best_ver), "%s", suffix);
        }
    }

    if (best && ver_out && ver_len)
        snprintf(ver_out, ver_len, "%s", best_ver);
    return best;
}

/* مقارنة أسماء (لمصفوفة من السلاسل) */
static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* يبني مصفوفة أسماء مرتَّبة (نسخة مملوكة) من تصديرات صورة */
static char **pe_sorted_names(ntb0_pe *pe, int *n_out)
{
    int n = ntb0_pe_export_count(pe);
    char **arr;
    int i;

    *n_out = 0;
    if (n <= 0)
        return NULL;

    arr = (char **)calloc((size_t)n, sizeof(char *));
    if (!arr)
        return NULL;

    for (i = 0; i < n; i++) {
        ntb0_pe_export e;

        if (ntb0_pe_export_named_at(pe, i, &e) == 0 && e.name)
            arr[i] = xstrdup(e.name);
    }
    qsort(arr, (size_t)n, sizeof(char *), cmp_str);
    *n_out = n;
    return arr;
}

static void free_names(char **arr, int n)
{
    int i;

    for (i = 0; i < n; i++)
        free(arr[i]);
    free(arr);
}

/*
 * يشتقّ مضيف عائلة من الملفات الحقيقية:
 *   1) ابحث عن stub العائلة في System32 (مثل api-ms-win-core-file-l1-1-0.dll)
 *   2) خذ أسماء تصديراته (قائمة العقد)
 *   3) ابحث عن الوحدة التي تُصدّر أكبر نسبة من هذه الأسماء
 * يُعيد 0 عند النجاح.
 */
static int derive_family_host(ntb0_apiset *a, const char *family, char *host,
                              size_t host_len, char *note, size_t note_len)
{
    char stub[512], ver[64] = "";
    const char *stub_path;
    ntb0_pe *pe;
    char **names = NULL;
    int n_names = 0, c;
    int best_cov = 0, best_total = 0, best_count = 0, best_idx = -1;
    const char *best_mod = NULL;
    char best_path[512] = "";

    stub_path = find_family_stub(a, family, ver, sizeof(ver));
    if (!stub_path)
        return -1;

    snprintf(stub, sizeof(stub), "%s", stub_path);

    pe = ntb0_pe_open(stub);
    if (!pe)
        return -1;

    names = pe_sorted_names(pe, &n_names);
    ntb0_pe_close(pe);
    if (!names || n_names <= 0) {
        free_names(names, n_names);
        return -1;
    }

    for (c = 0; c < N_CANDIDATES; c++) {
        const char *cand = g_candidates[c];
        const char *cand_path;
        ntb0_pe *cpe;
        char **cnames = NULL;
        int ccount = 0, covered = 0, total = 0, j;

        if (ci_prefix(cand, "api-ms-"))
            continue;               /* لا نرشّح الـ stubs نفسها */

        cand_path = find_file_ci(a, cand);
        if (!cand_path)
            continue;

        cpe = ntb0_pe_open(cand_path);
        if (!cpe)
            continue;

        cnames = pe_sorted_names(cpe, &ccount);
        total = ccount;
        ntb0_pe_close(cpe);
        if (!cnames)
            continue;

        for (j = 0; j < n_names; j++) {
            if (!names[j])
                continue;
            if (bsearch(&names[j], cnames, (size_t)ccount, sizeof(char *),
                        cmp_str))
                covered++;
        }

        if (getenv("NTBOX_APISET_DEBUG"))
            fprintf(stderr, "    [apiset] %-16s covers %d/%d of '%s'\n", cand,
                    covered, n_names, family);

        free_names(cnames, ccount);

        if (total == 0)
            continue;

        /* النسبة = التغطية (%) — المضيف يجب أن يغطّي كل أسماء العقد */
        if (covered * 100 / n_names > best_cov ||
            (covered * 100 / n_names == best_cov && c < best_idx)) {
            best_cov = covered * 100 / n_names;
            best_total = covered;
            best_count = total;
            best_idx = c;
            best_mod = cand;
            snprintf(best_path, sizeof(best_path), "%s", cand_path);
        }
    }

    free_names(names, n_names);

    if (!best_mod || best_cov < 50)
        return -1;                  /* غير حاسم ⇒ الجدول الاحتياطي */

    snprintf(host, host_len, "%s", best_mod);
    if (note && note_len) {
        const char *base = strrchr(stub, '/');

        snprintf(note, note_len,
                 "%.24s covers %d/%d exports of %.40s (%d%%), %d exports total",
                 best_mod, best_total, n_names, base ? base + 1 : stub, best_cov,
                 best_count);
    }
    (void)best_path;
    (void)ver;
    return 0;
}

/* ذاكرة مؤقتة للعائلة ⇒ المضيف */
static const char *cached_family_host(ntb0_apiset *a, const char *family)
{
    int i;

    for (i = 0; i < a->n_fams; i++)
        if (ci_eq(a->fams[i].family, family))
            return a->fams[i].host;
    return NULL;
}

static void cache_family_host(ntb0_apiset *a, const char *family,
                              const char *host)
{
    struct apiset_fam *nf = (struct apiset_fam *)realloc(
        a->fams, (size_t)(a->n_fams + 1) * sizeof(*a->fams));

    if (!nf)
        return;
    a->fams = nf;
    a->fams[a->n_fams].family = xstrdup(family);
    a->fams[a->n_fams].host = xstrdup(host);
    if (!a->fams[a->n_fams].family || !a->fams[a->n_fams].host)
        return;
    a->n_fams++;
}

/* ------------------------------------------------------------------ */
/* الجدول الاحتياطي (FALLBACK)                                          */
/* ------------------------------------------------------------------ */
/*
 * ====================================================================
 *  FALLBACK — جدول ثابت احتياطي.
 *  ليس مُشتقاً من هذا الجهاز: مبني على خريطة العقود المنشورة من
 *  Microsoft لويندوز 8/10 (api-ms-win-<family> ⇒ DLL المضيف).
 *  يُستعمل **فقط** عندما لا يوجد دليل محلي:
 *     - لا stub على القرص للعائلة (عقود Win8+/Win10)، و
 *     - لا مخطّط في الريجستري.
 *  كل ما سبق (الأول) هو المصدر الفعّال على شجرة ويندوز 7.
 * ====================================================================
 */
static const struct {
    const char *prefix;
    const char *host;
} g_fallback[] = {
    { "api-ms-win-core-rtlsupport",   "ntdll.dll"    },
    { "api-ms-win-core-xstate",        "ntdll.dll"    },
    { "api-ms-win-core-delayload",     "shlwapi.dll"  },
    { "api-ms-win-core-winrt",         "combase.dll"  },
    { "api-ms-win-core-memory",        "kernelbase.dll" },
    { "api-ms-win-core-heap",          "kernelbase.dll" },
    { "api-ms-win-core-file",          "kernelbase.dll" },
    { "api-ms-win-core-processthreads","kernelbase.dll" },
    { "api-ms-win-core-synch",         "kernelbase.dll" },
    { "api-ms-win-core-libraryloader", "kernelbase.dll" },
    { "api-ms-win-core-localization",  "kernelbase.dll" },
    { "api-ms-win-core-sysinfo",       "kernelbase.dll" },
    { "api-ms-win-core-errorhandling", "kernelbase.dll" },
    { "api-ms-win-core-handle",        "kernelbase.dll" },
    { "api-ms-win-core-debug",         "kernelbase.dll" },
    { "api-ms-win-core-interlocked",   "kernelbase.dll" },
    { "api-ms-win-core-profile",       "kernelbase.dll" },
    { "api-ms-win-core-string",        "kernelbase.dll" },
    { "api-ms-win-core-util",          "kernelbase.dll" },
    { "api-ms-win-core-timezone",      "kernelbase.dll" },
    { "api-ms-win-core-namedpipe",     "kernelbase.dll" },
    { "api-ms-win-core-processenvironment", "kernelbase.dll" },
    { "api-ms-win-core-normalization", "kernelbase.dll" },
    { "api-ms-win-core-fibers",        "kernelbase.dll" },
    { "api-ms-win-core-",              "kernel32.dll" },
    { "api-ms-win-security-lsalookup", "secur32.dll"  },
    { "api-ms-win-security-",          "advapi32.dll" },
    { "api-ms-win-service-",           "advapi32.dll" },
    { "api-ms-win-crt-",               "ucrtbase.dll" },
    { "api-ms-win-downlevel-",         "kernel32.dll" },
    /* لا توجد قاعدة عامة "api-ms-win-*" عمداً: عقدة غير معروفة يجب أن
     * تُبلَّغ "غير معروفة" (NTB0_APISET_ENOENT) لا أن تُخمَّن. */
};

#define N_FALLBACK ((int)(sizeof(g_fallback) / sizeof(g_fallback[0])))

static int fallback_lookup(const char *family, char *host, size_t host_len)
{
    int i;

    for (i = 0; i < N_FALLBACK; i++) {
        if (ci_prefix(family, g_fallback[i].prefix)) {
            snprintf(host, host_len, "%s", g_fallback[i].host);
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* الحلّ                                                               */
/* ------------------------------------------------------------------ */

int ntb0_apiset_resolve(ntb0_apiset *a, const char *contract,
                         ntb0_apiset_result *out)
{
    char family[256];
    char host[64];
    char note[160];
    const char *cached;
    const char *exact;

    if (!a || !contract || !*contract || !out)
        return NTB0_APISET_ENOENT;

    memset(out, 0, sizeof(*out));
    out->src = NTB0_APISET_SRC_NONE;

    /* (1) المخطّط من الريجستري — إن وُجد وحُلِّل */
    if (a->schema_loaded &&
        schema_lookup(a, contract, host, sizeof(host))) {
        out->src = NTB0_APISET_SRC_SCHEMA;
        snprintf(out->host, sizeof(out->host), "%s", host);
        snprintf(out->note, sizeof(out->note), "from the registry api-set schema");
    } else {
        if (!ntb0_apiset_family(contract, family, sizeof(family)))
            return NTB0_APISET_ENOENT;

        /* هل ملف العقد نفسه موجود في شجرة ويندوز 7؟ */
        {
            char with_dll[300];
            size_t cl = strlen(contract);

            if (cl > 4 && ci_eq(contract + cl - 4, ".dll"))
                snprintf(with_dll, sizeof(with_dll), "%s", contract);
            else
                snprintf(with_dll, sizeof(with_dll), "%s.dll", contract);

            exact = find_file_ci(a, with_dll);
            if (!exact)
                exact = find_file_ci(a, contract);
        }
        if (exact) {
            out->have_stub = 1;
            snprintf(out->stub_path, sizeof(out->stub_path), "%s", exact);
        }

        /* (2) الاشتقاق من الملفات الحقيقية */
        cached = cached_family_host(a, family);
        if (cached) {
            out->src = NTB0_APISET_SRC_WIN7;
            snprintf(out->host, sizeof(out->host), "%s", cached);
            snprintf(out->note, sizeof(out->note),
                     "family '%.100s' resolved from the Win7 files", family);
        } else if (derive_family_host(a, family, host, sizeof(host), note,
                                      sizeof(note)) == 0) {
            cache_family_host(a, family, host);
            out->src = NTB0_APISET_SRC_WIN7;
            snprintf(out->host, sizeof(out->host), "%s", host);
            snprintf(out->note, sizeof(out->note), "%s", note);
        } else if (fallback_lookup(family, host, sizeof(host))) {
            /* (3) الجدول الاحتياطي الثابت */
            out->src = NTB0_APISET_SRC_FALLBACK;
            snprintf(out->host, sizeof(out->host), "%s", host);
            snprintf(out->note, sizeof(out->note),
                     "FALLBACK table (no local evidence for '%.90s')", family);
        } else {
            return NTB0_APISET_ENOENT;
        }
    }

    return NTB0_APISET_OK;
}

/* ------------------------------------------------------------------ */
/* الإنشاء والتحرير                                                    */
/* ------------------------------------------------------------------ */

ntb0_apiset *ntb0_apiset_new(void)
{
    ntb0_apiset *a = (ntb0_apiset *)calloc(1, sizeof(*a));

    if (!a)
        return NULL;
    snprintf(a->status, sizeof(a->status), "no api-set schema loaded");
    return a;
}

int ntb0_apiset_set_sysdir(ntb0_apiset *a, const char *dir)
{
    if (!a || !dir || !*dir)
        return NTB0_APISET_EIO;
    snprintf(a->sysdir, sizeof(a->sysdir), "%s", dir);
    a->dir_scanned = 0;
    return NTB0_APISET_OK;
}

void ntb0_apiset_free(ntb0_apiset *a)
{
    int i;

    if (!a)
        return;

    free_schema(a);

    for (i = 0; i < a->n_files; i++) {
        free(a->files[i].name);
        free(a->files[i].path);
    }
    free(a->files);

    for (i = 0; i < a->n_fams; i++) {
        free(a->fams[i].family);
        free(a->fams[i].host);
    }
    free(a->fams);

    free(a);
}
