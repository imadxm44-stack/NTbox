/*
 * ntb0_thread.h — إدارة الخيوط في المترجم (T-040 · T-042).
 *
 * لماذا وحدة مستقلة؟
 *   خيط NT = خيط pthread مع اصطلاح استدعاء **ويندوز**، وله عدّاد تعليق
 *   (suspend count) يبدأ عند 1 إذا طُلب THREAD_CREATE_SUSPENDED.
 *   والمقبض يشير إلى "خيط انتهى" فيُنبَّه المنتظر (eventfd) — تماماً
 *   كما في NT حيث مقبض الخيط قابل للانتظار.
 *
 * ملاحظة: نقطة نداء الخيط (start routine) تُنفَّذ **بكود ويندوز**
 *   ⇒ نحتاج جسر اصطلاح (ntb0_thread_entry في ntb0_trampoline.S)
 *   ينقل rdi ⇒ rcx.
 */
#ifndef NTB0_THREAD_H
#define NTB0_THREAD_H

#include <stdint.h>

/*
 * ينشئ خيطاً جديداً:
 *   start_routine  عنوان نقطة الدخول (Windows ABI)
 *   param          المعامل (يُمرَّر في rcx)
 *   flags          1 = THREAD_CREATE_SUSPENDED
 *   stack_size     حجم المكدس (0 ⇒ افتراضي)
 * ينشئ مقبضاً في جدول المقابض (نوعه THREAD) ويعيده في handle_out.
 */
long ntb0_thread_create(uint64_t start_routine, uint64_t param, uint32_t flags,
                        uint64_t stack_size, uint64_t *handle_out);

/* ينقص عدّاد التعليق ويوقظ الخيط إن وصل إلى الصفر */
long ntb0_thread_resume(uint64_t handle, uint32_t *prev_count);

/* يزيد عدّاد التعليق (NtSuspendThread) */
long ntb0_thread_suspend(uint64_t handle, uint32_t *prev_count);

/* ينهي الخيط (pthread_cancel) وينتظره */
long ntb0_thread_terminate(uint64_t handle, uint32_t exit_status);

/* ينتظر انتهاء الخيط ويعيد رمز خروجه (للاختبار) */
long ntb0_thread_join(uint64_t handle, uint32_t *exit_code);

/* هل انتهى الخيط؟ */
int ntb0_thread_finished(uint64_t handle);

/* معرّف الخيط بصيغة NT (CLIENT_ID.UniqueThreadId) */
uint32_t ntb0_thread_tid(uint64_t handle);

/* يفتح مقبضاً جديداً على خيط معروف بمعرّفه (NtOpenThread) */
long ntb0_thread_open(uint32_t tid, uint64_t *handle_out);

/* رمز خروج الخيط (ExitStatus) */
void ntb0_thread_exit_code(uint64_t handle, uint32_t *code);

/* ------------------------------------------------------------------ */
/* التحكم بالخيط (T-048: NtSetInformationThread)                       */
/* ------------------------------------------------------------------ */

/* معرّف الخيط على لينكس (-1 إن لم يُعرف) */
long ntb0_thread_linux_tid(uint64_t handle);

/* تحويل أولوية NT ⇒ nice على لينكس */
int ntb0_thread_win_priority_to_nice(int win_priority);

/* ضبط أولوية خيط (tid < 0 ⇒ الخيط الحالي) */
long ntb0_thread_set_priority(long tid, int win_priority);

/* ضبط ألفة خيط (قناع 64 بت) — tid < 0 ⇒ الخيط الحالي */
long ntb0_thread_set_affinity(long tid, uint64_t mask);

/* مؤشر TEB — لكل خيط على حدة (thread-local في المترجم) */
void   ntb0_thread_set_current_teb(uint64_t teb);
uint64_t ntb0_thread_get_current_teb(void);

/* مؤشر TEB الخاص بخيط مُدار (من جدول الخيوط) */
long    ntb0_thread_set_teb(uint64_t handle, uint64_t teb);
uint64_t ntb0_thread_get_teb(uint64_t handle);

#endif /* NTB0_THREAD_H */
