/*
 * ntboxd_libc.h — طبقة C صغيرة (استيرادات msvcrt.dll) لِ ntboxd.
 *
 * المصابيح: LIB-001..LIB-003
 *   LIB-001 OK   — استيراد msvcrt مُحلّ إلى تنفيذ حقيقي
 *   LIB-002 OK   — استيراد msvcrt غير معروف ⇒ stub عام
 */
#ifndef NTBOXD_LIBC_H
#define NTBOXD_LIBC_H

#include <stddef.h>

/* يبحث عن تنفيذ بالاسم (msvcrt) — NULL إن لم يُعرف */
void *ntboxd_libc_lookup(const char *name);

/* عدد الدوال المتاحة (تشخيص) */
size_t ntboxd_libc_count(void);

#endif /* NTBOXD_LIBC_H */
