/*
 * scm.h — مُسيّر الخدمات (Service Control Manager) لِ NT-Box.
 *
 * المصدر الوحيد للحقيقة: خلية SYSTEM **الحقيقية** لويندوز 7
 *   <win7root>/Windows/System32/config/SYSTEM
 *   ⇒ ControlSet001\services\{ServiceName}
 *   (Fallback: ControlSet002 ثم CurrentControlSet)
 *
 * كل سجل خدمة يحمل ما هو في الريجستري الحقيقي (بلا اختلاق):
 *   DisplayName · ImagePath · Description · ObjectName · Group · ServiceDll
 *   Start    (0=boot 1=system 2=auto 3=demand 4=disabled)
 *   Type     (0x1=kernel 0x2=fs 0x10=own 0x20=share · بتّ 0x100=interactive)
 *   ErrorControl · DependOnService (REG_MULTI_SZ) · RequiredPrivileges
 *
 * القاعدة (NTBOX.md §9): كل نقطة = ID + مصباح + اختبار.
 *   المصابيح: SVC-001 (جدول الخدمات ودورة حياة الحالة)
 *              SVC-004 (نواة SCM تعمل في وضع المستخدم)
 *
 * أمانة (مهمّة):
 *   هذه الطبقة **لا تُشغّل أي عملية**. start()/stop() تنقل الحالة فقط
 *   (STOPPED→START_PENDING→RUNNING، RUNNING→STOP_PENDING→STOPPED)
 *   وتسجّل ترتيب البدء. تشغيل الملف الحقيقي (T-165) عملٌ لاحق —
 *   لا ندّعي ما لم ننفّذه.
 */
#ifndef NTB0_SCM_H
#define NTB0_SCM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* حدود                                                               */
/* ------------------------------------------------------------------ */
#define NTB0_SCM_MAX_NAME        96
#define NTB0_SCM_MAX_DISPLAY    256
#define NTB0_SCM_MAX_DESC       256
#define NTB0_SCM_MAX_IMAGE      512
#define NTB0_SCM_MAX_OBJECT     128
#define NTB0_SCM_MAX_GROUP       64
#define NTB0_SCM_MAX_DLL        256
#define NTB0_SCM_MAX_DEPS        24
#define NTB0_SCM_MAX_DEP_NAME   128
#define NTB0_SCM_MAX_PRIVS       16
#define NTB0_SCM_MAX_PRIV_NAME   64

/* ------------------------------------------------------------------ */
/* حالات الخدمة (توافق SERVICE_STATUS.dwCurrentState)                */
/* ------------------------------------------------------------------ */
enum ntb0_svc_state {
    NTB0_SVC_DISABLED      = 0,  /* Start == 4 ⇒ معطّلة من الريجستري */
    NTB0_SVC_STOPPED       = 1,
    NTB0_SVC_START_PENDING = 2,
    NTB0_SVC_STOP_PENDING  = 3,
    NTB0_SVC_RUNNING       = 4,
    NTB0_SVC_PAUSED        = 7,
};

/* ------------------------------------------------------------------ */
/* أنواع البدء (قيمة Start في الريجستري)                              */
/* ------------------------------------------------------------------ */
enum ntb0_svc_start {
    NTB0_SVC_START_BOOT     = 0,
    NTB0_SVC_START_SYSTEM   = 1,
    NTB0_SVC_START_AUTO     = 2,
    NTB0_SVC_START_DEMAND   = 3,
    NTB0_SVC_START_DISABLED = 4,
};

/* ------------------------------------------------------------------ */
/* أنواع الخدمة (قيمة Type في الريجستري)                              */
/* ------------------------------------------------------------------ */
#define NTB0_SVC_TYPE_KERNEL_DRIVER 0x001u
#define NTB0_SVC_TYPE_FILE_SYSTEM    0x002u
#define NTB0_SVC_TYPE_ADAPTER        0x004u
#define NTB0_SVC_TYPE_RECOGNIZER     0x008u
#define NTB0_SVC_TYPE_WIN32_OWN      0x010u
#define NTB0_SVC_TYPE_WIN32_SHARE    0x020u
#define NTB0_SVC_TYPE_INTERACTIVE    0x100u

/* ------------------------------------------------------------------ */
/* أوامر التحكم (SERVICE_CONTROL_*)                                    */
/* ------------------------------------------------------------------ */
#define NTB0_SVC_CONTROL_STOP        1u
#define NTB0_SVC_CONTROL_PAUSE       2u
#define NTB0_SVC_CONTROL_CONTINUE    3u
#define NTB0_SVC_CONTROL_INTERROGATE 4u

/* ------------------------------------------------------------------ */
/* رموز الأخطاء (مطابقة لـ GetLastError المعتادة)                    */
/* ------------------------------------------------------------------ */
#define NTB0_SCM_OK             0
#define NTB0_SCM_ERR_NOTFOUND  (-1)  /* ERROR_SERVICE_DOES_NOT_EXIST  */
#define NTB0_SCM_ERR_DISABLED  (-2)  /* ERROR_SERVICE_DISABLED       */
#define NTB0_SCM_ERR_DEPENDENCY (-3) /* ERROR_DEPENDENCIES_OCCUR     */
#define NTB0_SCM_ERR_STATE     (-4)  /* ERROR_SERVICE_NOT_ACTIVE     */
#define NTB0_SCM_ERR_MEMORY    (-5)
#define NTB0_SCM_ERR_RANGE     (-6)
#define NTB0_SCM_ERR_EXISTS    (-7)  /* ERROR_SERVICE_EXISTS         */

