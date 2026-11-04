/*
 * dll_loader.h — محمّل DLLs الحقيقية لويندوز 7 في فضاء المستخدم.
 *
 * لماذا هذا الملف؟
 *   NT-Box سيشغّل **Win7 حقيقياً** داخل الصندوق. الآلية الموجودة
 *   (ntb0_trap.c) يرسم صورة PE ويعدّل ستوبات الـ syscall **في مكانها**
 *   داخل ntdll/kernel32 الحقيقيين. لكن قبل ذلك يجب أن تُحمَّل الصورة:
 *     1) رسم الأقسام (sections) على العنوان الأساسي المفضّل (ImageBase)
 *     2) تطبيق **إعادة التوجيه** (base relocations — دليل .reloc)
 *     3) حلّ **جدول الاستيراد** (imports) مقابل *مُحلِّل خارجي* (callback)
 *        — وليس dynamic linker الخاص بالمضيف.
 *
 * ملاحظات تصميمية (مهمة):
 *   - **لا تنفيذ لأي كود Win7**: المحمّل يرسم + ينقل + يحلّ فقط.
 *     استدعاء DllMain أو ستوب syscall على Linux = انهيار فوري.
 *   - **لا dynamic linking للمضيف**: كل استيراد يُمرَّر إلى
 *     `ntb0_dll_resolve_fn` (صندوقنا هو من يقرّر العنوان).
 *   - **كل المؤشرات محروسة**: المحمّل يقرأ ملفات ثنائية غير موثوقة ⇒
 *     كل RVA وكل اسم وكل IAT slot يُفحص قبل الاستخدام. ملف تالف ⇒
 *     رفض نظيف (لا انهيار، لا تسريب ذاكرة).
 *   - **api-set**: أسماء `api-ms-win-*` تُمرَّر أولاً إلى محلّل
 *     api-set (apiset.c) لتحويلها إلى DLL مضيف حقيقي.
 *
 * المصابيح (Tracing) — نفس نمط ntb0_trace.h (نسخة محلية هنا؛ الملف
 * الأصلي لم يُعدَّل):  "[ntbox][DLL-00X][OK] ..."
 */
#ifndef NTB0_DLL_LOADER_H
#define NTB0_DLL_LOADER_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "pe_export.h"

/* بنية محلّل api-set (مُستعارة من apiset.h — لا نُضمّنه هنا) */
typedef struct ntb0_apiset ntb0_apiset;

/* ------------------------------------------------------------------ */
/* رموز الخطأ                                                         */
/* ------------------------------------------------------------------ */
#define NTB0_DLL_OK            0
#define NTB0_DLL_ENOENT      (-1)   /* الملف غير موجود / لا يُفتح      */
#define NTB0_DLL_EFORMAT     (-2)   /* PE تالف / غير مدعوم            */
#define NTB0_DLL_ENOMEM      (-3)   /* لا ذاكرة                       */
#define NTB0_DLL_EIO         (-4)   /* خطأ قراءة                      */
#define NTB0_DLL_ERELOC      (-5)   /* دليل إعادة التوجيه تالف        */
#define NTB0_DLL_EIMPORTS    (-6)   /* دليل الاستيراد تالف            */
#define NTB0_DLL_EBASE       (-7)   /* تعذّر رسم الصورة على العنوان   */
#define NTB0_DLL_ENOTFOUND   (-8)   /* لا يوجد تصدير / وحدة            */
#define NTB0_DLL_EEXISTS     (-9)   /* الوحدة محمَّلة مسبقاً           */
#define NTB0_DLL_EBUSY      (-10)   /* مراجع قائمة (قيد الاستخدام)     */

