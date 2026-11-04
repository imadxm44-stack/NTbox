#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <sys/types.h>

#include "ntb0_async_io.h"
#include "ntb0_handle.h"
#include "ntb0_nt.h"
#include "ntb0_trace.h"

static long as_errno_to_ntstatus(int e)
{
    switch (e) {
    case 0:            return NTB0_ST_SUCCESS;
    case EBADF:        return NTB0_ST_INVALID_HANDLE;
    case EACCES:
    case EPERM:        return NTB0_ST_ACCESS_DENIED;
    case ENOENT:       return NTB0_ST_OBJECT_NAME_NOT_FOUND;
    case EEXIST:       return NTB0_ST_OBJECT_NAME_COLLISION;
    case EISDIR:       return NTB0_ST_FILE_IS_A_DIRECTORY;
    case ENOTDIR:      return NTB0_ST_NOT_A_DIRECTORY;
    case ENAMETOOLONG:
    case ELOOP:        return NTB0_ST_OBJECT_PATH_SYNTAX_BAD;
    case ENOTEMPTY:    return NTB0_ST_STATUS_LOGICAL_ERROR;
    case ENOMEM:       return NTB0_ST_NO_MEMORY;
    case EINVAL:       return NTB0_ST_INVALID_PARAMETER;
    default:           return NTB0_ST_UNSUCCESSFUL;
    }
}

typedef enum { AS_PENDING = 0, AS_RUNNING, AS_DONE, AS_CANCELLED } as_state_t;

typedef struct as_node {
    uint64_t          handle;
    int               type;
    void             *buffer;
    size_t            length;
    uint64_t          event_handle;
    void             *iosb;          /* IO_STATUS_BLOCK (16بايت) أو NULL */
    int64_t           byte_offset;   /* -1 = fd موضع الحالي              */
    void             *apc;           /* IO_APC_ROUTINE (win64 ABI)       */
    uint64_t          apc_ctx;
    long              status;
    size_t            bytes_done;
    ntb0_async_cb     cb;
    void             *cb_ctx;
    volatile int      state;
    volatile int      cancelled;
    struct as_node   *next;
} as_node_t;

typedef struct as_apc {
    ntb0_async_apc_fn fn;
    void             *ctx;
    struct as_apc    *next;
} as_apc_t;

static as_node_t   *g_head = NULL;
static as_apc_t    *g_apc_head = NULL;
static as_apc_t    *g_apc_tail = NULL;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond = PTHREAD_COND_INITIALIZER;
static pthread_t       g_worker;
static int             g_worker_started = 0;
static int             g_stop = 0;

/* ملء IO_STATUS_BLOCK بذاكرة العميل — تخطيط Win64:
 *     [0..3]  Status     (union)
 *     [8..15] Information
 */
static void as_fill_iosb(void *iosb, long status, uint64_t information)
{
    if (!iosb)
        return;
    *(volatile int32_t *)iosb = (int32_t)status;
    *(volatile uint64_t *)(void *)((uintptr_t)iosb + 8) = information;
}

/* إشارة eventfd (يُستدعى من الـworker عند الإتمام بدلاً من poll) */
static void as_signal_event(uint64_t event_handle)
{
    if (event_handle != 0) {
        int efd = ntb0_handle_fd(event_handle);
        if (efd >= 0) {
            uint64_t one = 1;
            ssize_t _wr = write(efd, &one, sizeof(one));
            (void)_wr;
        }
    }
}

/* توصيل عنصر واحد مكتمل: cb القديم + APC عبر مساعد النداء win64.
 * التعبير (Win64):  rcx=ApcContext  rdx=IoStatusBlock  r8=0
 * ملاحظة: ntb0_call_win64 يعمل على هذا المكدس وتُنادى بالاصطلاح الحقيقي. */
static void as_deliver_node(as_node_t *n)
{
    if (n->cb)
        n->cb(n->handle, n->type, n->status, n->bytes_done, n->cb_ctx);
    if (n->apc) {
        (void)ntb0_call_win64(n->apc,
                              (long)(uintptr_t)n->apc_ctx,
                              (long)(uintptr_t)n->iosb,
                              0, 0, 0, 0, 0, 0, 0, 0, 0);
    }
}

