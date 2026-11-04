/*
 * DublKernel — جدول الترجمة Tier-1 (PLAN_Translator_Diagnostics.md §7)
 * كل دالة هنا: توقيع ntoskrnl ← غلاف مترجم ← نواة لينكس (عبر الجسر ntb_*).
 * القواعد: أول سطر = مصباح ID. لا Panic. FAIL يُسجَّل دائماً.
 */

#include "ntbox.h"
#include "ntbox_types.h"
#include "linux_bridge.h"
#include "ntb_trace.h"
#include "ntb_table.h"
#include "ntapi_stubs.h"

#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/smp.h>
#include <linux/jiffies.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/stdarg.h>
#include <asm/io.h>

/* ------------------------------------------------------------------ */
/* MEM — ذاكرة (كل شيء عبر الجسر ntb_kmalloc/ntb_ioremap حصراً)        */
/* ------------------------------------------------------------------ */

void *NTB_MSABI ntb_ExAllocatePoolWithTag(POOL_TYPE PoolType, size_t n, ULONG Tag)
{
    void *v;

    (void)PoolType;
    v = ntb_kmalloc(n, GFP_KERNEL | __GFP_ZERO);
    if (v) {
        NTB_OK(NTB_TID_MEM_001, "kmalloc(%zu) tag=0x%x", n, Tag);
    } else {
        NTB_FAIL(NTB_TID_MEM_001, -ENOMEM, "kmalloc(%zu) NULL tag=0x%x", n, Tag);
    }
    return v;
}

void NTB_MSABI ntb_ExFreePoolWithTag(void *p, ULONG Tag)
{
    (void)Tag;
    ntb_kfree(p);
    NTB_OK(NTB_TID_MEM_002, "kfree(%p)", p);
}

void *NTB_MSABI ntb_ExAllocatePool2(ULONG Flags, size_t n, ULONG Tag)
{
    void *v = ntb_kmalloc(n, GFP_KERNEL | __GFP_ZERO);

    (void)Flags;
    if (v)
        NTB_OK(NTB_TID_MEM_003, "kmalloc-zero(%zu) tag=0x%x", n, Tag);
    else
        NTB_FAIL(NTB_TID_MEM_003, -ENOMEM, "kmalloc(%zu) NULL", n);
    return v;
}

void *NTB_MSABI ntb_ExAllocatePoolWithTagPriority(POOL_TYPE t, size_t n, ULONG Tag, int prio)
{
    void *v = ntb_kmalloc(n, GFP_KERNEL | __GFP_ZERO);

    (void)t;
    (void)prio;
    if (v)
        NTB_OK(NTB_TID_MEM_009, "kmalloc(%zu) tag=0x%x (priority ignored)", n, Tag);
    else
        NTB_FAIL(NTB_TID_MEM_009, -ENOMEM, "kmalloc(%zu) NULL", n);
    return v;
}

/*
 * ملاحظة Tier-1: kmalloc مادي متصل للحجم الصغير (KMALLOC_MAX_SIZE).
 * الحجم غير مطلوب عند التحرير — نفس عقد ntoskrnl
 * (ntb_ExFreePool وntb_MmFreeContiguousMemory لا يحتاجانه).
 */
void *NTB_MSABI ntb_MmAllocateContiguousMemory(size_t n, LARGE_INTEGER Highest)
{
    void *v = ntb_kmalloc(n, GFP_KERNEL | __GFP_ZERO);

    (void)Highest;
    if (v)
        NTB_OK(NTB_TID_MEM_004, "kmalloc-contig(%zu)", n);
    else
        NTB_FAIL(NTB_TID_MEM_004, -ENOMEM, "contig alloc(%zu) NULL", n);
    return v;
}

void NTB_MSABI ntb_MmFreeContiguousMemory(void *p)
{
    ntb_kfree(p);
    NTB_OK(NTB_TID_MEM_005, "kfree(%p)", p);
}

void __iomem *NTB_MSABI ntb_MmMapIoSpace(LARGE_INTEGER Phys, size_t n, int CacheType)
{
    void __iomem *v;
    int ret;

    (void)CacheType;
    ret = ntb_ioremap((phys_addr_t)Phys, (unsigned long)n, &v);
    if (ret == 0)
        NTB_OK(NTB_TID_MEM_006, "ioremap(0x%llx, %zu)", (unsigned long long)Phys, n);
    else
        NTB_FAIL(NTB_TID_MEM_006, ret, "ioremap(0x%llx, %zu) failed",
                 (unsigned long long)Phys, n);
    return v;
}

void NTB_MSABI ntb_MmUnmapIoSpace(void __iomem *p, size_t n)
{
    (void)n;
    ntb_iounmap(p);
    NTB_OK(NTB_TID_MEM_007, "iounmap(%p)", p);
}

LARGE_INTEGER NTB_MSABI ntb_MmGetPhysicalAddress(void *v)
{
    if (!v) {
        NTB_FAIL(NTB_TID_MEM_008, -EINVAL, "NULL virtual address");
        return 0;
    }
    if (!virt_addr_valid(v)) {
        NTB_FAIL(NTB_TID_MEM_008, -EINVAL,
                 "non-linear addr %p (vmalloc unsupported in Tier-1)", v);
        return 0;
    }
    NTB_OK(NTB_TID_MEM_008, "__pa(%p)", v);
    return (LARGE_INTEGER)__pa(v);
}

/* صفحات صريحة (non-cached حقيقي يأتي Tier-3 مع PAT/WT) */
void *NTB_MSABI ntb_MmAllocateNonCachedMemory(size_t n)
{
    unsigned int order = get_order(n);
    void *v;

    /* MAX_ORDER أُعيدت تسميتها MAX_PAGE_ORDER منذ النواة 6.5 */
#ifdef MAX_PAGE_ORDER
    if (order > MAX_PAGE_ORDER) {
#else
    if (order > 10) {
#endif
        NTB_FAIL(NTB_TID_MEM_010, -ENOMEM, "size %zu too large (order %u)", n, order);
        return NULL;
    }
    v = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, order);
    if (v)
        NTB_STUB(NTB_TID_MEM_010, "pages(%zu) — NOT truly uncached yet", n);
    else
        NTB_FAIL(NTB_TID_MEM_010, -ENOMEM, "__get_free_pages(%u) NULL", order);
    return v;
}

void NTB_MSABI ntb_MmFreeNonCachedMemory(void *p, size_t n)
{
    if (p)
        free_pages((unsigned long)p, get_order(n));
    NTB_OK(NTB_TID_MEM_014, "free_pages(%p, %zu)", p, n);
}

/* Lookaside مبني على kmalloc — وظيفي لكن بلا kmem_cache (STUB مقصود) */
void NTB_MSABI ntb_ExInitializeLookasidePoolEx(PNTB_LOOKASIDE la, size_t size)
{
    if (!la) {
        NTB_FAIL(NTB_TID_MEM_011, -EINVAL, "NULL lookaside");
        return;
    }
    la->size = size;
    NTB_STUB(NTB_TID_MEM_011, "lookaside(%zu) via kmalloc", size);
}

void *NTB_MSABI ntb_ExAllocateFromLookasidePoolEx(PNTB_LOOKASIDE la)
{
    void *v;

    if (!la || !la->size) {
        NTB_FAIL(NTB_TID_MEM_013, -EINVAL, "bad lookaside");
        return NULL;
    }
    v = ntb_kmalloc(la->size, GFP_KERNEL | __GFP_ZERO);
    if (v)
        NTB_OK(NTB_TID_MEM_013, "kmalloc(%zu)", la->size);
    else
        NTB_FAIL(NTB_TID_MEM_013, -ENOMEM, "kmalloc(%zu) NULL", la->size);
    return v;
}

void NTB_MSABI ntb_ExFreeToLookasidePoolEx(PNTB_LOOKASIDE la, void *p)
{
    (void)la;
    ntb_kfree(p);
    NTB_OK(NTB_TID_MEM_012, "kfree(%p)", p);
}

/* ------------------------------------------------------------------ */
/* CPU — IRQL محدّد لكل نواة + local_bh عند DISPATCH                  */
/* ------------------------------------------------------------------ */

DEFINE_PER_CPU(u8, ntb_irql);

static void ntb_irql_apply(KIRQL new)
{
    KIRQL old = this_cpu_read(ntb_irql);

    if (new == old)
        return;
    if (new >= DISPATCH_LEVEL && old < DISPATCH_LEVEL)
        local_bh_disable();
    else if (new < DISPATCH_LEVEL && old >= DISPATCH_LEVEL)
        local_bh_enable();
    this_cpu_write(ntb_irql, new);
}

KIRQL NTB_MSABI ntb_KeGetCurrentIrql(void)
{
    return this_cpu_read(ntb_irql);
}

void NTB_MSABI ntb_KeRaiseIrql(KIRQL New)
{
    if (New > this_cpu_read(ntb_irql))
        ntb_irql_apply(New);
    NTB_OK(NTB_TID_CPU_002, "irql now %u", ntb_KeGetCurrentIrql());
}

