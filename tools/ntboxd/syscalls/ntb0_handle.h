/*
 * ntb0_handle.h — جدول المقابض (HandleTable) في مترجم الـ syscalls.
 *
 * لماذا هذا الملف:
 *   المقبض في ويندوز (HANDLE) **ليس** رقم fd في لينكس. في NT-Box
 *   يجب أن يكون المقبض الذي تراه شيفرة ويندوز شفافاً وبلا معنى لنظام
 *   لينكس: ملف يُفتح مرتين = مقبضان مختلفان لهما نفس الملف، إغلاق
 *   مقبض لا يُغلق الملف إن كان له نسخة أخرى، ومشاركة الوصول تُطبَّق
 *   كما في NT (STATUS_SHARING_VIOLATION).
 *
 *   الحل: **كعكة** (cookie) داخل المقبض:
 *        bits 63..48 = سحر ثابت 0x4E54 ('NT')
 *        bits 47..24 = جيل (generation) — يزيد كلما أُعيد استخدام الخانة
 *        bits 23..0  = رقم الخانة + 1 (0 = مقبض غير صالح)
 *   الجيل يجعل المقبض القديم (المغلق) غير صالح حتى لو أُعيدت الخانة.
 *
 * القاعدة (NTBOX.md §9): كل دالة هنا = ID + مصباح + اختبار.
 * المصابيح: HANDLE-001..HANDLE-006
 *   HANDLE-001 OK   — إنشاء مقبض
 *   HANDLE-002 OK   — إغلاق مقبض
 *   HANDLE-003 FAIL — مقبض غير صالح
 *   HANDLE-004 OK   — فحص المشاركة
 *   HANDLE-005 FAIL — تعارض مشاركة (STATUS_SHARING_VIOLATION)
 *   HANDLE-006 OK   — نسخ مقبض (NtDuplicateObject)
 */
#ifndef NTB0_HANDLE_H
#define NTB0_HANDLE_H

#include <stdint.h>
#include <stddef.h>

/* سحر المقبض ('NT') — يميّز مقابض المترجم عن أي رقم عادي */
#define NTB0_HANDLE_MAGIC 0x4E54ull

/* أقصى عدد مقابض مفتوحة في نفس الوقت */
#define NTB0_HANDLE_MAX 1024

/* نوع الكائن خلف المقبض */
enum ntb0_handle_kind {
    NTB0_HK_FILE = 0,    /* ملف/مجلد (fd عادي) */
    NTB0_HK_EVENT = 1,   /* حدث (eventfd) */
    NTB0_HK_SECTION = 2, /* مقطع ذاكرة (fd: memfd أو نسخة من ملف) */
    NTB0_HK_THREAD = 3,  /* خيط (fd: eventfd يُنبَّه عند الانتهاء) */
    NTB0_HK_PROCESS = 4, /* عملية (fd: /proc/<pid>) */
    NTB0_HK_PIPE = 5,    /* أنبوب مسمّى (fd: طرف القراءة؛ priv: طرف الكتابة) */
    NTB0_HK_REGKEY = 6,  /* مفتاح ريجستري (priv: ntb0_reg_key) */
};

/* ------------------------------------------------------------------ */
/* فك/تجميع الكعكة                                                     */
/* ------------------------------------------------------------------ */

#define NTB0_HANDLE_IDX(h) ((uint32_t)((h) & 0x00FFFFFFull))
#define NTB0_HANDLE_GEN(h) ((uint32_t)(((h) >> 24) & 0x00FFFFFFull))

/* هل القيمة بشكلها مقبض من جدولنا؟ (بلا قفل — للاختبارات فقط) */
int ntb0_handle_looks_valid(uint64_t handle);

/* ------------------------------------------------------------------ */
/* العمليات                                                            */
/* ------------------------------------------------------------------ */

/*
 * ينشئ مقبضاً جديداً من fd مفتوح.
 *   - يفحص وضع المشاركة ShareAccess أولاً (T-024).
 *   - عند الفشل: يغلق fd ويُعيد NTSTATUS (لا يتسرّب fd).
 *   - عند النجاح: يكتب المقبض في handle_out.
 *   - path: مسار الملف (نسخة من المكتبة) — للاختبار والتشخيص.
 */
long ntb0_handle_create(int fd, uint32_t desired, uint32_t share,
                        const char *path, uint64_t *handle_out);

/*
 * فحص المشاركة قبل الفتح (T-024) — مستقل عن الإنشاء (للاختبار).
 *   - يُعيد NTB0_ST_SUCCESS أو NTB0_ST_SHARING_VIOLATION.
 *   - fd يجب أن يكون مفتوحاً (يُستدعى fstat عليه).
 */
long ntb0_handle_share_check(int fd, uint32_t desired, uint32_t share);