static void *as_worker_main(void *arg)
{
    (void)arg;
    for (;;) {
        as_node_t *n = NULL;

        pthread_mutex_lock(&g_lock);
        while (!g_stop) {
            as_node_t *cur = g_head;
            while (cur) {
                if (cur->state == AS_PENDING) { n = cur; break; }
                cur = cur->next;
            }
            if (n) {
                n->state = AS_RUNNING;
                break;
            }
            pthread_cond_wait(&g_cond, &g_lock);
        }
        if (g_stop) {
            pthread_mutex_unlock(&g_lock);
            return NULL;
        }
        pthread_mutex_unlock(&g_lock);

        if (!n)
            continue;

        if (n->cancelled) {
            pthread_mutex_lock(&g_lock);
            n->status = NTB0_ST_CANCELLED;
            n->bytes_done = 0;
            n->state = AS_DONE;
            pthread_cond_broadcast(&g_cond);
            pthread_mutex_unlock(&g_lock);
            continue;
        }

        int fd = ntb0_handle_fd(n->handle);
        long status = NTB0_ST_SUCCESS;
        size_t done = 0;

        if (fd < 0) {
            status = NTB0_ST_INVALID_HANDLE;
        } else {
            int fl = fcntl(fd, F_GETFL);
            if (fl < 0) {
                status = NTB0_ST_INVALID_HANDLE;
            } else {
                int accmode = fl & O_ACCMODE;
                if (n->buffer == NULL && n->length > 0) {
                    status = NTB0_ST_INVALID_PARAMETER;
                } else if (n->type == NTB0_ASYNC_READ) {
                    if (accmode == O_WRONLY) status = NTB0_ST_ACCESS_DENIED;
                } else {
                    if (accmode == O_RDONLY) status = NTB0_ST_ACCESS_DENIED;
                }
            }
        }

        if (status == NTB0_ST_SUCCESS && n->length > 0) {
            int use_off = (n->byte_offset >= 0);
            if (n->type == NTB0_ASYNC_READ) {
                char *p = (char *)n->buffer;
                size_t rem = n->length;
                int err = 0;
                while (rem > 0) {
                    ssize_t r = use_off
                        ? pread(fd, p, rem, (off_t)(n->byte_offset + (int64_t)done))
                        : read(fd, p, rem);
                    if (r < 0) {
                        if (errno == EINTR) continue;
                        err = errno;
                        break;
                    }
                    if (r == 0) break;
                    p += r;
                    rem -= (size_t)r;
                    done += (size_t)r;
                }
                if (err) status = as_errno_to_ntstatus(err);
                else if (done == 0) status = NTB0_ST_END_OF_FILE;
                else status = NTB0_ST_SUCCESS;
            } else {
                const char *p = (const char *)n->buffer;
                size_t rem = n->length;
                int err = 0;
                while (rem > 0) {
                    ssize_t r = use_off
                        ? pwrite(fd, p, rem, (off_t)(n->byte_offset + (int64_t)done))
                        : write(fd, p, rem);
                    if (r < 0) {
                        if (errno == EINTR) continue;
                        err = errno;
                        break;
                    }
                    if (r == 0) { err = EIO; break; }
                    p += r;
                    rem -= (size_t)r;
                    done += (size_t)r;
                }
                if (err) status = as_errno_to_ntstatus(err);
                else status = NTB0_ST_SUCCESS;
            }
        }

        /* إتمام حقيقي: ملء IOSB + إشارة event ثم تعليم DONE (T-072/T-073).
         * العقدة الملغاة أثناء RUNNING تكتمل بحالة STATUS_CANCELLED مثل
         * ويندوز حيث يكون الـIRP مُلغى بعد بدء الشغل. */
        if (n->cancelled)
            status = NTB0_ST_CANCELLED;

        pthread_mutex_lock(&g_lock);
        n->status = status;
        n->bytes_done = (status == NTB0_ST_CANCELLED) ? 0 : done;
        as_fill_iosb(n->iosb, (long)n->status, (uint64_t)n->bytes_done);
        n->state = AS_DONE;
        pthread_cond_broadcast(&g_cond);
        pthread_mutex_unlock(&g_lock);
        as_signal_event(n->event_handle);
    }
}

static void as_ensure_worker(void)
{
    if (!g_worker_started) {
        int _rc = pthread_create(&g_worker, NULL, as_worker_main, NULL);
        if (_rc == 0)
            g_worker_started = 1;
    }
}

/* إدراج عقدة في رأس قائمة الإرسال (ترتيب القائمة: الأحدث أولاً). */
static long as_enqueue(as_node_t *n)
{
    pthread_mutex_lock(&g_lock);
    n->next = g_head;
    g_head = n;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
    return NTB0_ASYNC_PENDING;
}

