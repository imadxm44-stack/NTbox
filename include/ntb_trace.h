#ifndef NTB_TRACE_H
#define NTB_TRACE_H

/*
 * DublKernel — نظام المصابيح المرقّم (PLAN_Translator_Diagnostics.md §3)
 * كل نقطة ترجمة تحمل ID ثابت وتسجّل حالة OK/STUB/FAIL/UNIMPL.
 * ممنوع أي تسجيل بدون ID.
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/proc_fs.h>

/* المصباح — سطر واحد مرتّب في dmesg */
#define NTB_TP(id, status, fmt, ...) \
    pr_info("[ntbox][%s][%s] " fmt "\n", id, status, ##__VA_ARGS__)

/* الحالات */
enum ntb_st {
    NTB_ST_UNIMPL = 0,
    NTB_ST_OK,
    NTB_ST_STUB,
    NTB_ST_FAIL,
};

/* قائمة المعرفات — الأرقام ثابتة للأبد (لو حُذفت دالة يبقى رقمها محجوزاً) */
#define NTB_TRACE_LIST(X) \
    X(MEM_001, "MEM-001", "ExAllocatePoolWithTag") \
    X(MEM_002, "MEM-002", "ExFreePoolWithTag") \
    X(MEM_003, "MEM-003", "ExAllocatePool2") \
    X(MEM_004, "MEM-004", "MmAllocateContiguousMemory") \
    X(MEM_005, "MEM-005", "MmFreeContiguousMemory") \
    X(MEM_006, "MEM-006", "MmMapIoSpace") \
    X(MEM_007, "MEM-007", "MmUnmapIoSpace") \
    X(MEM_008, "MEM-008", "MmGetPhysicalAddress") \
    X(MEM_009, "MEM-009", "ExAllocatePoolWithTagPriority") \
    X(MEM_010, "MEM-010", "MmAllocateNonCachedMemory") \
    X(MEM_011, "MEM-011", "ExInitializeLookasidePoolEx") \
    X(MEM_012, "MEM-012", "ExFreeToLookasidePoolEx") \
    X(MEM_013, "MEM-013", "ExAllocateFromLookasidePoolEx") \
    X(MEM_014, "MEM-014", "MmFreeNonCachedMemory") \
    X(MEM_015, "MEM-015", "MmPageEntireDriver") \
    X(CPU_001, "CPU-001", "KeGetCurrentIrql") \
    X(CPU_002, "CPU-002", "KeRaiseIrql") \
    X(CPU_003, "CPU-003", "KeLowerIrql") \
    X(CPU_004, "CPU-004", "KfRaiseIrql") \
    X(CPU_005, "CPU-005", "KeStallExecutionProcessor") \
    X(CPU_006, "CPU-006", "KeGetCurrentProcessorNumber") \
    X(CPU_007, "CPU-007", "KeQueryActiveProcessorCount") \
    X(CPU_008, "CPU-008", "DbgPrint") \
    X(CPU_009, "CPU-009", "KeBugCheck") \
    X(CPU_010, "CPU-010", "DbgBreakPoint") \
    X(CPU_011, "CPU-011", "KeBugCheckEx") \
    X(SYNC_001, "SYNC-001", "KeInitializeSpinLock") \
    X(SYNC_002, "SYNC-002", "KeAcquireSpinLock") \
    X(SYNC_003, "SYNC-003", "KeReleaseSpinLock") \
    X(SYNC_004, "SYNC-004", "KeAcquireSpinLockAtDpcLevel") \
    X(SYNC_005, "SYNC-005", "KeReleaseSpinLockFromDpcLevel") \
    X(SYNC_006, "SYNC-006", "KeInitializeEvent") \
    X(SYNC_007, "SYNC-007", "KeSetEvent") \
    X(SYNC_008, "SYNC-008", "KeResetEvent") \
    X(SYNC_009, "SYNC-009", "KeWaitForSingleObject") \
    X(SYNC_010, "SYNC-010", "KeDelayExecutionThread") \
    X(SYNC_011, "SYNC-011", "KeInitializeDpc") \
    X(SYNC_012, "SYNC-012", "KeInsertQueueDpc") \
    X(SYNC_013, "SYNC-013", "KeInitializeMutex") \
    X(SYNC_014, "SYNC-014", "KeReadStateEvent") \
    X(SYNC_015, "SYNC-015", "KeAcquireMutex") \
    X(RTL_001, "RTL-001", "RtlInitUnicodeString") \
    X(RTL_002, "RTL-002", "RtlCopyMemory") \
    X(RTL_003, "RTL-003", "RtlZeroMemory") \
    X(RTL_004, "RTL-004", "RtlMoveMemory") \
    X(RTL_005, "RTL-005", "RtlFillMemory") \
    X(RTL_006, "RTL-006", "RtlAnsiStringToUnicodeString") \
    X(RTL_007, "RTL-007", "RtlFreeUnicodeString") \
    X(RTL_008, "RTL-008", "RtlCompareMemory") \
    X(RTL_009, "RTL-009", "RtlAppendUnicodeToString") \
    X(RTL_010, "RTL-010", "RtlEqualUnicodeString") \
    X(PAR_001, "PAR-001", "partitions scan (proc/partitions)") \
    X(PAR_002, "PAR-002", "dev exists check") \
    X(PAR_003, "PAR-003", "policy allow") \
    X(PAR_004, "PAR-004", "policy deny") \
    X(PAR_005, "PAR-005", "policy list") \
    X(PAR_006, "PAR-006", "attach drive") \
    X(PAR_007, "PAR-007", "detach drive") \
    X(PAR_008, "PAR-008", "open in place") \
    X(PAR_009, "PAR-009", "open PE in place (run)") \
    X(PAR_010, "PAR-010", "open other in place (info)") \
    X(PAR_011, "PAR-011", "result read") \
    X(IRP_011, "IRP-011", "IoCreateDevice") \
    X(IRP_012, "IRP-012", "IoDeleteDevice") \
    X(IRP_013, "IRP-013", "IoCreateSymbolicLink") \
    X(IRP_014, "IRP-014", "IoDeleteSymbolicLink") \
    X(IRP_015, "IRP-015", "IoStartNextPacket") \
    X(IRP_016, "IRP-016", "IofCompleteRequest") \
    X(THRD_001, "THRD-001", "PsCreateSystemThread") \
    X(THRD_002, "THRD-002", "PsTerminateSystemThread") \
    X(THRD_003, "THRD-003", "PsGetCurrentThreadId") \
    X(IRP_001, "IRP-001", "IoAllocateIrp") \
    X(IRP_002, "IRP-002", "IoFreeIrp") \
    X(IRP_003, "IRP-003", "IofCallDriver") \
    X(IRP_004, "IRP-004", "IoAttachDeviceToDeviceStack") \
    X(IRP_005, "IRP-005", "IoDetachDevice") \
    X(IRP_006, "IRP-006", "IoAllocateMdl") \
    X(IRP_007, "IRP-007", "IoFreeMdl") \
    X(IRP_008, "IRP-008", "IoAllocateWorkItem") \
    X(IRP_009, "IRP-009", "IoFreeWorkItem") \
    X(IRP_010, "IRP-010", "IoQueueWorkItem") \
    X(DISK_001, "DISK-001", "ZwCreateFile") \
    X(DISK_002, "DISK-002", "ZwReadFile") \
    X(DISK_003, "DISK-003", "ZwWriteFile") \
    X(DISK_004, "DISK-004", "ZwClose") \
    X(DISK_005, "DISK-005", "ZwOpenKey") \
    X(DISK_006, "DISK-006", "ZwQueryValueKey") \
    X(DISK_007, "DISK-007", "ZwSetValueKey") \
    X(DISK_008, "DISK-008", "ZwCreateKey") \
    X(DISK_009, "DISK-009", "ZwEnumerateKey") \
    X(DISK_010, "DISK-010", "ZwQuerySystemInformation") \
    X(PCI_001, "PCI-001", "HalGetBusData") \
    X(PCI_002, "PCI-002", "HalSetBusData") \
    X(PCI_003, "PCI-003", "READ_PORT_UCHAR") \
    X(PCI_004, "PCI-004", "WRITE_PORT_UCHAR") \
    X(PCI_005, "PCI-005", "READ_PORT_USHORT") \
    X(PCI_006, "PCI-006", "WRITE_PORT_USHORT") \
    X(PCI_007, "PCI-007", "READ_PORT_ULONG") \
    X(PCI_008, "PCI-008", "WRITE_PORT_ULONG") \
    X(PNP_001, "PNP-001", "IoRegisterDeviceInterface") \
    X(PNP_002, "PNP-002", "IoSetDeviceInterfaceState") \
    X(PNP_003, "PNP-003", "IoRegisterPlugPlayNotification") \
    X(PNP_004, "PNP-004", "IoOpenDeviceRegistryKey") \
    X(PE_001, "PE-001", "PE unresolved imports (abort)") \
    X(PE_002, "PE-002", "PE validation (sections/entry)") \
    X(USR_001, "USR-001", "ExitProcess") \
    X(USR_002, "USR-002", "GetStdHandle") \
    X(USR_003, "USR-003", "WriteFile") \
    X(USR_004, "USR-004", "GetCommandLineW") \
    X(USR_005, "USR-005", "GetModuleHandleW") \
    X(TIMR_001, "TIMR-001", "KeInitializeTimer") \
    X(TIMR_002, "TIMR-002", "KeSetTimer") \
    X(TIMR_003, "TIMR-003", "KeCancelTimer") \
    X(TIMR_004, "TIMR-004", "KeSetTimerEx") \
    X(OBJ_001, "OBJ-001", "ObfDereferenceObject") \
    X(OBJ_002, "OBJ-002", "ObReferenceObjectByHandle") \
    X(OBJ_003, "OBJ-003", "ObfReferenceObject") \
    X(OBJ_004, "OBJ-004", "ObOpenObjectByPointer") \
    X(RTL_011, "RTL-011", "RtlCopyUnicodeString") \
    X(IRP_017, "IRP-017", "IoBuildDeviceIoControlRequest") \
    X(IRP_018, "IRP-018", "IoGetDeviceObjectPointer") \
    X(MM_001, "MM-001", "MmMapLockedPagesSpecifyCache") \
    X(MM_002, "MM-002", "MmBuildMdlForNonPagedPool") \
    X(CPU_012, "CPU-012", "KeEnterCriticalRegion") \
    X(CPU_013, "CPU-013", "KeLeaveCriticalRegion") \
    X(IRP_019, "IRP-019", "PoCallDriver") \
    X(IRP_020, "IRP-020", "IoReleaseCancelSpinLock") \
    /* M8: GPU / كرت الشاشة — ترجمة أوامر ويندوز إلى نواة لينكس (DRM/KMS) */ \
    X(GENERIC_001, "GENERIC-001", "generic deferred stub") \
    X(GPU_001, "GPU-001", "gpu open render node") \
    X(GPU_002, "GPU-002", "gpu close render node") \
    X(GPU_003, "GPU-003", "gpu create GEM buffer") \
    X(GPU_004, "GPU-004", "gpu mmap GEM buffer") \
    X(GPU_005, "GPU-005", "gpu get dmabuf fd") \
    X(GPU_006, "GPU-006", "gpu submit command buffer") \
    X(GPU_007, "GPU-007", "gpu present / flip") \
    X(GPU_008, "GPU-008", "gpu query adapter info") \
    /* M9: جسر الكرت الحقيقي (T-209..T-212) — تسلسل جديد بعد GPU-008 */ \
    X(GPU_009, "GPU-009", "gpu drm ioctl bridge (user-copy bounce page)") \
    X(GPU_010, "GPU-010", "gpu amdgpu context create/free") \
    X(GPU_011, "GPU-011", "gpu amdgpu bo list create/destroy") \
    X(GPU_012, "GPU-012", "gpu amdgpu GPUVM va map/unmap") \
    X(GPU_013, "GPU-013", "gpu amdgpu CS fence wait") \
    X(GFX_020, "GFX-020", "gfx bridge init") \
    X(GFX_021, "GFX-021", "gfx d3d11 device create") \
    X(GFX_022, "GFX-022", "gfx swapchain create") \
    X(GFX_023, "GFX-023", "gfx resources create") \
    X(GFX_024, "GFX-024", "gfx present path") \
    X(GFX_025, "GFX-025", "gfx draw command") \
    X(GFX_026, "GFX-026", "gfx state bind") \
    X(GFX_027, "GFX-027", "gfx rtv create") \
    X(GFX_028, "GFX-028", "gfx dsv create") \
    X(GFX_029, "GFX-029", "gfx texture/buffer create")

enum ntb_tid {
#define X(e, s, n) NTB_TID_##e,
    NTB_TRACE_LIST(X)
#undef X
    NTB_TID_COUNT
};

struct ntb_tstat {
    u32 hits;
    u32 fails;
    s32 last_err;
    u8 st;
};

extern struct ntb_tstat ntb_ts[NTB_TID_COUNT];

void ntb_t_report(enum ntb_tid id, u8 st, long err, const char *fmt, ...);
const char *ntb_id_str(enum ntb_tid id);
const char *ntb_st_str(u8 st);

/* مصباح مختصر — OK/STUB يُسجّل أول مرة فقط، FAIL دائماً */
#define NTB_OK(id, fmt, ...)    ntb_t_report(id, NTB_ST_OK,   0, fmt, ##__VA_ARGS__)
#define NTB_STUB(id, fmt, ...)  ntb_t_report(id, NTB_ST_STUB, 0, fmt, ##__VA_ARGS__)
#define NTB_FAIL(id, err, fmt, ...) ntb_t_report(id, NTB_ST_FAIL, err, fmt, ##__VA_ARGS__)

/* /proc/ntbox/stats + /proc/ntbox/selftest */
int ntb_diag_init(struct proc_dir_entry *dir);
void ntb_diag_exit(struct proc_dir_entry *dir);

#endif