/* يُعيد fd الخاص بالمقبض، أو -1 إن كان غير صالح */
int ntb0_handle_fd(uint64_t handle);

/* يُعيد fd الخاص بالمقبض إن كان مجلداً (RootDirectory)، أو -1 */
int ntb0_handle_dirfd(uint64_t handle);

/* يغلق المقبض (و الـ fd) — NTB0_ST_INVALID_HANDLE إن كان غير صالح */
long ntb0_handle_close(uint64_t handle);

/*
 * ينشئ مقبضاً لحدث (eventfd) — لا فحص مشاركة (الأحداث ليست ملفات).
 * (T-037: NtCreateEvent)
 */
long ntb0_handle_create_event(int eventfd, uint64_t *handle_out);

/*
 * ينشئ مقبض مقطع ذاكرة (T-034) — fd حقيقي (memfd أو نسخة من ملف).
 * لا فحص مشاركة (الأقسام ليست ملفات مفتوحة للقارئ).
 */
long ntb0_handle_create_section(int fd, uint64_t size, uint64_t *handle_out);

/* حجم المقطع المخزَّن (0 إن لم يكن مقطعاً) */
uint64_t ntb0_handle_size(uint64_t handle);

/*
 * ينشئ مقبض خيط (T-040): fd = eventfd (لجعل المقبض قابلاً للانتظار)
 * وpriv = حالة الخيط (مملوكة لوحدة ntb0_thread).
 */
long ntb0_handle_create_thread(int fd, void *priv, uint64_t *handle_out);

/*
 * ينشئ مقبض عملية (T-041): fd = /proc/<pid> وpriv = pid (كعدد صحيح).
 */
long ntb0_handle_create_process(int fd, void *priv, uint64_t *handle_out);

/*
 * ينشئ مقبضاً مع محرّر للـ priv (يُستدعى عند إغلاق المقبض).
 * يُستخدم لمفاتيح الريجستري (fd = -1: لا fd خلفه).
 */
long ntb0_handle_create_priv(int fd, void *priv, void (*priv_free)(void *),
                             int kind, uint64_t *handle_out);

/* الحالة الخاصة بالمقبض (خيط/عملية) — NULL إن لم تكن موجودة */
void *ntb0_handle_priv(uint64_t handle);

/*
 * ينتظر على كائن قابل للانتظار (حدث) — poll().
 *   timeout_ms < 0 ⇒ انتظار بلا مهلة.
 *   يُعيد 0 عند الإشارة · 1 عند انتهاء المهلة · -1 عند خطأ.
 */
int ntb0_handle_wait(uint64_t handle, int timeout_ms);

/* نوع الكائن خلف المقبض (FILE / EVENT) — -1 إن لم يكن صالحاً */
int ntb0_handle_kind(uint64_t handle);

/*
 * تدفّق المجلد المرتبط بالمقبض (T-054: NtQueryDirectoryFile).
 * يُنشئه عند الطلب (fdopendir على نسخة من الـ fd ⇒ يتشارك موضع الملف).
 * يُعيد NULL عند الفشل. (يُغلق مع المقبض.)
 */
void *ntb0_handle_dir_stream(uint64_t handle);

/* نسخة ثانية من نفس الملف: fd جديد (dup) + مقبض جديد (T-026)
 *   desired == 0 ⇒ يأخذ نفس صلاحية المصدر. */
long ntb0_handle_duplicate(uint64_t src, uint32_t desired,
                           uint64_t *handle_out);

/* عدد المقابض المفتوحة الآن (تشخيص/اختبار) */
int ntb0_handle_count(void);

/* مسار الملف المرتبط بالمقبض (NULL إن لم يوجد) */
const char *ntb0_handle_path(uint64_t handle);

/*
 * يحدّث المسار المخزَّن للمقبض (بعد إعادة تسمية الملف) — NT: المقبض
 * يبقى صالحاً ويشير إلى الملف بعد改名.
 */
int ntb0_handle_set_path(uint64_t handle, const char *path);

/* DesiredAccess المخزّن للمقبض (0 إن لم يوجد) */
uint32_t ntb0_handle_desired(uint64_t handle);

/* ShareAccess المخزّن للمقبض (0 إن لم يوجد) */
uint32_t ntb0_handle_share(uint64_t handle);

/*
 * وضع "الفشل المتوقّع" (للاختبار فقط):
 *   عند تفعيله تُطبع المصابيح المتوقّعة كـ OK بدل FAIL — فلا يُخلط
 *   فشل مقصود (مثل تعارض المشاركة) مع خطأ حقيقي في تقرير الحالة.
 */
void ntb0_handle_set_expect_fail(int on);

#endif /* NTB0_HANDLE_H */