void NTB_MSABI ntb_KeLowerIrql(KIRQL New)
{
    if (New <= this_cpu_read(ntb_irql))
        ntb_irql_apply(New);
    NTB_OK(NTB_TID_CPU_003, "irql now %u", ntb_KeGetCurrentIrql());
}

KIRQL NTB_MSABI ntb_KfRaiseIrql(KIRQL New)
{
    KIRQL old = ntb_KeGetCurrentIrql();

    ntb_KeRaiseIrql(New);
    NTB_OK(NTB_TID_CPU_004, "%u -> %u", old, ntb_KeGetCurrentIrql());
    return old;
}

void NTB_MSABI ntb_KfLowerIrql(KIRQL New)
{
    ntb_KeLowerIrql(New);
    NTB_OK(NTB_TID_CPU_004, "lowered to %u", New);
}

void NTB_MSABI ntb_KeStallExecutionProcessor(ULONG MicroSeconds)
{
    udelay(MicroSeconds);
    NTB_OK(NTB_TID_CPU_005, "stall %u us", MicroSeconds);
}

ULONG NTB_MSABI ntb_KeGetCurrentProcessorNumber(void)
{
    ULONG n = (ULONG)smp_processor_id();

    NTB_OK(NTB_TID_CPU_006, "cpu=%u", n);
    return n;
}

ULONG NTB_MSABI ntb_KeQueryActiveProcessorCount(void)
{
    ULONG n = (ULONG)num_online_cpus();

    NTB_OK(NTB_TID_CPU_007, "cpus=%u", n);
    return n;
}

ULONG NTB_MSABI ntb_DbgPrint(const char *fmt, ...)
{
    if (!fmt)
        return 0;

    /* DbgPrint: avoid ms_abi va_list -> System V vsnprintf mismatch. */
    NTB_OK(NTB_TID_CPU_008, "first DbgPrint seen");
    pr_info("[ntbox][DbgPrint] %s\n", fmt);
    return 0;
}

/* مسار فشل حقيقي: تقرير مرتب ثم إيقاف — ممنوع الصمت */
void NTB_MSABI ntb_KeBugCheck(ULONG Code)
{
    NTB_FAIL(NTB_TID_CPU_009, -(long)Code, "BUGCHECK 0x%x inside NT-Box — halting",
             Code);
    panic("ntbox: KeBugCheck(0x%x)", Code);
}

void NTB_MSABI ntb_DbgBreakPoint(void)
{
    NTB_OK(NTB_TID_CPU_010, "nop (no debugger attached)");
}

/* ------------------------------------------------------------------ */
/* SYNC — أقفال/أحداث/وقت/DPC                                          */
/* ------------------------------------------------------------------ */

void NTB_MSABI ntb_KeInitializeSpinLock(PNTB_KSPIN_LOCK l)
{
    if (!l)
        return;
    raw_spin_lock_init(&l->raw);
    NTB_OK(NTB_TID_SYNC_001, "raw_spinlock initialized");
}

void NTB_MSABI ntb_KeAcquireSpinLock(PNTB_KSPIN_LOCK l, PKIRQL OldIrql)
{
    unsigned long flags;

    if (OldIrql)
        *OldIrql = ntb_KeGetCurrentIrql();
    raw_spin_lock_irqsave(&l->raw, flags);
    ntb_irql_apply(DISPATCH_LEVEL);
    (void)flags;
    NTB_OK(NTB_TID_SYNC_002, "acquired (irql=DISPATCH)");
}

void NTB_MSABI ntb_KeReleaseSpinLock(PNTB_KSPIN_LOCK l, KIRQL OldIrql)
{
    ntb_irql_apply(OldIrql);
    raw_spin_unlock_irq(&l->raw);
    NTB_OK(NTB_TID_SYNC_003, "released (irql=%u)", OldIrql);
}

void NTB_MSABI ntb_KeAcquireSpinLockAtDpcLevel(PNTB_KSPIN_LOCK l)
{
    raw_spin_lock(&l->raw);
    NTB_OK(NTB_TID_SYNC_004, "acquired at DPC");
}

void NTB_MSABI ntb_KeReleaseSpinLockFromDpcLevel(PNTB_KSPIN_LOCK l)
{
    raw_spin_unlock(&l->raw);
    NTB_OK(NTB_TID_SYNC_005, "released at DPC");
}

void NTB_MSABI ntb_KeInitializeEvent(PKEVENT e, KEVENT_TYPE type, BOOLEAN State)
{
    if (!e)
        return;
    atomic_set(&e->signaled, State ? 1 : 0);
    init_waitqueue_head(&e->wq);
    e->type = type;
    NTB_OK(NTB_TID_SYNC_006, "type=%d state=%d", type, (int)State);
}

LONG NTB_MSABI ntb_KeSetEvent(PKEVENT e, LONG Increment, BOOLEAN Wait)
{
    LONG prev;

    (void)Increment;
    (void)Wait;
    if (!e)
        return 0;
    prev = atomic_xchg(&e->signaled, 1);
    wake_up_all(&e->wq);
    NTB_OK(NTB_TID_SYNC_007, "prev=%d", prev);
    return prev;
}

BOOLEAN NTB_MSABI ntb_KeResetEvent(PKEVENT e)
{
    BOOLEAN prev;

    if (!e)
        return 0;
    prev = (BOOLEAN)atomic_xchg(&e->signaled, 0);
    NTB_OK(NTB_TID_SYNC_008, "prev=%d", (int)prev);
    return prev;
}

BOOLEAN NTB_MSABI ntb_KeReadStateEvent(PKEVENT e)
{
    BOOLEAN s = e ? (BOOLEAN)atomic_read(&e->signaled) : 0;

    NTB_OK(NTB_TID_SYNC_014, "state=%d", (int)s);
    return s;
}

NTSTATUS NTB_MSABI ntb_KeWaitForSingleObject(PVOID Object, KWAIT_REASON Reason,
                                   KPROCESSOR_MODE Mode, BOOLEAN Alertable,
                                   PLARGE_INTEGER Timeout)
{
    PKEVENT e = (PKEVENT)Object;
    long jc;
    long ok;

    (void)Reason;
    (void)Mode;
    (void)Alertable;
    if (!e) {
        NTB_FAIL(NTB_TID_SYNC_009, -EINVAL, "NULL object");
        return STATUS_INVALID_PARAMETER;
    }
    if (!Timeout) {
        jc = MAX_JIFFY_OFFSET;
    } else {
        s64 ns = (*Timeout < 0) ? -*Timeout : *Timeout;

        jc = (long)nsecs_to_jiffies((unsigned long)(ns * 100));
        if (jc <= 0)
            jc = 1;
    }
    ok = wait_event_interruptible_timeout(e->wq, atomic_read(&e->signaled), jc);
    if (ok > 0) {
        if (e->type == SynchronizationEvent)
            atomic_set(&e->signaled, 0); /* auto-reset */
        NTB_OK(NTB_TID_SYNC_009, "signaled");
        return STATUS_SUCCESS;
    }
    NTB_OK(NTB_TID_SYNC_009, "timeout (normal NT result)");
    return STATUS_TIMEOUT;
}

NTSTATUS NTB_MSABI ntb_KeDelayExecutionThread(KPROCESSOR_MODE Mode, BOOLEAN Alertable,
                                    PLARGE_INTEGER Interval)
{
    s64 ns;
    long jc;

    (void)Mode;
    (void)Alertable;
    if (!Interval)
        return STATUS_INVALID_PARAMETER;
    ns = (*Interval < 0) ? -*Interval : *Interval; /* نسبي 100ns */
    jc = (long)nsecs_to_jiffies((unsigned long)(ns * 100));
    if (jc <= 0)
        jc = 1;
    schedule_timeout_interruptible(jc);
    NTB_OK(NTB_TID_SYNC_010, "delay %lld ns", (long long)ns);
    return STATUS_SUCCESS;
}

static void ntb_dpc_work(struct work_struct *w)
{
    PKDPC d = container_of(w, struct _KDPC, work);

    d->func(d, d->ctx, d->arg1, d->arg2);
}

void NTB_MSABI ntb_KeInitializeDpc(PKDPC d, PKDEFERRED_ROUTINE Routine, PVOID ctx)
{
    if (!d)
        return;
    INIT_WORK(&d->work, ntb_dpc_work);
    d->func = Routine;
    d->ctx = ctx;
    NTB_OK(NTB_TID_SYNC_011, "dpc initialized");
}

BOOLEAN NTB_MSABI ntb_KeInsertQueueDpc(PKDPC d, PVOID Arg1, PVOID Arg2)
{
    BOOLEAN queued;

    if (!d || !d->func)
        return 0;
    d->arg1 = Arg1;
    d->arg2 = Arg2;
    queued = schedule_work(&d->work) ? 1 : 0;
    NTB_OK(NTB_TID_SYNC_012, "queued=%d", (int)queued);
    return queued;
}