/* ------------------------------------------------------------------ */
/* الأعلام                                                            */
/* ------------------------------------------------------------------ */
#define NTB0_DLL_FLAG_FIXED_BASE     (1u << 0) /* يجب استعمال ImageBase */
#define NTB0_DLL_FLAG_STRICT_IMPORTS (1u << 1) /* استيراد غير محلول = فشل */
#define NTB0_DLL_FLAG_PREFER_LOCAL   (1u << 2) /* جرّب الوحدات المحمَّلة أولاً */
#define NTB0_DLL_FLAG_AUTO_DEPS      (1u << 3) /* حمّل الاعتماديات تلقائياً */
#define NTB0_DLL_FLAG_NO_RELOCS      (1u << 4) /* تجاهل دليل .reloc      */
/*
 * البوّابة الصريحة لتنفيذ DllMain/TLS callbacks (T-131/T-132):
 * القاعدة الصارمة للمحمّل: **كود Win7 لا يُنفَّذ** — افتراضياً لا
 * DllMain ولا TLS callbacks. ‎NTB0_DLL_FLAG_RUN_ENTRY يفتح الباب
 * فقط لصاحب التحميل الذي يطلب الدعوة (كودنا الاختباري)، وبشرط
 * الحصانة: استيرادات محلولة بالكامل + نقطة دخول داخل قسم تنفيذي.
 * مُقاس: usp10.dll+0x7aa65 (كود حقيقي + IAT فيه أصفار) ⇒ اندثار
 * عبر نداء NULL — من هنا القاعدة.
 */
#define NTB0_DLL_FLAG_RUN_ENTRY      (1u << 5) /* نفّذ DllMain/TLS (أمين) */

/* ------------------------------------------------------------------ */
/* مُحلِّل الاستيراد (callback خارجي)                                  */
/* ------------------------------------------------------------------ */
/*
 *   dll  : اسم الـ DLL المطلوب (بعد تمرير api-set إن لزم)
 *   func : اسم الدالة، أو "#<الترتيب>" للاستيراد **بالترتيب** (ordinal)
 *   out  : يُملأ بالعنوان النهائي (mapped address)
 * يُعيد 0 عند النجاح، أو !=0 عند الفشل (يُحتسب استيراداً غير محلول).
 *
 * ملاحظة: صيغة "#<n>" هي اصطلاح ويندوز نفسه (نفسه المستعمل في
 * سلاسل الـ forwarders) ⇒ callback واحدة تخدم الاسم والترتيب.
 */
typedef int (*ntb0_dll_resolve_fn)(void *ctx, const char *dll, const char *func,
                                   uint64_t *out_addr);

/* ------------------------------------------------------------------ */
/* سجلّ استيراد واحد                                                   */
/* ------------------------------------------------------------------ */
typedef struct {
    char     dll[128];      /* اسم الـ DLL كما في جدول الاستيراد      */
    char     host[128];     /* DLL المضيف بعد حلّ api-set (= dll غالباً) */
    char     func[192];     /* اسم الدالة أو "#<ترتيب>"              */
    uint32_t iat_rva;       /* RVA خانة الـ IAT التي كُتب فيها       */
    uint16_t ordinal;       /* الترتيب (0 ⇒ استيراد بالاسم)           */
    int      by_ordinal;    /* 1 ⇒ استيراد بالترتيب                   */
    int      resolved;      /* 1 ⇒ حُلّ بنجاح                        */
    uint64_t addr;          /* العنوان الذي حُلّ إليه                 */
} ntb0_dll_import;

/* ------------------------------------------------------------------ */
/* الوحدة المحمَّلة                                                     */
/* ------------------------------------------------------------------ */
typedef struct ntb0_dll_module ntb0_dll_module;

/* ------------------------------------------------------------------ */
/* المحمّل                                                              */
/* ------------------------------------------------------------------ */
typedef struct ntb0_dll_loader ntb0_dll_loader;

/* ينشئ محمّلاً (يُعيد NULL عند فشل التخصيص) */
ntb0_dll_loader *ntb0_dll_loader_new(void);

/* يحرّر: يفكّ كل الوحدات ثم الكائن (آمن مع NULL) */
void ntb0_dll_loader_free(ntb0_dll_loader *ld);

/* يضبط مُحلِّل الاستيراد (callback) — بلا مُحلِّل ⇒ كل الاستيرادات
 * تبقى غير محلول (تُحتسب) ولا يفشل التحميل (إلا مع STRICT) */
int ntb0_dll_loader_set_resolver(ntb0_dll_loader *ld, ntb0_dll_resolve_fn fn,
                                 void *ctx);

/* محلّل api-set (مُستعار — لا يملكه المحمّل) */
int ntb0_dll_loader_set_apiset(ntb0_dll_loader *ld, ntb0_apiset *as);

