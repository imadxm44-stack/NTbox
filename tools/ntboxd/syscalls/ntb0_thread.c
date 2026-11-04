/*
 * ntb0_thread.c — إدارة الخيوط في المترجم (T-040 · T-042).
 *
 * كل خيط NT يُترجم إلى:
 *   - خيط pthread (clone) بدل pthread_create
 *   - جسر اصطلاح: ntb0_thread_entry(param, start) ينقل rdi ⇒ rcx
 *   - عدّاد تعليق (suspend count) محميّ بـ mutex + condvar
 *   - eventfd يُنبَّه عند انتهاء الخيط ⇒ المقبض قابل للانتظار (poll)
 *
 * الحدّ الموثّق: إلغاء الخيط (NtTerminateThread) يتم بـ pthread_cancel
 * ⇒ يلزم reaching نقطة إلغاء (نظامية/انتظار). الخيط الذي لا يدخل
 * أي استدعاء نظام لن يُلغى فوراً (كما في NT تقريباً).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <sched.h>

#include "ntb0_thread.h"
#include "ntb0_handle.h"
#include "ntb0_nt.h"
#include "ntb0_trace.h"

#define NTB0_MAX_THREADS 64

struct ntb0_thread {
    int      in_use;
    pthread_t tid;
    uint64_t start;
    uint64_t param;
    int      suspend_count;
    int      terminated;
    int      finished;
    uint32_t exit_code;
    uint32_t nt_tid;              /* معرّف NT للخيط */
    int      linux_tid;           /* معرّف الخيط على لينكس (gettid) */
    uint64_t teb;                 /* مؤشر TEB الذي حدده كود ويندوز */
    int      efd;                 /* يُنبَّه عند الانتهاء */
    pthread_mutex_t lock;
    pthread_cond_t  cond;
};

static struct ntb0_thread ntb0_threads[NTB0_MAX_THREADS];
static pthread_mutex_t     ntb0_thread_lock = PTHREAD_MUTEX_INITIALIZER;

/* معرّفات الخيوط بصيغة NT (تبدأ من 1) */
static uint32_t ntb0_next_tid = 1;

/* جسر اصطلاح الاستدعاء (ينقل rdi ⇒ rcx) — معرَّف في ntb0_trampoline.S */
extern uint64_t ntb0_thread_entry(uint64_t param, uint64_t start);

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static struct ntb0_thread *ntb0_thread_find(uint64_t handle)
{
    void *priv = ntb0_handle_priv(handle);

    if (!priv)
        return NULL;
    return (struct ntb0_thread *)priv;
}

/* ------------------------------------------------------------------ */
/* نقطة دخول الخيط (pthread)                                          */
/* ------------------------------------------------------------------ */

/*
 * تنظيف الخيط — يعمل في الحالتين:
 *   - خروج طبيعي (pthread_cleanup_pop(1))
 *   - إلغاء (pthread_cancel) ⇒ يُنفَّذ معالج التنظيف ✓
 * يجعل المقبض "قابلاً للانتظار" (يكتب في الـ eventfd) ويضع علم الانتهاء.
 */
static void ntb0_thread_cleanup(void *arg)
{
    struct ntb0_thread *t = (struct ntb0_thread *)arg;
    uint64_t one = 1;

    pthread_mutex_lock(&t->lock);
    t->finished = 1;
    pthread_cond_broadcast(&t->cond);
    pthread_mutex_unlock(&t->lock);

    if (t->efd >= 0) {
        if (write(t->efd, &one, sizeof(one)) < 0) {
            /* المقبض أُغلق ⇒ لا بأس */
        }
    }
}

