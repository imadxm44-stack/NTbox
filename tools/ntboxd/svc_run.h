/*
 * svc_run.h — مضيف الخدمات (service host) لِ NT-Box (W7 / T-165 … T-172).
 *
 * يُشغّل ملف خدمة **حقيقي** (PE ويندوز 7) داخل الصندوق عبر محمّل
 * ntboxd: fork()+execv("./tools/ntboxd/ntboxd <binary>").  Linux لا
 * يُشغّل PE مباشرة، لذا نستضيفه داخل ntboxd (انظر honest caveats).
 *
 * التكامل مع SCM: svc_run_start() ينقل الحالة STOPPED→START_PENDING→RUNNING
 * إن وُجد الملف على القرص، وإلا يرفض (NTB0_SVC_RUN_ERR_NOBINARY) بلا تغيير
 * للحالة. عند خروج الابن تُعلَّم الخدمة STOPPED.
 */
#ifndef NTB0_SVC_RUN_H
#define NTB0_SVC_RUN_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "scm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* رمز الخطأ: binary غير موجود على القرص (رفض صحيح ⇒ مصباح OK) */
#define NTB0_SVC_RUN_ERR_NOBINARY (-1)

/* --- ربط SCM (اختياري؛ إن لم يُربط يُفتح default) --- */
void ntb0_svc_run_set_scm(ntb0_scm *scm);

/* --- تشغيل / إيقاف --- */
int  ntb0_svc_run_start(const char *name);   /* 0 نجاح، -1 خطأ، NOBINARY */
void ntb0_svc_run_stop(const char *name);    /* SIGTERM ثم SIGKILL بعد 5s  */

/* --- استعلام --- */
int   ntb0_svc_run_is_alive(const char *name);
pid_t ntb0_svc_run_pid(const char *name);

/* --- مساعدة: حل مسار ImagePath إلى ملف فعلي (0 إن وُجد) --- */
int ntb0_svc_run_resolve_image(const char *image, char *out, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* NTB0_SVC_RUN_H */