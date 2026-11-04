/*
 * ntb0_handle.c — جدول المقابض (HandleTable) في المترجم.
 *
 * انظر ntb0_handle.h للسبب والمخطط. باختصار:
 *   المقبض = 0x4E54_gggg_iiiiiiii  (سحر + جيل + رقم الخانة)
 *   الجدول = مصفوفة ثابتة NTB0_HANDLE_MAX خانة + قفل خيوط.
 *
 * وضع المشاركة (T-024) — قواعد NT:
 *   فتح جديد مسموح iff:
 *     (1) وضع مشاركة القائم يسمح بالوصول المطلوب من الجديد
 *     (2) وضع مشاركة الجديد يسمح بالوصول الذي grabbedه القائم
 *   "الوصول" = بتات read/write/delete من DesiredAccess.
 *   الهوية = (dev, ino) من fstat — أدقّ من مقارنة المسار.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <dirent.h>

#include "ntb0_handle.h"
#include "ntb0_nt.h"
#include "ntb0_trace.h"

struct ntb0_handle_entry {
    int      used;
    uint32_t generation;
    int      kind;      /* NTB0_HK_FILE / NTB0_HK_EVENT */
    int      fd;
    uint32_t desired;
    uint32_t share;
    dev_t    dev;
    ino_t    ino;
    int      is_dir;
    uint64_t size;      /* حجم المقطع (للأقسام) */
    char    *path;
    void    *dir;        /* DIR* لتعداد المجلد (NtQueryDirectoryFile) */
    void    *priv;       /* حالة خاصة (خيط/عملية/مفتاح ريجستري) */
    void   (*priv_free)(void *); /* محرّر الـ priv عند إغلاق المقبض */
};

static struct ntb0_handle_entry ntb0_handles[NTB0_HANDLE_MAX];
static pthread_mutex_t          ntb0_handle_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * وضع "الفشل المتوقّع" (للاختبار فقط): يحوّل مصباح FAIL المتوقّع
 * (مثل تعارض المشاركة) إلى OK حتى لا يُحسب خطأً في تقرير الحالة.
 */
static int ntb0_h_expect_fail;

void ntb0_handle_set_expect_fail(int on)
{
    ntb0_h_expect_fail = on;
}

static void ntb0_h_fail(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    if (ntb0_h_expect_fail) {
        fprintf(stderr, "[ntbox][SYS-007][OK] ");
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, " (expected failure)\n");
    } else {
        fprintf(stderr, "[ntbox][SYS-007][FAIL] ");
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
    }
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* أدوات                                                               */
/* ------------------------------------------------------------------ */

/*
 * تحويل DesiredAccess إلى بتات المشاركة المطلوبة:
 *   قراءة  → FILE_SHARE_READ
 *   كتابة  → FILE_SHARE_WRITE
 *   حذف    → FILE_SHARE_DELETE
 */
static uint32_t ntb0_access_to_share_req(uint32_t desired)
{
    uint32_t r = 0;

    if (desired & (NTB0_FILE_READ_DATA | NTB0_FILE_READ_EA |
                   NTB0_FILE_READ_ATTRIBUTES | NTB0_GENERIC_READ))
        r |= NTB0_FILE_SHARE_READ;

    if (desired & (NTB0_FILE_WRITE_DATA | NTB0_FILE_APPEND_DATA |
                   NTB0_FILE_WRITE_EA | NTB0_FILE_WRITE_ATTRIBUTES |
                   NTB0_GENERIC_WRITE))
        r |= NTB0_FILE_SHARE_WRITE;

    if (desired & (NTB0_FILE_DELETE_CHILD | NTB0_DELETE))
        r |= NTB0_FILE_SHARE_DELETE;

    return r;
}

static uint64_t ntb0_handle_encode(uint32_t idx, uint32_t gen)
{
    return ((uint64_t)NTB0_HANDLE_MAGIC << 48) |
           ((uint64_t)(gen & 0x00FFFFFFu) << 24) |
           (uint64_t)(idx + 1u);
}

