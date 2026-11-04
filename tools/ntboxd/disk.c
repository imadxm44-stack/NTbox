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
#include <dirent.h>
#include <strings.h>
#include <libgen.h>

#include "disk.h"
#include "ntb0_trace.h"
#include "ntb0_nt.h"
#include "ntb0_handle.h"
#include "ntb0_path.h"

/* ------------------------------------------------------------------ */
/* جدول الملفات المفتوحة (path ⇒ fd, refcount)                       */
/* ------------------------------------------------------------------ */

#define DISK_OF_MAX 256

struct disk_open_file {
    int  used;
    char path[NTB0_PATH_MAX];
    int  fd;        /* fd مخزَّن (open file description) — never handed out */
    int  refcount;  /* عدد المقابض (handles) التي تشاركه */
};

static struct disk_open_file disk_of[DISK_OF_MAX];

static int disk_of_find(const char *path)
{
    int i;

    for (i = 0; i < DISK_OF_MAX; i++) {
        if (disk_of[i].used && strcmp(disk_of[i].path, path) == 0)
            return i;
    }
    return -1;
}

static void disk_of_release(const char *path)
{
    int idx = disk_of_find(path);

    if (idx < 0)
        return;
    if (disk_of[idx].refcount > 0)
        disk_of[idx].refcount--;
    if (disk_of[idx].refcount <= 0) {
        if (disk_of[idx].fd >= 0)
            close(disk_of[idx].fd);
        disk_of[idx].fd = -1;
        disk_of[idx].used = 0;
        disk_of[idx].path[0] = '\0';
    }
}

static void disk_of_rename(const char *oldp, const char *newp)
{
    int idx = disk_of_find(oldp);

    if (idx < 0)
        return;
    {
        size_t ln = strlen(newp);
        if (ln >= sizeof(disk_of[idx].path))
            ln = sizeof(disk_of[idx].path) - 1;
        memcpy(disk_of[idx].path, newp, ln);
        disk_of[idx].path[ln] = '\0';
    }
}