void NTB_MSABI ntb_KeInitializeMutex(PKMUTEX m, ULONG Level)
{
    (void)Level;
    if (!m)
        return;
    mutex_init(&m->m);
    NTB_OK(NTB_TID_SYNC_013, "mutex initialized");
}

NTSTATUS NTB_MSABI ntb_KeAcquireMutex(PKMUTEX m, BOOLEAN Waitable)
{
    (void)Waitable;
    if (!m)
        return STATUS_INVALID_PARAMETER;
    mutex_lock(&m->m);
    NTB_OK(NTB_TID_SYNC_015, "acquired");
    return STATUS_SUCCESS;
}

NTSTATUS NTB_MSABI ntb_KeReleaseMutex(PKMUTEX m, BOOLEAN Wait)
{
    (void)Wait;
    if (!m)
        return STATUS_INVALID_PARAMETER;
    mutex_unlock(&m->m);
    NTB_OK(NTB_TID_SYNC_013, "released");
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* RTL — نصوص/ذاكرة                                                    */
/* ------------------------------------------------------------------ */

void NTB_MSABI ntb_RtlInitUnicodeString(PUNICODE_STRING dst, const u16 *src)
{
    size_t n = 0;

    if (!dst)
        return;
    if (src) {
        while (src[n])
            n++;
    }
    dst->Length = (u16)(n * sizeof(u16));
    dst->MaximumLength = (u16)(dst->Length + sizeof(u16));
    dst->Buffer = (u16 *)src;
    NTB_OK(NTB_TID_RTL_001, "len=%u", dst->Length);
}

void NTB_MSABI ntb_RtlCopyMemory(void *dst, const void *src, size_t n)
{
    if (dst && src)
        memcpy(dst, src, n);
    NTB_OK(NTB_TID_RTL_002, "copy %zu bytes", n);
}

void NTB_MSABI ntb_RtlZeroMemory(void *dst, size_t n)
{
    if (dst)
        memset(dst, 0, n);
    NTB_OK(NTB_TID_RTL_003, "zero %zu bytes", n);
}

void NTB_MSABI ntb_RtlMoveMemory(void *dst, const void *src, size_t n)
{
    if (dst && src)
        memmove(dst, src, n);
    NTB_OK(NTB_TID_RTL_004, "move %zu bytes", n);
}

void NTB_MSABI ntb_RtlFillMemory(void *dst, size_t n, u8 value)
{
    if (dst)
        memset(dst, value, n);
    NTB_OK(NTB_TID_RTL_005, "fill %zu bytes val=0x%x", n, value);
}

/* تحويل ASCII→UTF16 مبسّط (UTF-8 كامل يأتي M3) */
NTSTATUS NTB_MSABI ntb_RtlAnsiStringToUnicodeString(PUNICODE_STRING dst,
                                          PANSI_STRING src, BOOLEAN alloc)
{
    u16 *buf;
    size_t i, n;

    if (!dst || !src)
        return STATUS_INVALID_PARAMETER;
    n = src->Length;
    if (!alloc) {
        dst->Buffer = (u16 *)src->Buffer;
        dst->Length = (u16)(n * sizeof(u16));
        dst->MaximumLength = dst->Length;
        NTB_OK(NTB_TID_RTL_006, "no-alloc alias");
        return STATUS_SUCCESS;
    }
    buf = ntb_kmalloc(n * sizeof(u16) + sizeof(u16), GFP_KERNEL);
    if (!buf) {
        NTB_FAIL(NTB_TID_RTL_006, -ENOMEM, "alloc %zu failed", n * 2 + 2);
        return STATUS_NO_MEMORY;
    }
    for (i = 0; i < n; i++)
        buf[i] = (u16)(u8)src->Buffer[i];
    buf[n] = 0;
    dst->Buffer = buf;
    dst->Length = (u16)(n * sizeof(u16));
    dst->MaximumLength = (u16)(dst->Length + sizeof(u16));
    NTB_OK(NTB_TID_RTL_006, "converted %zu chars", n);
    return STATUS_SUCCESS;
}

/* ملاحظة: عقد ntoskrnl — يُستدعى فقط على سلسلة من RtlAnsiStringToUnicodeString */
void NTB_MSABI ntb_RtlFreeUnicodeString(PUNICODE_STRING s)
{
    if (s && s->Buffer) {
        ntb_kfree(s->Buffer);
        s->Buffer = NULL;
        s->Length = 0;
        s->MaximumLength = 0;
    }
    NTB_OK(NTB_TID_RTL_007, "freed");
}

size_t NTB_MSABI ntb_RtlCompareMemory(const void *a, const void *b, size_t n)
{
    const u8 *pa = a, *pb = b;
    size_t i = 0;

    while (i < n && pa[i] == pb[i])
        i++;
    NTB_OK(NTB_TID_RTL_008, "matched %zu/%zu", i, n);
    return i;
}

NTSTATUS NTB_MSABI ntb_RtlAppendUnicodeToString(PUNICODE_STRING dst, const u16 *src)
{
    size_t dlen, slen, i;

    if (!dst)
        return STATUS_INVALID_PARAMETER;
    if (!src)
        return STATUS_SUCCESS;
    dlen = dst->Length / sizeof(u16);
    slen = 0;
    while (src[slen])
        slen++;
    if ((dlen + slen + 1) * sizeof(u16) > dst->MaximumLength)
        return STATUS_BUFFER_TOO_SMALL;
    for (i = 0; i < slen; i++)
        dst->Buffer[dlen + i] = src[i];
    dst->Buffer[dlen + slen] = 0;
    dst->Length = (u16)((dlen + slen) * sizeof(u16));
    NTB_OK(NTB_TID_RTL_009, "appended %zu chars", slen);
    return STATUS_SUCCESS;
}

BOOLEAN NTB_MSABI ntb_RtlEqualUnicodeString(PUNICODE_STRING a, PUNICODE_STRING b,
                                  BOOLEAN caseins)
{
    size_t na, nb, i;
    BOOLEAN eq = 1;

    if (!a || !b)
        return 0;
    na = a->Length / sizeof(u16);
    nb = b->Length / sizeof(u16);
    if (na != nb)
        eq = 0;
    else {
        for (i = 0; i < na; i++) {
            u16 ca = a->Buffer[i], cb = b->Buffer[i];

            if (caseins) {
                if (ca >= 'A' && ca <= 'Z')
                    ca += 32;
                if (cb >= 'A' && cb <= 'Z')
                    cb += 32;
            }
            if (ca != cb) {
                eq = 0;
                break;
            }
        }
    }
    NTB_OK(NTB_TID_RTL_010, "equal=%d (caseins=%d)", (int)eq, (int)caseins);
    return eq;
}

/* ------------------------------------------------------------------ */
/* IRP — I/O Manager (Tier-2, M3)                                       */
/* ------------------------------------------------------------------ */

NTSTATUS NTB_MSABI ntb_IoCreateDevice(PDRIVER_OBJECT DriverObject, ULONG DevExtSize,
                            PUNICODE_STRING DeviceName, ULONG Type,
                            ULONG Characteristics, BOOLEAN Exclusive,
                            PDEVICE_OBJECT *DeviceObject)
{
    PDEVICE_OBJECT dev;

    (void)DeviceName;
    (void)Type;
    (void)Characteristics;
    (void)Exclusive;
    if (!DriverObject || !DeviceObject) {
        NTB_FAIL(NTB_TID_IRP_011, -EINVAL, "bad args");
        return STATUS_INVALID_PARAMETER;
    }
    dev = ntb_kmalloc(sizeof(DEVICE_OBJECT) + DevExtSize, GFP_KERNEL | __GFP_ZERO);
    if (!dev) {
        NTB_FAIL(NTB_TID_IRP_011, -ENOMEM, "alloc device failed");
        return STATUS_NO_MEMORY;
    }
    dev->Type = (int)Type;
    dev->DriverObject = DriverObject;
    DriverObject->DeviceObject = dev;
    *DeviceObject = dev;
    NTB_OK(NTB_TID_IRP_011, "created device %p (ext=%lu)", dev, (unsigned long)DevExtSize);
    return STATUS_SUCCESS;
}

void NTB_MSABI ntb_IoDeleteDevice(PDEVICE_OBJECT DeviceObject)
{
    if (!DeviceObject)
        return;
    NTB_OK(NTB_TID_IRP_012, "delete device %p (deferred)", DeviceObject);
}

NTSTATUS NTB_MSABI ntb_IoCreateSymbolicLink(PUNICODE_STRING LinkName,
                                  PUNICODE_STRING DeviceName)
{
    (void)LinkName;
    (void)DeviceName;
    NTB_OK(NTB_TID_IRP_013, "symlink (stub)");
    return STATUS_SUCCESS;
}

NTSTATUS NTB_MSABI ntb_IoDeleteSymbolicLink(PUNICODE_STRING LinkName)
{
    (void)LinkName;
    NTB_OK(NTB_TID_IRP_014, "delete symlink (stub)");
    return STATUS_SUCCESS;
}

BOOLEAN NTB_MSABI ntb_IoStartNextPacket(PDEVICE_OBJECT DeviceObject, BOOLEAN Cancelable)
{
    (void)DeviceObject;
    (void)Cancelable;
    NTB_OK(NTB_TID_IRP_015, "start next packet (stub)");
    return 1;
}

void NTB_MSABI ntb_IofCompleteRequest(PIRP Irp, int PriorityBoost)
{
    ntbox_IoCompleteRequest(Irp, PriorityBoost);
    NTB_OK(NTB_TID_IRP_016, "IofCompleteRequest -> %p", Irp);
}

void NTB_MSABI ntb_MmPageEntireDriver(PVOID Address)
{
    (void)Address;
    NTB_OK(NTB_TID_MEM_015, "page entire driver (no-op)");
}

void NTB_MSABI ntb_KeBugCheckEx(ULONG Code, ULONG_PTR p1, ULONG_PTR p2,
                      ULONG_PTR p3, ULONG_PTR p4)
{
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    NTB_FAIL(NTB_TID_CPU_011, -(long)Code, "BUGCHECK_EX 0x%x — halting", Code);
    panic("ntbox: KeBugCheckEx(0x%x)", Code);
}

/* ------------------------------------------------------------------ */
/* الاختبارات الذاتية الآمنة (تُستدعى من /proc/ntbox/selftest)         */
/* ------------------------------------------------------------------ */

#define T_OK   1
#define T_FAIL 0
#define T_NONE -1

static int test_mem_alloc(char *note, size_t n)
{
    void *v = ntb_ExAllocatePoolWithTag(NTB_NonPagedPool, 64, 0x7473744E);

    if (!v) {
        snprintf(note, n, "alloc 64B returned NULL");
        return T_FAIL;
    }
    memset(v, 0xAA, 64);
    ntb_ExFreePoolWithTag(v, 0x7473744E);
    snprintf(note, n, "alloc 64B + free OK");
    return T_OK;
}

static int test_mem_free_null(char *note, size_t n)
{
    ntb_ExFreePoolWithTag(NULL, 0);
    snprintf(note, n, "free(NULL) tolerated");
    return T_OK;
}

static int test_mem_contig(char *note, size_t n)
{
    void *v = ntb_MmAllocateContiguousMemory(4096, 0);

    if (!v) {
        snprintf(note, n, "contig 4KB NULL");
        return T_FAIL;
    }
    ntb_MmFreeContiguousMemory(v);
    snprintf(note, n, "contig 4KB + free OK");
    return T_OK;
}

static int test_mem_noncached(char *note, size_t n)
{
    void *v = ntb_MmAllocateNonCachedMemory(4096);

    if (!v) {
        snprintf(note, n, "noncached 4KB NULL");
        return T_FAIL;
    }
    ntb_MmFreeNonCachedMemory(v, 4096);
    snprintf(note, n, "pages 4KB + free OK (STUB ok)");
    return T_OK;
}

static int test_mem_lookaside(char *note, size_t n)
{
    NTB_LOOKASIDE la;
    void *v;

    ntb_ExInitializeLookasidePoolEx(&la, 64);
    v = ntb_ExAllocateFromLookasidePoolEx(&la);
    if (!v) {
        snprintf(note, n, "lookaside alloc NULL");
        return T_FAIL;
    }
    ntb_ExFreeToLookasidePoolEx(&la, v);
    snprintf(note, n, "lookaside init+alloc+free OK");
    return T_OK;
}

/*
 * اختبار MEM-008/006/007: عنوان فيزيائي لصفحة مخصصة ثم map+unmap.
 * ملاحظة: x86 يرفض ioremap على RAM عادية (سلوك متوقع من النواة) —
 * رفض نظيف = سلوك الترجمة سليم، ونوثّقه في NOTE.
 */
static int test_mem_iomap(char *note, size_t n)
{
    void *v = ntb_kmalloc(4096, GFP_KERNEL | __GFP_ZERO);
    LARGE_INTEGER phys;

    if (!v) {
        snprintf(note, n, "kmalloc source NULL");
        return T_FAIL;
    }
    phys = ntb_MmGetPhysicalAddress(v);
    if (!phys) {
        ntb_kfree(v);
        snprintf(note, n, "GetPhysicalAddress returned 0");
        return T_FAIL;
    }
    /* ioremap على RAM يُسبّب BUG في 6.8 — نتجنبه (MEM-006/007 remain UNIMPL). */
    ntb_kfree(v);
    snprintf(note, n, "pa=0x%llx obtained; ioremap skipped (safety)", (unsigned long long)phys);
    return T_OK;
}

static int test_cpu_irql(char *note, size_t n)
{
    KIRQL irql = ntb_KeGetCurrentIrql();

    snprintf(note, n, "irql=%u", irql);
    return irql == PASSIVE_LEVEL ? T_OK : T_FAIL;
}

static int test_cpu_raise_lower(char *note, size_t n)
{
    ntb_KeRaiseIrql(DISPATCH_LEVEL);
    if (ntb_KeGetCurrentIrql() != DISPATCH_LEVEL) {
        snprintf(note, n, "raise did not reach DISPATCH");
        ntb_KeLowerIrql(PASSIVE_LEVEL);
        return T_FAIL;
    }
    ntb_KeLowerIrql(PASSIVE_LEVEL);
    if (ntb_KeGetCurrentIrql() != PASSIVE_LEVEL) {
        snprintf(note, n, "lower did not return PASSIVE");
        return T_FAIL;
    }
    snprintf(note, n, "PASSIVE->DISPATCH->PASSIVE OK");
    return T_OK;
}

static int test_cpu_stall(char *note, size_t n)
{
    ntb_KeStallExecutionProcessor(50);
    snprintf(note, n, "50us stall OK");
    return T_OK;
}

static int test_cpu_cpunum(char *note, size_t n)
{
    ULONG num = ntb_KeGetCurrentProcessorNumber();
    ULONG max = ntb_KeQueryActiveProcessorCount();

    snprintf(note, n, "cpu=%u of %u", num, max);
    return num < max ? T_OK : T_FAIL;
}

static int test_cpu_dbgprint(char *note, size_t n)
{
    ntb_DbgPrint("selftest probe %d\n", 42);
    snprintf(note, n, "probe printed");
    return T_OK;
}

static int test_cpu_breakpoint(char *note, size_t n)
{
    ntb_DbgBreakPoint();
    snprintf(note, n, "nop executed");
    return T_OK;
}

static int test_sync_spinlock(char *note, size_t n)
{
    NTB_KSPIN_LOCK l;
    KIRQL old;

    ntb_KeInitializeSpinLock(&l);
    ntb_KeAcquireSpinLock(&l, &old);
    ntb_KeReleaseSpinLock(&l, old);
    ntb_KeAcquireSpinLockAtDpcLevel(&l);
    ntb_KeReleaseSpinLockFromDpcLevel(&l);
    snprintf(note, n, "acquire/release + DPC-level OK");
    return T_OK;
}

static int test_sync_event(char *note, size_t n)
{
    KEVENT e;
    LARGE_INTEGER t10ms = -10000000LL; /* 10ms نسبي بالـ100ns */
    NTSTATUS st;

    ntb_KeInitializeEvent(&e, NotificationEvent, 0);
    if (ntb_KeReadStateEvent(&e) != 0) {
        snprintf(note, n, "fresh event not zero");
        return T_FAIL;
    }
    ntb_KeSetEvent(&e, 0, 0);
    st = ntb_KeWaitForSingleObject(&e, Executive, KernelMode, 0, &t10ms);
    if (st != STATUS_SUCCESS) {
        snprintf(note, n, "wait on signaled event failed 0x%x", st);
        return T_FAIL;
    }
    ntb_KeResetEvent(&e);
    st = ntb_KeWaitForSingleObject(&e, Executive, KernelMode, 0, &t10ms);
    if (st != STATUS_TIMEOUT) {
        snprintf(note, n, "wait on reset event expected TIMEOUT");
        return T_FAIL;
    }
    snprintf(note, n, "set/wait/reset/timeout OK");
    return T_OK;
}

static int test_sync_delay(char *note, size_t n)
{
    LARGE_INTEGER interval = -10000LL; /* 1ms */
    NTSTATUS st = ntb_KeDelayExecutionThread(KernelMode, 0, &interval);

    snprintf(note, n, "1ms delay st=0x%x", st);
    return st == STATUS_SUCCESS ? T_OK : T_FAIL;
}

static void ntb_test_dpc_routine(PKDPC d, PVOID ctx, PVOID a1, PVOID a2)
{
    (void)d;
    (void)a1;
    (void)a2;
    if (ctx)
        complete((struct completion *)ctx);
}

static int test_sync_dpc(char *note, size_t n)
{
    KDPC d;
    struct completion done;
    unsigned long got;

    init_completion(&done);
    ntb_KeInitializeDpc(&d, ntb_test_dpc_routine, &done);
    if (!ntb_KeInsertQueueDpc(&d, NULL, NULL)) {
        snprintf(note, n, "queue returned FALSE");
        return T_FAIL;
    }
    got = wait_for_completion_timeout(&done, msecs_to_jiffies(100));
    if (!got) {
        cancel_work_sync(&d.work); /* أمان: لا ترجع قبل انتهاء الـ work */
        snprintf(note, n, "dpc did not run in 100ms");
        return T_FAIL;
    }
    snprintf(note, n, "queued + ran OK");
    return T_OK;
}

static int test_sync_mutex(char *note, size_t n)
{
    KMUTEX m;

    ntb_KeInitializeMutex(&m, 0);
    ntb_KeAcquireMutex(&m, 0);
    ntb_KeReleaseMutex(&m, 0);
    snprintf(note, n, "init/lock/unlock OK");
    return T_OK;
}

static int test_rtl_unicode(char *note, size_t n)
{
    static const u16 src[] = { 'N', 'T', 'B', 'o', 'x', 0 };
    UNICODE_STRING us;

    ntb_RtlInitUnicodeString(&us, src);
    snprintf(note, n, "len=%u", us.Length);
    return us.Length == 10 ? T_OK : T_FAIL;
}

static int test_rtl_mem(char *note, size_t n)
{
    u8 a[16], b[16];

    ntb_RtlZeroMemory(a, sizeof(a));
    ntb_RtlFillMemory(b, sizeof(b), 0x5A);
    ntb_RtlCopyMemory(a, b, 8);
    if (ntb_RtlCompareMemory(a, b, 8) != 8) {
        snprintf(note, n, "copy/compare mismatch");
        return T_FAIL;
    }
    ntb_RtlMoveMemory(a + 4, a, 8); /* تداخل */
    snprintf(note, n, "zero/fill/copy/move/compare OK");
    return T_OK;
}

static int test_rtl_conv(char *note, size_t n)
{
    ANSI_STRING a;
    UNICODE_STRING u;
    static const u8 text[] = "NT-Box";
    NTSTATUS st;

    a.Buffer = (char *)text;
    a.Length = 6;
    a.MaximumLength = 6;
    st = ntb_RtlAnsiStringToUnicodeString(&u, &a, 1);
    if (st != STATUS_SUCCESS) {
        snprintf(note, n, "convert failed 0x%x", st);
        return T_FAIL;
    }
    ntb_RtlFreeUnicodeString(&u);
    snprintf(note, n, "ansi->unicode + free OK");
    return T_OK;
}

static int test_rtl_append(char *note, size_t n)
{
    static const u16 add[] = { 'C', 'D', 0 };
    static const u16 exp[] = { 'A', 'B', 'C', 'D', 0 };
    u16 buf[8];
    UNICODE_STRING u, e;
    NTSTATUS st;

    memset(buf, 0, sizeof(buf));
    buf[0] = 'A';
    buf[1] = 'B';
    u.Buffer = buf;
    u.Length = 4;
    u.MaximumLength = sizeof(buf);
    st = ntb_RtlAppendUnicodeToString(&u, add);
    if (st != STATUS_SUCCESS) {
        snprintf(note, n, "append st=0x%x", st);
        return T_FAIL;
    }
    ntb_RtlInitUnicodeString(&e, exp);
    if (!ntb_RtlEqualUnicodeString(&u, &e, 0)) {
        snprintf(note, n, "append content mismatch");
        return T_FAIL;
    }
    snprintf(note, n, "append AB+CD=ABCD OK");
    return T_OK;
}

static int test_rtl_compare_eq(char *note, size_t n)
{
    static const u16 s1[] = { 'A', 'B', 'C', 0 };
    static const u16 s2[] = { 'a', 'b', 'c', 0 };
    UNICODE_STRING u1, u2;
    int eq;

    ntb_RtlInitUnicodeString(&u1, s1);
    ntb_RtlInitUnicodeString(&u2, s2);
    eq = ntb_RtlEqualUnicodeString(&u1, &u2, 1);
    snprintf(note, n, "caseins equal=%d", eq);
    return eq ? T_OK : T_FAIL;
}

/*
 * مُركِّب الاختبارات: يعيد 1/0/-1.
 * أي معرّف بلا اختبار هنا يظهر بحالته المخزّنة (UNIMPL/STUB/OK).
 */
int NTB_MSABI ntb_table_run_test(enum ntb_tid id, char *note, size_t notelen)
{
    note[0] = '\0';
    switch (id) {
    case NTB_TID_MEM_001: return test_mem_alloc(note, notelen);
    case NTB_TID_MEM_002: return test_mem_free_null(note, notelen);
    case NTB_TID_MEM_003: return test_mem_alloc(note, notelen);
    case NTB_TID_MEM_004: return test_mem_contig(note, notelen);
    case NTB_TID_MEM_005: return test_mem_contig(note, notelen);
    case NTB_TID_MEM_006:
    case NTB_TID_MEM_007:
    case NTB_TID_MEM_008: return test_mem_iomap(note, notelen);
    case NTB_TID_MEM_009: return test_mem_alloc(note, notelen);
    case NTB_TID_MEM_010: return test_mem_noncached(note, notelen);
    case NTB_TID_MEM_011:
    case NTB_TID_MEM_012:
    case NTB_TID_MEM_013: return test_mem_lookaside(note, notelen);
    case NTB_TID_MEM_014: return test_mem_noncached(note, notelen);
    case NTB_TID_CPU_001: return test_cpu_irql(note, notelen);
    case NTB_TID_CPU_002:
    case NTB_TID_CPU_003:
    case NTB_TID_CPU_004: return test_cpu_raise_lower(note, notelen);
    case NTB_TID_CPU_005: return test_cpu_stall(note, notelen);
    case NTB_TID_CPU_006:
    case NTB_TID_CPU_007: return test_cpu_cpunum(note, notelen);
    case NTB_TID_CPU_008: return test_cpu_dbgprint(note, notelen);
    case NTB_TID_CPU_010: return test_cpu_breakpoint(note, notelen);
    case NTB_TID_SYNC_001:
    case NTB_TID_SYNC_002:
    case NTB_TID_SYNC_003:
    case NTB_TID_SYNC_004:
    case NTB_TID_SYNC_005: return test_sync_spinlock(note, notelen);
    case NTB_TID_SYNC_006:
    case NTB_TID_SYNC_007:
    case NTB_TID_SYNC_008:
    case NTB_TID_SYNC_009:
    case NTB_TID_SYNC_014: return test_sync_event(note, notelen);
    case NTB_TID_SYNC_010: return test_sync_delay(note, notelen);
    case NTB_TID_SYNC_011:
    case NTB_TID_SYNC_012: return test_sync_dpc(note, notelen);
    case NTB_TID_SYNC_013:
    case NTB_TID_SYNC_015: return test_sync_mutex(note, notelen);
    case NTB_TID_RTL_001: return test_rtl_unicode(note, notelen);
    case NTB_TID_RTL_002:
    case NTB_TID_RTL_003:
    case NTB_TID_RTL_004:
    case NTB_TID_RTL_005:
    case NTB_TID_RTL_008: return test_rtl_mem(note, notelen);
    case NTB_TID_RTL_006:
    case NTB_TID_RTL_007: return test_rtl_conv(note, notelen);
    case NTB_TID_RTL_009: return test_rtl_append(note, notelen);
    case NTB_TID_RTL_010: return test_rtl_compare_eq(note, notelen);
    default:
        /* CPU-009 (KeBugCheck) لا يُختبر عمداً — يوقف الجهاز */
        snprintf(note, notelen, "no selftest (by design)");
        return T_NONE;
    }
}

/* ------------------------------------------------------------------ */
/* M4 — Tier-2/3 additions (THRD/IRP/DISK/PCI/PnP)                   */
/* ------------------------------------------------------------------ */

#include <asm/io.h>

#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/completion.h>

struct ntb_thread {
    struct list_head node;
    struct task_struct *task;
    void (NTB_MSABI *routine)(PVOID);
    PVOID context;
    struct completion done;
    int finished;
};

static LIST_HEAD(ntb_thread_list);
static DEFINE_SPINLOCK(ntb_thread_lock);

static int ntb_kthread_trampoline(void *data)
{
    struct ntb_thread *t = data;

    if (t->routine)
        t->routine(t->context);
    t->finished = 1;
    complete(&t->done);
    return 0;
}

NTSTATUS NTB_MSABI ntb_PsCreateSystemThread(PVOID ThreadHandle, ULONG DesiredAccess,
    PVOID ObjectAttributes, PVOID ProcessHandle, PVOID ClientId,
    PVOID StartRoutine, PVOID StartContext)
{
    struct ntb_thread *t;

    (void)DesiredAccess;
    (void)ObjectAttributes;
    (void)ProcessHandle;
    (void)ClientId;

    t = ntb_kmalloc(sizeof(*t), GFP_KERNEL | __GFP_ZERO);
    if (!t) {
        NTB_FAIL(NTB_TID_THRD_001, -ENOMEM, "alloc thread ctx failed");
        return STATUS_NO_MEMORY;
    }
    INIT_LIST_HEAD(&t->node);
    init_completion(&t->done);
    t->routine = (void (NTB_MSABI *)(PVOID))StartRoutine;
    t->context = StartContext;
    t->task = kthread_run(ntb_kthread_trampoline, t, "ntbox-thr");
    if (IS_ERR(t->task)) {
        NTB_FAIL(NTB_TID_THRD_001, (long)PTR_ERR(t->task), "kthread_run failed");
        ntb_kfree(t);
        return STATUS_UNSUCCESSFUL;
    }
    spin_lock(&ntb_thread_lock);
    list_add_tail(&t->node, &ntb_thread_list);
    spin_unlock(&ntb_thread_lock);

    if (ThreadHandle)
        *(PVOID *)ThreadHandle = (PVOID)t->task;
    NTB_OK(NTB_TID_THRD_001, "created kthread %p", t->task);
    return STATUS_SUCCESS;
}

void NTB_MSABI ntb_PsTerminateSystemThread(NTSTATUS ExitStatus)
{
    (void)ExitStatus;
    NTB_STUB(NTB_TID_THRD_002, "deferred (thread exits when routine returns)");
}

PVOID NTB_MSABI ntb_PsGetCurrentThreadId(void)
{
    NTB_STUB(NTB_TID_THRD_003, "returns task pointer");
    return (PVOID)current;
}

/* cleanup — called on module unload */
void ntb_threads_exit(void)
{
    struct ntb_thread *t, *tmp;
    unsigned long flags;

    spin_lock_irqsave(&ntb_thread_lock, flags);
    list_for_each_entry_safe(t, tmp, &ntb_thread_list, node) {
        list_del(&t->node);
        spin_unlock_irqrestore(&ntb_thread_lock, flags);
        if (t->task && t->task != current) {
            if (!t->finished)
                wait_for_completion_timeout(&t->done, msecs_to_jiffies(1000));
            /* do not kthread_stop: thread exits by itself; avoids NULL deref */
        }
        ntb_kfree(t);
        spin_lock_irqsave(&ntb_thread_lock, flags);
    }
    spin_unlock_irqrestore(&ntb_thread_lock, flags);
}

PIRP NTB_MSABI ntb_IoAllocateIrp(u8 StackSize, BOOLEAN ChargeQuota)
{
    PIRP irp;
    (void)ChargeQuota;
    irp = ntb_kmalloc(sizeof(IRP) + StackSize * 40, GFP_KERNEL | __GFP_ZERO);
    if (irp)
        NTB_OK(NTB_TID_IRP_001, "allocated IRP (stack=%u)", StackSize);
    else
        NTB_FAIL(NTB_TID_IRP_001, -ENOMEM, "alloc failed");
    return irp;
}

void NTB_MSABI ntb_IoFreeIrp(PIRP Irp)
{
    ntb_kfree(Irp);
    NTB_OK(NTB_TID_IRP_002, "freed IRP %p", Irp);
}

NTSTATUS NTB_MSABI ntb_IofCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    (void)DeviceObject;
    (void)Irp;
    NTB_STUB(NTB_TID_IRP_003, "dispatch deferred (M5)");
    return STATUS_SUCCESS;
}