static as_node_t *as_new_node(uint64_t handle, int type, const void *buf,
                              size_t len, uint64_t event_handle, void *iosb,
                              int64_t byte_offset, void *apc, uint64_t apc_ctx,
                              ntb0_async_cb cb, void *ctx)
{
    as_node_t *n = calloc(1, sizeof(*n));
    if (!n)
        return NULL;
    n->handle = handle;
    n->type = type;
    n->buffer = (void *)buf;
    n->length = len;
    n->event_handle = event_handle;
    n->iosb = iosb;
    n->byte_offset = byte_offset;
    n->apc = apc;
    n->apc_ctx = apc_ctx;
    n->cb = cb;
    n->cb_ctx = ctx;
    n->state = AS_PENDING;
    return n;
}

long ntb0_async_submit_read(uint64_t handle, void *buf, size_t len,
                            uint64_t event_handle, ntb0_async_cb cb,
                            void *ctx)
{
    return ntb0_async_submit_read_ex(handle, event_handle, buf, len, NULL, -1,
                                     NULL, 0, cb, ctx);
}

long ntb0_async_submit_write(uint64_t handle, const void *buf, size_t len,
                             uint64_t event_handle, ntb0_async_cb cb,
                             void *ctx)
{
    return ntb0_async_submit_write_ex(handle, event_handle, buf, len, NULL, -1,
                                      NULL, 0, cb, ctx);
}

long ntb0_async_submit_read_ex(uint64_t handle, uint64_t event_handle,
                               void *buf, size_t len, void *iosb,
                               int64_t byte_offset, void *apc_routine,
                               uint64_t apc_context, ntb0_async_cb cb, void *ctx)
{
    if (ntb0_handle_fd(handle) < 0)
        return NTB0_ST_INVALID_HANDLE;

    as_ensure_worker();

    as_node_t *n = as_new_node(handle, NTB0_ASYNC_READ, buf, len, event_handle,
                               iosb, byte_offset, apc_routine, apc_context,
                               cb, ctx);
    if (!n)
        return NTB0_ST_NO_MEMORY;

    return as_enqueue(n);
}

long ntb0_async_submit_write_ex(uint64_t handle, uint64_t event_handle,
                                const void *buf, size_t len, void *iosb,
                                int64_t byte_offset, void *apc_routine,
                                uint64_t apc_context, ntb0_async_cb cb, void *ctx)
{
    if (ntb0_handle_fd(handle) < 0)
        return NTB0_ST_INVALID_HANDLE;

    as_ensure_worker();

    as_node_t *n = as_new_node(handle, NTB0_ASYNC_WRITE, buf, len, event_handle,
                               iosb, byte_offset, apc_routine, apc_context,
                               cb, ctx);
    if (!n)
        return NTB0_ST_NO_MEMORY;

    return as_enqueue(n);
}

int ntb0_async_poll(void)
{
    as_node_t *completed = NULL;
    as_apc_t  *apcs = NULL;

    pthread_mutex_lock(&g_lock);
    as_node_t *prev = NULL;
    as_node_t *cur = g_head;
    while (cur) {
        as_node_t *next = cur->next;
        if (cur->state == AS_DONE || cur->state == AS_CANCELLED) {
            if (prev) prev->next = next;
            else g_head = next;
            cur->next = completed;
            completed = cur;
        } else {
            prev = cur;
        }
        cur = next;
    }
    apcs = g_apc_head;
    g_apc_head = NULL;
    g_apc_tail = NULL;
    pthread_mutex_unlock(&g_lock);

    int drained = 0;
    as_node_t *n = completed;
    while (n) {
        as_node_t *nx = n->next;
        /* إشارة event إن كان لم يُنبَّه من الـworker (أمان مضاعف) */
        as_signal_event(n->event_handle);
        as_deliver_node(n);
        drained++;
        free(n);
        n = nx;
    }

    as_apc_t *a = apcs;
    while (a) {
        as_apc_t *nx = a->next;
        if (a->fn) a->fn(a->ctx);
        free(a);
        a = nx;
    }
    return drained;
}