/* يُعيد 0 عند النجاح، -1 إن لم يكن الرقم شكل مقبض من جدولنا */
static int ntb0_handle_decode(uint64_t h, uint32_t *idx, uint32_t *gen)
{
    uint32_t i;

    if ((h >> 48) != NTB0_HANDLE_MAGIC)
        return -1;

    i = (uint32_t)(h & 0x00FFFFFFull);
    if (i == 0 || i > NTB0_HANDLE_MAX)
        return -1;

    *idx = i - 1u;
    *gen = (uint32_t)((h >> 24) & 0x00FFFFFFull);
    return 0;
}

/* يُعيد الخانة المطابقة (تحت القفل) أو NULL */
static struct ntb0_handle_entry *ntb0_handle_find_locked(uint64_t h)
{
    uint32_t idx, gen;

    if (ntb0_handle_decode(h, &idx, &gen) < 0)
        return NULL;
    if (!ntb0_handles[idx].used)
        return NULL;
    if (ntb0_handles[idx].generation != gen)
        return NULL;
    return &ntb0_handles[idx];
}

int ntb0_handle_looks_valid(uint64_t handle)
{
    int ok;

    pthread_mutex_lock(&ntb0_handle_lock);
    ok = ntb0_handle_find_locked(handle) != NULL;
    pthread_mutex_unlock(&ntb0_handle_lock);
    return ok;
}

/* ------------------------------------------------------------------ */
/* فحص المشاركة (T-024)                                                */
/* ------------------------------------------------------------------ */

long ntb0_handle_share_check(int fd, uint32_t desired, uint32_t share)
{
    struct stat st;
    uint32_t req;
    long rc = NTB0_ST_SUCCESS;
    int i;

    if (fd < 0)
        return NTB0_ST_INVALID_HANDLE;

    if (fstat(fd, &st) < 0)
        return NTB0_ST_UNSUCCESSFUL;

    req = ntb0_access_to_share_req(desired);

    pthread_mutex_lock(&ntb0_handle_lock);

    for (i = 0; i < NTB0_HANDLE_MAX && rc == NTB0_ST_SUCCESS; i++) {
        struct ntb0_handle_entry *e = &ntb0_handles[i];

        if (!e->used)
            continue;
        if (e->dev != st.st_dev || e->ino != st.st_ino)
            continue;

        /* (1) مشاركة القائم يجب أن تسمح بما نطلبه */
        if (req & ~e->share) {
            ntb0_h_fail("share conflict: existing share=0x%x does not allow "
                        "requested access 0x%x (path=%s)",
                        e->share, req, e->path ? e->path : "?");
            rc = NTB0_ST_SHARING_VIOLATION;
            break;
        }

        /* (2) مشاركتنا يجب أن تسمح بما grabbedه القائم */
        if (ntb0_access_to_share_req(e->desired) & ~share) {
            ntb0_h_fail("share conflict: our share=0x%x does not allow the "
                        "existing access 0x%x (path=%s)",
                        share, ntb0_access_to_share_req(e->desired),
                        e->path ? e->path : "?");
            rc = NTB0_ST_SHARING_VIOLATION;
            break;
        }
    }

    pthread_mutex_unlock(&ntb0_handle_lock);

    if (rc == NTB0_ST_SUCCESS)
        NTB0_OK("SYS-007", "share ok (desired=0x%x share=0x%x)", desired,
                share);
    return rc;
}

/* ------------------------------------------------------------------ */
/* إنشاء / إغلاق / نسخ                                                 */
/* ------------------------------------------------------------------ */