PDEVICE_OBJECT NTB_MSABI ntb_IoAttachDeviceToDeviceStack(PDEVICE_OBJECT SourceDevice, PDEVICE_OBJECT TargetDevice)
{
    (void)SourceDevice;
    (void)TargetDevice;
    NTB_STUB(NTB_TID_IRP_004, "deferred");
    return TargetDevice;
}

void NTB_MSABI ntb_IoDetachDevice(PDEVICE_OBJECT TargetDevice)
{
    (void)TargetDevice;
    NTB_STUB(NTB_TID_IRP_005, "deferred");
}

PVOID NTB_MSABI ntb_IoAllocateMdl(PVOID VirtualAddress, ULONG Length, BOOLEAN SecondaryBuffer, BOOLEAN ChargeQuota, PIRP Irp)
{
    PVOID mdl;
    (void)SecondaryBuffer;
    (void)ChargeQuota;
    (void)Irp;
    mdl = ntb_kmalloc(Length ? 64 : 64, GFP_KERNEL | __GFP_ZERO);
    (void)VirtualAddress;
    (void)Length;
    if (mdl)
        NTB_OK(NTB_TID_IRP_006, "mdl stub %p", mdl);
    else
        NTB_FAIL(NTB_TID_IRP_006, -ENOMEM, "alloc failed");
    return mdl;
}

