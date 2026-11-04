#ifndef NTB_TABLE_H
#define NTB_TABLE_H

/*
 * DublKernel — جدول الترجمة Tier-1 (PLAN_Translator_Diagnostics.md §7)
 * كل دالة تحمل مصباحها عبر NTB_OK/NTB_STUB/NTB_FAIL.
 * ntb_table_run_test(): يشغّل حالة اختبار آمنة للمعرّف وتُستدعى من /proc/ntbox/selftest.
 */

#include "ntbox_types.h"
#include "ntb_trace.h"
#include <linux/types.h>

/* --- MEM: ذاكرة (عبر الجسر ntb_kmalloc/ntb_ioremap حصراً) --- */
void *NTB_MSABI ntb_ExAllocatePoolWithTag(POOL_TYPE PoolType, size_t n, ULONG Tag);
void  NTB_MSABI ntb_ExFreePoolWithTag(void *p, ULONG Tag);
void *NTB_MSABI ntb_ExAllocatePool2(ULONG Flags, size_t n, ULONG Tag);
void *NTB_MSABI ntb_ExAllocatePoolWithTagPriority(POOL_TYPE t, size_t n, ULONG Tag, int prio);
void *NTB_MSABI ntb_MmAllocateContiguousMemory(size_t n, LARGE_INTEGER Highest);
void  NTB_MSABI ntb_MmFreeContiguousMemory(void *p);
void __iomem *NTB_MSABI ntb_MmMapIoSpace(LARGE_INTEGER Phys, size_t n, int CacheType);
void  NTB_MSABI ntb_MmUnmapIoSpace(void __iomem *p, size_t n);
LARGE_INTEGER NTB_MSABI ntb_MmGetPhysicalAddress(void *v);
void *NTB_MSABI ntb_MmAllocateNonCachedMemory(size_t n);
void  NTB_MSABI ntb_MmFreeNonCachedMemory(void *p, size_t n);
void  NTB_MSABI ntb_MmPageEntireDriver(PVOID Address);
void  NTB_MSABI ntb_ExInitializeLookasidePoolEx(PNTB_LOOKASIDE la, size_t size);
void *NTB_MSABI ntb_ExAllocateFromLookasidePoolEx(PNTB_LOOKASIDE la);
void  NTB_MSABI ntb_ExFreeToLookasidePoolEx(PNTB_LOOKASIDE la, void *p);

/* --- CPU: IRQL/دقّة/أنوية/طباعة --- */
KIRQL NTB_MSABI ntb_KeGetCurrentIrql(void);
void  NTB_MSABI ntb_KeRaiseIrql(KIRQL New);
void  NTB_MSABI ntb_KeLowerIrql(KIRQL New);
KIRQL NTB_MSABI ntb_KfRaiseIrql(KIRQL New);
void  NTB_MSABI ntb_KfLowerIrql(KIRQL New);
void  NTB_MSABI ntb_KeStallExecutionProcessor(ULONG MicroSeconds);
LARGE_INTEGER NTB_MSABI ntb_KeQueryPerformanceCounter(PVOID PerformanceFrequency);
ULONG NTB_MSABI ntb_KeGetCurrentProcessorNumber(void);
ULONG NTB_MSABI ntb_KeQueryActiveProcessorCount(void);
ULONG NTB_MSABI ntb_DbgPrint(const char *fmt, ...);
void  NTB_MSABI ntb_KeBugCheck(ULONG Code);
void  NTB_MSABI ntb_KeBugCheckEx(ULONG Code, ULONG_PTR p1, ULONG_PTR p2, ULONG_PTR p3, ULONG_PTR p4);
void  NTB_MSABI ntb_DbgBreakPoint(void);

