/*
 * disk.h — طبقة "قرص/ملف" (disk namespace) لِ NT-Box (W10 / T-241 … T-249).
 *
 * تفتح جذر ويندوز 7 (C:) وتُوفّر:
 *   - NtCreateFile على NTFS (مسار NT ⇒ ملف لينكس) مع CreateDisposition /
 *     CreateOptions.
 *   - NtQueryDirectoryFile ⇒ قائمة مدخلات مجلد (اسم، صفات، حجم).
 *   - إعادة تسمية / حذف (FileRenameInformation / FileDispositionInformation).
 *   - O_DIRECT / posix_fadvise عند FILE_NO_INTERMEDIATE_BUFFERING.
 *   - مسارات \??\C:\ ⇒ جذر؛ أقراص أخرى ⇒ DEVICE_NOT_CONNECTED.
 *   - جدول ملفات مفتوحة (path ⇒ fd, refcount) لمشاركة الحالة.
 */
#ifndef NTB0_DISK_H
#define NTB0_DISK_H

#include <stddef.h>
#include <stdint.h>

#include "ntb0_nt.h"
#include "ntb0_handle.h"
#include "ntb0_path.h"

/* CreateOptions إضافي غير موجود في ntb0_nt.h */
#define NTB0_FILE_NO_INTERMEDIATE_BUFFERING 0x00000008u

/* صفات ملف إضافية (منطق دالة `dir` — T-242). READONLY/DIRECTORY/NORMAL/
 * REPARSE_POINT موجودة في ntb0_nt.h. */
#ifndef NTB0_FILE_ATTRIBUTE_HIDDEN
#define NTB0_FILE_ATTRIBUTE_HIDDEN  0x00000002u
#endif
#ifndef NTB0_FILE_ATTRIBUTE_SYSTEM
#define NTB0_FILE_ATTRIBUTE_SYSTEM  0x00000004u
#endif
#ifndef NTB0_FILE_ATTRIBUTE_ARCHIVE
#define NTB0_FILE_ATTRIBUTE_ARCHIVE 0x00000020u
#endif

typedef struct ntb0_disk_dir_entry {
    char     name[260];
    uint32_t attributes;
    uint64_t size;
} ntb0_disk_dir_entry;

/* تهيئة: root = NULL ⇒ ensure_root() (NTBOX_ROOT / ./win7root / …) */
int ntb0_disk_init(const char *root);

/* NtCreateFile على NTFS. handle_out يحصل على مقبض (handle table). */
long ntb0_disk_create_file(const ntb0_unicode_string *us,
                           uint32_t desired, uint32_t share,
                           uint32_t disposition, uint32_t create_options,
                           uint64_t *handle_out);

/* إغلاق مقبض + تحديث جدول الملفات المفتوحة */
long ntb0_disk_close(uint64_t handle);

/* NtQueryDirectoryFile: مدخلات مجلد (اسم/صفات/حجم). count_out = عدد مُعبَّأ. */
long ntb0_disk_query_directory(uint64_t dir_handle,
                               ntb0_disk_dir_entry *entries,
                               size_t max_entries, size_t *count_out);

/* FileRenameInformation: إعادة تسمية (new_name = اسم جديد، نفس المجلد). */
long ntb0_disk_rename(uint64_t handle, const char *new_name);

/* FileDispositionInformation: delete_on_close != 0 ⇒ حذف الآن. */
long ntb0_disk_set_disposition(uint64_t handle, int delete_on_close);

/* عدد إدخالات جدول الملفات المفتوحة (refcount > 0). */
size_t ntb0_disk_open_file_count(void);

/* ------------------------------------------------------------------ */
/* W10 المتبقّي: T-245 / T-246 / T-249                                  */
/* ------------------------------------------------------------------ */

/*
 * T-249 — قياس النسخ: عدد البايتات التي نسخها الصندوق بنفسه نيابةً عن
 * نداءات القرص (حالياً: مدخلات استعلام المجلد). مسار قراءة الملف يجب
 * أن يظل 0 بايت دائماً — البيانات تُقرأ من الـ fd مباشرة إلى المخزن
 * الخاص بالمستخدم (نسخة واحدة kernel→user، بلا وسيط).
 */
uint64_t ntb0_disk_copy_bytes(void);

/* T-245 — تقرير مجسّ O_DIRECT */
typedef struct ntb0_disk_odirect_report {
    int      direct_ok;     /* 1 = O_DIRECT فتح + كتابة + قراءة عكسيّة نجحت   */
    int      refused;       /* 1 = مرفوض صادقاً من نظام الملفات (honest)      */
    int      refused_errno; /* errno عند الرفض (EINVAL/EOPNOTSUPP …)         */
    int      fadvise_ok;    /* 1 = posix_fadvise(DONTNEED) أعاد 0             */
    uint64_t probe_bytes;   /* البايتات المكتوبة والمقروءة عكسيّاً بنجاح      */
} ntb0_disk_odirect_report;

/*
 * مجسّ O_DIRECT + posix_fadvise (T-245). يفتح ملفاً مؤقتاً بـ O_DIRECT
 * على جذر القرص، يكتب 4096 بايت، يستدعي posix_fadvise(DONTNEED)، ثم
 * يقرأ عكسيّاً ويتحقّق من المحتوى. النتيجة دائماً "قياس صادق": نجاح
 * (direct_ok) أو رفض صادق (refused + errno). يعيد NTB0_ST_SUCCESS عند
 * نجاح القياس في الحالتين (أو UNSUCCESSFUL عند فشل قياس غير متوقّع).
 */
long ntb0_disk_odirect_probe(ntb0_disk_odirect_report *rep);

#endif /* NTB0_DISK_H */