/* ------------------------------------------------------------------ */
/* سجل خدمة واحدة                                                     */
/* ------------------------------------------------------------------ */
typedef struct ntb0_svc {
    char     name[NTB0_SCM_MAX_NAME];        /* اسم مفتاح الريجستري      */
    char     display[NTB0_SCM_MAX_DISPLAY];  /* DisplayName              */
    char     description[NTB0_SCM_MAX_DESC]; /* Description              */
    char     image[NTB0_SCM_MAX_IMAGE];      /* ImagePath (خام/غير مفسّر) */
    char     object[NTB0_SCM_MAX_OBJECT];    /* ObjectName (الحساب)      */
    char     group[NTB0_SCM_MAX_GROUP];      /* مجموعة svchost           */
    char     dll[NTB0_SCM_MAX_DLL];          /* ServiceDll (المضيف)      */
    uint32_t type;                           /* Type                     */
    uint32_t start;                          /* Start                    */
    uint32_t error_control;                  /* ErrorControl             */
    uint32_t sid_type;                       /* ServiceSidType           */
    int      ndeps;                          /* عدد التبعيات            */
    char     deps[NTB0_SCM_MAX_DEPS][NTB0_SCM_MAX_DEP_NAME];
    int      nprivs;                         /* عدد الامتيازات           */
    char     privs[NTB0_SCM_MAX_PRIVS][NTB0_SCM_MAX_PRIV_NAME];
    int      state;                          /* ntb0_svc_state           */
    int      order;                          /* موضعها في ترتيب البدء    */
    unsigned seq_start;                      /* عدّاد تسلسل البدء        */
    unsigned seq_stop;                       /* عدّاد تسلسل الإيقاف      */
} ntb0_svc;

/* ------------------------------------------------------------------ */
/* مُسيّر الخدمات                                                     */
/* ------------------------------------------------------------------ */
typedef struct ntb0_scm ntb0_scm;

/* --- الفتح والإغلاق --- */
/* يفتح خلية SYSTEM من مسار صريح */
ntb0_scm *ntb0_scm_open(const char *hive_path);
/* يفتح خلية SYSTEM الافتراضية من جذر ويندوز 7 ($NTBOX_ROOT …) */
ntb0_scm *ntb0_scm_open_default(void);
void      ntb0_scm_close(ntb0_scm *scm);

const char *ntb0_scm_hive_path(const ntb0_scm *scm);
const char *ntb0_scm_control_set(const ntb0_scm *scm);

/* --- الاستعلام --- */
int         ntb0_scm_count(const ntb0_scm *scm);
ntb0_svc   *ntb0_scm_at(const ntb0_scm *scm, int index);
ntb0_svc   *ntb0_scm_find(const ntb0_scm *scm, const char *name); /* غير حساس */
const char *ntb0_scm_name(const ntb0_scm *scm, int index);

/* تبعيات خدمة (أسماء) — تُعيد عددها */
int ntb0_scm_deps(const ntb0_scm *scm, const char *name, const char **out, int max);
/* امتيازات خدمة (أسماء) */
int ntb0_scm_privs(const ntb0_scm *scm, const char *name, const char **out, int max);
/* خدمات مستضافة داخل svchost (Type == 0x20) */
int ntb0_scm_hosted(const ntb0_scm *scm, const char **out, int max);
/* عدد المجموعات (svchost groups) المختلفة */
int ntb0_scm_groups(const ntb0_scm *scm, const char **out, int max);

/* --- ترتيب البدء (فرز طوبولوجي حقيقي) --- */
/* يكتب في out فهارس الخدمات بترتيب البدء (الأول = يُبدأ أولاً) */
int ntb0_scm_start_order(const ntb0_scm *scm, int *out, int max);
/* موضع خدمة في ترتيب البدء (-1 إن لم تكن فيه/دورة) */
int ntb0_scm_order_of(const ntb0_scm *scm, const char *name);
/* الخدمات المشتركة في دورات تبعيات (فهرس داخل الجدول) */
int ntb0_scm_cycles(const ntb0_scm *scm, int *out, int max);
/* تقرير نصّي عن أول دورة مكتشفة: "A -> B -> A" */
int ntb0_scm_cycle_report(const ntb0_scm *scm, char *buf, size_t len);
/* تحقّق مستقل: هل كل خدمة تسبق كل تبعياتها في ترتيب البدء؟ */
int ntb0_scm_check_order(const ntb0_scm *scm);

/* --- دورة حياة الحالة --- */
int ntb0_scm_start_service(ntb0_scm *scm, const char *name);
int ntb0_scm_stop_service(ntb0_scm *scm, const char *name);
int ntb0_scm_control_service(ntb0_scm *scm, const char *name, uint32_t control);
int ntb0_scm_state_of(const ntb0_scm *scm, const char *name);

/* CreateService (في الذاكرة فقط — الريجستري للقراءة في هذه الطبقة) */
int ntb0_scm_create_service(ntb0_scm *scm, const char *name, const char *display,
                            const char *image, uint32_t type, uint32_t start,
                            uint32_t error_control, const char *const *deps,
                            int ndeps);

/* --- أدوات نصّية --- */
const char *ntb0_scm_state_name(int state);
const char *ntb0_scm_start_name(uint32_t start);
const char *ntb0_scm_type_name(uint32_t type);

#ifdef __cplusplus
}
#endif

#endif /* NTB0_SCM_H */