/* --- SYNC: أقفال/أحداث/وقت/DPC --- */
void NTB_MSABI ntb_KeInitializeSpinLock(PNTB_KSPIN_LOCK l);
void NTB_MSABI ntb_KeAcquireSpinLock(PNTB_KSPIN_LOCK l, PKIRQL OldIrql);
void NTB_MSABI ntb_KeReleaseSpinLock(PNTB_KSPIN_LOCK l, KIRQL OldIrql);
void NTB_MSABI ntb_KeAcquireSpinLockAtDpcLevel(PNTB_KSPIN_LOCK l);
void NTB_MSABI ntb_KeReleaseSpinLockFromDpcLevel(PNTB_KSPIN_LOCK l);
void NTB_MSABI ntb_KeInitializeEvent(PKEVENT e, KEVENT_TYPE type, BOOLEAN State);
LONG NTB_MSABI ntb_KeSetEvent(PKEVENT e, LONG Increment, BOOLEAN Wait);
BOOLEAN NTB_MSABI ntb_KeResetEvent(PKEVENT e);
BOOLEAN NTB_MSABI ntb_KeReadStateEvent(PKEVENT e);
NTSTATUS NTB_MSABI ntb_KeWaitForSingleObject(PVOID Object, KWAIT_REASON Reason,
                                   KPROCESSOR_MODE Mode, BOOLEAN Alertable,
                                   PLARGE_INTEGER Timeout);
NTSTATUS NTB_MSABI ntb_KeDelayExecutionThread(KPROCESSOR_MODE Mode, BOOLEAN Alertable,
                                    PLARGE_INTEGER Interval);
void NTB_MSABI ntb_KeInitializeDpc(PKDPC d, PKDEFERRED_ROUTINE Routine, PVOID ctx);
BOOLEAN NTB_MSABI ntb_KeInsertQueueDpc(PKDPC d, PVOID Arg1, PVOID Arg2);
void NTB_MSABI ntb_KeInitializeMutex(PKMUTEX m, ULONG Level);
NTSTATUS NTB_MSABI ntb_KeAcquireMutex(PKMUTEX m, BOOLEAN Waitable);
NTSTATUS NTB_MSABI ntb_KeReleaseMutex(PKMUTEX m, BOOLEAN Wait);

/* --- IRP: I/O Manager (Tier-2, M3) --- */
NTSTATUS NTB_MSABI ntb_IoCreateDevice(PDRIVER_OBJECT DriverObject, ULONG DevExtSize,
                            PUNICODE_STRING DeviceName, ULONG Type,
                            ULONG Characteristics, BOOLEAN Exclusive,
                            PDEVICE_OBJECT *DeviceObject);
void     NTB_MSABI ntb_IoDeleteDevice(PDEVICE_OBJECT DeviceObject);
NTSTATUS NTB_MSABI ntb_IoCreateSymbolicLink(PUNICODE_STRING LinkName,
                                  PUNICODE_STRING DeviceName);
NTSTATUS NTB_MSABI ntb_IoDeleteSymbolicLink(PUNICODE_STRING LinkName);
BOOLEAN  NTB_MSABI ntb_IoStartNextPacket(PDEVICE_OBJECT DeviceObject, BOOLEAN Cancelable);
void     NTB_MSABI ntb_IofCompleteRequest(PIRP Irp, int PriorityBoost);

/* --- THRD: خيوط (Tier-2) --- */
NTSTATUS NTB_MSABI ntb_PsCreateSystemThread(PVOID ThreadHandle, ULONG DesiredAccess,
    PVOID ObjectAttributes, PVOID ProcessHandle, PVOID ClientId,
    PVOID StartRoutine, PVOID StartContext);
void     NTB_MSABI ntb_PsTerminateSystemThread(NTSTATUS ExitStatus);
PVOID    NTB_MSABI ntb_PsGetCurrentThreadId(void);

