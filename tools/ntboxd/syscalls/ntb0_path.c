/*
 * ntb0_path.c — ترجمة مسار NT (ويندوز) إلى مسار لينكس.
 *
 * كل دالة هنا = ID + مصباح + اختبار (NTBOX.md §9).
 * المصابيح: PATH-001..PATH-006
 *   PATH-001 FAIL — تعذّر فتح جذر ويندوز
 *   PATH-002 OK   — تثبيت الجذر
 *   PATH-003 OK   — ترجمة المسار
 *   PATH-004 FAIL — UTF-16 غير صالح
 *   PATH-005 STUB — صيغة مسار غير مدعومة بعد (رجستري / UNC / قرص غير موصول)
 *   PATH-006 OK   — رفض مسار (حماية: لا هروب خارج جذر ويندوز)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "ntb0_path.h"
#include "ntb0_trace.h"

#include <strings.h>

static int   ntb0_root_fd = -1;
static char  ntb0_root_path[NTB0_PATH_MAX];

/* ------------------------------------------------------------------ */
/* أدوات نصّية                                                         */
/* ------------------------------------------------------------------ */

/* مقارنة بادئة بدون حساسية حالة الحروف (بادئات NT تُكتب بأحرف كبيرة عادة) */
static int ntb0_has_prefix_ci(const char *s, const char *prefix)
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
/* UTF-16LE -> UTF-8                                                   */
/* ------------------------------------------------------------------ */

/*
 * تحويل UTF-16LE (بوحدات UTF-16) إلى UTF-8.
 * يدعم أزواج البدائل (surrogate pairs).
 * يُعيد 0 عند النجاح، -1 عند خطأ (نص غير صالح أو لا مساحة).
 */