/* مجلد البحث عن الـ DLLs (مثل Win7root/Windows/System32) */
int ntb0_dll_loader_set_sysdir(ntb0_dll_loader *ld, const char *dir);

/* مجلد SysWOW64 للـ DLLs 32-bit (مثل Win7root/Windows/SysWOW64) — peer to sysdir */
int ntb0_dll_loader_set_syswow64_sysdir(ntb0_dll_loader *ld, const char *dir);

/*
 * يحمّل DLL من مسار كامل.
 *   flags ⇒ NTB0_DLL_FLAG_* (0 = السلوك الافتراضي).
 * يُعيد الوحدة أو NULL (سبب الخطأ في ntb0_dll_last_error).
 */
ntb0_dll_module *ntb0_dll_load(ntb0_dll_loader *ld, const char *path,
                                unsigned flags);

/*
 * يحمّل DLL على **عنوان محدّد** (base != 0 ⇒ العنوان الإجباري؛
 * base == 0 ⇒ العنوان الأساسي المفضّل إن أمكن، وإلا أي عنوان).
 * مفيد للصندوق: اختيار عنوان ثابت لوحدة (مثل ntdll) أو تجنّب
 * تصادم العناوين المفضّلة.
 */
ntb0_dll_module *ntb0_dll_load_at(ntb0_dll_loader *ld, const char *path,
                                   uint64_t base, unsigned flags);

/* Dedicated PE image loader path (supports both PE32 and PE32+) */
ntb0_dll_module *ntb0_pe_load_image(ntb0_dll_loader *ld, const char *path,
                                    uint64_t base, unsigned flags);
ntb0_dll_module *ntb0_pe32_load_image(ntb0_dll_loader *ld, const char *path,
                                      uint64_t base, unsigned flags);

/*
 * شبيه LoadLibrary: يبحث عن الملف في sysdir (أو مسار مطلق)؛ وإن كانت
 * الوحدة محمَّلة مسبقاً ⇒ يعيدها (مع عدّ المراجع) بدون تحميل جديد.
 */
ntb0_dll_module *ntb0_dll_load_name(ntb0_dll_loader *ld, const char *dllname,
                                     unsigned flags);

/*
 * يفكّ الوحدة (munmap + free) ويزيلها من القائمة.
 * وحدة مرجَعة أكثر من مرّة (LoadLibrary عدّة مرّات) تبقى محمَّلة حتى
 * يستهلك كل المراجع (دلالة FreeLibrary في ويندوز).
 */
int ntb0_dll_unload(ntb0_dll_loader *ld, ntb0_dll_module *mod);

/* GetProcAddress: اسم ⇒ عنوان الدالة **في الصورة المرسومة** */
void *ntb0_dll_get_proc(ntb0_dll_loader *ld, ntb0_dll_module *mod,
                        const char *name);

/* GetProcAddress بالترتيب */
void *ntb0_dll_get_proc_ord(ntb0_dll_loader *ld, ntb0_dll_module *mod,
                            uint16_t ordinal);

/* قائمة الوحدات المحمَّلة */
int           ntb0_dll_module_count(const ntb0_dll_loader *ld);
ntb0_dll_module *ntb0_dll_module_at(ntb0_dll_loader *ld, int idx);
/* يبحث عن وحدة بالاسم (غير حسّاس لحالة الأحرف) */
ntb0_dll_module *ntb0_dll_find_module(ntb0_dll_loader *ld, const char *name);

/* سجلّات الاستيراد لوحدة */
int ntb0_dll_import_count(const ntb0_dll_module *mod);
int ntb0_dll_import_at(const ntb0_dll_module *mod, int idx,
                       ntb0_dll_import *out);

/* الاعتماديات (أسماء الـ DLLs المستورَدة) */
int         ntb0_dll_dep_count(const ntb0_dll_module *mod);
const char *ntb0_dll_dep_at(const ntb0_dll_module *mod, int idx);

/* عدد الدورات المرصودة في شجرة الاعتماديات (T-126) */
unsigned ntb0_dll_cycle_count(const ntb0_dll_loader *ld);

