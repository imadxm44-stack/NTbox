/*
 * svc_real.h — تشغيل الخدمات الحقيقية (Real Service Runtime) لـ NT-Box.
 *
 * يُشغّل ملف خدمة **حقيقي** (PE ويندوز 7) داخل الصندوق عبر محمّل
 * ntboxd: fork()+execv("ntboxd <binary>"). يدمج مع SCM لتحديث
 * الحالة تلقائياً عند بدء/إيقاف الخدمة.
 *
 * المصابيح (NTBOX.md §9 · عددّان معاً كالموجود في svc_run.c):
 *   SVC-002 — ملف خدمة **حقيقي** من الريجستري يُشغَّل داخل الصندوق (T-165)
 *   SVC-003 — رفض/موت مُلاحَظ: binary مفقود أو خروج الابن مُتتبَّع
 *             (نصف المُرسِل الجانب-المضيف؛ ServiceMain على جانب الخدمة للمرجع T-166)
 *   SVC-006 — spawn via ntboxd
 *   SVC-007 — missing binary refusal
 *   SVC-008 — host tracked
 *   SVC-009 — stopped
 *   SVC-010 — state transition
 */
#ifndef NTB0_SVC_REAL_H
#define NTB0_SVC_REAL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "scm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* رموز الخطأ                                                          */
/* ------------------------------------------------------------------ */
#define NTB0_SVC_REAL_OK              0
#define NTB0_SVC_REAL_ERR_NOTFOUND   -1   /* الخدمة غير موجودة في SCM */
#define NTB0_SVC_REAL_ERR_NOBINARY   -2   /* الملف التنفيذي غير موجود */
#define NTB0_SVC_REAL_ERR_DISABLED   -3   /* الخدمة معطّلة (Start=4) */
#define NTB0_SVC_REAL_ERR_DEPENDENCY -4   /* تبعية غير مشغّلة */
#define NTB0_SVC_REAL_ERR_FORK       -5   /* فشل fork */
#define NTB0_SVC_REAL_ERR_EXEC       -6   /* فشل exec */
#define NTB0_SVC_REAL_ERR_NOSLOT     -7   /* لا يوجد مكان للتتبع */
#define NTB0_SVC_REAL_ERR_NOTPE32P   -8   /* الملف موجود لكنه ليس PE32+ (ntboxd 64 يرفضه) */

/* ------------------------------------------------------------------ */
/* فحص الجرد: كم خدمة في الريجستري تملك ملفاً حقيقياً؟                  */
/* ------------------------------------------------------------------ */
typedef struct ntb0_svc_scan_stats {
    int total;          /* كل الخدمات في SCM                          */
    int with_image;     /* تملك ImagePath                             */
    int img_exists;     /* ImagePath يُحلّ إلى ملف موجود فعلاً          */
    int img_pe32plus;   /* موجود + PE32+ (يدعمه ntboxd)               */
    int img_pe32;       /* موجود لكن PE32 (ntboxd يرفضه: "not PE32+") */
    int img_kernel;     /* Type = driver/fs (لا spawn مستخدم)         */
    int img_missing;    /* ImagePath موجود لكن الملف مفقود            */
    int no_image;       /* بلا ImagePath                              */
} ntb0_svc_scan_stats;

/* ------------------------------------------------------------------ */
/* واجهة البرمجة                                                       */
/* ------------------------------------------------------------------ */

/**
 * ntb0_svc_real_set_scm — يربط SCM عام للاستخدام من قبل الدوال
 * التي لا تستقبل scm كمعامل (مثل is_alive, stop).
 * يجب استدعاؤه مرة واحدة بعد تحميل SCM.
 */
void ntb0_svc_real_set_scm(ntb0_scm *scm);

/**
 * ntb0_svc_real_start — يبدأ خدمة حقيقية
 * @scm:  مُسيّر الخدمات المحمّل
 * @name: اسم الخدمة (مفتاح الريجستري)
 *
 * يقرأ ImagePath من SCM، يحل المسار، ينشئ عملية ابن عبر
 * fork()+execv("ntboxd <binary>")، يسجّل PID، ينقل حالة SCM
 * STOPPED→START_PENDING→RUNNING.
 *
 * يعيد 0 عند النجاح، رمز خطأ سالب عند الفشل.
 * عند تعذّر إيجاد الملف: يسجّل SVC-007، لا يغيّر الحالة إلى RUNNING.
 */
int ntb0_svc_real_start(ntb0_scm *scm, const char *name);

/**
 * ntb0_svc_real_stop — يوقف خدمة حقيقية
 * @name: اسم الخدمة
 *
 * يرسل SIGTERM، ينتظر 5 ثوانٍ، ثم SIGKILL.
 * عند الانتهاء: ينقل حالة SCM إلى STOPPED، يسجّل SVC-009.
 */
void ntb0_svc_real_stop(const char *name);

/**
 * ntb0_svc_real_is_alive — يتحقّق إن كانت الخدمة لا تزال حية
 * @name: اسم الخدمة
 *
 * يعيد 1 إن كان العملية الفرعية لا تزال تعمل، 0 غير ذلك.
 * عند اكتشاف خروج الابن: يُحدّث حالة SCM إلى STOPPED.
 */
int ntb0_svc_real_is_alive(const char *name);

/**
 * ntb0_svc_real_get_pid — يعيد PID العملية الفرعية
 * @name: اسم الخدمة
 *
 * يعيد PID إن وجد، -1 إن لم يكن مُتتبّعاً.
 */
pid_t ntb0_svc_real_get_pid(const char *name);

/**
 * ntb0_svc_real_reap_dead — يفحص جميع الخدمات المُتتبّعة
 * ويحدّث حالة أي منها خرج (reap + SCM state → STOPPED).
 * يُستدعى دورياً من الحلقة الرئيسية.
 */
void ntb0_svc_real_reap_dead(ntb0_scm *scm);

/**
 * ntb0_svc_real_probe_pe — فحص فعلي لملف PE:
 *   1  = PE32+ (يعمل في ntboxd)
 *   0  = PE32 موجود لكنه 32-بت (ntboxd يرفضه)
 *  -1  = ليس ملف PE أصلاً
 */
int ntb0_svc_real_probe_pe(const char *path);

/**
 * ntb0_svc_real_scan — جرد الصندوق: يمشي على **كل** خدمات SCM ويحلّ
 * ImagePath إلى ملف تحت win7root (عبر نفس محمّل scm.c)، ويصنّف
 * الموجود/المفقود/PE32+/PE32/مستضاف svchost (dll بلا exe).
 * يعيد عبر out إحصاءات القياس. يعيد 0 عند النجاح.
 */
int ntb0_svc_real_scan(ntb0_scm *scm, ntb0_svc_scan_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* NTB0_SVC_REAL_H */