/*
 * ntb0_reg.c — جسر الريجستري في المترجم (T-110 … T-114).
 *
 * المسار في ويندوز:
 *     \Registry\Machine\SYSTEM\ControlSet001\Services\RpcSs
 *                      └ خلية  └ مسار داخل الخلية
 *
 * الترجمة:
 *   1) فصل اسم الخلية (SYSTEM/SOFTWARE/DEFAULT/SAM) عن المسار الداخلي.
 *   2) فتح الخلية الحقيقية من <win7root>/Windows/System32/config/<NAME>
 *      (قارئ hive.c — مبني على قياس حقيقي).
 *   3) فتح المفتاح بالاسم داخل الخلية.
 *   4) إعادة النتائج كـ بنى NT (KEY_VALUE_INFORMATION / KEY_BASIC_INFORMATION)
 *      التي يتوقعها كود ويندوز (advapi32 …).
 *
 * المصابيح: SYS-049 (NtOpenKey) · SYS-050 (NtQueryValueKey)
 *            SYS-051 (NtEnumerateKey) · SYS-052 (NtEnumerateValueKey)
 *            SYS-053 (NtFlushKey)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <pthread.h>

#include "ntb0_reg.h"
#include "ntb0_handle.h"
#include "ntb0_nt.h"
#include "ntb0_trace.h"
#include "hive.h" /* tools/ntboxreg/hive.h */

/* ------------------------------------------------------------------ */
/* كائن المفتاح                                                         */
/* ------------------------------------------------------------------ */

struct ntb0_reg_key {
    ntb0_hive *hive;
    uint32_t   cell;
};

static void ntb0_reg_key_free(void *p)
{
    free(p);
}

/* ------------------------------------------------------------------ */
/* ذاكرة الخلايا (فتح كسول)                                            */
/* ------------------------------------------------------------------ */

#define NTB0_REG_MAX_HIVES 8

struct ntb0_reg_slot {
    char      name[64];
    ntb0_hive *hive;
};

static struct ntb0_reg_slot ntb0_reg_slots[NTB0_REG_MAX_HIVES];
static pthread_mutex_t      ntb0_reg_lock = PTHREAD_MUTEX_INITIALIZER;

/* يجلب الخلية (يفتحها عند الحاجة) — الاسم بحروف كبيرة كما في القرص */
static ntb0_hive *ntb0_reg_get_hive(const char *name)
{
    int i;
    ntb0_hive *h = NULL;
    char path[1024];

    if (!name || !*name)
        return NULL;

    pthread_mutex_lock(&ntb0_reg_lock);

    for (i = 0; i < NTB0_REG_MAX_HIVES; i++) {
        if (ntb0_reg_slots[i].hive && strcmp(ntb0_reg_slots[i].name, name) == 0) {
            h = ntb0_reg_slots[i].hive;
            pthread_mutex_unlock(&ntb0_reg_lock);
            return h;
        }
    }

    pthread_mutex_unlock(&ntb0_reg_lock);

    if (ntb0_hive_config_path(name, path, sizeof(path)) < 0) {
        NTB0_OK("SYS-049", "registry hive '%s' not found", name);
        return NULL;
    }

    h = ntb0_hive_open(path);
    if (!h)
        return NULL;

    pthread_mutex_lock(&ntb0_reg_lock);
    for (i = 0; i < NTB0_REG_MAX_HIVES; i++) {
        if (!ntb0_reg_slots[i].hive) {
            snprintf(ntb0_reg_slots[i].name, sizeof(ntb0_reg_slots[i].name),
                     "%s", name);
            ntb0_reg_slots[i].hive = h;
            break;
        }
    }
    pthread_mutex_unlock(&ntb0_reg_lock);

    return h;
}

/* ------------------------------------------------------------------ */
/* أدوات النص                                                           */
/* ------------------------------------------------------------------ */