/* آخر رسالة خطأ + نص رمز الخطأ */
const char *ntb0_dll_last_error(const ntb0_dll_loader *ld);
const char *ntb0_dll_strerror(int rc);

/* information about the image */
uint64_t ntb0_dll_module_base(const ntb0_dll_module *mod);
uint64_t ntb0_dll_module_pref_base(const ntb0_dll_module *mod);
int64_t  ntb0_dll_module_delta(const ntb0_dll_module *mod);
size_t   ntb0_dll_module_size(const ntb0_dll_module *mod);
int      ntb0_dll_module_is64(const ntb0_dll_module *mod);
int      ntb0_dll_module_nsec(const ntb0_dll_module *mod);
const char *ntb0_dll_module_path(const ntb0_dll_module *mod);
const char *ntb0_dll_module_name(const ntb0_dll_module *mod);
unsigned ntb0_dll_module_reloc_blocks(const ntb0_dll_module *mod);
unsigned ntb0_dll_module_reloc_entries(const ntb0_dll_module *mod);
unsigned ntb0_dll_module_reloc_applied(const ntb0_dll_module *mod);
unsigned ntb0_dll_module_reloc_absolute(const ntb0_dll_module *mod);
unsigned ntb0_dll_module_reloc_unsupported(const ntb0_dll_module *mod);
unsigned ntb0_dll_module_imports_resolved(const ntb0_dll_module *mod);
unsigned ntb0_dll_module_imports_unresolved(const ntb0_dll_module *mod);
/* عدد الـ DLLs المستورَدة (عدد واصفات الاستيراد) */
unsigned ntb0_dll_module_import_dll_count(const ntb0_dll_module *mod);
/* عدد أسماء api-ms-win-* التي شوهدت في جدول الاستيراد */
unsigned ntb0_dll_module_apiset_count(const ntb0_dll_module *mod);
/* نقطة الدخول (RVA) */
uint32_t ntb0_dll_module_entry_rva(const ntb0_dll_module *mod);

/* RVA ⇒ عنوان داخل الصورة المرسومة (0 إن كان خارجها) */
uint64_t ntb0_dll_module_rva_to_addr(const ntb0_dll_module *mod, uint32_t rva);

/* DllMain/TLS callbacks (T-131/T-132) */
void    *ntb0_dll_module_dllmain(const ntb0_dll_module *mod);
int      ntb0_dll_module_dllmain_called(const ntb0_dll_module *mod);
unsigned ntb0_dll_module_tls_callback_count(const ntb0_dll_module *mod);
uint64_t ntb0_dll_module_tls_callback_at(const ntb0_dll_module *mod, unsigned idx);
int      ntb0_dll_module_is_system(const ntb0_dll_module *mod);

/*
 * يجعل الصورة كاملة قابلة للكتابة (لتصحيح ستوبات الـ syscall **في
 * مكانها** — انظر ntb0_trap.c). يُعيد 0 عند النجاح.
 */
int ntb0_dll_module_make_writable(ntb0_dll_module *mod);

/* يعيد صلاحيات الأقسام الأصلية (بعد make_writable) */
int ntb0_dll_module_restore_protections(ntb0_dll_module *mod);

/* يستخرج الترتيب من صيغة "#<n>" (-1 إن لم تكن صيغة ترتيب) */
int ntb0_dll_ordinal_parse(const char *func);

/* ------------------------------------------------------------------ */
/* نمط المصباح (نسخة محلية — ntb0_trace.h لم يُعدَّل)                */
/* ------------------------------------------------------------------ */
#define NTB0_DLL_LTP(id, st, fmt, ...) \
    fprintf(stderr, "[ntbox][%s][%s] " fmt "\n", (id), (st), ##__VA_ARGS__)

#define NTB0_DLL_LOK(id, fmt, ...)   NTB0_DLL_LTP(id, "OK", fmt, ##__VA_ARGS__)
#define NTB0_DLL_LFAIL(id, fmt, ...) NTB0_DLL_LTP(id, "FAIL", fmt, ##__VA_ARGS__)
#define NTB0_DLL_LSTUB(id, fmt, ...) NTB0_DLL_LTP(id, "STUB", fmt, ##__VA_ARGS__)

#endif /* NTB0_DLL_LOADER_H */
