/*
 * apiset.h — محلّل أسماء api-set ("api-ms-win-core-file-l1-2-0.dll") إلى
 *            DLL حقيقي ("kernel32.dll").
 *
 * لماذا؟
 *   DLLs ويندوز 7 الحقيقية **تستورد** أسماء api-set:
 *     kernel32.dll  ⇒ API-MS-Win-Core-RtlSupport-L1-1-0.dll
 *     shell32.dll   ⇒ API-MS-Win-Core-File-L1-1-0.dll
 *     msvcrt.dll    ⇒ API-MS-Win-Core-Console-L1-1-0.dll
 *   ⇒ لو لم يُحلَّل اسم api-set لاسم DLL حقيقي، لا تُحمَّل هذه DLLs إطلاقاً.
 *
 * ثلاثة مصادر بالترتيب:
 *   1) SCHEMA  — مخطّط api-set من الريجستري (SOFTWARE hive) إن وُجد.
 *                صيغته الثنائية موثّقة في apiset.c (magic "APISET").
 *   2) WIN7    — **مشتق من الشجرة الحقيقية**: على ويندوز 7 يوجد
 *                276 ملف stub باسم api-ms-win-*.dll داخل win7root، وكل
 *                stub يحمل *قائمة الأسماء* التي يوفّرها العقد ⇒ نبحث عن
 *                الوحدة التي تُصدّر نفس الأسماء ⇒ المضيف الحقيقي.
 *                (مقيس: api-ms-win-core-file-l1-1-0.dll (76 تصديراً)
 *                 ⇒ kernel32.dll يصدّر 76/76 ⇒ المضيف = kernel32.dll.)
 *   3) FALLBACK— جدول ثابت **احتياطي** (موثّق بوضوح في apiset.c) للعقود
 *                التي لا وجود لها في ويندوز 7 (l2-0-0، crt-* …).
 *
 * تنبيه مهمّ (الأمانة العلمية):
 *   شجرة ويندوز 7 المستعملة في NT-Box **لا تحتوي** مخطّط api-set في
 *   الريجستري (لا قيمة "apisetschema" في خلية SOFTWARE — تم الفحص بالبايت
 *   على الخلية نفسها). لذلك المسار (1) غير مُفعَّل في هذه الشجرة، والمصدر
 *   الفعّال هو (2) ثم (3). لا يدّعي هذا الملف أن المخطّط مُحلَّل إن لم يكن.
 *
 * المصابيح: DLL-009 (حلّ العقد) · DLL-010 (قراءة apisetschema من الخلية).
 */
#ifndef NTB0_APISET_H
#define NTB0_APISET_H

#include <stddef.h>
#include <stdint.h>

/* بنية المحلّل (Opaque) */
typedef struct ntb0_apiset ntb0_apiset;

/* مصدر النتيجة */
typedef enum {
    NTB0_APISET_SRC_NONE = 0,  /* لم يُحلَّل */
    NTB0_APISET_SRC_SCHEMA,    /* من مخطّط الريجستري (مُحلَّل) */
    NTB0_APISET_SRC_WIN7,      /* مشتق من ملفات ويندوز 7 الحقيقية */
    NTB0_APISET_SRC_FALLBACK   /* جدول ثابت (احتياطي — موثّق) */
} ntb0_apiset_src;

/* نتيجة حلّ عقدة واحدة */
typedef struct {
    char           host[64];      /* اسم DLL المضيف ("kernel32.dll") */
    ntb0_apiset_src src;          /* من أين جاءت النتيجة */
    int            have_stub;     /* 1 ⇒ ملف العقد نفسه موجود على القرص */
    char           stub_path[512];/* مسار ذلك الملف (إن وُجد) */
    char           note[160];     /* ملاحظة قصيرة (مصدر/سبب) */
} ntb0_apiset_result;

/* رموز الخطأ */
#define NTB0_APISET_OK           0
#define NTB0_APISET_ENOENT     (-1)  /* عقدة غير معروفة */
#define NTB0_APISET_ENOSCHEMA  (-2)  /* لا يوجد مخطّط apisetschema في الخلية */
#define NTB0_APISET_EBADMAGIC  (-3)  /* الكتلة ليست بصيغة APISET */
#define NTB0_APISET_EFORMAT    (-4)  /* بنية المخطّط غير مفهومة */
#define NTB0_APISET_ENOMEM     (-5)
#define NTB0_APISET_EIO        (-6)  /* الخلية أو الملف غير قابل للقراءة */