/* UTF-16LE ⇒ UTF-8 (BMP) */
static int ntb0_reg_utf16_to_utf8(const uint16_t *src, size_t nchars, char *out,
                                  size_t out_len)
{
    size_t i, o = 0;

    if (!src || !out || out_len == 0)
        return -1;

    for (i = 0; i < nchars; i++) {
        uint16_t c = src[i];

        if (c < 0x80) {
            if (o + 1 >= out_len)
                return -1;
            out[o++] = (char)c;
        } else if (c < 0x800) {
            if (o + 2 >= out_len)
                return -1;
            out[o++] = (char)(0xC0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            if (o + 3 >= out_len)
                return -1;
            out[o++] = (char)(0xE0 | (c >> 12));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }

    out[o] = '\0';
    return (int)o;
}

/* UTF-8 ⇒ UTF-16LE */
static int ntb0_reg_utf8_to_utf16(const char *src, uint16_t *out,
                                  size_t out_chars)
{
    size_t i = 0, o = 0;
    const unsigned char *s = (const unsigned char *)src;

    if (!src || !out || out_chars == 0)
        return -1;

    while (s[i] && o + 1 < out_chars) {
        uint32_t cp;
        int extra;

        if (s[i] < 0x80) {
            cp = s[i];
            extra = 0;
        } else if ((s[i] & 0xE0) == 0xC0) {
            cp = s[i] & 0x1Fu;
            extra = 1;
        } else if ((s[i] & 0xF0) == 0xE0) {
            cp = s[i] & 0x0Fu;
            extra = 2;
        } else if ((s[i] & 0xF8) == 0xF0) {
            cp = s[i] & 0x07u;
            extra = 3;
        } else {
            cp = '?';
            extra = 0;
        }
        i++;

        while (extra-- > 0 && (s[i] & 0xC0) == 0x80) {
            cp = (cp << 6) | (uint32_t)(s[i] & 0x3Fu);
            i++;
        }

        if (cp < 0x10000) {
            out[o++] = (uint16_t)cp;
        } else {
            cp -= 0x10000u;
            if (o + 2 >= out_chars)
                break;
            out[o++] = (uint16_t)(0xD800u + (cp >> 10));
            out[o++] = (uint16_t)(0xDC00u + (cp & 0x3FFu));
        }
    }

    out[o] = 0;
    return (int)o;
}

/* مقارنة بادئة بدون حساسية حالة الحروف */
static int ntb0_reg_has_prefix_ci(const char *s, const char *prefix)
{
    while (*prefix) {
        char a = *s, b = *prefix;

        if (a >= 'a' && a <= 'z')
            a = (char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z')
            b = (char)(b - 'a' + 'A');
        if (a != b)
            return 0;
        s++;
        prefix++;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* فصل المسار                                                           */
/* ------------------------------------------------------------------ */

/*
 * \Registry\Machine\SYSTEM\ControlSet001\Services\RpcSs
 *   → hive_name = "SYSTEM" · key_path = "ControlSet001\Services\RpcSs"
 *
 * يُعيد 0 عند النجاح، -1 عند مسار غير مدعوم.
 */
static int ntb0_reg_split_path(const ntb0_unicode_string *us, char *hive_name,
                                size_t hn_len, char *key_path, size_t kp_len)
{
    char full[1024];
    char *p, *sep;
    size_t n;

    if (!us || !us->buffer || !hive_name || !key_path)
        return -1;

    n = (size_t)us->length / 2;
    if (n * 4 + 1 > sizeof(full))
        return -1;

    if (ntb0_reg_utf16_to_utf8(us->buffer, n, full, sizeof(full)) < 0)
        return -1;

    p = full;
    while (*p == '\\' || *p == '/')
        p++;

    if (!ntb0_reg_has_prefix_ci(p, "Registry\\")) {
        NTB0_OK("SYS-049", "not a machine registry path: %s", full);
        return -1;
    }
    p += 9;

    if (ntb0_reg_has_prefix_ci(p, "User\\")) {
        NTB0_OK("SYS-049", "per-user hives are not supported: %s", full);
        return -1;
    }

    if (!ntb0_reg_has_prefix_ci(p, "Machine\\"))
        return -1;
    p += 8;

    sep = p;
    while (*sep && *sep != '\\' && *sep != '/')
        sep++;
    if (sep == p)
        return -1;

    n = (size_t)(sep - p);
    if (n + 1 > hn_len)
        return -1;
    memcpy(hive_name, p, n);
    hive_name[n] = '\0';
    /* اسم الخلية على القرص بحروف كبيرة */
    {
        size_t i;

        for (i = 0; i < n; i++)
            hive_name[i] = (char)toupper((unsigned char)hive_name[i]);
    }

    p = sep;
    while (*p == '\\' || *p == '/')
        p++;
    snprintf(key_path, kp_len, "%s", p);

    return 0;
}

/* ------------------------------------------------------------------ */
/* NtOpenKey (SYS-049)                                                  */
/* ------------------------------------------------------------------ */

long ntb0_reg_open_key(uint64_t *handle_out, const ntb0_unicode_string *path)
{
    char hive_name[64];
    char key_path[1024];
    ntb0_hive *h;
    struct ntb0_reg_key *key;
    uint32_t cell;
    long rc;

    if (!handle_out)
        return NTB0_ST_INVALID_PARAMETER;

    if (ntb0_reg_split_path(path, hive_name, sizeof(hive_name), key_path,
                            sizeof(key_path)) < 0)
        return NTB0_ST_NOT_SUPPORTED;

    h = ntb0_reg_get_hive(hive_name);
    if (!h)
        return NTB0_ST_OBJECT_NAME_NOT_FOUND;

    cell = ntb0_hive_open_path(h, key_path);
    if (cell == 0xFFFFFFFFu) {
        NTB0_OK("SYS-049", "key '%s\\%s' not found", hive_name, key_path);
        return NTB0_ST_OBJECT_NAME_NOT_FOUND;
    }

    key = (struct ntb0_reg_key *)calloc(1, sizeof(*key));
    if (!key)
        return NTB0_ST_NO_MEMORY;

    key->hive = h;
    key->cell = cell;

    rc = ntb0_handle_create_priv(-1, key, ntb0_reg_key_free, NTB0_HK_REGKEY,
                                  handle_out);
    if (rc != NTB0_ST_SUCCESS) {
        free(key);
        return rc;
    }

    NTB0_OK("SYS-049", "registry key '%s\\%s' opened -> handle 0x%llx",
            hive_name, key_path, (unsigned long long)*handle_out);
    return NTB0_ST_SUCCESS;
}

static struct ntb0_reg_key *ntb0_reg_key_of(uint64_t handle)
{
    if (ntb0_handle_kind(handle) != NTB0_HK_REGKEY)
        return NULL;
    return (struct ntb0_reg_key *)ntb0_handle_priv(handle);
}

/* ------------------------------------------------------------------ */
/* NtQueryValueKey (SYS-050)                                            */
/* ------------------------------------------------------------------ */

long ntb0_reg_query_value(uint64_t handle, const ntb0_unicode_string *name,
                          uint32_t cls, void *buf, uint32_t len, uint32_t *ret)
{
    struct ntb0_reg_key *k = ntb0_reg_key_of(handle);
    char vname[512];
    uint32_t type = 0, size = 0;
    const uint8_t *data = NULL;
    unsigned char *out = (unsigned char *)buf;
    uint32_t name_bytes;
    uint32_t need;
    long rc;

    if (!k)
        return NTB0_ST_INVALID_HANDLE;
    if (!name || !name->buffer)
        return NTB0_ST_INVALID_PARAMETER;
    if (!out)
        return NTB0_ST_INVALID_PARAMETER;

    if (ntb0_reg_utf16_to_utf8(name->buffer, (size_t)name->length / 2, vname,
                               sizeof(vname)) < 0)
        return NTB0_ST_INVALID_PARAMETER;

    if (!ntb0_hive_find_value(k->hive, k->cell, vname, &type, &data, &size))
        return NTB0_ST_OBJECT_NAME_NOT_FOUND;

    name_bytes = name->length; /* بالبايت (UTF-16) */

    switch (cls) {
    case NTB0_KVI_BASIC:
        need = 0x0C + name_bytes;
        if (len < need) {
            if (ret)
                *ret = need;
            return NTB0_ST_STATUS_LOGICAL_ERROR;
        }
        memset(out, 0, need);
        *(uint32_t *)(out + 0x00) = 0; /* TitleIndex */
        *(uint32_t *)(out + 0x04) = type;
        *(uint32_t *)(out + 0x08) = name_bytes;
        memcpy(out + 0x0C, name->buffer, name_bytes);
        break;

    case NTB0_KVI_FULL:
        need = 0x14 + name_bytes + size;
        if (len < need) {
            if (ret)
                *ret = need;
            return NTB0_ST_STATUS_LOGICAL_ERROR;
        }
        memset(out, 0, need);
        *(uint32_t *)(out + 0x00) = 0;
        *(uint32_t *)(out + 0x04) = type;
        *(uint32_t *)(out + 0x08) = 0; /* Flags */
        *(uint32_t *)(out + 0x0C) = name_bytes;
        *(uint32_t *)(out + 0x10) = size;
        memcpy(out + 0x14, name->buffer, name_bytes);
        if (size)
            memcpy(out + 0x14 + name_bytes, data, size);
        break;

    case NTB0_KVI_PARTIAL:
    case NTB0_KVI_PARTIAL_ALIGN64:
        need = 0x0C + size;
        if (len < need) {
            if (ret)
                *ret = need;
            return NTB0_ST_STATUS_LOGICAL_ERROR;
        }
        memset(out, 0, need);
        *(uint32_t *)(out + 0x00) = 0;
        *(uint32_t *)(out + 0x04) = type;
        *(uint32_t *)(out + 0x08) = size;
        if (size)
            memcpy(out + 0x0C, data, size);
        break;

    default:
        return NTB0_ST_NOT_SUPPORTED;
    }

    if (ret)
        *ret = need;

    rc = NTB0_ST_SUCCESS;
    NTB0_OK("SYS-050", "value '%s' (type %u, %u bytes) returned", vname, type,
            size);
    return rc;
}

/* ------------------------------------------------------------------ */
/* NtEnumerateKey (SYS-051)                                             */
/* ------------------------------------------------------------------ */

long ntb0_reg_enum_key(uint64_t handle, uint32_t cls, uint32_t index, void *buf,
                       uint32_t len, uint32_t *ret)
{
    struct ntb0_reg_key *k = ntb0_reg_key_of(handle);
    unsigned char *out = (unsigned char *)buf;
    uint16_t nm[512];
    int nchars;
    int64_t ftime = 0;
    uint32_t name_bytes;
    uint32_t need;

    if (!k)
        return NTB0_ST_INVALID_HANDLE;
    if (!out)
        return NTB0_ST_INVALID_PARAMETER;

    if (index >= (uint32_t)ntb0_hive_key_subkey_count(k->hive, k->cell))
        return NTB0_ST_NO_MORE_FILES;

    nchars = ntb0_hive_key_subkey_utf16(k->hive, k->cell, (int)index, nm,
                                        sizeof(nm) / sizeof(nm[0]));
    if (nchars < 0)
        return NTB0_ST_OBJECT_NAME_NOT_FOUND;

    ntb0_hive_key_time(k->hive, k->cell, &ftime);
    name_bytes = (uint32_t)nchars * 2;

    switch (cls) {
    case NTB0_KEI_BASIC:
        need = 0x10 + name_bytes;
        if (len < need) {
            if (ret)
                *ret = need;
            return NTB0_ST_STATUS_LOGICAL_ERROR;
        }
        memset(out, 0, need);
        *(int64_t *)(out + 0x00) = ftime;
        *(uint32_t *)(out + 0x08) = index;
        *(uint32_t *)(out + 0x0C) = name_bytes;
        memcpy(out + 0x10, nm, name_bytes);
        break;

    case NTB0_KEI_FULL:
        need = 0x14 + name_bytes;
        if (len < need) {
            if (ret)
                *ret = need;
            return NTB0_ST_STATUS_LOGICAL_ERROR;
        }
        memset(out, 0, need);
        *(int64_t *)(out + 0x00) = ftime;
        *(uint32_t *)(out + 0x08) = index;
        *(uint32_t *)(out + 0x0C) = name_bytes;
        *(uint32_t *)(out + 0x10) = 0; /* ClassNameLength */
        memcpy(out + 0x14, nm, name_bytes);
        break;

    default:
        return NTB0_ST_NOT_SUPPORTED;
    }

    if (ret)
        *ret = need;

    NTB0_OK("SYS-051", "subkey #%u returned (%d chars)", index, nchars);
    return NTB0_ST_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* NtEnumerateValueKey (SYS-052)                                        */
/* ------------------------------------------------------------------ */

long ntb0_reg_enum_value(uint64_t handle, uint32_t cls, uint32_t index,
                         void *buf, uint32_t len, uint32_t *ret)
{
    struct ntb0_reg_key *k = ntb0_reg_key_of(handle);
    unsigned char *out = (unsigned char *)buf;
    char vname[512];
    uint16_t nm16[512];
    uint32_t type = 0, size = 0, name_bytes;
    const uint8_t *data = NULL;
    uint32_t need;
    int nchars;

    if (!k)
        return NTB0_ST_INVALID_HANDLE;
    if (!out)
        return NTB0_ST_INVALID_PARAMETER;

    if (index >= (uint32_t)ntb0_hive_key_value_count(k->hive, k->cell))
        return NTB0_ST_NO_MORE_FILES;

    if (ntb0_hive_key_value(k->hive, k->cell, (int)index, vname, sizeof(vname),
                            &type, &data, &size) != 0)
        return NTB0_ST_OBJECT_NAME_NOT_FOUND;

    nchars = ntb0_reg_utf8_to_utf16(vname, nm16, sizeof(nm16) / sizeof(nm16[0]));
    if (nchars < 0)
        return NTB0_ST_OBJECT_NAME_NOT_FOUND;
    name_bytes = (uint32_t)nchars * 2;

    switch (cls) {
    case NTB0_KVI_BASIC:
        need = 0x0C + name_bytes;
        if (len < need) {
            if (ret)
                *ret = need;
            return NTB0_ST_STATUS_LOGICAL_ERROR;
        }
        memset(out, 0, need);
        *(uint32_t *)(out + 0x00) = index;
        *(uint32_t *)(out + 0x04) = type;
        *(uint32_t *)(out + 0x08) = name_bytes;
        memcpy(out + 0x0C, nm16, name_bytes);
        break;

    case NTB0_KVI_FULL:
        need = 0x14 + name_bytes + size;
        if (len < need) {
            if (ret)
                *ret = need;
            return NTB0_ST_STATUS_LOGICAL_ERROR;
        }
        memset(out, 0, need);
        *(uint32_t *)(out + 0x00) = index;
        *(uint32_t *)(out + 0x04) = type;
        *(uint32_t *)(out + 0x08) = 0;
        *(uint32_t *)(out + 0x0C) = name_bytes;
        *(uint32_t *)(out + 0x10) = size;
        memcpy(out + 0x14, nm16, name_bytes);
        if (size)
            memcpy(out + 0x14 + name_bytes, data, size);
        break;

    case NTB0_KVI_PARTIAL:
    case NTB0_KVI_PARTIAL_ALIGN64:
        need = 0x0C + size;
        if (len < need) {
            if (ret)
                *ret = need;
            return NTB0_ST_STATUS_LOGICAL_ERROR;
        }
        memset(out, 0, need);
        *(uint32_t *)(out + 0x00) = index;
        *(uint32_t *)(out + 0x04) = type;
        *(uint32_t *)(out + 0x08) = size;
        if (size)
            memcpy(out + 0x0C, data, size);
        break;

    default:
        return NTB0_ST_NOT_SUPPORTED;
    }

    if (ret)
        *ret = need;

    NTB0_OK("SYS-052", "value #%u ('%s', type %u, %u bytes) returned", index,
            vname, type, size);
    return NTB0_ST_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* NtFlushKey (SYS-053)                                                 */
/* ------------------------------------------------------------------ */

long ntb0_reg_flush(uint64_t handle)
{
    struct ntb0_reg_key *k = ntb0_reg_key_of(handle);

    if (!k)
        return NTB0_ST_INVALID_HANDLE;

    /* الخلية للقراءة فقط ⇒ لا شيء يُكتب */
    NTB0_OK("SYS-053", "key flushed (read-only hive — nothing to write)");
    return NTB0_ST_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* T-107 — RegLoadAppKey (REG-002)                                       */
/* تُحمَّل خلية عشوائية (ملف regf) كمفتاح مستقل — انظر ntb0_hive_load_app.  */
/* ------------------------------------------------------------------ */

/* نماذج دوال hive.c الجديدة (hive.h خارج ملكية هذه الملفات) */
extern ntb0_hive *ntb0_hive_load_app(const char *path);
extern int ntb0_hive_set_value(ntb0_hive *h, uint32_t key, const char *name,
                               uint32_t type, const void *data, uint32_t size);
extern int ntb0_hive_delete_value(ntb0_hive *h, uint32_t key, const char *name);
extern int ntb0_hive_set_link(ntb0_hive *h, uint32_t parent, const char *name,
                              const char *target);
extern int ntb0_hive_key_symlink_target(ntb0_hive *h, uint32_t key, char *buf,
                                        size_t len);
extern int ntb0_hive_overlay_stats(ntb0_hive *h, uint32_t *values,
                                   uint32_t *links, uint32_t *writes);

#define NTB0_REG_MAX_APP_HIVES 8

struct ntb0_reg_app_slot {
    char      path[1024];
    ntb0_hive *hive;
};

static struct ntb0_reg_app_slot ntb0_reg_app_slots[NTB0_REG_MAX_APP_HIVES];
static pthread_mutex_t ntb0_reg_app_lock = PTHREAD_MUTEX_INITIALIZER;

static ntb0_hive *ntb0_reg_app_hive(const char *path)
{
    int i, free_i = -1;
    ntb0_hive *h = NULL;

    if (!path || !*path)
        return NULL;

    pthread_mutex_lock(&ntb0_reg_app_lock);

    for (i = 0; i < NTB0_REG_MAX_APP_HIVES; i++) {
        if (ntb0_reg_app_slots[i].hive &&
            strcmp(ntb0_reg_app_slots[i].path, path) == 0) {
            /* الخلية محمّلة سابقاً بنفس المسار ⇒ إعادة استخدامها
             * (في NT: RegLoadAppKey يفشل لو الخلية محمّلة مرتين). */
            h = ntb0_reg_app_slots[i].hive;
            break;
        }
        if (!ntb0_reg_app_slots[i].hive && free_i < 0)
            free_i = i;
    }

    pthread_mutex_unlock(&ntb0_reg_app_lock);

    if (h)
        return h;

    /* خارج القفل: الفتح بطيء (يقرأ الملف كاملاً) */
    h = ntb0_hive_load_app(path); /* مصباح REG-002 */
    if (!h)
        return NULL;

    pthread_mutex_lock(&ntb0_reg_app_lock);
    if (free_i >= 0) {
        snprintf(ntb0_reg_app_slots[free_i].path,
                 sizeof(ntb0_reg_app_slots[free_i].path), "%s", path);
        ntb0_reg_app_slots[free_i].hive = h;
    }
    pthread_mutex_unlock(&ntb0_reg_app_lock);

    if (free_i < 0) {
        /* الجدول ممتلئ — لا نُطرد خلية قد تكون مقابض مفتوحة تشير إليها */
        NTB0_FAIL("REG-002", "app hive table full (%d loaded)",
                  NTB0_REG_MAX_APP_HIVES);
        ntb0_hive_close(h);
        return NULL;
    }

    return h;
}

/* RegLoadAppKey: يفتح خلية عمومية ويعيد مقبض مفتاحها الجذر */
long ntb0_reg_load_app_key(uint64_t *handle_out, const char *path)
{
    ntb0_hive *h;
    struct ntb0_reg_key *key;
    long rc;

    if (!handle_out || !path || !*path)
        return NTB0_ST_INVALID_PARAMETER;
    *handle_out = 0;

    h = ntb0_reg_app_hive(path);
    if (!h)
        return NTB0_ST_OBJECT_NAME_NOT_FOUND;

    key = (struct ntb0_reg_key *)calloc(1, sizeof(*key));
    if (!key)
        return NTB0_ST_NO_MEMORY;

    key->hive = h;
    key->cell = ntb0_hive_root(h);

    rc = ntb0_handle_create_priv(-1, key, ntb0_reg_key_free, NTB0_HK_REGKEY,
                                 handle_out);
    if (rc != NTB0_ST_SUCCESS) {
        free(key);
        return rc;
    }

    NTB0_OK("REG-002", "RegLoadAppKey '%s' -> handle 0x%llx (root key)", path,
            (unsigned long long)*handle_out);
    return NTB0_ST_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* T-109 — NtSetValueKey (REG-004)                                       */
/* طبقة كتابة في الذاكرة فوق قارئ الخلية. **الحدّ الصادق**: كل ما يُكتب  */
/* يفنى مع إغلاق الخلية — NtFlushKey لن يكتبه على القرص أبداً.            */
/* ------------------------------------------------------------------ */

long ntb0_reg_set_value(uint64_t handle, const ntb0_unicode_string *name,
                        uint32_t type, const void *data, uint32_t size)
{
    struct ntb0_reg_key *k = ntb0_reg_key_of(handle);
    char vname[512];

    if (!k)
        return NTB0_ST_INVALID_HANDLE;
    if (!name || (size > 0 && !data))
        return NTB0_ST_INVALID_PARAMETER;

    if (ntb0_reg_utf16_to_utf8(name->buffer, (size_t)name->length / 2, vname,
                               sizeof(vname)) < 0)
        return NTB0_ST_INVALID_PARAMETER;

    if (ntb0_hive_set_value(k->hive, k->cell, vname, type, data, size) != 0)
        return NTB0_ST_ACCESS_DENIED;

    NTB0_OK("REG-004", "NtSetValueKey '%s' (type %u, %u bytes) accepted — "
                       "overlay only, lost when the hive is closed", vname, type,
            size);
    return NTB0_ST_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* T-108 — مفتاح رمزي REG_LINK (REG-003)                                 */
/* ينشئ رابطاً تحت المفتاح؛ لاحقاً أي فتح بالاسم يتبع الهدف انطلاقاً      */
/* من الجذر (ntb0_hive_open_key → ntb0_hive_link_resolve).               */
/* ------------------------------------------------------------------ */

long ntb0_reg_set_link(uint64_t handle, const ntb0_unicode_string *name,
                       const ntb0_unicode_string *target)
{
    struct ntb0_reg_key *k = ntb0_reg_key_of(handle);
    char lname[512], ltarget[1024];

    if (!k)
        return NTB0_ST_INVALID_HANDLE;
    if (!name || !target)
        return NTB0_ST_INVALID_PARAMETER;

    if (ntb0_reg_utf16_to_utf8(name->buffer, (size_t)name->length / 2, lname,
                               sizeof(lname)) < 0)
        return NTB0_ST_INVALID_PARAMETER;
    if (ntb0_reg_utf16_to_utf8(target->buffer, (size_t)target->length / 2,
                               ltarget, sizeof(ltarget)) < 0)
        return NTB0_ST_INVALID_PARAMETER;

    if (ntb0_hive_set_link(k->hive, k->cell, lname, ltarget) != 0)
        return NTB0_ST_ACCESS_DENIED;

    NTB0_OK("REG-003", "REG_LINK symbolic key '%s' -> '%s' registered "
                       "(in-memory overlay)", lname, ltarget);
    return NTB0_ST_SUCCESS;
}