long ntb0_handle_create(int fd, uint32_t desired, uint32_t share,
                        const char *path, uint64_t *handle_out)
{
    struct ntb0_handle_entry *e = NULL;
    struct stat st;
    long rc;
    uint32_t idx;
    int i;

    if (!handle_out || fd < 0)
        return NTB0_ST_INVALID_PARAMETER;

    /* فحص المشاركة أولاً — على الـ fd المفتوح فعلاً */
    rc = ntb0_handle_share_check(fd, desired, share);
    if (rc != NTB0_ST_SUCCESS) {
        close(fd); /* الملكية تنتقل إلينا — لا يتسرّب */
        return rc;
    }

    if (fstat(fd, &st) < 0) {
        close(fd);
        return NTB0_ST_UNSUCCESSFUL;
    }

    pthread_mutex_lock(&ntb0_handle_lock);

    for (i = 0; i < NTB0_HANDLE_MAX; i++) {
        if (!ntb0_handles[i].used) {
            e = &ntb0_handles[i];
            break;
        }
    }

    if (!e) {
        pthread_mutex_unlock(&ntb0_handle_lock);
        close(fd);
        NTB0_FAIL("SYS-007", "handle table full (%d)", NTB0_HANDLE_MAX);
        return NTB0_ST_INSUFFICIENT_RESOURCES;
    }

    /* جيل جديد ⇒ كل المقابض القديمة لنفس الخانة تصبح غير صالحة */
    e->generation = (e->generation + 1u) & 0x00FFFFFFu;
    if (e->generation == 0)
        e->generation = 1;

    e->used    = 1;
    e->kind    = NTB0_HK_FILE;
    e->fd      = fd;
    e->desired = desired;
    e->share   = share;
    e->dev     = st.st_dev;
    e->ino     = st.st_ino;
    e->is_dir  = S_ISDIR(st.st_mode) ? 1 : 0;
    e->size    = 0;
    e->path    = path ? strdup(path) : NULL;
    e->dir     = NULL;
    e->priv    = NULL;

    idx = (uint32_t)(e - ntb0_handles);
    *handle_out = ntb0_handle_encode(idx, e->generation);

    pthread_mutex_unlock(&ntb0_handle_lock);

    NTB0_OK("SYS-007", "handle 0x%llx -> fd %d (path=%s)",
            (unsigned long long)*handle_out, fd, path ? path : "?");
    return NTB0_ST_SUCCESS;
}

long ntb0_handle_close(uint64_t handle)
{
    struct ntb0_handle_entry *e;
    int fd;

    pthread_mutex_lock(&ntb0_handle_lock);

    e = ntb0_handle_find_locked(handle);
    if (!e) {
        pthread_mutex_unlock(&ntb0_handle_lock);
        ntb0_h_fail("close of invalid handle 0x%llx",
                    (unsigned long long)handle);
        return NTB0_ST_INVALID_HANDLE;
    }

    fd = e->fd;
    if (e->dir) {
        closedir((DIR *)e->dir);
        e->dir = NULL;
    }
    free(e->path);
    e->path = NULL;
    if (e->priv_free && e->priv) {
        void (*freefn)(void *) = e->priv_free;
        void *priv = e->priv;

        e->priv_free = NULL;
        e->priv = NULL;
        pthread_mutex_unlock(&ntb0_handle_lock);
        freefn(priv);
        pthread_mutex_lock(&ntb0_handle_lock);
        e = ntb0_handle_find_locked(handle);
        if (!e) {
            pthread_mutex_unlock(&ntb0_handle_lock);
            return NTB0_ST_SUCCESS;
        }
    }
    e->priv = NULL;
    e->used = 0;
    e->kind = NTB0_HK_FILE;
    e->fd = -1;
    e->desired = 0;
    e->share = 0;

    pthread_mutex_unlock(&ntb0_handle_lock);

    if (fd >= 0)
        close(fd);

    NTB0_OK("SYS-007", "handle 0x%llx closed (fd=%d)",
            (unsigned long long)handle, fd);
    return NTB0_ST_SUCCESS;
}