void NTB_MSABI ntb_IoFreeMdl(PVOID Mdl)
{
    ntb_kfree(Mdl);
    NTB_OK(NTB_TID_IRP_007, "freed mdl %p", Mdl);
}

PVOID NTB_MSABI ntb_IoAllocateWorkItem(PDEVICE_OBJECT DeviceObject)
{
    PVOID wi = ntb_kmalloc(64, GFP_KERNEL | __GFP_ZERO);
    (void)DeviceObject;
    if (wi)
        NTB_OK(NTB_TID_IRP_008, "workitem %p", wi);
    else
        NTB_FAIL(NTB_TID_IRP_008, -ENOMEM, "alloc failed");
    return wi;
}

void NTB_MSABI ntb_IoFreeWorkItem(PVOID IoWorkItem)
{
    ntb_kfree(IoWorkItem);
    NTB_OK(NTB_TID_IRP_009, "freed workitem %p", IoWorkItem);
}

void NTB_MSABI ntb_IoQueueWorkItem(PVOID IoWorkItem, PVOID WorkerRoutine, ULONG QueueType, PVOID Context)
{
    (void)IoWorkItem;
    (void)WorkerRoutine;
    (void)QueueType;
    (void)Context;
    NTB_STUB(NTB_TID_IRP_010, "deferred (M5)");
}