/* --- IRP: Tier-2 additions --- */
PIRP     NTB_MSABI ntb_IoAllocateIrp(u8 StackSize, BOOLEAN ChargeQuota);
void     NTB_MSABI ntb_IoFreeIrp(PIRP Irp);
NTSTATUS NTB_MSABI ntb_IofCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp);
PDEVICE_OBJECT NTB_MSABI ntb_IoAttachDeviceToDeviceStack(PDEVICE_OBJECT SourceDevice, PDEVICE_OBJECT TargetDevice);
void     NTB_MSABI ntb_IoDetachDevice(PDEVICE_OBJECT TargetDevice);
PVOID    NTB_MSABI ntb_IoAllocateMdl(PVOID VirtualAddress, ULONG Length, BOOLEAN SecondaryBuffer, BOOLEAN ChargeQuota, PIRP Irp);
void     NTB_MSABI ntb_IoFreeMdl(PVOID Mdl);
PVOID    NTB_MSABI ntb_IoAllocateWorkItem(PDEVICE_OBJECT DeviceObject);
void     NTB_MSABI ntb_IoFreeWorkItem(PVOID IoWorkItem);
void     NTB_MSABI ntb_IoQueueWorkItem(PVOID IoWorkItem, PVOID WorkerRoutine, ULONG QueueType, PVOID Context);

/* --- DISK: Zw* (Tier-2, deferred) --- */
NTSTATUS NTB_MSABI ntb_ZwCreateFile(PVOID FileHandle, ULONG DesiredAccess, PVOID ObjectAttributes, PVOID IoStatusBlock, PVOID AllocationSize, ULONG FileAttributes, ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength);
NTSTATUS NTB_MSABI ntb_ZwReadFile(PVOID FileHandle, PVOID Event, PVOID ApcRoutine, PVOID ApcContext, PVOID IoStatusBlock, PVOID Buffer, ULONG Length, PVOID ByteOffset, PVOID Key);
NTSTATUS NTB_MSABI ntb_ZwWriteFile(PVOID FileHandle, PVOID Event, PVOID ApcRoutine, PVOID ApcContext, PVOID IoStatusBlock, PVOID Buffer, ULONG Length, PVOID ByteOffset, PVOID Key);
NTSTATUS NTB_MSABI ntb_ZwClose(PVOID Handle);
NTSTATUS NTB_MSABI ntb_ZwOpenKey(PVOID KeyHandle, ULONG DesiredAccess, PVOID ObjectAttributes);
NTSTATUS NTB_MSABI ntb_ZwQueryValueKey(PVOID KeyHandle, PVOID ValueName, ULONG KeyValueInformationClass, PVOID KeyValueInformation, ULONG Length, u32 * ResultLength);
NTSTATUS NTB_MSABI ntb_ZwSetValueKey(PVOID KeyHandle, PVOID ValueName, ULONG TitleIndex, ULONG Type, PVOID Data, ULONG DataSize);
NTSTATUS NTB_MSABI ntb_ZwCreateKey(PVOID KeyHandle, ULONG DesiredAccess, PVOID ObjectAttributes, ULONG TitleIndex, PVOID Class, ULONG CreateOptions, u32 * Disposition);
NTSTATUS NTB_MSABI ntb_ZwEnumerateKey(PVOID KeyHandle, ULONG Index, ULONG InformationClass, PVOID Information, ULONG Length, u32 * ResultLength);
NTSTATUS NTB_MSABI ntb_ZwQuerySystemInformation(ULONG SystemInformationClass, PVOID SystemInformation, ULONG SystemInformationLength, u32 * ReturnLength);