size_t ntb0_disk_open_file_count(void)
{
    size_t n = 0;
    int i;

    for (i = 0; i < DISK_OF_MAX; i++) {
        if (disk_of[i].used && disk_of[i].refcount > 0)
            n++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* قياس النسخ (T-249)                                                  */
/* ------------------------------------------------------------------ */

/*
 * كل بايت ينسخه الصندوق بنفسه نيابةً عن نداء القرص يُحصى هنا.
 * مسار قراءة الملف (NtReadFile ⇒ read(fd)) لا يمرّ هنا أبداً:
 * البيانات تنتقل مباشرة من النواة إلى مخزن المستخدم (نسخة واحدة
 * kernel→user). المسار الوحيد الذي ينسخ عبر الصندوق اليوم هو
 * ntb0_disk_query_directory (مدخلات المجلد — ميتاداتا).
 */
static uint64_t disk_copy_bytes;

uint64_t ntb0_disk_copy_bytes(void)
{
    return disk_copy_bytes;
}

/* ------------------------------------------------------------------ */
/* فحص أولي للمسار NT (ASCII فقط — نفس ترتيب ntb0_path.c) — T-246      */
/* ------------------------------------------------------------------ */

/* مطابقة بادئة ASCII بدون حساسية حالة ⇒ تُقدّم *pos عند النجاح */
static int ntb0_disk_wide_prefix_ci(const uint16_t *w, size_t n, size_t *pos,
                                    const char *prefix)
{
    size_t p = *pos, i = 0;

    while (prefix[i]) {
        uint16_t c1, c2;

        if (p >= n)
            return 0;
        c1 = w[p];
        c2 = (uint16_t)(unsigned char)prefix[i];
        if (c1 >= 'a' && c1 <= 'z')
            c1 = (uint16_t)(c1 - 'a' + 'A');
        if (c2 >= 'a' && c2 <= 'z')
            c2 = (uint16_t)(c2 - 'a' + 'A');
        if (c1 != c2)
            return 0;
        p++;
        i++;
    }
    *pos = p;
    return 1;
}

static int ntb0_disk_is_sep(uint16_t c)
{
    return c == '\\' || c == '/';
}

/*
 * يعيد NTB0_ST_SUCCESS ويضبط *is_root=1 إذا كان المسار يدل على جذر
 * القرص نفسه (\??\C: أو \??\C:\ بمختلف أشكاله — T-246). يعيد رمز
 * خطأ صادقاً (UNC ⇒ NOT_SUPPORTED) عند رفض هنا.
 */
static long ntb0_disk_ntpath_preflight(const ntb0_unicode_string *us,
                                       int *is_root)
{
    const uint16_t *w;
    size_t n, pos = 0;

    *is_root = 0;
    if (!us || !us->buffer || us->length < 2)
        return NTB0_ST_SUCCESS;
    w = us->buffer;
    n = (size_t)us->length / 2;

    while (ntb0_disk_wide_prefix_ci(w, n, &pos, "\\??\\"))
        ;
    ntb0_disk_wide_prefix_ci(w, n, &pos, "\\DosDevices\\");

    if (ntb0_disk_wide_prefix_ci(w, n, &pos, "UNC\\")) {
        NTB0_OK("DISK-021", "\\??\\UNC\\ refused honestly (no network root)");
        return NTB0_ST_NOT_SUPPORTED;
    }

    if (ntb0_disk_wide_prefix_ci(w, n, &pos, "\\Device\\")) {
        if (ntb0_disk_wide_prefix_ci(w, n, &pos, "HarddiskVolume")) {
            size_t q;

            while (pos < n && w[pos] >= '0' && w[pos] <= '9')
                pos++;
            if (pos < n && ntb0_disk_is_sep(w[pos]))
                pos++;
            if (pos + 1 < n && w[pos + 1] == ':')
                return NTB0_ST_SUCCESS; /* حرف قرص خلف الجهاز ⇒ طبقة المسار */
            q = pos;
            while (q < n && ntb0_disk_is_sep(w[q]))
                q++;
            if (q >= n)
                *is_root = 1; /* \Device\HarddiskVolumeN (وحده) ⇒ جذر */
            return NTB0_ST_SUCCESS;
        }
        if (ntb0_disk_wide_prefix_ci(w, n, &pos, "Mup\\") ||
            ntb0_disk_wide_prefix_ci(w, n, &pos, "LanmanRedirector\\")) {
            NTB0_OK("DISK-021", "\\Device\\ UNC prefix refused honestly");
            return NTB0_ST_NOT_SUPPORTED;
        }
        return NTB0_ST_SUCCESS;
    }

    /* حرف القرص: بعد C: يجب ألا يبقى إلا فواصل (أو لا شيء) ⇒ جذر */
    if (pos + 1 < n && w[pos + 1] == ':') {
        uint16_t drv = w[pos];

        if (drv >= 'a' && drv <= 'z')
            drv = (uint16_t)(drv - 'a' + 'A');
        if (drv != 'C')
            return NTB0_ST_SUCCESS; /* قرص آخر ⇒ DEVICE_NOT_CONNECTED في طبقة المسار */
        pos += 2;
        while (pos < n && ntb0_disk_is_sep(w[pos]))
            pos++;
        if (pos >= n)
            *is_root = 1;
    }

    return NTB0_ST_SUCCESS;
}

/* ------------------------------------------------------------------ */

/*
 * acquire: يُعيد fd جديد (dup) يشارك نفس open file description عند
 * وجود إدخال. عند bypass (CREATE/SUPERSEDE/…) يُلغى الإدخال القديم.
 */
static int disk_of_acquire(const char *rel, int flags, mode_t mode, int bypass,
                           int *out_fd)
{
    int i, fd, nfd;

    if (!rel || !out_fd)
        return -1;

    /* bypass: ألغِ أي إدخال قديم (truncate/create) */
    if (bypass) {
        int idx = disk_of_find(rel);

        if (idx >= 0) {
            if (disk_of[idx].fd >= 0)
                close(disk_of[idx].fd);
            disk_of[idx].used = 0;
            disk_of[idx].path[0] = '\0';
            disk_of[idx].fd = -1;
            disk_of[idx].refcount = 0;
        }
    } else {
        int idx = disk_of_find(rel);

        if (idx >= 0 && disk_of[idx].refcount > 0 && disk_of[idx].fd >= 0) {
            nfd = dup(disk_of[idx].fd);
            if (nfd < 0)
                return -1;
            disk_of[idx].refcount++;
            *out_fd = nfd;
            return 0;
        }
    }

    fd = openat(ntb0_path_dirfd(), rel, flags, mode);
    if (fd < 0)
        return -1;

    for (i = 0; i < DISK_OF_MAX; i++) {
        if (!disk_of[i].used)
            break;
    }
    if (i >= DISK_OF_MAX) {
        close(fd);
        return -1;
    }

    disk_of[i].used = 1;
    strncpy(disk_of[i].path, rel, sizeof(disk_of[i].path) - 1);
    disk_of[i].path[sizeof(disk_of[i].path) - 1] = '\0';
    disk_of[i].fd = fd;
    disk_of[i].refcount = 1;

    nfd = dup(fd);
    if (nfd < 0) {
        close(fd);
        disk_of[i].used = 0;
        disk_of[i].path[0] = '\0';
        disk_of[i].fd = -1;
        disk_of[i].refcount = 0;
        return -1;
    }
    *out_fd = nfd;
    return 0;
}

/* ------------------------------------------------------------------ */
/* API                                                                */
/* ------------------------------------------------------------------ */

int ntb0_disk_init(const char *root)
{
    int i;

    for (i = 0; i < DISK_OF_MAX; i++) {
        disk_of[i].used = 0;
        disk_of[i].path[0] = '\0';
        disk_of[i].fd = -1;
        disk_of[i].refcount = 0;
    }

    if (root && *root) {
        if (ntb0_path_set_root(root) != 0)
            return -1;
    } else {
        if (ntb0_path_ensure_root() != NTB0_ST_SUCCESS)
            return -1;
    }
    if (ntb0_path_dirfd() < 0)
        return -1;
    return 0;
}

long ntb0_disk_create_file(const ntb0_unicode_string *us,
                           uint32_t desired, uint32_t share,
                           uint32_t disposition, uint32_t create_options,
                           uint64_t *handle_out)
{
    char rel[NTB0_PATH_MAX];
    long rc;
    int flags = O_CLOEXEC;
    int bypass = 0;
    int is_dir = (create_options & NTB0_FILE_DIRECTORY_FILE) != 0;
    int non_dir = (create_options & NTB0_FILE_NON_DIRECTORY_FILE) != 0;
    int is_root = 0;
    int fd, rootfd;
    struct stat st;

    if (!us || !handle_out)
        return NTB0_ST_INVALID_PARAMETER;

    if (ntb0_path_dirfd() < 0) {
        if (ntb0_disk_init(NULL) != 0)
            return NTB0_ST_OBJECT_PATH_NOT_FOUND;
    }

    /* T-246: جذر القرص / UNC — يُلتقط قبل طبقة المسار */
    {
        long pre = ntb0_disk_ntpath_preflight(us, &is_root);

        if (pre != NTB0_ST_SUCCESS)
            return pre;
        if (is_root) {
            if (disposition != NTB0_FILE_OPEN)
                return NTB0_ST_INVALID_PARAMETER; /* لا إنشاء لنواة الحجم */
            if (non_dir)
                return NTB0_ST_FILE_IS_A_DIRECTORY;
            is_dir = 1;
            disposition = NTB0_FILE_OPEN;
        }
    }

    if (is_root) {
        /* \??\C:\ ⇒ مفتاح جذر يفتح المجلد "." داخل dirfd */
        strcpy(rel, ".");
        NTB0_OK("DISK-020", "volume root opened: \\??\\C:\\ ⇒ .");
        rc = NTB0_ST_SUCCESS;
    } else {
        rc = ntb0_path_from_nt(us, rel, sizeof(rel));
        if (rc != NTB0_ST_SUCCESS)
            return rc;
    }

    rootfd = ntb0_path_dirfd();
    if (rootfd < 0)
        return NTB0_ST_OBJECT_PATH_NOT_FOUND;

    /* DesiredAccess ⇒ read/write flags */
    {
        int want_write = desired & (NTB0_FILE_WRITE_DATA | NTB0_FILE_APPEND_DATA |
                                    NTB0_FILE_WRITE_EA | NTB0_FILE_WRITE_ATTRIBUTES |
                                    NTB0_GENERIC_WRITE);
        int want_read  = desired & (NTB0_FILE_READ_DATA | NTB0_FILE_READ_EA |
                                    NTB0_FILE_READ_ATTRIBUTES | NTB0_GENERIC_READ);

        if (want_write && want_read)
            flags |= O_RDWR;
        else if (want_write)
            flags |= O_WRONLY;
        else
            flags |= O_RDONLY;
    }

    switch (disposition) {
        case NTB0_FILE_SUPERSEDE:
            flags |= O_CREAT | O_TRUNC;
            bypass = 1;
            break;
        case NTB0_FILE_OPEN:
            break;
        case NTB0_FILE_CREATE:
            flags |= O_CREAT | O_EXCL;
            bypass = 1;
            break;
        case NTB0_FILE_OPEN_IF:
            flags |= O_CREAT;
            break;
        case NTB0_FILE_OVERWRITE:
            flags |= O_TRUNC;
            bypass = 1;
            break;
        case NTB0_FILE_OVERWRITE_IF:
            flags |= O_CREAT | O_TRUNC;
            bypass = 1;
            break;
        default:
            return NTB0_ST_INVALID_PARAMETER;
    }

    if (is_dir) {
        /* المجلدات: O_RDONLY|O_DIRECTORY حصراً — قياساً على هذه النواة:
         * O_RDWR على مجلد = EINVAL، و open(O_CREAT|O_DIRECTORY) على مسار
         * غائب = EINVAL (والمجلد لا يُنشأ بـ open بل بـ mkdirat). */
        int create_dir = (flags & O_CREAT) != 0;

        flags = O_RDONLY | O_CLOEXEC | O_DIRECTORY;
        if (create_dir) {
            if (fstatat(rootfd, rel, &st, AT_SYMLINK_NOFOLLOW) < 0) {
                if (mkdirat(rootfd, rel, 0755) != 0 && errno != EEXIST) {
                    /* افتح بالأسفل حتى يُصنَّف الخطأ (ENOENT ⇒ NAME_NOT_FOUND) */
                }
            } else if (disposition == NTB0_FILE_CREATE) {
                /* إنشاء مجلد موجود ⇒ متراكب (قاعدة NT) */
                return NTB0_ST_OBJECT_NAME_COLLISION;
            }
        }
    }
    if (create_options & NTB0_FILE_NO_INTERMEDIATE_BUFFERING) {
        flags |= O_DIRECT;
        bypass = 1;
    }
    if (create_options & NTB0_FILE_SYNCHRONOUS_IO_NONALERT)
        flags |= O_SYNC;
    if (create_options & NTB0_FILE_WRITE_THROUGH)
        flags |= O_SYNC;

    /* محاولة open — فشلها يُصنَّف صادقاً (errno ⇒ NT status) */
    if (disk_of_acquire(rel, flags, 0644, bypass, &fd) < 0) {
        {
            int e = errno;

            if (e == ENOENT)
                return NTB0_ST_OBJECT_NAME_NOT_FOUND;
            if (e == EEXIST)
                return NTB0_ST_OBJECT_NAME_COLLISION;
            if (e == ENOTDIR)
                return NTB0_ST_OBJECT_PATH_NOT_FOUND;
            if (e == EISDIR)
                return NTB0_ST_FILE_IS_A_DIRECTORY;
            if (e == ENAMETOOLONG)
                return NTB0_ST_OBJECT_PATH_SYNTAX_BAD;
            if (e == EINVAL && (flags & O_DIRECT)) {
                /* T-245: نظام ملفات لا يدعم O_DIRECT ⇒ رفض صادق */
                NTB0_OK("DISK-024", "O_DIRECT honestly refused by fs (EINVAL)");
                return NTB0_ST_NOT_SUPPORTED;
            }
            return NTB0_ST_UNSUCCESSFUL;
        }
    }

    if (fstat(fd, &st) < 0) {
        close(fd);
        disk_of_release(rel);
        return NTB0_ST_UNSUCCESSFUL;
    }

    if (is_dir && !S_ISDIR(st.st_mode)) {
        close(fd);
        disk_of_release(rel);
        return NTB0_ST_NOT_A_DIRECTORY;
    }
    if (non_dir && S_ISDIR(st.st_mode)) {
        close(fd);
        disk_of_release(rel);
        return NTB0_ST_FILE_IS_A_DIRECTORY;
    }

    /* FILE_OPEN (موجود) على nonexistent handled by open failure above. */

    if (flags & O_DIRECT)
        posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM);

    {
        uint64_t h = 0;
        long hr = ntb0_handle_create(fd, desired, share, rel, &h);

        if (hr != NTB0_ST_SUCCESS) {
            disk_of_release(rel);
            return hr;
        }
        *handle_out = h;
    }

    return NTB0_ST_SUCCESS;
}

long ntb0_disk_close(uint64_t handle)
{
    const char *p = ntb0_handle_path(handle);
    char path[NTB0_PATH_MAX];

    if (p) {
        strncpy(path, p, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    } else {
        path[0] = '\0';
    }

    long rc = ntb0_handle_close(handle);
    if (path[0])
        disk_of_release(path);
    return rc;
}

/* دالة `dir`: مقارنة بالاسم، بدون حساسية حالة الحروف (T-242)؛
 * عند التعادل (A/a) تحدّد حساسية الحالة كما تفعل NT. */
static int disk_entry_cmp(const void *pa, const void *pb)
{
    const ntb0_disk_dir_entry *a = pa;
    const ntb0_disk_dir_entry *b = pb;
    int c = strcasecmp(a->name, b->name);

    if (!c)
        c = strcmp(a->name, b->name);
    return c;
}

/* صفات الملف كما يعرضها `dir` (T-242):
 *   مجلد ⇒ DIRECTORY · ملف عادي ⇒ ARCHIVE · أرشيف بلا صلاحية كتابة
 *   ⇒ READONLY · رمزي ⇒ REPARSE_POINT · اسم يبدأ بنقطة ⇒ HIDDEN.
 * FILE_ATTRIBUTE_NORMAL لا يُمزج مع أي صفة أخرى (قاعدة NT). */
static uint32_t disk_attrs_from_mode(mode_t m, const char *name)
{
    uint32_t a = 0;

    if (S_ISDIR(m))
        a |= NTB0_FILE_ATTRIBUTE_DIRECTORY;
    else
        a |= NTB0_FILE_ATTRIBUTE_ARCHIVE;
    if (S_ISLNK(m))
        a |= NTB0_FILE_ATTRIBUTE_REPARSE_POINT;
    if (!(m & S_IWUSR))
        a |= NTB0_FILE_ATTRIBUTE_READONLY;
    if (name[0] == '.')
        a |= NTB0_FILE_ATTRIBUTE_HIDDEN;
    return a;
}

long ntb0_disk_query_directory(uint64_t dir_handle,
                               ntb0_disk_dir_entry *entries,
                               size_t max_entries, size_t *count_out)
{
    int fd, nfd;
    DIR *d;
    struct dirent *de;
    ntb0_disk_dir_entry *all = NULL;
    size_t cap = 64, n = 0, i, copied;

    if (count_out)
        *count_out = 0;
    if (!entries || max_entries == 0)
        return NTB0_ST_INVALID_PARAMETER;

    fd = ntb0_handle_fd(dir_handle);
    if (fd < 0)
        return NTB0_ST_INVALID_HANDLE;

    nfd = dup(fd);
    if (nfd < 0)
        return NTB0_ST_UNSUCCESSFUL;

    d = fdopendir(nfd);
    if (!d) {
        close(nfd);
        return NTB0_ST_UNSUCCESSFUL;
    }

    /* المرحلة 1 — تجميع كل المدخلات (قراءة المجلد كاملاً قبل الفرز) */
    all = malloc(cap * sizeof(*all));
    if (!all) {
        closedir(d);
        return NTB0_ST_NO_MEMORY;
    }

    while ((de = readdir(d)) != NULL) {
        struct stat st;
        ntb0_disk_dir_entry *e;
        ntb0_disk_dir_entry *tmp;
        size_t nl;

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        if (fstatat(fd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
            continue;

        if (n == cap) {
            tmp = realloc(all, cap * 2 * sizeof(*all));
            if (!tmp)
                break; /* نُفرز ما جمعناه */
            all = tmp;
            cap *= 2;
        }

        e = &all[n++];
        nl = strlen(de->d_name);
        if (nl >= sizeof(e->name))
            nl = sizeof(e->name) - 1;
        memset(e, 0, sizeof(*e));
        memcpy(e->name, de->d_name, nl);
        e->name[nl] = '\0';
        /* `dir` يعرض المجلدات بحجم 0 (<DIR>) */
        e->size = S_ISDIR(st.st_mode) ? 0u : (uint64_t)st.st_size;
        e->attributes = disk_attrs_from_mode(st.st_mode, e->name);
    }

    closedir(d);

    /* المرحلة 2 — فرز بأسلوب `dir` ثم النسخ إلى المخزن المطلوب.
     * هذه النسخة من الميتاداتا تُحصى في قياس النسخ (T-249). */
    qsort(all, n, sizeof(*all), disk_entry_cmp);

    copied = (max_entries < n) ? max_entries : n;
    for (i = 0; i < copied; i++) {
        entries[i] = all[i];
        disk_copy_bytes += sizeof(entries[i]);
    }
    free(all);
    if (count_out)
        *count_out = copied;
    return NTB0_ST_SUCCESS;
}

long ntb0_disk_rename(uint64_t handle, const char *new_name)
{
    const char *old = ntb0_handle_path(handle);
    char oldbuf[NTB0_PATH_MAX];
    char newpath[NTB0_PATH_MAX];

    if (!old || !new_name || !*new_name)
        return NTB0_ST_INVALID_PARAMETER;
    if (strlen(new_name) > 255)
        return NTB0_ST_INVALID_PARAMETER;
    if (strcmp(old, ".") == 0) {
        /* T-246: لا إعادة تسمية لنواة الحجم (\??\C:\) */
        NTB0_OK("DISK-022", "rename of volume root refused honestly");
        return NTB0_ST_ACCESS_DENIED;
    }

    strncpy(oldbuf, old, sizeof(oldbuf) - 1);
    oldbuf[sizeof(oldbuf) - 1] = '\0';

    {
        char *slash = strrchr(oldbuf, '/');

        if (slash) {
            *slash = '\0';
            {
                size_t _ol = strlen(oldbuf);
                size_t _nl = strlen(new_name);
                if (_ol + 1 + _nl + 1 > sizeof(newpath))
                    return NTB0_ST_OBJECT_PATH_SYNTAX_BAD;
                memcpy(newpath, oldbuf, _ol);
                newpath[_ol] = '/';
                memcpy(newpath + _ol + 1, new_name, _nl);
                newpath[_ol + 1 + _nl] = '\0';
            }
        } else {
            if (strlen(new_name) + 1 > sizeof(newpath))
                return NTB0_ST_OBJECT_PATH_SYNTAX_BAD;
            snprintf(newpath, sizeof(newpath), "%s", new_name);
        }
    }

    if (renameat(ntb0_path_dirfd(), old, ntb0_path_dirfd(), newpath) != 0) {
        if (errno == ENOENT)
            return NTB0_ST_OBJECT_NAME_NOT_FOUND;
        return NTB0_ST_UNSUCCESSFUL;
    }

    ntb0_handle_set_path(handle, newpath);
    disk_of_rename(old, newpath);
    return NTB0_ST_SUCCESS;
}

long ntb0_disk_set_disposition(uint64_t handle, int delete_on_close)
{
    int fd;
    const char *path;
    struct stat st;

    if (!delete_on_close)
        return NTB0_ST_SUCCESS;

    fd = ntb0_handle_fd(handle);
    path = ntb0_handle_path(handle);
    if (fd < 0 || !path)
        return NTB0_ST_INVALID_HANDLE;

    if (strcmp(path, ".") == 0) {
        /* T-246: لا حذف لنواة الحجم (\??\C:\) */
        NTB0_OK("DISK-023", "delete of volume root refused honestly");
        return NTB0_ST_ACCESS_DENIED;
    }

    if (fstat(fd, &st) < 0)
        return NTB0_ST_UNSUCCESSFUL;

    if (S_ISDIR(st.st_mode)) {
        if (unlinkat(ntb0_path_dirfd(), path, AT_REMOVEDIR) != 0) {
            if (errno == ENOENT)
                return NTB0_ST_OBJECT_NAME_NOT_FOUND;
            return NTB0_ST_UNSUCCESSFUL;
        }
    } else {
        if (unlinkat(ntb0_path_dirfd(), path, 0) != 0) {
            if (errno == ENOENT)
                return NTB0_ST_OBJECT_NAME_NOT_FOUND;
            return NTB0_ST_UNSUCCESSFUL;
        }
    }
    return NTB0_ST_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* مجسّ O_DIRECT / posix_fadvise (T-245)                               */
/* ------------------------------------------------------------------ */

/*
 * يفتح ملفاً مؤقتاً بـ O_DIRECT على جذر القرص، يكتب 4096 بايت بمخزن
 * متاحٍ (محاذات 4096 — شرط O_DIRECT)، يستدعي posix_fadvise(DONTNEED)،
 * ثم يقرأ عكسيّاً ويتحقّق من المحتوى بايتًا بايتًا. ملف المصباح المحتمل
 * .ntbox_odirect_probe.tmp يُمسح دائماً.
 */
long ntb0_disk_odirect_probe(ntb0_disk_odirect_report *rep)
{
    static const char probe_name[] = ".ntbox_odirect_probe.tmp";
    unsigned char *wbuf = NULL, *rbuf = NULL;
    int fd, i, fa;
    ssize_t wr, rd;

    if (!rep)
        return NTB0_ST_INVALID_PARAMETER;
    memset(rep, 0, sizeof(*rep));

    if (ntb0_path_dirfd() < 0) {
        if (ntb0_disk_init(NULL) != 0)
            return NTB0_ST_OBJECT_PATH_NOT_FOUND;
    }

    fd = openat(ntb0_path_dirfd(), probe_name,
                O_DIRECT | O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        int e = errno;

        if (e == EINVAL || e == EOPNOTSUPP || e == ENOTSUP) {
            /* رفض صادق: نظام الملفات لا يدعم O_DIRECT — مقاس وموثّق */
            rep->refused = 1;
            rep->refused_errno = e;
            NTB0_OK("DISK-026",
                    "O_DIRECT probe: honest refusal by fs (errno=%d %s)",
                    e, strerror(e));
            unlinkat(ntb0_path_dirfd(), probe_name, 0);
            return NTB0_ST_SUCCESS;
        }
        NTB0_FAIL("DISK-026", "O_DIRECT probe open failed: %s",
                  strerror(e));
        return NTB0_ST_UNSUCCESSFUL;
    }

    if (posix_memalign((void **)&wbuf, 4096, 4096) != 0 ||
        posix_memalign((void **)&rbuf, 4096, 4096) != 0) {
        free(wbuf);
        free(rbuf);
        close(fd);
        unlinkat(ntb0_path_dirfd(), probe_name, 0);
        return NTB0_ST_NO_MEMORY;
    }

    for (i = 0; i < 4096; i++)
        wbuf[i] = (unsigned char)(i * 131 + 7);

    fa = -1;
    rd = -1;
    wr = write(fd, wbuf, 4096);
    if (wr == 4096) {
        fa = posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
        rep->fadvise_ok = (fa == 0);
        if (lseek(fd, 0, SEEK_SET) == 0)
            rd = read(fd, rbuf, 4096);
    }

    if (wr == 4096 && rd == 4096 && memcmp(wbuf, rbuf, 4096) == 0) {
        rep->direct_ok = 1;
        rep->probe_bytes = 4096;
        NTB0_OK("DISK-025",
                "O_DIRECT probe: write+readback of 4096 bytes verified "
                "(posix_fadvise DONTNEED rc=%d)", fa);
    } else {
        NTB0_FAIL("DISK-026",
                  "O_DIRECT probe data path failed (wr=%zd rd=%zd)", wr, rd);
        free(wbuf);
        free(rbuf);
        close(fd);
        unlinkat(ntb0_path_dirfd(), probe_name, 0);
        return NTB0_ST_UNSUCCESSFUL;
    }

    free(wbuf);
    free(rbuf);
    close(fd);
    unlinkat(ntb0_path_dirfd(), probe_name, 0);
    return NTB0_ST_SUCCESS;
}