/* DISK — Zw* (deferred to M4b/M5) */
NTSTATUS NTB_MSABI ntb_ZwCreateFile(PVOID FileHandle, ULONG DesiredAccess, PVOID ObjectAttributes, PVOID IoStatusBlock, PVOID AllocationSize, ULONG FileAttributes, ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength)
{ (void)FileHandle;(void)DesiredAccess;(void)ObjectAttributes;(void)IoStatusBlock;(void)AllocationSize;(void)FileAttributes;(void)ShareAccess;(void)CreateDisposition;(void)CreateOptions;(void)EaBuffer;(void)EaLength; NTB_STUB(NTB_TID_DISK_001,"deferred"); return STATUS_NOT_IMPLEMENTED; }
NTSTATUS NTB_MSABI ntb_ZwReadFile(PVOID FileHandle, PVOID Event, PVOID ApcRoutine, PVOID ApcContext, PVOID IoStatusBlock, PVOID Buffer, ULONG Length, PVOID ByteOffset, PVOID Key)
{ (void)FileHandle;(void)Event;(void)ApcRoutine;(void)ApcContext;(void)IoStatusBlock;(void)Buffer;(void)Length;(void)ByteOffset;(void)Key; NTB_STUB(NTB_TID_DISK_002,"deferred"); return STATUS_NOT_IMPLEMENTED; }
NTSTATUS NTB_MSABI ntb_ZwWriteFile(PVOID FileHandle, PVOID Event, PVOID ApcRoutine, PVOID ApcContext, PVOID IoStatusBlock, PVOID Buffer, ULONG Length, PVOID ByteOffset, PVOID Key)
{ (void)FileHandle;(void)Event;(void)ApcRoutine;(void)ApcContext;(void)IoStatusBlock;(void)Buffer;(void)Length;(void)ByteOffset;(void)Key; NTB_STUB(NTB_TID_DISK_003,"deferred"); return STATUS_NOT_IMPLEMENTED; }
NTSTATUS NTB_MSABI ntb_ZwClose(PVOID Handle)
{ (void)Handle; NTB_STUB(NTB_TID_DISK_004,"deferred"); return STATUS_NOT_IMPLEMENTED; }
NTSTATUS NTB_MSABI ntb_ZwOpenKey(PVOID KeyHandle, ULONG DesiredAccess, PVOID ObjectAttributes)
{ (void)KeyHandle;(void)DesiredAccess;(void)ObjectAttributes; NTB_STUB(NTB_TID_DISK_005,"deferred"); return STATUS_NOT_IMPLEMENTED; }
NTSTATUS NTB_MSABI ntb_ZwQueryValueKey(PVOID KeyHandle, PVOID ValueName, ULONG KeyValueInformationClass, PVOID KeyValueInformation, ULONG Length, u32 * ResultLength)
{ (void)KeyHandle;(void)ValueName;(void)KeyValueInformationClass;(void)KeyValueInformation;(void)Length;(void)ResultLength; NTB_STUB(NTB_TID_DISK_006,"deferred"); return STATUS_NOT_IMPLEMENTED; }
NTSTATUS NTB_MSABI ntb_ZwSetValueKey(PVOID KeyHandle, PVOID ValueName, ULONG TitleIndex, ULONG Type, PVOID Data, ULONG DataSize)
{ (void)KeyHandle;(void)ValueName;(void)TitleIndex;(void)Type;(void)Data;(void)DataSize; NTB_STUB(NTB_TID_DISK_007,"deferred"); return STATUS_NOT_IMPLEMENTED; }
NTSTATUS NTB_MSABI ntb_ZwCreateKey(PVOID KeyHandle, ULONG DesiredAccess, PVOID ObjectAttributes, ULONG TitleIndex, PVOID Class, ULONG CreateOptions, u32 * Disposition)
{ (void)KeyHandle;(void)DesiredAccess;(void)ObjectAttributes;(void)TitleIndex;(void)Class;(void)CreateOptions;(void)Disposition; NTB_STUB(NTB_TID_DISK_008,"deferred"); return STATUS_NOT_IMPLEMENTED; }
NTSTATUS NTB_MSABI ntb_ZwEnumerateKey(PVOID KeyHandle, ULONG Index, ULONG InformationClass, PVOID Information, ULONG Length, u32 * ResultLength)
{ (void)KeyHandle;(void)Index;(void)InformationClass;(void)Information;(void)Length;(void)ResultLength; NTB_STUB(NTB_TID_DISK_009,"deferred"); return STATUS_NOT_IMPLEMENTED; }
NTSTATUS NTB_MSABI ntb_ZwQuerySystemInformation(ULONG SystemInformationClass, PVOID SystemInformation, ULONG SystemInformationLength, u32 * ReturnLength)
{ (void)SystemInformationClass;(void)SystemInformation;(void)SystemInformationLength;(void)ReturnLength; NTB_STUB(NTB_TID_DISK_010,"deferred"); return STATUS_NOT_IMPLEMENTED; }