/* ------------------------------------------------------------------ */
/* صيغة مخطّط api-set (APISET)                                         */
/* ------------------------------------------------------------------ */
/*
 * الترويسة (56 بايت، كلها little-endian):
 *   0x00  6  magic "APISET"
 *   0x06  2  version
 *   0x08  4  flags
 *   0x0C  4  عدد العقد (entries)
 *   0x10  4  إزاحة جدول أسماء العقد
 *   0x14  4  إزاحة جدول أسماء المضيفين
 *   0x18  4  إزاحة جدول سلاسل flags
 *   0x1C  4  إزاحة جدول تجزئة الأسماء (hash → index)
 *   0x20  4  إزاحة جدول تجزئة المضيفين
 *   0x24  4  حجم جدول الأسماء
 *   0x28  4  حجم جدول المضيفين
 *   0x2C  4  حجم جدول flags
 *   0x30  4  حجم جدول تجزئة الأسماء
 *   0x34  4  حجم جدول تجزئة المضيفين
 * جداول النصوص: سلاسل ASCII منتهية بـ NUL متلاصقة.
 * جداول التجزئة: أزواج (u32 hash, u32 index).
 *
 * تنبيه: هذا الوصف مُستند إلى وصف ملف المهمة (W5/T-127) — **لم يُقارن**
 * بكتلة Microsoft حقيقية (لا وجود لها في شجرة ويندوز 7 لدينا ولا شبكة
 * للوصول إليها). لذلك يفحص المحلّل الـ magic والحدود، ويُعيد خطأً واضحاً
 * (EBADMAGIC/EFORMAT) إن لم يطابق التخطيط بدل التخمين.
 */
#define NTB0_APISET_MAGIC        "APISET"
#define NTB0_APISET_MAGIC_LEN    6
#define NTB0_APISET_HDR_SIZE     56

#define NTB0_APISET_OFF_VERSION    6
#define NTB0_APISET_OFF_FLAGS      8
#define NTB0_APISET_OFF_COUNT     12
#define NTB0_APISET_OFF_NAMES     16
#define NTB0_APISET_OFF_HOSTS     20
#define NTB0_APISET_OFF_FLAGSSTR  24
#define NTB0_APISET_OFF_NAMEHASH  28
#define NTB0_APISET_OFF_HOSTHASH  32
#define NTB0_APISET_OFF_NAMESZ    36
#define NTB0_APISET_OFF_HOSTSZ    40
#define NTB0_APISET_OFF_FLAGSZ    44
#define NTB0_APISET_OFF_NAMEHASHSZ 48
#define NTB0_APISET_OFF_HOSTHASHSZ 52

/* ------------------------------------------------------------------ */
/* الواجهة                                                            */
/* ------------------------------------------------------------------ */

/* ينشئ محلّلاً (يُعيد NULL عند فشل التخصيص) */
ntb0_apiset *ntb0_apiset_new(void);

/* يحرّر (آمن مع NULL) */
void ntb0_apiset_free(ntb0_apiset *a);

/*
 * يحدّد مجلد System32 في شجرة ويندوز (يُستعمل للاشتقاق من الملفات
 * الحقيقية). لا يجب استدعاؤه إن كان المطلوب استخدام المخطّط فقط.
 */
int ntb0_apiset_set_sysdir(ntb0_apiset *a, const char *dir);

/*
 * يقرأ مخطّط api-set من خلية SOFTWARE:
 *   "Microsoft\Windows NT\CurrentVersion\apisetschema" (قيمة REG_BINARY)
 * أو المفتاح ...\apisetschema (قيمة "apisetschema" أو الافتراضية).
 * يُعيد 0 عند النجاح، أو رمز خطأ.
 * (يستعمل قارئ hive.c الموجود — لا تعديل عليه.)
 */
int ntb0_apiset_load_schema(ntb0_apiset *a, const char *software_hive);

/* يحلّل كتلة مخطّط (APISET) مباشرة — 0 عند النجاح */
int ntb0_apiset_parse_blob(ntb0_apiset *a, const uint8_t *blob, uint32_t size);

/* 1 ⇒ المخطّط محمَّل فعلاً */
int ntb0_apiset_schema_loaded(const ntb0_apiset *a);

/* وصف قصير لحالة المخطّط (للتقرير) */
const char *ntb0_apiset_status(const ntb0_apiset *a);

/*
 * يحلّ عقدة ⇒ النتيجة.
 * يُعيد 0 عند النجاح، أو NTB0_APISET_ENOENT إن لم تُعرف العقدة.
 */
int ntb0_apiset_resolve(ntb0_apiset *a, const char *contract,
                         ntb0_apiset_result *out);

/* اسم المصدر كنص ("schema" / "win7-derived" / "fallback-table" / "none") */
const char *ntb0_apiset_src_name(ntb0_apiset_src s);

/*
 * يستخرج "العائلة" من اسم العقد (يحذف اللاحقة -lX-Y-Z و .dll):
 *   "api-ms-win-core-file-l1-2-0.dll" ⇒ "api-ms-win-core-file"
 * يُعيد buf أو NULL.
 */
const char *ntb0_apiset_family(const char *contract, char *buf, size_t len);

#endif /* NTB0_APISET_H */
