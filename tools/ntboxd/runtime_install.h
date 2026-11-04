/*
 * runtime_install.h — مثبّت بيئة التشغيل (Java / VC++ / .NET) لِ NT-Box.
 *
 * يُسجّل مثبّتات بيئة التشغيل داخل شجرة Win7 (نسخها إلى
 * win7root/ntbox/runtimes/<name>/) ويُحدّث manifest at
 * win7root/ntbox/runtimes.json. الـ hive للقراءة فقط ⇒ لا نكتب في
 * الريجستري؛ الـ manifest هو مصدر الحقيقة.
 *
 * Lamps: RT-001 (list), RT-002 (install), RT-003 (run), RT-004 (uninstall).
 */
#ifndef NTB0_RUNTIME_INSTALL_H
#define NTB0_RUNTIME_INSTALL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ntb0_runtime {
    const char *name;              /* مفتاح قصير (java, vcpp2010, dotnet4x, ...) */
    const char *display_name;      /* اسم عرض */
    const char *category;          /* "java" / "vcpp" / "dotnet" */
    const char *installer_pattern; /* نمط اسم ملف المثبّت */
} ntb0_runtime_t;

/* قائمة بيئة التشغيل المعروفة (مصفوفة ثابتة). count_out اختياري. */
const ntb0_runtime_t *ntb0_runtime_list(size_t *count_out);
int ntb0_runtime_count(void);
const ntb0_runtime_t *ntb0_runtime_find(const char *name);

/* تثبيت: نسخ installer_path إلى <root>/ntbox/runtimes/<name>/ + تحديث manifest.
 * root NULL ⇒ ntb0_hive_win7_root(). يُعيد 0 / -1. */
int ntb0_runtime_install(const char *root, const char *name,
                         const char *installer_path);

/* هل مثبّت؟ 1 نعم، 0 لا (أو خطأ). */
int ntb0_runtime_is_installed(const char *root, const char *name);

/* إلغاء: حذف الإدخال + الملفات. يُعيد 0 / -1. */
int ntb0_runtime_uninstall(const char *root, const char *name);

/* تشغيل: exec ntboxd على ملف المثبّت المنسوخ، إعادة رمز الخروج
 * (أو -1 عند فشل الإطلاق). */
int ntb0_runtime_run(const char *root, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* NTB0_RUNTIME_INSTALL_H */