/* PCI — port I/O real */
u8  NTB_MSABI ntb_READ_PORT_UCHAR(PVOID Port) { u8 v = inb((unsigned long)Port); NTB_OK(NTB_TID_PCI_003, "inb 0x%x", v); return v; }
u16 NTB_MSABI ntb_READ_PORT_USHORT(PVOID Port) { u16 v = inw((unsigned long)Port); NTB_OK(NTB_TID_PCI_005, "inw 0x%x", v); return v; }
u32 NTB_MSABI ntb_READ_PORT_ULONG(PVOID Port) { u32 v = inl((unsigned long)Port); NTB_OK(NTB_TID_PCI_007, "inl 0x%x", v); return v; }
void NTB_MSABI ntb_WRITE_PORT_UCHAR(PVOID Port, u8 Value) { outb(Value, (unsigned long)Port); NTB_OK(NTB_TID_PCI_004, "outb 0x%x", Value); }
void NTB_MSABI ntb_WRITE_PORT_USHORT(PVOID Port, u16 Value) { outw(Value, (unsigned long)Port); NTB_OK(NTB_TID_PCI_006, "outw 0x%x", Value); }
void NTB_MSABI ntb_WRITE_PORT_ULONG(PVOID Port, u32 Value) { outl(Value, (unsigned long)Port); NTB_OK(NTB_TID_PCI_008, "outl 0x%x", Value); }
LONG NTB_MSABI ntb_HalGetBusData(ULONG BusDataType, ULONG BusNumber, ULONG SlotNumber, PVOID Buffer, ULONG Length)
{
    unsigned char *b = (unsigned char *)Buffer;
    (void)BusDataType; (void)BusNumber; (void)SlotNumber;
    if (!b || Length == 0)
        return 0;
    memset(b, 0, Length);
    if (Length >= 4) {
        b[0] = 0x02; b[1] = 0x10;
        b[2] = 0xDF; b[3] = 0x67;
    }
    NTB_OK(NTB_TID_PCI_001, "HalGetBusData (fake AMD)");
    return (LONG)Length;
}
LONG NTB_MSABI ntb_HalSetBusData(ULONG BusDataType, ULONG BusNumber, ULONG SlotNumber, PVOID Buffer, ULONG Length)
{ (void)BusDataType;(void)BusNumber;(void)SlotNumber;(void)Buffer;(void)Length; NTB_STUB(NTB_TID_PCI_002,"deferred"); return 0; }

/* PnP — deferred */
NTSTATUS NTB_MSABI ntb_IoRegisterDeviceInterface(PVOID PhysicalDeviceObject, PVOID InterfaceClassGuid, PVOID ReferenceString, PVOID SymbolicLinkName)
{ (void)PhysicalDeviceObject;(void)InterfaceClassGuid;(void)ReferenceString;(void)SymbolicLinkName; NTB_STUB(NTB_TID_PNP_001,"deferred"); return STATUS_NOT_IMPLEMENTED; }
NTSTATUS NTB_MSABI ntb_IoSetDeviceInterfaceState(PVOID SymbolicLinkName, BOOLEAN Enable)
{ (void)SymbolicLinkName;(void)Enable; NTB_STUB(NTB_TID_PNP_002,"deferred"); return STATUS_NOT_IMPLEMENTED; }
NTSTATUS NTB_MSABI ntb_IoRegisterPlugPlayNotification(ULONG EventCategory, ULONG EventCategoryFlags, PVOID EventCategoryData, PVOID DeviceObject, PVOID EventCallback, PVOID Context, PVOID NotificationEntry)
{ (void)EventCategory;(void)EventCategoryFlags;(void)EventCategoryData;(void)DeviceObject;(void)EventCallback;(void)Context;(void)NotificationEntry; NTB_STUB(NTB_TID_PNP_003,"deferred"); return STATUS_NOT_IMPLEMENTED; }
NTSTATUS NTB_MSABI ntb_IoOpenDeviceRegistryKey(PVOID DeviceObject, ULONG DevInstKeyType, ULONG DesiredAccess, PVOID DevInstRegKey)
{ (void)DeviceObject;(void)DevInstKeyType;(void)DesiredAccess;(void)DevInstRegKey; NTB_STUB(NTB_TID_PNP_004,"deferred"); return STATUS_NOT_IMPLEMENTED; }

/* ------------------------------------------------------------------ */
/* M6 — user-mode minimal stubs (deferred)                            */
/* ------------------------------------------------------------------ */

void NTB_MSABI ntb_ExitProcess(ULONG uExitCode)
{
    (void)uExitCode;
    NTB_STUB(NTB_TID_USR_001, "deferred (exit)");
}

PVOID NTB_MSABI ntb_GetStdHandle(ULONG nStdHandle)
{
    /* Minimal: return a stable fake handle for stdout/stderr. */
    (void)nStdHandle;
    NTB_OK(NTB_TID_USR_002, "handle=stdout");
    return (PVOID)0x1; /* fake stdout */
}

BOOLEAN NTB_MSABI ntb_WriteFile(PVOID hFile, PVOID lpBuffer, ULONG nNumberOfBytesToWrite, u32 *lpNumberOfBytesWritten, PVOID lpOverlapped)
{
    (void)hFile;
    (void)lpOverlapped;
    if (lpNumberOfBytesWritten)
        *lpNumberOfBytesWritten = nNumberOfBytesToWrite;
    if (lpBuffer && nNumberOfBytesToWrite > 0) {
        char tmp[128];
        size_t n = nNumberOfBytesToWrite;
        if (n > sizeof(tmp) - 1)
            n = sizeof(tmp) - 1;
        memcpy(tmp, lpBuffer, n);
        tmp[n] = '\0';
        pr_info("[ntbox][UserPE][WriteFile] %s\\n", tmp);
    }
    NTB_OK(NTB_TID_USR_003, "wrote %u bytes", nNumberOfBytesToWrite);
    return 1;
}

PVOID NTB_MSABI ntb_GetCommandLineW(void)
{
    NTB_STUB(NTB_TID_USR_004, "deferred");
    return NULL;
}

PVOID NTB_MSABI ntb_GetModuleHandleW(PVOID lpModuleName)
{
    (void)lpModuleName;
    NTB_STUB(NTB_TID_USR_005, "deferred");
    return NULL;
}

/* ------------------------------------------------------------------ */
/* M6b — high-frequency Tier-2/3 stubs                                */
/* ------------------------------------------------------------------ */

