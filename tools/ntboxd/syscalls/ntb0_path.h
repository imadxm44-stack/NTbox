/*
 * ntb0_path.h — ترجمة مسار NT (ويندوز) إلى مسار لينكس.
 *
 * لماذا هذا الملف:
 *   طلب ويندوز للملف يصل كـ UNICODE_STRING داخل OBJECT_ATTRIBUTES.
 *   المسار قد يكون بصيغة NT (\Device\HarddiskVolume1\...) أو DOS (\??\C:\...).
 *   جسر الترجمة (linux_bridge) يعمل على مسار لينكس حقيقي — فنحتاج
 *   تحويلاً صريحاً ومقيساً، لا تخميناً.
 *
 * القاعدة (NTBOX.md §9): كل دالة هنا = ID + مصباح + اختبار.
 *
 * حدود هذه المرحلة (P0) — موثّقة صراحة:
 *   - القرص C: فقط (جذر ويندوز = جذر NT-Box). الأقراص الأخرى → DEVICE_NOT_CONNECTED.
 *   - مسارات UNC (\Device\Mup\...) → NOT_SUPPORTED.
 *   - مسارات الريجستري (\Registry\...) → NOT_SUPPORTED (قارئ الريجستري = مرحلة 1).
 *   - Relative إلى RootDirectory → NOT_SUPPORTED (لا جدول مقابض بعد).
 *   - ".." مرفوض (لا هروب خارج جذر ويندوز).
 */
#ifndef NTB0_PATH_H
#define NTB0_PATH_H

#include <stddef.h>

#include "ntb0_nt.h"

/* الحد الأقصى لطول مسار NT (بالحروف) — Win32 حدّه 32767 */
#define NTB0_PATH_MAX 4096

/* الجذر الافتراضي لجذر ويندوز (يُستبدل بـ NTBOX_ROOT) */
#define NTB0_DEFAULT_ROOT "/tmp/opencode/win7root"

/*
 * يثبّت جذر ويندوز (مجلد) ويستخدم openat() عليه.
 * يُعيد 0 عند النجاح، -1 عند الفشل (مع مصباح PATH-001).
 *
 * ملاحظة: الجذر حالة عامة تُثبَّت مرة واحدة عند بدء ntboxd (قبل أي
 * استدعاء syscall) — لا حماية خيوط بعد (المرحلة الحالية: مسار واحد).
 */
int ntb0_path_set_root(const char *root);

/* الجذر الحالي (NULL إن لم يُثبَّت) */
const char *ntb0_path_root(void);

/* fd الخاص بالجذر — للاستعمال مع openat() */
int ntb0_path_dirfd(void);

/*
 * يضمن وجود جذر — يقرأ NTBOX_ROOT أو NTB0_DEFAULT_ROOT عند الحاجة.
 * يُعيد NTSTATUS.
 */
long ntb0_path_ensure_root(void);

/*
 * يترجم UNICODE_STRING (مسار NT) إلى مسار لينكس نسبي للجذر.
 * out_len = حجم المخزن.
 * يُعيد NTB0_ST_SUCCESS عند النجاح، أو NTSTATUS خطأ.
 */
long ntb0_path_from_nt(const ntb0_unicode_string *us, char *out, size_t out_len);

/*
 * يترجم مساراً **نسبياً** (لاستخدامه مع RootDirectory — T-025).
 *
 * قواعد NT:
 *   - المسار الن��بي يجب ألّا يكون مطلقاً (لا \, ولا حرف قرص).
 *   - ".." مرفوض (هروب خارج المجلد) — نفس قاعدة PATH-006.
 *   - الفواصل \ تُحوَّل إلى /.
 *
 * المصباح: PATH-007 (ترجمة مسار نسبي) / PATH-006 (رفض "..").
 */
long ntb0_path_rel_from_nt(const ntb0_unicode_string *us, char *out,
                            size_t out_len);

/* ------------------------------------------------------------------ */
/* الأنابيب المسمّاة (\Device\NamedPipe\...)                            */
/* ------------------------------------------------------------------ */

/* مجلد الأنابيب الافتراضي (يُ overriden بـ NTBOX_PIPE_DIR) */
#define NTB0_PIPE_DIR_DEFAULT "/tmp/opencode/ntbox_pipes"

/*
 * يترجم مسار أنبوب NT (\Device\NamedPipe\Foo) إلى مسار FIFO على لينكس:
 *     <pipe_dir>/Foo
 *   - ينشئ مجلد الأنابيب إن لم يوجد (mkdir -p).
 *   - اسم الأنبوب يُنقّى: كل حرف خارج [A-Za-z0-9_.-] ⇒ '_'
 *     (⇒ لا هروب من المجلد ولا أسماء غريبة).
 *   - المسارات الأخرى (ليست NamedPipe) ⇒ STATUS_OBJECT_PATH_SYNTAX_BAD.
 *
 * المصباح: PATH-008 (ترجمة مسار أنبوب) / PATH-009 (رفض اسم أنبوب).
 */
long ntb0_path_pipe_from_nt(const ntb0_unicode_string *us, char *out,
                            size_t out_len);

/* مجلد الأنابيب الفعّال (env NTBOX_PIPE_DIR أو الافتراضي) */
const char *ntb0_path_pipe_dir(void);

#endif /* NTB0_PATH_H */