/* --- PCI: HAL port I/O (Tier-3) --- */
u8       NTB_MSABI ntb_READ_PORT_UCHAR(PVOID Port);
u16      NTB_MSABI ntb_READ_PORT_USHORT(PVOID Port);
u32      NTB_MSABI ntb_READ_PORT_ULONG(PVOID Port);
void     NTB_MSABI ntb_WRITE_PORT_UCHAR(PVOID Port, u8 Value);
void     NTB_MSABI ntb_WRITE_PORT_USHORT(PVOID Port, u16 Value);
void     NTB_MSABI ntb_WRITE_PORT_ULONG(PVOID Port, u32 Value);
LONG     NTB_MSABI ntb_HalGetBusData(ULONG BusDataType, ULONG BusNumber, ULONG SlotNumber, PVOID Buffer, ULONG Length);
LONG     NTB_MSABI ntb_HalGetBusDataByOffset(ULONG BusDataType, ULONG BusNumber, ULONG SlotNumber, PVOID Buffer, ULONG Offset, ULONG Length);
LONG     NTB_MSABI ntb_HalSetBusDataByOffset(ULONG BusDataType, ULONG BusNumber, ULONG SlotNumber, PVOID Buffer, ULONG Offset, ULONG Length);
LONG     NTB_MSABI ntb_HalSetBusData(ULONG BusDataType, ULONG BusNumber, ULONG SlotNumber, PVOID Buffer, ULONG Length);

/* --- PnP (Tier-3) --- */
NTSTATUS NTB_MSABI ntb_IoRegisterDeviceInterface(PVOID PhysicalDeviceObject, PVOID InterfaceClassGuid, PVOID ReferenceString, PVOID SymbolicLinkName);
NTSTATUS NTB_MSABI ntb_IoSetDeviceInterfaceState(PVOID SymbolicLinkName, BOOLEAN Enable);
NTSTATUS NTB_MSABI ntb_IoRegisterPlugPlayNotification(ULONG EventCategory, ULONG EventCategoryFlags, PVOID EventCategoryData, PVOID DeviceObject, PVOID EventCallback, PVOID Context, PVOID NotificationEntry);
NTSTATUS NTB_MSABI ntb_IoOpenDeviceRegistryKey(PVOID DeviceObject, ULONG DevInstKeyType, ULONG DesiredAccess, PVOID DevInstRegKey);

/* --- RTL: مكتبة النصوص/الذاكرة --- */
void NTB_MSABI ntb_RtlInitUnicodeString(PUNICODE_STRING dst, const u16 *src);
void NTB_MSABI ntb_RtlCopyMemory(void *dst, const void *src, size_t n);
void NTB_MSABI ntb_RtlZeroMemory(void *dst, size_t n);
void NTB_MSABI ntb_RtlMoveMemory(void *dst, const void *src, size_t n);
void NTB_MSABI ntb_RtlFillMemory(void *dst, size_t n, u8 value);
NTSTATUS NTB_MSABI ntb_RtlAnsiStringToUnicodeString(PUNICODE_STRING dst,
                                          PANSI_STRING src, BOOLEAN alloc);
void NTB_MSABI ntb_RtlFreeUnicodeString(PUNICODE_STRING s);
size_t NTB_MSABI ntb_RtlCompareMemory(const void *a, const void *b, size_t n);
NTSTATUS NTB_MSABI ntb_RtlAppendUnicodeToString(PUNICODE_STRING dst, const u16 *src);
BOOLEAN NTB_MSABI ntb_RtlEqualUnicodeString(PUNICODE_STRING a, PUNICODE_STRING b,
                                  BOOLEAN caseins);

/*
 * اختبار ذاتي آمن.
 * يعيد: 1 = نجح، 0 = فشل، -1 = لا يوجد اختبار لهذا المعرّف.
 */
int NTB_MSABI ntb_table_run_test(enum ntb_tid id, char *note, size_t notelen);

/* M5: thread cleanup on unload */
void ntb_threads_exit(void);

/* M6: user-mode (minimal stubs) */
void NTB_MSABI ntb_ExitProcess(ULONG uExitCode);
PVOID NTB_MSABI ntb_GetStdHandle(ULONG nStdHandle);
BOOLEAN NTB_MSABI ntb_WriteFile(PVOID hFile, PVOID lpBuffer, ULONG nNumberOfBytesToWrite, u32 *lpNumberOfBytesWritten, PVOID lpOverlapped);
PVOID NTB_MSABI ntb_GetCommandLineW(void);
PVOID NTB_MSABI ntb_GetModuleHandleW(PVOID lpModuleName);

