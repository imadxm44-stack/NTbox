/*
 * pe_export.h — قراءة جدول تصديرات PE (EAT) من صور ويندوز الحقيقية.
 *
 * لماذا هذا الملف؟
 *   NT-Box يحمّل **DLL حقيقية** من ويندوز 7 (kernel32/user32/gdi32…).
 *   المحمّل يحتاج Resolve-by-name (GetProcAddress) ⇒ لا بدّ من قراءة
 *   دليل التصديرات (IMAGE_EXPORT_DIRECTORY) من الملف كما هو على القرص.
 *
 * كل شيء هنا **قراءة فقط** على ملف مُسقَط (mmap) — لا تنفيذ ولا كتابة.
 * كل حدود الملف مُتحقَّق منها (bounds-checked) ⇒ ملف تالف ⇒ خطأ نظيف
 * (لا انهيار، لا تسريب ذاكرة).
 *
 * الصيغ المدعومة: PE32 (Magic 0x10b) و PE32+ (Magic 0x20b) — مقياس على
 * kernel32.dll و user32.dll الحقيقيين من win7root.
 *
 * المصباح: DLL-001 (قراءة جدول التصديرات) — يُطبعه اختبار test_dll.
 */
#ifndef NTB0_PE_EXPORT_H
#define NTB0_PE_EXPORT_H

#include <stddef.h>
#include <stdint.h>

/* بنية الصورة المحمَّلة (Opaque) */
typedef struct ntb0_pe ntb0_pe;

/* تصدير واحد */
typedef struct {
    const char *name;       /* اسم التصدير (ASCII) — NULL إن لم يكن له اسم */
    uint16_t    ordinal;    /* الترتيب = Base + index */
    uint32_t    rva;        /* RVA الدالة (أو RVA سلسلة الـ forwarder) */
    int         forwarded;  /* 1 ⇒ rva يشير إلى سلسلة forwarder */
    const char *forwarder;  /* "KERNEL32.Foo" (أو "KERNEL32.#12") */
} ntb0_pe_export;

/* رموز الخطأ (سلبية) */
#define NTB0_PE_OK        0
#define NTB0_PE_ENOENT   (-1)  /* الملف غير موجود / لا يُفتح */
#define NTB0_PE_EFORMAT  (-2)  /* ليس PE صالحاً / تالف */
#define NTB0_PE_ENOMEM   (-3)  /* لا ذاكرة */
#define NTB0_PE_EIO      (-4)  /* خطأ قراءة/خريطة */

/*
 * يفتح صورة PE (mmap للقراءة فقط) ويحلّل الترويسات + جدول التصديرات.
 * يُعيد NULL عند الفشل (سبب الخطأ في ntb0_pe_last_error).
 */
ntb0_pe *ntb0_pe_open(const char *path);

/* إغلاق وتحرير (آمن مع NULL) */
void ntb0_pe_close(ntb0_pe *pe);

/* آخر رسالة خطأ (مخزَّنة داخلياً) */
const char *ntb0_pe_last_error(void);

/* اسم DLL الداخلي من الترويسة (مثل "KERNEL32.dll") — "" إن لم يوجد */
const char *ntb0_pe_dll_name(const ntb0_pe *pe);

/* مسار الملف كما فُتح */
const char *ntb0_pe_path(const ntb0_pe *pe);

/* 1 ⇒ PE32+ (64 بت) · 0 ⇒ PE32 (32 بت) */
int ntb0_pe_is_64(const ntb0_pe *pe);

/* ImageBase من الترويسة الاختيارية */
uint32_t ntb0_pe_image_base(const ntb0_pe *pe);

/* عدد التصديرات **بأسماء** (NumberOfNames) */
int ntb0_pe_export_count(const ntb0_pe *pe);

/* عدد خانات جدول التصديرات (NumberOfFunctions) — قد يحوي فجوات */
int ntb0_pe_export_slot_count(const ntb0_pe *pe);

/* أول ترتيب (Ordinal Base) */
int ntb0_pe_export_ordinal_base(const ntb0_pe *pe);

/*
 * التصدير رقم index **مرتَّباً بالترتيب** (ordinal).
 * index counted over *all* slots (قد تكون name = NULL).
 * يُعيد 0 عند النجاح و -1 خارج المدى.
 */
int ntb0_pe_export_at(const ntb0_pe *pe, int index, ntb0_pe_export *out);

/*
 * التصدير رقم index **بأسماء فقط** — مرتَّباً بالترتيب.
 * (للطباعة: "أول 10 أسماء مرتَّبة بالترتيب")
 */
int ntb0_pe_export_named_at(const ntb0_pe *pe, int index, ntb0_pe_export *out);

/* بحث بالاسم (GetProcAddress) — 1 عند النجاح */
int ntb0_pe_find_by_name(ntb0_pe *pe, const char *name, ntb0_pe_export *out);

/* بحث بالترتيب — 1 عند النجاح */
int ntb0_pe_find_by_ordinal(ntb0_pe *pe, uint16_t ordinal, ntb0_pe_export *out);

/* RVA ⇒ إزاحة في الملف (0 عند النجاح) */
int ntb0_pe_rva_to_offset(const ntb0_pe *pe, uint32_t rva, uint32_t *off);

/* حجم الملف بالبايت */
size_t ntb0_pe_file_size(const ntb0_pe *pe);

#endif /* NTB0_PE_EXPORT_H */