long ntb0_handle_duplicate(uint64_t src, uint32_t desired, uint64_t *handle_out)
{
    struct ntb0_handle_entry *e;
    uint32_t want_desired;
    uint32_t want_share;
    char *path_copy = NULL;
    int nfd;
    long rc;

    if (!handle_out)
        return NTB0_ST_INVALID_PARAMETER;

    pthread_mutex_lock(&ntb0_handle_lock);

    e = ntb0_handle_find_locked(src);
    if (!e) {
        pthread_mutex_unlock(&ntb0_handle_lock);
        ntb0_h_fail("duplicate of invalid handle 0x%llx",
                    (unsigned long long)src);
        return NTB0_ST_INVALID_HANDLE;
    }

    want_desired = desired ? desired : e->desired;
    want_share   = e->share;
    if (e->path)
        path_copy = strdup(e->path);

    nfd = dup(e->fd);
    if (nfd < 0) {
        int err = errno;
        free(path_copy);
        pthread_mutex_unlock(&ntb0_handle_lock);
        ntb0_h_fail("dup failed: %s", strerror(err));
        return (err == EBADF) ? NTB0_ST_INVALID_HANDLE : NTB0_ST_UNSUCCESSFUL;
    }

    pthread_mutex_unlock(&ntb0_handle_lock);

    /* النسخة تشارك نفس الملف ⇒ لا فحص مشاركة (كما في DuplicateHandle) */
    rc = ntb0_handle_create(nfd, want_desired, want_share, path_copy,
                            handle_out);
    free(path_copy);
    if (rc != NTB0_ST_SUCCESS)
        return rc;

    NTB0_OK("SYS-007", "duplicated handle 0x%llx -> 0x%llx",
            (unsigned long long)src, (unsigned long long)*handle_out);
    return NTB0_ST_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* الأحداث (T-037)                                                      */
/* ------------------------------------------------------------------ */

long ntb0_handle_create_event(int eventfd, uint64_t *handle_out)
{
    struct ntb0_handle_entry *e = NULL;
    uint32_t idx;
    int i;

    if (!handle_out || eventfd < 0)
        return NTB0_ST_INVALID_PARAMETER;

    pthread_mutex_lock(&ntb0_handle_lock);

    for (i = 0; i < NTB0_HANDLE_MAX; i++) {
        if (!ntb0_handles[i].used) {
            e = &ntb0_handles[i];
            break;
        }
    }

    if (!e) {
        pthread_mutex_unlock(&ntb0_handle_lock);
        NTB0_FAIL("SYS-007", "handle table full (%d)", NTB0_HANDLE_MAX);
        return NTB0_ST_INSUFFICIENT_RESOURCES;
    }

    e->generation = (e->generation + 1u) & 0x00FFFFFFu;
    if (e->generation == 0)
        e->generation = 1;

    e->used    = 1;
    e->kind    = NTB0_HK_EVENT;
    e->size    = 0;
    e->dir     = NULL;
    e->priv    = NULL;
    e->fd      = eventfd;
    e->desired = 0;
    e->share   = 0;
    e->dev     = 0;
    e->ino     = 0;
    e->is_dir  = 0;
    e->size    = 0;
    e->path    = NULL;

    idx = (uint32_t)(e - ntb0_handles);
    *handle_out = ntb0_handle_encode(idx, e->generation);

    pthread_mutex_unlock(&ntb0_handle_lock);

    NTB0_OK("SYS-007", "event handle 0x%llx -> eventfd %d",
            (unsigned long long)*handle_out, eventfd);
    return NTB0_ST_SUCCESS;
}

long ntb0_handle_create_section(int fd, uint64_t size, uint64_t *handle_out)
{
    struct ntb0_handle_entry *e = NULL;
    uint32_t idx;
    int i;

    if (!handle_out || fd < 0)
        return NTB0_ST_INVALID_PARAMETER;

    pthread_mutex_lock(&ntb0_handle_lock);

    for (i = 0; i < NTB0_HANDLE_MAX; i++) {
        if (!ntb0_handles[i].used) {
            e = &ntb0_handles[i];
            break;
        }
    }

    if (!e) {
        pthread_mutex_unlock(&ntb0_handle_lock);
        NTB0_FAIL("SYS-007", "handle table full (%d)", NTB0_HANDLE_MAX);
        return NTB0_ST_INSUFFICIENT_RESOURCES;
    }

    e->generation = (e->generation + 1u) & 0x00FFFFFFu;
    if (e->generation == 0)
        e->generation = 1;

    e->used    = 1;
    e->kind    = NTB0_HK_SECTION;
    e->fd      = fd;
    e->desired = 0;
    e->share   = 0;
    e->dev     = 0;
    e->ino     = 0;
    e->is_dir  = 0;
    e->size    = size;
    e->path    = NULL;
    e->dir     = NULL;
    e->priv    = NULL;

    idx = (uint32_t)(e - ntb0_handles);
    *handle_out = ntb0_handle_encode(idx, e->generation);

    pthread_mutex_unlock(&ntb0_handle_lock);

    NTB0_OK("SYS-007", "section handle 0x%llx -> fd %d (size 0x%llx)",
            (unsigned long long)*handle_out, fd, (unsigned long long)size);
    return NTB0_ST_SUCCESS;
}

/*
 * إنشاء مقبض خيط/عملية — نفس نمط مقبض المقطع (بلا فحص مشاركة).
 */
static long ntb0_handle_create_kind(int fd, void *priv, int kind,
                                    uint64_t *handle_out)
{
    struct ntb0_handle_entry *e = NULL;
    uint32_t idx;
    int i;

    /* fd = -1 مسموح (مفتاح ريجستري: لا fd خلفه) */
    if (!handle_out || fd < -1)
        return NTB0_ST_INVALID_PARAMETER;

    pthread_mutex_lock(&ntb0_handle_lock);

    for (i = 0; i < NTB0_HANDLE_MAX; i++) {
        if (!ntb0_handles[i].used) {
            e = &ntb0_handles[i];
            break;
        }
    }

    if (!e) {
        pthread_mutex_unlock(&ntb0_handle_lock);
        NTB0_FAIL("SYS-007", "handle table full (%d)", NTB0_HANDLE_MAX);
        return NTB0_ST_INSUFFICIENT_RESOURCES;
    }

    e->generation = (e->generation + 1u) & 0x00FFFFFFu;
    if (e->generation == 0)
        e->generation = 1;

    e->used    = 1;
    e->kind    = kind;
    e->fd      = fd;
    e->desired = 0;
    e->share   = 0;
    e->dev     = 0;
    e->ino     = 0;
    e->is_dir  = 0;
    e->size    = 0;
    e->path    = NULL;
    e->dir     = NULL;
    e->priv    = priv;
    e->priv_free = NULL;

    idx = (uint32_t)(e - ntb0_handles);
    *handle_out = ntb0_handle_encode(idx, e->generation);

    pthread_mutex_unlock(&ntb0_handle_lock);

    NTB0_OK("SYS-007", "handle 0x%llx -> kind %d (fd %d)",
            (unsigned long long)*handle_out, kind, fd);
    return NTB0_ST_SUCCESS;
}

/*
 * إنشاء مقبض مع محرّر للـ priv (يُستدعى عند إغلاق المقبض).
 * يُستخدم لمفاتيح الريجستري (كائن لكل مقبض).
 */
long ntb0_handle_create_priv(int fd, void *priv, void (*priv_free)(void *),
                             int kind, uint64_t *handle_out)
{
    struct ntb0_handle_entry *e;
    long rc;
    int i;

    rc = ntb0_handle_create_kind(fd, priv, kind, handle_out);
    if (rc != NTB0_ST_SUCCESS)
        return rc;

    pthread_mutex_lock(&ntb0_handle_lock);
    for (i = 0; i < NTB0_HANDLE_MAX; i++) {
        e = &ntb0_handles[i];
        if (e->used && ntb0_handle_encode((uint32_t)i, e->generation) ==
                          *handle_out) {
            e->priv_free = priv_free;
            break;
        }
    }
    pthread_mutex_unlock(&ntb0_handle_lock);

    return NTB0_ST_SUCCESS;
}

long ntb0_handle_create_thread(int fd, void *priv, uint64_t *handle_out)
{
    return ntb0_handle_create_kind(fd, priv, NTB0_HK_THREAD, handle_out);
}

long ntb0_handle_create_process(int fd, void *priv, uint64_t *handle_out)
{
    return ntb0_handle_create_kind(fd, priv, NTB0_HK_PROCESS, handle_out);
}

void *ntb0_handle_priv(uint64_t handle)
{
    struct ntb0_handle_entry *e;
    void *p = NULL;

    pthread_mutex_lock(&ntb0_handle_lock);
    e = ntb0_handle_find_locked(handle);
    if (e)
        p = e->priv;
    pthread_mutex_unlock(&ntb0_handle_lock);
    return p;
}

uint64_t ntb0_handle_size(uint64_t handle)
{
    struct ntb0_handle_entry *e;
    uint64_t sz = 0;

    pthread_mutex_lock(&ntb0_handle_lock);
    e = ntb0_handle_find_locked(handle);
    if (e)
        sz = e->size;
    pthread_mutex_unlock(&ntb0_handle_lock);
    return sz;
}

int ntb0_handle_wait(uint64_t handle, int timeout_ms)
{
    struct pollfd pfd;
    int fd;

    fd = ntb0_handle_fd(handle);
    if (fd < 0)
        return -1;

    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    for (;;) {
        int r = poll(&pfd, 1, timeout_ms);

        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            return 1; /* انتهت المهلة */
        return 0;    /* إشارة */
    }
}

void *ntb0_handle_dir_stream(uint64_t handle)
{
    struct ntb0_handle_entry *e;
    void *dir = NULL;

    pthread_mutex_lock(&ntb0_handle_lock);
    e = ntb0_handle_find_locked(handle);
    if (e && e->kind == NTB0_HK_FILE && e->is_dir) {
        if (!e->dir) {
            int nfd = dup(e->fd);
            if (nfd >= 0) {
                DIR *d = fdopendir(nfd);
                if (d)
                    e->dir = d;
                else
                    close(nfd);
            }
        }
        dir = e->dir;
    }
    pthread_mutex_unlock(&ntb0_handle_lock);
    return dir;
}

int ntb0_handle_kind(uint64_t handle)
{
    struct ntb0_handle_entry *e;
    int kind = -1;

    pthread_mutex_lock(&ntb0_handle_lock);
    e = ntb0_handle_find_locked(handle);
    if (e)
        kind = e->kind;
    pthread_mutex_unlock(&ntb0_handle_lock);
    return kind;
}

/* ------------------------------------------------------------------ */
/* استعلامات                                                            */
/* ------------------------------------------------------------------ */

int ntb0_handle_fd(uint64_t handle)
{
    struct ntb0_handle_entry *e;
    int fd = -1;

    pthread_mutex_lock(&ntb0_handle_lock);
    e = ntb0_handle_find_locked(handle);
    if (e)
        fd = e->fd;
    pthread_mutex_unlock(&ntb0_handle_lock);
    return fd;
}

int ntb0_handle_dirfd(uint64_t handle)
{
    struct ntb0_handle_entry *e;
    int fd = -1;

    pthread_mutex_lock(&ntb0_handle_lock);
    e = ntb0_handle_find_locked(handle);
    if (e && e->is_dir)
        fd = e->fd;
    pthread_mutex_unlock(&ntb0_handle_lock);
    return fd;
}

int ntb0_handle_count(void)
{
    int n = 0, i;

    pthread_mutex_lock(&ntb0_handle_lock);
    for (i = 0; i < NTB0_HANDLE_MAX; i++)
        if (ntb0_handles[i].used)
            n++;
    pthread_mutex_unlock(&ntb0_handle_lock);
    return n;
}

const char *ntb0_handle_path(uint64_t handle)
{
    struct ntb0_handle_entry *e;
    const char *p = NULL;

    pthread_mutex_lock(&ntb0_handle_lock);
    e = ntb0_handle_find_locked(handle);
    if (e)
        p = e->path;
    pthread_mutex_unlock(&ntb0_handle_lock);
    return p;
}

int ntb0_handle_set_path(uint64_t handle, const char *path)
{
    struct ntb0_handle_entry *e;
    char *copy = NULL;
    int rc = -1;

    if (!path)
        return -1;

    copy = strdup(path);
    if (!copy)
        return -1;

    pthread_mutex_lock(&ntb0_handle_lock);
    e = ntb0_handle_find_locked(handle);
    if (e) {
        free(e->path);
        e->path = copy;
        copy = NULL;
        rc = 0;
    }
    pthread_mutex_unlock(&ntb0_handle_lock);

    free(copy);
    return rc;
}

uint32_t ntb0_handle_desired(uint64_t handle)
{
    struct ntb0_handle_entry *e;
    uint32_t d = 0;

    pthread_mutex_lock(&ntb0_handle_lock);
    e = ntb0_handle_find_locked(handle);
    if (e)
        d = e->desired;
    pthread_mutex_unlock(&ntb0_handle_lock);
    return d;
}

uint32_t ntb0_handle_share(uint64_t handle)
{
    struct ntb0_handle_entry *e;
    uint32_t s = 0;

    pthread_mutex_lock(&ntb0_handle_lock);
    e = ntb0_handle_find_locked(handle);
    if (e)
        s = e->share;
    pthread_mutex_unlock(&ntb0_handle_lock);
    return s;
}