/* M6b: high-frequency Tier-2/3 stubs */
void NTB_MSABI ntb_KeInitializeTimer(PVOID Timer, int Type);
BOOLEAN NTB_MSABI ntb_KeSetTimer(PVOID Timer, LARGE_INTEGER DueTime, PVOID Dpc);
BOOLEAN NTB_MSABI ntb_KeCancelTimer(PVOID Timer);
BOOLEAN NTB_MSABI ntb_KeSetTimerEx(PVOID Timer, LARGE_INTEGER DueTime, LONG Period, PVOID Dpc);
void NTB_MSABI ntb_ObfDereferenceObject(PVOID Object);
NTSTATUS NTB_MSABI ntb_ObReferenceObjectByHandle(PVOID Handle, ULONG DesiredAccess, PVOID ObjectType, int AccessMode, PVOID *Object, PVOID *HandleInformation);
void NTB_MSABI ntb_ObfReferenceObject(PVOID Object);
NTSTATUS NTB_MSABI ntb_ObOpenObjectByPointer(PVOID Object, ULONG HandleAttributes, PVOID PassedAccessState, ULONG DesiredAccess, PVOID ObjectType, int AccessMode, PVOID *Handle);
void NTB_MSABI ntb_RtlCopyUnicodeString(PUNICODE_STRING DestinationString, PUNICODE_STRING SourceString);
PVOID NTB_MSABI ntb_IoBuildDeviceIoControlRequest(ULONG IoControlCode, PVOID DeviceObject, PVOID InputBuffer, ULONG InputBufferLength, PVOID OutputBuffer, ULONG OutputBufferLength, BOOLEAN InternalDeviceIoControl, PVOID Event, PVOID IoStatusBlock);
NTSTATUS NTB_MSABI ntb_IoGetDeviceObjectPointer(PUNICODE_STRING ObjectName, ULONG DesiredAccess, PVOID *FileObject, PVOID *DeviceObject);
PVOID NTB_MSABI ntb_MmMapLockedPagesSpecifyCache(PVOID Mdl, int AccessMode, int CacheType, PVOID BaseAddress, ULONG Protect, ULONG Priority);
void NTB_MSABI ntb_MmBuildMdlForNonPagedPool(PVOID Mdl);
void NTB_MSABI ntb_KeEnterCriticalRegion(void);
void NTB_MSABI ntb_KeLeaveCriticalRegion(void);
NTSTATUS NTB_MSABI ntb_PoCallDriver(PVOID DeviceObject, PIRP Irp);
void NTB_MSABI ntb_IoReleaseCancelSpinLock(KIRQL Irql);

/* M8: generic deferred stub */
void *NTB_MSABI ntb_generic_stub(void);

/* ------------------------------------------------------------------ */
/* M8: GPU / كرت الشاشة — ترجمة أوامر كرت الشاشة من ويندوز إلى نواة لينكس */
/* التعريفات تُثبت داخل الصندوق كما في ويندوز، pero الأوامر تُرسَل عبر */
/* linux_bridge إلى تعريفات لينكس (amdgpu / nvidia / i915 — مفتوح أو مغلق المصدر). */
/* ------------------------------------------------------------------ */

int NTB_MSABI ntb_gpu_open_render(const char *devnode);
void NTB_MSABI ntb_gpu_close_render(int fd);
int NTB_MSABI ntb_gpu_create_gem(int fd, size_t size, u32 *handle);
int NTB_MSABI ntb_gpu_mmap_gem(int fd, u32 handle, size_t size, void **addr);
int NTB_MSABI ntb_gpu_get_dmabuf(int fd, u32 handle, int *dmabuf_fd);
int NTB_MSABI ntb_gpu_submit_cmd(int fd, const void *cmd, size_t cmd_size);
int NTB_MSABI ntb_gpu_present(int fd, u32 handle, u32 width, u32 height);
int NTB_MSABI ntb_gpu_query_adapter(int fd, char *info, size_t infolen);

#endif