static void *ntb0_thread_main(void *arg)
{
    struct ntb0_thread *t = (struct ntb0_thread *)arg;
    volatile uint64_t rc = 0;

    /* معرّف الخيط الحقيقي على لينكس (لضبط الأولوية/الألفة على هذا الخيط) */
    t->linux_tid = (int)syscall(SYS_gettid);

    pthread_cleanup_push(ntb0_thread_cleanup, t);

    /* احترام عدّاد التعليق قبل التنفيذ */
    pthread_mutex_lock(&t->lock);
    while (t->suspend_count > 0 && !t->terminated)
        pthread_cond_wait(&t->cond, &t->lock);
    pthread_mutex_unlock(&t->lock);

    if (!t->terminated)
        rc = ntb0_thread_entry(t->param, t->start);

    /*
     * إن أُنهِي الخيط (NtTerminateThread) فالرمز المعتمد هو الذي حدّده
     * صاحب الأمر — لا تُدهن به قيمة الإرجاع الطبيعية.
     */
    pthread_mutex_lock(&t->lock);
    if (!t->terminated)
        t->exit_code = (uint32_t)rc;
    pthread_mutex_unlock(&t->lock);

    pthread_cleanup_pop(1); /* ينفّذ المعالج ⇒ يحدّث الحالة وينبّه المنتظر */

    return NULL;
}

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

long ntb0_thread_create(uint64_t start_routine, uint64_t param, uint32_t flags,
                        uint64_t stack_size, uint64_t *handle_out)
{
    struct ntb0_thread *t = NULL;
    pthread_attr_t attr;
    int efd = -1;
    int hfd = -1;
    int i;
    int rc;

    if (!handle_out || !start_routine)
        return NTB0_ST_INVALID_PARAMETER;

    pthread_mutex_lock(&ntb0_thread_lock);

    for (i = 0; i < NTB0_MAX_THREADS; i++) {
        if (!ntb0_threads[i].in_use) {
            t = &ntb0_threads[i];
            break;
        }
    }

    if (!t) {
        pthread_mutex_unlock(&ntb0_thread_lock);
        NTB0_FAIL("SYS-032", "thread table full (%d)", NTB0_MAX_THREADS);
        return NTB0_ST_INSUFFICIENT_RESOURCES;
    }

    efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (efd < 0) {
        pthread_mutex_unlock(&ntb0_thread_lock);
        return NTB0_ST_UNSUCCESSFUL;
    }

    memset(t, 0, sizeof(*t));
    t->in_use = 1;
    /* ntb0_thread_lock مقفول أصلاً هنا ⇒ لا نعيد قفله (وإلا ⇒ deadlock) */
    t->nt_tid = ntb0_next_tid++;
    t->start = start_routine;
    t->param = param;
    t->efd = efd;
    t->suspend_count = (flags & 0x1u) ? 1 : 0; /* THREAD_CREATE_SUSPENDED */
    pthread_mutex_init(&t->lock, NULL);
    pthread_cond_init(&t->cond, NULL);

    pthread_mutex_unlock(&ntb0_thread_lock);

    /* المقبض يأخذ نسخة من الـ eventfd (حتى يبقى-living بعد إغلاق المقبض) */
    hfd = dup(efd);
    if (hfd < 0) {
        close(efd);
        return NTB0_ST_UNSUCCESSFUL;
    }

    rc = (int)ntb0_handle_create_thread(hfd, t, handle_out);
    if (rc != NTB0_ST_SUCCESS) {
        close(hfd);
        close(efd);
        t->in_use = 0;
        return rc;
    }

    pthread_attr_init(&attr);
    if (stack_size >= 64 * 1024)
        pthread_attr_setstacksize(&attr, (size_t)stack_size);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    if (pthread_create(&t->tid, &attr, ntb0_thread_main, t) != 0) {
        pthread_attr_destroy(&attr);
        ntb0_handle_close(*handle_out);
        close(efd);
        t->in_use = 0;
        return NTB0_ST_UNSUCCESSFUL;
    }

    pthread_attr_destroy(&attr);

    NTB0_OK("SYS-032", "thread created (start=0x%llx param=0x%llx) -> "
            "handle 0x%llx", (unsigned long long)start_routine,
            (unsigned long long)param, (unsigned long long)*handle_out);
    return NTB0_ST_SUCCESS;
}

long ntb0_thread_resume(uint64_t handle, uint32_t *prev_count)
{
    struct ntb0_thread *t = ntb0_thread_find(handle);
    uint32_t prev;

    if (!t)
        return NTB0_ST_INVALID_HANDLE;

    pthread_mutex_lock(&t->lock);
    prev = (uint32_t)t->suspend_count;
    if (t->suspend_count > 0)
        t->suspend_count--;
    if (t->suspend_count == 0)
        pthread_cond_broadcast(&t->cond);
    pthread_mutex_unlock(&t->lock);

    if (prev_count)
        *prev_count = prev;

    NTB0_OK("SYS-033", "NtResumeThread -> previous suspend count %u", prev);
    return NTB0_ST_SUCCESS;
}

