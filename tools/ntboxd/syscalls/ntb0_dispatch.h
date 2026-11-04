/*
 * ntb0_dispatch.h — واجهة المترجم (dispatcher).
 *
 * المترجم هو **جسر** بين كود ويندوز في وضع المستخدم ونواة لينكس.
 * يستقبل الطلبات الطرفية (leaf) فقط: موارد حقيقية (ملف/ذاكرة/مقبض).
 * استدعاء DLL داخل DLL يبقى مباشراً ولا يمرّ من هنا.
 */
#ifndef NTB0_DISPATCH_H
#define NTB0_DISPATCH_H

/*
 * نقطة الدخول من ntb0_trampoline.S.
 * num = رقم syscall (rax عند الدخول إلى القفزة)
 * a1..a4 = r10, rdx, r8, r9
 * a5     = [rsp+0x28] عند الدخول  ← في Win64 يُمرَّر على المكدس
 * a6..a11 = [rsp+0x30] .. [rsp+0x58]  ← بقية معاملات NT على المكدس
 * تُعيد NTSTATUS في rax.
 */
long ntb0_dispatch(long num, long a1, long a2, long a3, long a4, long a5,
                   long a6, long a7, long a8, long a9, long a10, long a11);

/*
 * خطّاف تشخيصي — للاختبار فقط.
 * إن كان non-NULL يُستدعى عند كل وصول للمترجم قبل المعالجة.
 */
typedef void (*ntb0_probe_fn)(long num, long a1, long a2, long a3, long a4,
                              long a5, long a6, long a7, long a8, long a9,
                              long a10, long a11);

void ntb0_set_probe(ntb0_probe_fn fn);

/*
 * وضع "الفشل المتوقّع" — للاختبار فقط.
 * في هذا الوضع يُسجَّل الفشل المتوقّع كمصباح OK (سلوك صحيح مُقصود)
 * حتى يبقى تقرير selftest "بلا FAIL" (NTBOX.md §9).
 */
void ntb0_set_expect_fail(int on);

/*
 * إحصاءات الـ syscalls المنفَّذة (NTBOX.md §11 — stats تحدد الأولويات).
 * يُعيد عدد السجلات المكتوبة (≤ max).
 */
struct ntb0_stat_pub {
    const char   *id;    /* "SYS-005" */
    const char   *name;  /* "NtCreateFile" */
    unsigned long calls;
    unsigned long fails;
};

int ntb0_stats_get(struct ntb0_stat_pub *out, int max);

#endif /* NTB0_DISPATCH_H */
