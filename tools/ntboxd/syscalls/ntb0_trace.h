/*
 * ntb0_trace — نظام المصابيح لطبقة الـ syscalls (user-space).
 *
 * نسخة user-space من include/ntb_trace.h (مكتبة النواة).
 * القاعدة نفسها (NTBOX.md §9): كل نقطة ترجمة تحمل ID ثابت
 * وتسجّل OK / STUB / FAIL / UNIMPL. ممنوع أي تسجيل بدون ID.
 */
#ifndef NTB0_TRACE_H
#define NTB0_TRACE_H

#include <stdio.h>

/* الحالات — نفس معنى ntb_st في النواة */
enum ntb0_st {
    NTB0_ST_UNIMPL = 0,
    NTB0_ST_OK,
    NTB0_ST_STUB,
    NTB0_ST_FAIL,
};

/* المصباح — سطر واحد مرتّب */
#define NTB0_TP(id, st, fmt, ...) \
    fprintf(stderr, "[ntbox][%s][%s] " fmt "\n", (id), (st), ##__VA_ARGS__)

#define NTB0_OK(id, fmt, ...)   NTB0_TP(id, "OK", fmt, ##__VA_ARGS__)
#define NTB0_STUB(id, fmt, ...) NTB0_TP(id, "STUB", fmt, ##__VA_ARGS__)
#define NTB0_FAIL(id, fmt, ...) NTB0_TP(id, "FAIL", fmt, ##__VA_ARGS__)
#define NTB0_SKIP(id, fmt, ...) NTB0_TP(id, "SKIP", fmt, ##__VA_ARGS__)
#define NTB0_UNIMPL(id, fmt, ...) NTB0_TP(id, "UNIMPL", fmt, ##__VA_ARGS__)

/* عدّادات لكل syscall — يُطبع في تقرير الحالة */
struct ntb0_stat {
    unsigned long calls;
    unsigned long fails;
};

#define NTB0_STAT_DECL(count) extern struct ntb0_stat ntb0_stats[count];

#endif /* NTB0_TRACE_H */