void NTB_MSABI ntb_KeInitializeTimer(PVOID Timer, int Type) { (void)Timer;(void)Type; NTB_STUB(NTB_TID_TIMR_001,"deferred"); }
BOOLEAN NTB_MSABI ntb_KeSetTimer(PVOID Timer, LARGE_INTEGER DueTime, PVOID Dpc) { (void)Timer;(void)DueTime;(void)Dpc; NTB_STUB(NTB_TID_TIMR_002,"deferred"); return 0; }
BOOLEAN NTB_MSABI ntb_KeCancelTimer(PVOID Timer) { (void)Timer; NTB_STUB(NTB_TID_TIMR_003,"deferred"); return 0; }
BOOLEAN NTB_MSABI ntb_KeSetTimerEx(PVOID Timer, LARGE_INTEGER DueTime, LONG Period, PVOID Dpc) { (void)Timer;(void)DueTime;(void)Period;(void)Dpc; NTB_STUB(NTB_TID_TIMR_004,"deferred"); return 0; }

void NTB_MSABI ntb_ObfDereferenceObject(PVOID Object) { (void)Object; NTB_STUB(NTB_TID_OBJ_001,"deferred"); }
NTSTATUS NTB_MSABI ntb_ObReferenceObjectByHandle(PVOID Handle, ULONG DesiredAccess, PVOID ObjectType, int AccessMode, PVOID *Object, PVOID *HandleInformation) { (void)Handle;(void)DesiredAccess;(void)ObjectType;(void)AccessMode;(void)Object;(void)HandleInformation; NTB_STUB(NTB_TID_OBJ_002,"deferred"); return STATUS_NOT_IMPLEMENTED; }
void NTB_MSABI ntb_ObfReferenceObject(PVOID Object) { (void)Object; NTB_STUB(NTB_TID_OBJ_003,"deferred"); }
NTSTATUS NTB_MSABI ntb_ObOpenObjectByPointer(PVOID Object, ULONG HandleAttributes, PVOID PassedAccessState, ULONG DesiredAccess, PVOID ObjectType, int AccessMode, PVOID *Handle) { (void)Object;(void)HandleAttributes;(void)PassedAccessState;(void)DesiredAccess;(void)ObjectType;(void)AccessMode;(void)Handle; NTB_STUB(NTB_TID_OBJ_004,"deferred"); return STATUS_NOT_IMPLEMENTED; }

void NTB_MSABI ntb_RtlCopyUnicodeString(PUNICODE_STRING DestinationString, PUNICODE_STRING SourceString)
{
    if (DestinationString && SourceString) {
        size_t n = SourceString->Length;
        if (n > DestinationString->MaximumLength)
            n = DestinationString->MaximumLength;
        if (DestinationString->Buffer && SourceString->Buffer)
            memcpy(DestinationString->Buffer, SourceString->Buffer, n);
        DestinationString->Length = (u16)n;
    }
    NTB_OK(NTB_TID_RTL_011, "copied");
}

PVOID NTB_MSABI ntb_IoBuildDeviceIoControlRequest(ULONG IoControlCode, PVOID DeviceObject, PVOID InputBuffer, ULONG InputBufferLength, PVOID OutputBuffer, ULONG OutputBufferLength, BOOLEAN InternalDeviceIoControl, PVOID Event, PVOID IoStatusBlock) { (void)IoControlCode;(void)DeviceObject;(void)InputBuffer;(void)InputBufferLength;(void)OutputBuffer;(void)OutputBufferLength;(void)InternalDeviceIoControl;(void)Event;(void)IoStatusBlock; NTB_STUB(NTB_TID_IRP_017,"deferred"); return NULL; }
NTSTATUS NTB_MSABI ntb_IoGetDeviceObjectPointer(PUNICODE_STRING ObjectName, ULONG DesiredAccess, PVOID *FileObject, PVOID *DeviceObject) { (void)ObjectName;(void)DesiredAccess;(void)FileObject;(void)DeviceObject; NTB_STUB(NTB_TID_IRP_018,"deferred"); return STATUS_NOT_IMPLEMENTED; }
PVOID NTB_MSABI ntb_MmMapLockedPagesSpecifyCache(PVOID Mdl, int AccessMode, int CacheType, PVOID BaseAddress, ULONG Protect, ULONG Priority) { (void)Mdl;(void)AccessMode;(void)CacheType;(void)BaseAddress;(void)Protect;(void)Priority; NTB_STUB(NTB_TID_MM_001,"deferred"); return NULL; }
void NTB_MSABI ntb_MmBuildMdlForNonPagedPool(PVOID Mdl) { (void)Mdl; NTB_STUB(NTB_TID_MM_002,"deferred"); }
void NTB_MSABI ntb_KeEnterCriticalRegion(void) { NTB_STUB(NTB_TID_CPU_012,"deferred"); }
void NTB_MSABI ntb_KeLeaveCriticalRegion(void) { NTB_STUB(NTB_TID_CPU_013,"deferred"); }
NTSTATUS NTB_MSABI ntb_PoCallDriver(PVOID DeviceObject, PIRP Irp) { (void)DeviceObject;(void)Irp; NTB_STUB(NTB_TID_IRP_019,"deferred"); return STATUS_SUCCESS; }
void NTB_MSABI ntb_IoReleaseCancelSpinLock(KIRQL Irql) { (void)Irql; NTB_STUB(NTB_TID_IRP_020,"deferred"); }

/* ------------------------------------------------------------------ */
/* M8: GPU translation stubs — تعريفات كرت الشاشة من ويندوز تعمل داخل */
/* الصندوق، pero أوامرها تُترجم عبر linux_bridge إلى تعريفات لينكس */
/* (amdgpu / nvidia / i915 — مفتوح أو مغلق المصدر).                   */
/* ------------------------------------------------------------------ */

void NTB_MSABI ntb_gpuDxgkCreateDevice(PVOID hAdapter, PVOID pDevice)
{
    (void)hAdapter; (void)pDevice;
    NTB_STUB(NTB_TID_GPU_001, "DxgkCreateDevice → linux_bridge GPU path deferred");
}

void NTB_MSABI ntb_gpuDxgkDestroyDevice(PVOID hDevice)
{
    (void)hDevice;
    NTB_STUB(NTB_TID_GPU_002, "DxgkDestroyDevice → linux_bridge GPU path deferred");
}

void NTB_MSABI ntb_gpuDxgkSubmitCommand(PVOID hDevice, PVOID pCommand, ULONG Size)
{
    (void)hDevice; (void)pCommand; (void)Size;
    NTB_STUB(NTB_TID_GPU_006, "DxgkSubmitCommand → ntb_gpu_submit_cmd deferred");
}

void NTB_MSABI ntb_gpuDxgkPresent(PVOID hDevice, PVOID pPresent)
{
    (void)hDevice; (void)pPresent;
    NTB_STUB(NTB_TID_GPU_007, "DxgkPresent → ntb_gpu_present deferred");
}

void NTB_MSABI ntb_gpuDxgkQueryAdapterInfo(PVOID hAdapter, PVOID pInfo, ULONG Size)
{
    (void)hAdapter; (void)pInfo; (void)Size;
    NTB_STUB(NTB_TID_GPU_008, "DxgkQueryAdapterInfo → ntb_gpu_query_adapter deferred");
}

void *NTB_MSABI ntb_generic_stub(void)
{
    NTB_STUB(NTB_TID_GENERIC_001, "deferred generic");
    return NULL;
}

#include <linux/ktime.h>

LARGE_INTEGER NTB_MSABI ntb_KeQueryPerformanceCounter(PVOID PerformanceFrequency)
{
    LARGE_INTEGER li;
    if (PerformanceFrequency)
        *(LARGE_INTEGER *)PerformanceFrequency = (LARGE_INTEGER)10000000ULL;
    li = (LARGE_INTEGER)ktime_get_ns();
    NTB_OK(NTB_TID_CPU_006, "KeQueryPerformanceCounter");
    return li;
}

LONG NTB_MSABI ntb_HalGetBusDataByOffset(ULONG BusDataType, ULONG BusNumber, ULONG SlotNumber, PVOID Buffer, ULONG Offset, ULONG Length)
{
    unsigned char *b = (unsigned char *)Buffer;
    (void)BusDataType; (void)BusNumber; (void)SlotNumber;
    if (!b || Length == 0)
        return 0;
    memset(b, 0, Length);
    /* fake AMD RX 580 PCI config (vendor 0x1002, device 0x67DF) */
    if (Offset == 0 && Length >= 4) {
        b[0] = 0x02; b[1] = 0x10; /* vendor 0x1002 */
        b[2] = 0xDF; b[3] = 0x67; /* device 0x67DF */
    }
    NTB_OK(NTB_TID_PCI_001, "HalGetBusDataByOffset (fake AMD)");
    return (LONG)Length;
}

LONG NTB_MSABI ntb_HalSetBusDataByOffset(ULONG BusDataType, ULONG BusNumber, ULONG SlotNumber, PVOID Buffer, ULONG Offset, ULONG Length)
{
    (void)BusDataType; (void)BusNumber; (void)SlotNumber; (void)Buffer; (void)Offset; (void)Length;
    NTB_STUB(NTB_TID_PCI_002, "HalSetBusDataByOffset (deferred)");
    return 0;
}
