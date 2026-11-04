/*
 * ntb0_reg.h — جسر الريجستري في مترجم الـ syscalls (T-110 … T-114).
 *
 * الفكرة:
 *   كود ويندوز (advapi32/نواة) يفتح مفتاحاً والمسار
 *       \Registry\Machine\SYSTEM\ControlSet001\Services\RpcSs
 *   ⇒ المترجم يحوّله إلى: خلية SYSTEM + مسار "ControlSet001\Services\RpcSs"
 *   ⇒ ثم يقرأ من **الخلية الحقيقية** (قارئ hive.c) ويحوّل النتيجة
 *     إلى بنى NT (KEY_VALUE_INFORMATION …) التي يتوقعها كود ويندوز.
 *
 * المصابيح: SYS-049 (NtOpenKey) · SYS-050 (NtQueryValueKey)
 *            SYS-051/052 (التعداد) · SYS-053 (NtFlushKey)
 */
#ifndef NTB0_REG_H
#define NTB0_REG_H

#include <stdint.h>

#include "ntb0_nt.h"

/* أصناف KEY_VALUE_INFORMATION_CLASS */
#define NTB0_KVI_BASIC           0u
#define NTB0_KVI_FULL            1u
#define NTB0_KVI_PARTIAL         2u
#define NTB0_KVI_PARTIAL_ALIGN64 3u

/* أصناف KEY_ENUMERATION_INFORMATION_CLASS */
#define NTB0_KEI_BASIC   0u
#define NTB0_KEI_FULL    1u
#define NTB0_KEI_PARTIAL 2u

/* يفتح مفتاحاً من مسار NT ⇒ مقبض جديد (نوعه NTB0_HK_REGKEY) */
long ntb0_reg_open_key(uint64_t *handle_out, const ntb0_unicode_string *path);

/* يقرأ قيمة (NtQueryValueKey) */
long ntb0_reg_query_value(uint64_t handle, const ntb0_unicode_string *name,
                          uint32_t cls, void *buf, uint32_t len, uint32_t *ret);

/* يعداد المفاتيح الفرعية (NtEnumerateKey) */
long ntb0_reg_enum_key(uint64_t handle, uint32_t cls, uint32_t index, void *buf,
                       uint32_t len, uint32_t *ret);

/* يعداد القيم (NtEnumerateValueKey) */
long ntb0_reg_enum_value(uint64_t handle, uint32_t cls, uint32_t index,
                         void *buf, uint32_t len, uint32_t *ret);

/* تفريغ (NtFlushKey) — للقراءة فقط ⇒ لا عمل */
long ntb0_reg_flush(uint64_t handle);

/* T-107 — تحميل ملف خلية كخلية تطبيق (NtLoadKey/RegLoadAppKey) */
long ntb0_reg_load_app_key(uint64_t *handle_out, const char *path);

/* T-109 — كتابة قيمة في تراكب الذاكرة (NtSetValueKey) — تفنى بإغلاق الخلية */
long ntb0_reg_set_value(uint64_t handle, const ntb0_unicode_string *name,
                        uint32_t type, const void *data, uint32_t size);

/* T-108 — إنشاء مفتاح رمزي REG_LINK تحت المفتاح (NtSetValueKey بمصفوفة 6) */
long ntb0_reg_set_link(uint64_t handle, const ntb0_unicode_string *name,
                       const ntb0_unicode_string *target);

#endif /* NTB0_REG_H */