int ntb0_async_wait_one(uint64_t handle, int ms)
{
    int has_timeout = (ms >= 0);
    struct timespec ts;
    if (has_timeout) {
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += ms / 1000;
        ts.tv_nsec += (long)(ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += ts.tv_nsec / 1000000000L;
            ts.tv_nsec %= 1000000000L;
        }
    }

    pthread_mutex_lock(&g_lock);
    for (;;) {
        as_node_t *cur = g_head;
        int done = 0;
        int any = 0;
        while (cur) {
            if (cur->handle == handle) {
                if (cur->state == AS_DONE || cur->state == AS_CANCELLED) done = 1;
                if (cur->state == AS_PENDING || cur->state == AS_RUNNING) any = 1;
            }
            cur = cur->next;
        }
        if (done) break;
        if (!any) {
            pthread_mutex_unlock(&g_lock);
            ntb0_async_poll();
            return 0;
        }
        int r;
        if (has_timeout)
            r = pthread_cond_timedwait(&g_cond, &g_lock, &ts);
        else
            r = pthread_cond_wait(&g_cond, &g_lock);
        if (has_timeout && r == ETIMEDOUT) {
            pthread_mutex_unlock(&g_lock);
            return 1;
        }
    }
    pthread_mutex_unlock(&g_lock);
    /* في نهاية الانتظار، تُنبَّه eventfd العقدة المكتملة إن لم يُثر worker */
    pthread_mutex_lock(&g_lock);
    as_node_t *d = NULL;
    for (as_node_t *c = g_head; c; c = c->next)
        if (c->handle == handle && (c->state == AS_DONE || c->state == AS_CANCELLED)) { d = c; break; }
    pthread_mutex_unlock(&g_lock);
    if (d)
        as_signal_event(d->event_handle);
    return 0;
}

/*
 * T-057 — سحب عنصر واحد مكتمل بحسب المقبض (قائمة إتمام لكل FileHandle):
 * يُوصِل cb / APC ثم يفُكّ العقدة ويعيد النتيجة. يُعيد 1 إن وجد عنصراً.
 */
int ntb0_async_next_done(uint64_t handle, long *status, size_t *info)
{
    as_node_t *got = NULL;

    pthread_mutex_lock(&g_lock);
    as_node_t *prev = NULL;
    for (as_node_t *cur = g_head; cur; cur = cur->next) {
        if (cur->handle == handle &&
            (cur->state == AS_DONE || cur->state == AS_CANCELLED)) {
            got = cur;
            if (prev) prev->next = cur->next;
            else g_head = cur->next;
            break;
        }
        prev = cur;
    }
    pthread_mutex_unlock(&g_lock);

    if (!got)
        return 0;

    long st = got->status;
    size_t bytes = got->bytes_done;
    as_deliver_node(got);
    free(got);

    if (status) *status = st;
    if (info)   *info = bytes;
    return 1;
}

/* NtCancelIoFile — إلغاء العناصر المُتاحة:
 *   - PENDING: يُفكّ من القائمة، يُملأ IOSB بـSTATUS_CANCELLED، و eventfd.
 *   - RUNNING: يُعلَّم فعن اكتمال المعالجة يظهر بالحالة CANCELLED.
 */
void ntb0_async_cancel(uint64_t handle)
{
    pthread_mutex_lock(&g_lock);
    as_node_t *freed = NULL;
    as_node_t *prev = NULL;
    as_node_t *cur = g_head;
    while (cur) {
        as_node_t *next = cur->next;
        if (cur->handle == handle && cur->state == AS_PENDING) {
            if (prev) prev->next = next;
            else g_head = next;
            cur->status = NTB0_ST_CANCELLED;
            cur->bytes_done = 0;
            cur->next = freed;
            freed = cur;
        } else {
            if (cur->handle == handle) cur->cancelled = 1;
            prev = cur;
        }
        cur = next;
    }
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);

    while (freed) {
        as_node_t *nx = freed->next;
        as_fill_iosb(freed->iosb, (long)NTB0_ST_CANCELLED, 0);
        as_signal_event(freed->event_handle);
        free(freed);
        freed = nx;
    }
}

void ntb0_async_apc_queue(ntb0_async_apc_fn fn, void *ctx)
{
    as_apc_t *a = calloc(1, sizeof(*a));
    if (!a) return;
    a->fn = fn;
    a->ctx = ctx;
    pthread_mutex_lock(&g_lock);
    if (g_apc_tail) {
        g_apc_tail->next = a;
        g_apc_tail = a;
    } else {
        g_apc_head = g_apc_tail = a;
    }
    pthread_mutex_unlock(&g_lock);
}

int ntb0_async_pending_count(void)
{
    int n = 0;
    pthread_mutex_lock(&g_lock);
    as_node_t *cur = g_head;
    while (cur) {
        if (cur->state == AS_PENDING) n++;
        cur = cur->next;
    }
    pthread_mutex_unlock(&g_lock);
    return n;
}