static int ntb0_utf16_to_utf8(const uint16_t *src, size_t n, char *out, size_t cap)
{
    size_t i = 0, o = 0;

    while (i < n) {
        uint32_t cp = src[i++];

        if (cp >= 0xD800 && cp <= 0xDBFF) {
            uint32_t lo;

            if (i >= n)
                return -1;
            lo = src[i++];
            if (lo < 0xDC00 || lo > 0xDFFF)
                return -1;
            cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return -1; /* بديل منخفض وحيد */
        }

        if (cp < 0x80) {
            if (o + 1 >= cap)
                return -1;
            out[o++] = (char)cp;
        } else if (cp < 0x800) {
            if (o + 2 >= cap)
                return -1;
            out[o++] = (char)(0xC0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            if (o + 3 >= cap)
                return -1;
            out[o++] = (char)(0xE0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            if (o + 4 >= cap)
                return -1;
            out[o++] = (char)(0xF0 | (cp >> 18));
            out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }

    if (o >= cap)
        return -1;
    out[o] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ */
/* الجذر                                                               */
/* ------------------------------------------------------------------ */

int ntb0_path_set_root(const char *root)
{
    int fd;

    if (!root || !*root) {
        NTB0_FAIL("PATH-001", "empty root");
        return -1;
    }

    fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        NTB0_FAIL("PATH-001", "open root '%s' failed: %s", root,
                  strerror(errno));
        return -1;
    }

    if (ntb0_root_fd >= 0)
        close(ntb0_root_fd);
    ntb0_root_fd = fd;
    strncpy(ntb0_root_path, root, sizeof(ntb0_root_path) - 1);
    ntb0_root_path[sizeof(ntb0_root_path) - 1] = '\0';

    NTB0_OK("PATH-002", "windows root = %s (fd=%d)", ntb0_root_path,
            ntb0_root_fd);
    return 0;
}

const char *ntb0_path_root(void)
{
    return ntb0_root_fd >= 0 ? ntb0_root_path : NULL;
}

int ntb0_path_dirfd(void)
{
    return ntb0_root_fd;
}

long ntb0_path_ensure_root(void)
{
    const char *r;

    if (ntb0_root_fd >= 0)
        return NTB0_ST_SUCCESS;

    r = getenv("NTBOX_ROOT");
    if (r && *r)
        return ntb0_path_set_root(r) < 0 ? NTB0_ST_OBJECT_PATH_NOT_FOUND
                                         : NTB0_ST_SUCCESS;

    /*
     * قائمة المرشحين — بالترتيب:
     *   1) /home/imadix/Downloads/Niga/win7root (المسار المطلوب)
     *   2) /tmp/opencode/win7root  (المسار الأصلي للمشروع)
     *   3) $HOME/win7root          (نسخة دائمة خارج /tmp — /tmp يُمسح عند
     *                               إعادة التشغيل ⇒ 테스트 "ويندوز حقيقي"
     *                               يفقد بيئته ⇒ hence the persistent copy)
     *   4) ./win7root              (نسخة داخل مجلد المشروع)
     */
    {
        char cand[NTB0_PATH_MAX];
        // 1) Current workspace win7root (or the dynamic path)
        if (ntb0_path_set_root("/home/imadix/Downloads/Niga/win7root") == 0)
            return NTB0_ST_SUCCESS;

        static const char *rel[] = { NTB0_DEFAULT_ROOT, NULL };
        const char *home = getenv("HOME");
        int i;

        for (i = 0; rel[i]; i++) {
            if (ntb0_path_set_root(rel[i]) == 0)
                return NTB0_ST_SUCCESS;
        }
        if (home && *home) {
            snprintf(cand, sizeof(cand), "%s/win7root", home);
            if (ntb0_path_set_root(cand) == 0)
                return NTB0_ST_SUCCESS;
        }
        if (ntb0_path_set_root("win7root") == 0)
            return NTB0_ST_SUCCESS;
    }

    return NTB0_ST_OBJECT_PATH_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/* الترجمة                                                            */
/* ------------------------------------------------------------------ */

long ntb0_path_from_nt(const ntb0_unicode_string *us, char *out, size_t out_len)
{
    size_t nchars, o = 0;
    char *w = NULL;
    const char *p;
    long rc = NTB0_ST_SUCCESS;

    if (!out || out_len == 0)
        return NTB0_ST_INVALID_PARAMETER;
    out[0] = '\0';

    if (!us || !us->buffer || us->length < 2)
        return NTB0_ST_OBJECT_NAME_NOT_FOUND;

    nchars = (size_t)us->length / 2;
    w = malloc(nchars * 4 + 1);
    if (!w) {
        NTB0_FAIL("PATH-003", "oom for NT path (%zu chars)", nchars);
        return NTB0_ST_NO_MEMORY;
    }

    if (ntb0_utf16_to_utf8(us->buffer, nchars, w, nchars * 4 + 1) < 0) {
        NTB0_FAIL("PATH-004", "invalid UTF-16 in NT path");
        free(w);
        return NTB0_ST_OBJECT_PATH_SYNTAX_BAD;
    }

    p = w;

    /* 1) بادئة \??\ (تظهر مرة أو مرتين: \??\??\C:\...) */
    while (ntb0_has_prefix_ci(p, "\\??\\"))
        p += 4;

    /* 2) بادئة \DosDevices\ */
    if (ntb0_has_prefix_ci(p, "\\DosDevices\\"))
        p += 12;

    /* 2b) \SystemRoot\ في سياق مسارات كائنات NT */
    if (ntb0_has_prefix_ci(p, "\\SystemRoot\\"))
        p += 12;

    /* 3) مسارات الريجستري — ليست ملفات (قارئ الريجستري = مرحلة 1) */
    if (ntb0_has_prefix_ci(p, "\\Registry\\")) {
        NTB0_STUB("PATH-005", "registry path not supported: %s", p);
        free(w);
        return NTB0_ST_NOT_SUPPORTED;
    }

    /* 4) \Device\... */
    if (ntb0_has_prefix_ci(p, "\\Device\\")) {
        const char *rest = p + 8;

        if (ntb0_has_prefix_ci(rest, "HarddiskVolume")) {
            const char *d = rest + 14; /* بعد "HarddiskVolume" */

            while (*d >= '0' && *d <= '9')
                d++;
            
            char* mapped_path = NULL;
            if (*d == '\\' || *d == '/') {
                /* Construct path starting with C: */
                size_t remaining_len = strlen(d + 1);
                mapped_path = malloc(4 + remaining_len + 1); /* "C:\" + path + null */
                if (!mapped_path) {
                    NTB0_FAIL("PATH-005", "oom for device path mapping");
                    free(w);
                    return NTB0_ST_NO_MEMORY;
                }
                snprintf(mapped_path, 4 + remaining_len + 1, "C:\\%s", d + 1);
            } else if (*d == '\0') {
                /* Path ends after volume number, e.g. \Device\HarddiskVolume1 */
                mapped_path = strdup("C:\\");
                if (!mapped_path) {
                    NTB0_FAIL("PATH-005", "oom for device path mapping");
                    free(w);
                    return NTB0_ST_NO_MEMORY;
                }
            } else {
                NTB0_STUB("PATH-005", "bad device path: %s", p);
                free(w);
                return NTB0_ST_OBJECT_PATH_SYNTAX_BAD;
            }
            /* Reassign w and p to the new mapped path */
            free(w); /* Free original buffer from malloc in ntb0_path_from_nt */
            w = mapped_path;
            p = w;

        } else if (ntb0_has_prefix_ci(rest, "Mup\\") ||
                   ntb0_has_prefix_ci(rest, "LanmanRedirector\\")) {
            NTB0_STUB("PATH-005", "UNC path not supported: %s", p);
            free(w);
            return NTB0_ST_NOT_SUPPORTED;
        } else {
            /* جهاز غير معروف — نعامله كمسار نسبي داخل الجذر */
            p = rest;
        }
    }

    /* 5) حرف القرص: C:\... → الجذر ؛ غير C → غير موصول */
    if (p[0] && p[1] == ':') {
        char drv = p[0];

        if (drv >= 'a' && drv <= 'z')
            drv = (char)(drv - 'a' + 'A');

        if (drv != 'C') {
            NTB0_STUB("PATH-005", "drive %c: not mounted", drv);
            free(w);
            return NTB0_ST_DEVICE_NOT_CONNECTED;
        }
        p += 2;
        if (*p == '\\' || *p == '/')
            p++;
    }

    /* 6) فواصل → '/' + رفض ".." */
    /* Normalize path separators */
    {
        char *q = (char *)p;
        (void)q;
        /* make a copy we can modify? p points into w, we can edit w */
    }
    while (*p == '\\' || *p == '/')
        p++;

    {
        const char *seg = p;

        while (*seg && rc == NTB0_ST_SUCCESS) {
            const char *e = seg;
            size_t len;

            while (*e && *e != '\\' && *e != '/')
                e++;
            len = (size_t)(e - seg);

            if (len == 2 && seg[0] == '.' && seg[1] == '.') {
                NTB0_OK("PATH-006", "path traversal refused: %s", w);
                rc = NTB0_ST_OBJECT_PATH_SYNTAX_BAD;
                break;
            }
            if (len == 1 && seg[0] == '.') {
                /* "." — نتجاهله */
            } else if (len) {
                if (o + len + 2 > out_len) {
                    rc = NTB0_ST_OBJECT_PATH_SYNTAX_BAD;
                    break;
                }
                if (o)
                    out[o++] = '/';
                memcpy(out + o, seg, len);
                o += len;
            }
            seg = (*e == '\\' || *e == '/') ? e + 1 : e;
        }
    }

    free(w);

    if (rc != NTB0_ST_SUCCESS) {
        out[0] = '\0';
        return rc;
    }
    if (o == 0)
        return NTB0_ST_OBJECT_NAME_NOT_FOUND;

    out[o] = '\0';
    NTB0_OK("PATH-003", "nt path -> linux path: %s", out);
    return NTB0_ST_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* مسار نسبي (RootDirectory — T-025)                                   */
/* ------------------------------------------------------------------ */

/*
 * المسار النسبي في NT يُفسَّر نسبةً إلى RootDirectory.
 * قواعد مُطبَّقة (من Win32، موثّقة في §9 من سجل التحقق):
 *   - لا يقبل مساراً مطلقاً (يبدأ بـ \ أو / أو له حرف قرص)
 *   - لا يقبل ".." (هروب خارج المجلد) — نفس قاعدة PATH-006
 *   - الفواصل \ تتحول إلى /
 */
long ntb0_path_rel_from_nt(const ntb0_unicode_string *us, char *out,
                            size_t out_len)
{
    size_t nchars, o = 0;
    char *w = NULL;
    const char *p;
    long rc = NTB0_ST_SUCCESS;

    if (!out || out_len == 0)
        return NTB0_ST_INVALID_PARAMETER;
    out[0] = '\0';

    if (!us || !us->buffer || us->length < 2)
        return NTB0_ST_OBJECT_NAME_NOT_FOUND;

    nchars = (size_t)us->length / 2;
    w = malloc(nchars * 4 + 1);
    if (!w) {
        NTB0_FAIL("PATH-003", "oom for NT relative path (%zu chars)", nchars);
        return NTB0_ST_NO_MEMORY;
    }

    if (ntb0_utf16_to_utf8(us->buffer, nchars, w, nchars * 4 + 1) < 0) {
        NTB0_FAIL("PATH-004", "invalid UTF-16 in NT relative path");
        free(w);
        return NTB0_ST_OBJECT_PATH_SYNTAX_BAD;
    }

    p = w;

    /* 1) لا يقبل مطلقاً */
    if (p[0] == '\\' || p[0] == '/') {
        NTB0_OK("PATH-007", "absolute path refused for a relative open: %s",
                w);
        free(w);
        return NTB0_ST_OBJECT_PATH_SYNTAX_BAD;
    }
    if (p[0] && p[1] == ':') {
        NTB0_OK("PATH-007", "drive letter refused for a relative open: %s",
                w);
        free(w);
        return NTB0_ST_OBJECT_PATH_SYNTAX_BAD;
    }

    /* 2) تحويل الفواصل + رفض ".." */
    {
        const char *seg = p;

        while (*seg && rc == NTB0_ST_SUCCESS) {
            const char *e = seg;
            size_t len;

            while (*e && *e != '\\' && *e != '/')
                e++;
            len = (size_t)(e - seg);

            if (len == 2 && seg[0] == '.' && seg[1] == '.') {
                NTB0_OK("PATH-006", "path traversal refused (relative): %s", w);
                rc = NTB0_ST_OBJECT_PATH_SYNTAX_BAD;
                break;
            }
            if (len == 1 && seg[0] == '.') {
                /* "." — نتجاهله */
            } else if (len) {
                if (o + len + 2 > out_len) {
                    rc = NTB0_ST_OBJECT_PATH_SYNTAX_BAD;
                    break;
                }
                if (o)
                    out[o++] = '/';
                memcpy(out + o, seg, len);
                o += len;
            }
            seg = (*e == '\\' || *e == '/') ? e + 1 : e;
        }
    }

    free(w);

    if (rc != NTB0_ST_SUCCESS) {
        out[0] = '\0';
        return rc;
    }
    if (o == 0)
        return NTB0_ST_OBJECT_NAME_NOT_FOUND;

    out[o] = '\0';
    NTB0_OK("PATH-007", "nt relative path -> linux relative path: %s", out);
    return NTB0_ST_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* الأنابيب المسمّاة (\Device\NamedPipe\...) — T-051                    */
/* ------------------------------------------------------------------ */

const char *ntb0_path_pipe_dir(void)
{
    const char *d = getenv("NTBOX_PIPE_DIR");

    if (d && *d)
        return d;
    return NTB0_PIPE_DIR_DEFAULT;
}

/* ينشئ مجلد الأنابيب (ومجلداته) إن لم يوجد */
static int ntb0_path_mkdir_p(const char *path)
{
    char tmp[NTB0_PATH_MAX];
    size_t len, i;

    len = strlen(path);
    if (len == 0 || len >= sizeof(tmp))
        return -1;
    memcpy(tmp, path, len + 1);

    for (i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
                return -1;
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

long ntb0_path_pipe_from_nt(const ntb0_unicode_string *us, char *out,
                            size_t out_len)
{
    size_t nchars, i, o = 0;
    char *w = NULL;
    const char *p;
    const char *dir;
    size_t dlen;

    if (!out || out_len == 0)
        return NTB0_ST_INVALID_PARAMETER;
    out[0] = '\0';

    if (!us || !us->buffer || us->length < 2)
        return NTB0_ST_OBJECT_NAME_NOT_FOUND;

    nchars = (size_t)us->length / 2;
    w = malloc(nchars * 4 + 1);
    if (!w) {
        NTB0_FAIL("PATH-008", "oom for NT pipe path (%zu chars)", nchars);
        return NTB0_ST_NO_MEMORY;
    }

    if (ntb0_utf16_to_utf8(us->buffer, nchars, w, nchars * 4 + 1) < 0) {
        NTB0_FAIL("PATH-004", "invalid UTF-16 in NT pipe path");
        free(w);
        return NTB0_ST_OBJECT_PATH_SYNTAX_BAD;
    }

    p = w;

    /* 1) البادئة: \Device\NamedPipe\  أو  \??\pipe\ */
    while (ntb0_has_prefix_ci(p, "\\??\\"))
        p += 4;
    if (ntb0_has_prefix_ci(p, "\\pipe\\"))
        p += 6;
    else if (ntb0_has_prefix_ci(p, "\\Device\\NamedPipe\\"))
        p += 18;
    else if (ntb0_has_prefix_ci(p, "\\Device\\"))
        p += 8; /* جهاز آخر ⇒ يُعامل كمجلد أنابيب */
    else {
        NTB0_OK("PATH-009", "not a named pipe path: %s", w);
        free(w);
        return NTB0_ST_OBJECT_PATH_SYNTAX_BAD;
    }

    /* 2) تنظيف الاسم: كل حرف خارج [A-Za-z0-9_.-] ⇒ '_' */
    while (*p == '\\' || *p == '/')
        p++;

    dir = ntb0_path_pipe_dir();
    dlen = strlen(dir);

    if (dlen + 2 + strlen(p) + 1 > out_len) {
        free(w);
        return NTB0_ST_OBJECT_PATH_SYNTAX_BAD;
    }

    if (ntb0_path_mkdir_p(dir) < 0) {
        NTB0_FAIL("PATH-008", "cannot create pipe dir '%s': %s", dir,
                  strerror(errno));
        free(w);
        return NTB0_ST_ACCESS_DENIED;
    }

    memcpy(out, dir, dlen);
    o = dlen;
    out[o++] = '/';

    for (i = 0; p[i] && p[i] != '\\' && p[i] != '/'; i++) {
        char c = p[i];

        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-'))
            c = '_';
        out[o++] = c;
    }

    if (o == dlen + 1) { /* اسم فارغ ⇒ مرفوض */
        NTB0_OK("PATH-009", "empty pipe name refused");
        free(w);
        out[0] = '\0';
        return NTB0_ST_OBJECT_NAME_NOT_FOUND;
    }

    out[o] = '\0';
    free(w);

    NTB0_OK("PATH-008", "nt pipe path -> fifo path: %s", out);
    return NTB0_ST_SUCCESS;
}