long ntb0_thread_suspend(uint64_t handle, uint32_t *prev_count)
{
    struct ntb0_thread *t = ntb0_thread_find(handle);
    uint32_t prev;

    if (!t)
        return NTB0_ST_INVALID_HANDLE;

    pthread_mutex_lock(&t->lock);
    prev = (uint32_t)t->suspend_count;
    t->suspend_count++;
    pthread_mutex_unlock(&t->lock);

    if (prev_count)
        *prev_count = prev;

    NTB0_OK("SYS-039", "NtSuspendThread -> previous suspend count %u", prev);
    return NTB0_ST_SUCCESS;
}

long ntb0_thread_terminate(uint64_t handle, uint32_t exit_status)
{
    struct ntb0_thread *t = ntb0_thread_find(handle);

    if (!t)
        return NTB0_ST_INVALID_HANDLE;

    pthread_mutex_lock(&t->lock);
    if (t->finished) {
        pthread_mutex_unlock(&t->lock);
        return NTB0_ST_SUCCESS; /* انتهى أصلاً */
    }
    t->terminated = 1;
    t->exit_code = exit_status;
    pthread_cond_broadcast(&t->cond);
    pthread_mutex_unlock(&t->lock);

    /* pthread_cancel: نقطة إلغاء حتمية (pthread_cond_wait نقطة إلغاء) */
    pthread_cancel(t->tid);

    NTB0_OK("SYS-034", "NtTerminateThread -> exit status 0x%x", exit_status);
    return NTB0_ST_SUCCESS;
}

long ntb0_thread_join(uint64_t handle, uint32_t *exit_code)
{
    struct ntb0_thread *t = ntb0_thread_find(handle);

    if (!t)
        return NTB0_ST_INVALID_HANDLE;

    pthread_mutex_lock(&t->lock);
    while (!t->finished && !t->terminated)
        pthread_cond_wait(&t->cond, &t->lock);
    if (exit_code)
        *exit_code = t->exit_code;
    pthread_mutex_unlock(&t->lock);

    return NTB0_ST_SUCCESS;
}

int ntb0_thread_finished(uint64_t handle)
{
    struct ntb0_thread *t = ntb0_thread_find(handle);
    int f;

    if (!t)
        return 0;

    pthread_mutex_lock(&t->lock);
    f = t->finished;
    pthread_mutex_unlock(&t->lock);
    return f;
}

void ntb0_thread_exit_code(uint64_t handle, uint32_t *code)
{
    struct ntb0_thread *t = ntb0_thread_find(handle);

    if (!t || !code)
        return;

    pthread_mutex_lock(&t->lock);
    *code = t->exit_code;
    pthread_mutex_unlock(&t->lock);
}

uint32_t ntb0_thread_tid(uint64_t handle)
{
    struct ntb0_thread *t = ntb0_thread_find(handle);

    return t ? t->nt_tid : 0;
}

long ntb0_thread_open(uint32_t tid, uint64_t *handle_out)
{
    struct ntb0_thread *found = NULL;
    int hfd = -1;
    long rc;
    int i;

    if (!handle_out || tid == 0)
        return NTB0_ST_INVALID_PARAMETER;

    pthread_mutex_lock(&ntb0_thread_lock);
    for (i = 0; i < NTB0_MAX_THREADS; i++) {
        if (ntb0_threads[i].in_use && ntb0_threads[i].nt_tid == tid) {
            found = &ntb0_threads[i];
            break;
        }
    }
    pthread_mutex_unlock(&ntb0_thread_lock);

    if (!found)
        return NTB0_ST_INVALID_HANDLE;

    /* نسخة من الـ eventfd ⇒ المقبض الجديد قابل للانتظار */
    hfd = dup(found->efd);
    if (hfd < 0)
        return NTB0_ST_UNSUCCESSFUL;

    rc = ntb0_handle_create_thread(hfd, found, handle_out);
    if (rc != NTB0_ST_SUCCESS)
        close(hfd);
    else
        NTB0_OK("SYS-038", "NtOpenThread(tid=%u) -> handle 0x%llx", tid,
                (unsigned long long)*handle_out);
    return rc;
}

