/*
 * ntb0_trap — مصيدة syscalls لنسخة ntdll.dll الحقيقية (Windows 7 x64).
 *
 * الفكرة (NTBOX.md §10):
 *   كود ويندوز (تطبيق + DLLs) يعمل **مباشرة**. لا نترجم 400 دالة API.
 *   نلتقط الـ syscallStub لحظة الطلب — عندها فقط يقع الترجمة.
 *
 * الآلية (مُقاسة على ntdll الحقيقي):
 *   1. نحمل صورة ntdll كما هي في ذاكرة المستخدم.
 *   2. نضع الـ trampoline في نفس الـ mmap (شرط: مسافة < 2GB لأن القفزة rel32).
 *   3. نمسح قسم .text ونبحث عن نمط الـ stub:
 *          4C 8B D1 | B8 <num32> | 0F 05
 *   4. لكل تطابق نكتب:
 *          E9 <rel32> | 90 90 90
 *      (8 بايت — البايتات 8..15 من خانة الـ stub الـ16-بايتية حرة)
 *   5. الـ trampoline يقرأ رقم syscall والـ arguments ويسلّمها للمترجم.
 *
 * لماذا التعديل **في مكانه** وليس بتعديل EAT:
 *   قِسنا: 133 من الـ 398 stub تُستدعى **داخلياً** من ntdll نفسه
 *   (1217 موقع استدعاء E8). تعديل EAT وحده كان سيتركها تتجاوز المصيدة.
 *   التعديل في مكانه يلتقط العمليتين (EAT + الاستدعاء الداخلي).
 */
#ifndef NTB0_TRAP_H
#define NTB0_TRAP_H

#include <stddef.h>
#include <stdint.h>

struct ntb0_image {
    void        *base;           /* قاعدة صورة PE المرسومة */
    size_t       size;           /* SizeOfImage */
    void        *tramp;          /* قفزة الاعتراض (داخل نفس الـ mmap) */
    size_t       text_off;       /* RVA قسم .text */
    size_t       text_size;      /* حجم قسم .text */
    unsigned int stubs_found;    /* عدد الأنماط المكتشفة */
    unsigned int stubs_patched;  /* عدد ما تم تعديله فعلياً */
    unsigned int tramp_size;     /* حجم كود القفزة */
};

/*
 * يرسم صورة PE كاملة في ذاكرة المستخدم مع قفزة اعتراض في نفس الـ mmap.
 * المسار: مسار ملف DLL على القرص.
 * يُعيد 0 عند النجاح.
 */
int ntb0_map_pe(const char *path, struct ntb0_image *img);

/*
 * يبحث داخل صورة محمّلة عن كل syscall stub ويعدّلها في مكانها.
 * يجب أن يكون img->tramp على بعد أقل من 2GB من img->base.
 */
int ntb0_trap_patch(struct ntb0_image *img);

/*
 * تعديل عام لصورة محمّلة (base/size) — يستخدم ntb0_trap_patch داخلياً.
 * يُعيد عدد الـ stubs المعدّلة، أو -1 عند الخطأ.
 */
long ntb0_patch_stubs(void *base, size_t size, void *tramp);

/*
 * كتابة قفزة الاعتراض في ذاكرة معيّنة (T-063: when the image is mapped by
 * the DLL loader, the trampoline must live *near* the image, not inside it).
 * العنوان يجب أن يكون ضمن Â±2GB عن الصورة (قيد E9 rel32).
 */
int ntb0_trampoline_write(void *addr);

#endif /* NTB0_TRAP_H */
