/*
 * DublKernel — العدّادات والتقرير الذاتي (PLAN_Translator_Diagnostics.md §4-5)
 * /proc/ntbox/stats     : العدّادات الخام لكل معرّف
 * /proc/ntbox/selftest  : يشغّل اختبارات آمنة ويطبع الجدول الموحّد
 */

#include "ntbox.h"
#include "ntb_trace.h"
#include "ntb_table.h"

#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/stdarg.h>

struct ntb_tstat ntb_ts[NTB_TID_COUNT];

static const char *const ntb_id_strs[] = {
#define X(e, s, n) s,
    NTB_TRACE_LIST(X)
#undef X
};

static const char *const ntb_name_strs[] = {
#define X(e, s, n) n,
    NTB_TRACE_LIST(X)
#undef X
};

const char *ntb_id_str(enum ntb_tid id)
{
    return ntb_id_strs[id];
}

const char *ntb_st_str(u8 st)
{
    switch (st) {
    case NTB_ST_OK:    return "OK";
    case NTB_ST_STUB:  return "STUB";
    case NTB_ST_FAIL:  return "FAIL";
    default:           return "UNIMPL";
    }
}

/*
 * المصباح: OK/STUB يُسجَّل أول مرة فقط ثم يُعدّ بصمت.
 * FAIL يُسجَّل دائماً مع الخطأ.
 */
void ntb_t_report(enum ntb_tid id, u8 st, long err, const char *fmt, ...)
{
    struct ntb_tstat *t = &ntb_ts[id];
    char msg[160];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    t->hits++;
    if (st == NTB_ST_FAIL) {
        t->fails++;
        t->last_err = (s32)err;
        t->st = NTB_ST_FAIL;
        pr_info("[ntbox][%s][FAIL] %s (err=%ld)\n", ntb_id_str(id), msg, err);
    } else {
        t->st = st;
        if (t->hits == 1)
            pr_info("[ntbox][%s][%s] first: %s\n",
                    ntb_id_str(id), ntb_st_str(st), msg);
    }
}

/* ---------------- بناء النصوص ---------------- */

#define DIAG_CAP 16384

typedef void (*diag_builder_t)(char *out, size_t cap, size_t *len);

static diag_builder_t g_build;
static char *g_out;
static size_t g_out_len;

static size_t emit(char *out, size_t cap, size_t w, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (w >= cap)
        return cap;
    va_start(ap, fmt);
    n = vsnprintf(out + w, cap - w, fmt, ap);
    va_end(ap);
    if (n < 0)
        return w;
    w += (size_t)n;
    return (w > cap) ? cap : w;
}

static void build_stats(char *out, size_t cap, size_t *len)
{
    size_t w = 0;
    int i;

    w = emit(out, cap, w, "ID        NAME                       ST      HITS  FAILS  LAST-ERR\n");
    for (i = 0; i < NTB_TID_COUNT; i++) {
        w = emit(out, cap, w, "%-9s %-26s %-7s %5u  %5u  %d\n",
                 ntb_id_str(i), ntb_name_strs[i], ntb_st_str(ntb_ts[i].st),
                 ntb_ts[i].hits, ntb_ts[i].fails, ntb_ts[i].last_err);
        if (w >= cap)
            break;
    }
    w = emit(out, cap, w, "SUMMARY: %d IDs total\n", NTB_TID_COUNT);
    *len = w;
}

static void build_selftest(char *out, size_t cap, size_t *len)
{
    size_t w = 0;
    int nok = 0, nstub = 0, nfail = 0, nunimpl = 0;
    int i;

    w = emit(out, cap, w, "ID        STATUS  HITS  LAST-ERR  NOTE\n");
    for (i = 0; i < NTB_TID_COUNT; i++) {
        char note[96];
        int r = ntb_table_run_test(i, note, sizeof(note));
        u8 st;

        if (r == 1)
            st = NTB_ST_OK;
        else if (r == 0)
            st = NTB_ST_FAIL;
        else
            st = ntb_ts[i].st;

        switch (st) {
        case NTB_ST_OK:   nok++;     break;
        case NTB_ST_STUB: nstub++;   break;
        case NTB_ST_FAIL: nfail++;   break;
        default:          nunimpl++; break;
        }
        w = emit(out, cap, w, "%-9s %-7s %5u  %8d  %s\n",
                 ntb_id_str(i), ntb_st_str(st), ntb_ts[i].hits,
                 ntb_ts[i].last_err, note);
        if (w >= cap)
            break;
    }
    w = emit(out, cap, w, "SUMMARY: %d OK / %d STUB / %d FAIL / %d UNIMPL\n",
             nok, nstub, nfail, nunimpl);
    *len = w;
}

/* ---------------- قراءة proc ---------------- */

static ssize_t diag_read_common(struct file *file, char __user *ubuf,
                                size_t count, loff_t *ppos)
{
    size_t n;

    (void)file;
    if (*ppos == 0) {
        if (g_out) {
            kfree(g_out);
            g_out = NULL;
        }
        g_out = kmalloc(DIAG_CAP, GFP_KERNEL);
        if (!g_out)
            return -ENOMEM;
        g_build(g_out, DIAG_CAP, &g_out_len);
    }
    if (!g_out || *ppos >= (loff_t)g_out_len)
        return 0;
    n = g_out_len - (size_t)*ppos;
    if (n > count)
        n = count;
    if (copy_to_user(ubuf, g_out + *ppos, n))
        return -EFAULT;
    *ppos += (loff_t)n;
    if (*ppos >= (loff_t)g_out_len) {
        kfree(g_out);
        g_out = NULL;
        g_out_len = 0;
    }
    return (ssize_t)n;
}

static ssize_t diag_stats_read(struct file *file, char __user *ubuf,
                               size_t count, loff_t *ppos)
{
    g_build = build_stats;
    return diag_read_common(file, ubuf, count, ppos);
}

static ssize_t diag_selftest_read(struct file *file, char __user *ubuf,
                                  size_t count, loff_t *ppos)
{
    g_build = build_selftest;
    return diag_read_common(file, ubuf, count, ppos);
}

static const struct proc_ops diag_stats_ops = {
    .proc_read = diag_stats_read,
};

static const struct proc_ops diag_selftest_ops = {
    .proc_read = diag_selftest_read,
};

/* ---------------- إنشاء/إزالة ---------------- */

int ntb_diag_init(struct proc_dir_entry *dir)
{
    struct proc_dir_entry *e;

    e = proc_create("stats", 0444, dir, &diag_stats_ops);
    if (!e)
        return -ENOMEM;
    e = proc_create("selftest", 0444, dir, &diag_selftest_ops);
    if (!e) {
        remove_proc_entry("stats", dir);
        return -ENOMEM;
    }
    NTB_TP("DIAG-000", "OK", "diagnostics ready (%d IDs)", NTB_TID_COUNT);
    return 0;
}

void ntb_diag_exit(struct proc_dir_entry *dir)
{
    if (g_out) {
        kfree(g_out);
        g_out = NULL;
        g_out_len = 0;
    }
    remove_proc_entry("selftest", dir);
    remove_proc_entry("stats", dir);
}