/* ------------------------------------------------------------------ */
/* التحكم بالخيط (T-048: NtSetInformationThread)                       */
/* ------------------------------------------------------------------ */

/*
 * تحويل أولوية NT إلى nice على لينكس:
 *   NT:  -2 (idle) .. 15 (time critical)، 0 = عادي
 *   POSIX nice: -20 (أعلى) .. 19 (أدنى)
 * ملاحظة: رفع الأولوية (nice سالب) يحتاج CAP_SYS_NICE ⇒ يعيد EACCES
 *   عند غي��اب الصلاحية (وهو ما يفعله NT أيضاً ⇒ PRIVILEGE_NOT_HELD).
 */
int ntb0_thread_win_priority_to_nice(int win_priority)
{
    if (win_priority > 0) {
        int n = win_priority > 19 ? 19 : win_priority;

        return -n; /* أولوية أعلى ⇒ nice سالب */
    }
    if (win_priority < 0) {
        int n = -win_priority;

        return n > 19 ? 19 : n; /* أولوية أدنى ⇒ nice موجب */
    }
    return 0;
}

long ntb0_thread_linux_tid(uint64_t handle)
{
    struct ntb0_thread *t = ntb0_thread_find(handle);

    if (!t)
        return -1;
    return (long)t->linux_tid;
}

/*
 * ضبط أولوية خيط.
 *   tid < 0 ⇒ الخيط الحالي (المترجم نفسه).
 *   يعيد NTSTATUS.
 */
long ntb0_thread_set_priority(long tid, int win_priority)
{
    int nice = ntb0_thread_win_priority_to_nice(win_priority);
    pid_t target = (pid_t)(tid < 0 ? 0 : tid);

    errno = 0;
    if (setpriority(PRIO_PROCESS, target, nice) < 0) {
        if (errno == EACCES || errno == EPERM)
            return NTB0_ST_PRIVILEGE_NOT_HELD;
        return NTB0_ST_UNSUCCESSFUL;
    }
    return NTB0_ST_SUCCESS;
}

/* ضبط ألفة الخيط (mask من NT = قناع معالجات 64 بت) */
long ntb0_thread_set_affinity(long tid, uint64_t mask)
{
    pid_t target = (pid_t)(tid < 0 ? 0 : tid);
    cpu_set_t set;

    if (mask == 0)
        return NTB0_ST_INVALID_PARAMETER;

    CPU_ZERO(&set);
    {
        int cpu;

        for (cpu = 0; cpu < 64 && cpu < CPU_SETSIZE; cpu++) {
            if (mask & (1ull << cpu))
                CPU_SET(cpu, &set);
        }
    }

    if (CPU_COUNT(&set) == 0)
        return NTB0_ST_INVALID_PARAMETER;

    if (sched_setaffinity(target, sizeof(set), &set) < 0) {
        if (errno == EINVAL)
            return NTB0_ST_INVALID_PARAMETER;
        return NTB0_ST_UNSUCCESSFUL;
    }
    return NTB0_ST_SUCCESS;
}

/* مؤشر TEB الذي حدده كود ويندوز (لكل خيط على حدة) */
static __thread uint64_t ntb0_tls_teb;

void ntb0_thread_set_current_teb(uint64_t teb)
{
    ntb0_tls_teb = teb;
}

uint64_t ntb0_thread_get_current_teb(void)
{
    return ntb0_tls_teb;
}

long ntb0_thread_set_teb(uint64_t handle, uint64_t teb)
{
    struct ntb0_thread *t = ntb0_thread_find(handle);

    if (!t)
        return NTB0_ST_INVALID_HANDLE;

    pthread_mutex_lock(&t->lock);
    t->teb = teb;
    pthread_mutex_unlock(&t->lock);

    return NTB0_ST_SUCCESS;
}

uint64_t ntb0_thread_get_teb(uint64_t handle)
{
    struct ntb0_thread *t = ntb0_thread_find(handle);
    uint64_t v;

    if (!t)
        return 0;

    pthread_mutex_lock(&t->lock);
    v = t->teb;
    pthread_mutex_unlock(&t->lock);

    return v;
}
