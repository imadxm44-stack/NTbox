/*
 * DublKernel M3 — import resolver (Windows 7 base)
 * Maps a (DLL, function name) from a PE's import table to our translated ntb_*.
 * Unknown imports return NULL — caller logs and treats as "not yet translated".
 */

#include "ntbox.h"
#include "ntbox_types.h"
#include "ntb_trace.h"
#include "ntb_table.h"
#include "pe_loader.h"
#include "linux_bridge.h"

#include <linux/string.h>

struct import_map {
    const char *name;
    void *fn;
};

/* ntoskrnl.exe exports we currently translate (Tier-1 + IRP Tier-2 slice) */
static const struct import_map ntoskrnl_map[] = {
    /* MEM */
    { "ExAllocatePoolWithTag", (void *)ntb_ExAllocatePoolWithTag },
    { "ExAllocatePool2", (void *)ntb_ExAllocatePool2 },
    { "ExAllocatePoolWithTagPriority", (void *)ntb_ExAllocatePoolWithTagPriority },
    { "ExFreePoolWithTag", (void *)ntb_ExFreePoolWithTag },
    { "MmAllocateContiguousMemory", (void *)ntb_MmAllocateContiguousMemory },
    { "MmFreeContiguousMemory", (void *)ntb_MmFreeContiguousMemory },
    { "MmMapIoSpace", (void *)ntb_MmMapIoSpace },
    { "MmUnmapIoSpace", (void *)ntb_MmUnmapIoSpace },
    { "MmGetPhysicalAddress", (void *)ntb_MmGetPhysicalAddress },
    { "MmAllocateNonCachedMemory", (void *)ntb_MmAllocateNonCachedMemory },
    { "MmFreeNonCachedMemory", (void *)ntb_MmFreeNonCachedMemory },
    { "MmPageEntireDriver", (void *)ntb_MmPageEntireDriver },
    /* CPU */
    { "KeGetCurrentIrql", (void *)ntb_KeGetCurrentIrql },
    { "KeRaiseIrql", (void *)ntb_KeRaiseIrql },
    { "KeLowerIrql", (void *)ntb_KeLowerIrql },
    { "KfRaiseIrql", (void *)ntb_KfRaiseIrql },
    { "KfLowerIrql", (void *)ntb_KfLowerIrql },
    { "KeStallExecutionProcessor", (void *)ntb_KeStallExecutionProcessor },
    { "KeGetCurrentProcessorNumber", (void *)ntb_KeGetCurrentProcessorNumber },
    { "KeQueryActiveProcessorCount", (void *)ntb_KeQueryActiveProcessorCount },
    { "KeBugCheck", (void *)ntb_KeBugCheck },
    { "KeBugCheckEx", (void *)ntb_KeBugCheckEx },
    { "DbgPrint", (void *)ntb_DbgPrint },
    { "DbgBreakPoint", (void *)ntb_DbgBreakPoint },
    /* SYNC */
    { "KeInitializeSpinLock", (void *)ntb_KeInitializeSpinLock },
    { "KeAcquireSpinLock", (void *)ntb_KeAcquireSpinLock },
    { "KeAcquireSpinLockRaiseToDpc", (void *)ntb_KeAcquireSpinLock },
    { "KeReleaseSpinLock", (void *)ntb_KeReleaseSpinLock },
    { "KeAcquireSpinLockAtDpcLevel", (void *)ntb_KeAcquireSpinLockAtDpcLevel },
    { "KeReleaseSpinLockFromDpcLevel", (void *)ntb_KeReleaseSpinLockFromDpcLevel },
    { "KeInitializeEvent", (void *)ntb_KeInitializeEvent },
    { "KeSetEvent", (void *)ntb_KeSetEvent },
    { "KeResetEvent", (void *)ntb_KeResetEvent },
    { "KeReadStateEvent", (void *)ntb_KeReadStateEvent },
    { "KeWaitForSingleObject", (void *)ntb_KeWaitForSingleObject },
    { "KeDelayExecutionThread", (void *)ntb_KeDelayExecutionThread },
    { "KeInitializeDpc", (void *)ntb_KeInitializeDpc },
    { "KeInsertQueueDpc", (void *)ntb_KeInsertQueueDpc },
    { "KeInitializeMutex", (void *)ntb_KeInitializeMutex },
    { "KeAcquireMutex", (void *)ntb_KeAcquireMutex },
    { "KeReleaseMutex", (void *)ntb_KeReleaseMutex },
    /* RTL */
    { "RtlInitUnicodeString", (void *)ntb_RtlInitUnicodeString },
    { "RtlCopyMemory", (void *)ntb_RtlCopyMemory },
    { "RtlZeroMemory", (void *)ntb_RtlZeroMemory },
    { "RtlMoveMemory", (void *)ntb_RtlMoveMemory },
    { "RtlFillMemory", (void *)ntb_RtlFillMemory },
    { "RtlAnsiStringToUnicodeString", (void *)ntb_RtlAnsiStringToUnicodeString },
    { "RtlFreeUnicodeString", (void *)ntb_RtlFreeUnicodeString },
    { "RtlCompareMemory", (void *)ntb_RtlCompareMemory },
    { "RtlAppendUnicodeToString", (void *)ntb_RtlAppendUnicodeToString },
    { "RtlEqualUnicodeString", (void *)ntb_RtlEqualUnicodeString },
    /* IRP */
    { "IoCreateDevice", (void *)ntb_IoCreateDevice },
    { "IoDeleteDevice", (void *)ntb_IoDeleteDevice },
    { "IoCreateSymbolicLink", (void *)ntb_IoCreateSymbolicLink },
    { "IoDeleteSymbolicLink", (void *)ntb_IoDeleteSymbolicLink },
    { "IoStartNextPacket", (void *)ntb_IoStartNextPacket },
    { "IofCompleteRequest", (void *)ntb_IofCompleteRequest },
    { "IoCompleteRequest", (void *)ntb_IofCompleteRequest }, /* alias */
    /* M6b high-frequency */
    { "KeInitializeTimer", (void *)ntb_KeInitializeTimer },
    { "KeSetTimer", (void *)ntb_KeSetTimer },
    { "KeCancelTimer", (void *)ntb_KeCancelTimer },
    { "KeSetTimerEx", (void *)ntb_KeSetTimerEx },
    { "ObfDereferenceObject", (void *)ntb_ObfDereferenceObject },
    { "ObReferenceObjectByHandle", (void *)ntb_ObReferenceObjectByHandle },
    { "ObfReferenceObject", (void *)ntb_ObfReferenceObject },
    { "ObOpenObjectByPointer", (void *)ntb_ObOpenObjectByPointer },
    { "RtlCopyUnicodeString", (void *)ntb_RtlCopyUnicodeString },
    { "IoBuildDeviceIoControlRequest", (void *)ntb_IoBuildDeviceIoControlRequest },
    { "IoGetDeviceObjectPointer", (void *)ntb_IoGetDeviceObjectPointer },
    { "MmMapLockedPagesSpecifyCache", (void *)ntb_MmMapLockedPagesSpecifyCache },
    { "MmBuildMdlForNonPagedPool", (void *)ntb_MmBuildMdlForNonPagedPool },
    { "KeEnterCriticalRegion", (void *)ntb_KeEnterCriticalRegion },
    { "KeLeaveCriticalRegion", (void *)ntb_KeLeaveCriticalRegion },
    { "PoCallDriver", (void *)ntb_PoCallDriver },
    { "IoReleaseCancelSpinLock", (void *)ntb_IoReleaseCancelSpinLock },
    /* M4 Tier-2/3 */
    { "PsCreateSystemThread", (void *)ntb_PsCreateSystemThread },
    { "PsTerminateSystemThread", (void *)ntb_PsTerminateSystemThread },
    { "PsGetCurrentThreadId", (void *)ntb_PsGetCurrentThreadId },
    { "IoAllocateIrp", (void *)ntb_IoAllocateIrp },
    { "IoFreeIrp", (void *)ntb_IoFreeIrp },
    { "IofCallDriver", (void *)ntb_IofCallDriver },
    { "IoCallDriver", (void *)ntb_IofCallDriver },
    { "IoAttachDeviceToDeviceStack", (void *)ntb_IoAttachDeviceToDeviceStack },
    { "IoDetachDevice", (void *)ntb_IoDetachDevice },
    { "IoAllocateMdl", (void *)ntb_IoAllocateMdl },
    { "IoFreeMdl", (void *)ntb_IoFreeMdl },
    { "IoAllocateWorkItem", (void *)ntb_IoAllocateWorkItem },
    { "IoFreeWorkItem", (void *)ntb_IoFreeWorkItem },
    { "IoQueueWorkItem", (void *)ntb_IoQueueWorkItem },
    { "ZwCreateFile", (void *)ntb_ZwCreateFile },
    { "ZwReadFile", (void *)ntb_ZwReadFile },
    { "ZwWriteFile", (void *)ntb_ZwWriteFile },
    { "ZwClose", (void *)ntb_ZwClose },
    { "ZwOpenKey", (void *)ntb_ZwOpenKey },
    { "ZwQueryValueKey", (void *)ntb_ZwQueryValueKey },
    { "ZwSetValueKey", (void *)ntb_ZwSetValueKey },
    { "ZwCreateKey", (void *)ntb_ZwCreateKey },
    { "ZwEnumerateKey", (void *)ntb_ZwEnumerateKey },
    { "ZwQuerySystemInformation", (void *)ntb_ZwQuerySystemInformation },
    { "READ_PORT_UCHAR", (void *)ntb_READ_PORT_UCHAR },
    { "READ_PORT_USHORT", (void *)ntb_READ_PORT_USHORT },
    { "READ_PORT_ULONG", (void *)ntb_READ_PORT_ULONG },
    { "WRITE_PORT_UCHAR", (void *)ntb_WRITE_PORT_UCHAR },
    { "WRITE_PORT_USHORT", (void *)ntb_WRITE_PORT_USHORT },
    { "WRITE_PORT_ULONG", (void *)ntb_WRITE_PORT_ULONG },
    { "HalGetBusData", (void *)ntb_HalGetBusData },
    { "HalSetBusData", (void *)ntb_HalSetBusData },
    { "IoRegisterDeviceInterface", (void *)ntb_IoRegisterDeviceInterface },
    { "IoSetDeviceInterfaceState", (void *)ntb_IoSetDeviceInterfaceState },
    { "IoRegisterPlugPlayNotification", (void *)ntb_IoRegisterPlugPlayNotification },
    { "IoOpenDeviceRegistryKey", (void *)ntb_IoOpenDeviceRegistryKey },
    { NULL, NULL }
};

static const struct import_map user_map[] = {
    { "ExitProcess", (void *)ntb_ExitProcess },
    { "GetStdHandle", (void *)ntb_GetStdHandle },
    { "WriteFile", (void *)ntb_WriteFile },
    { "GetCommandLineW", (void *)ntb_GetCommandLineW },
    { "GetModuleHandleW", (void *)ntb_GetModuleHandleW },
    { NULL, NULL }
};

void *ntbox_resolve_import(const char *dll, const char *name)
{
    const struct import_map *m;

    if (!dll || !name)
        return NULL;
    if (strcasecmp(dll, "ntoskrnl.exe") == 0) {
        for (m = ntoskrnl_map; m->name; m++) {
            if (strcmp(m->name, name) == 0)
                return m->fn;
        }
        /* M8: allow deferred loading via generic stub (stats-driven later) */
        return (void *)ntb_generic_stub;
    }
    if (strcasecmp(dll, "hal.dll") == 0) {
        if (strcmp(name, "HalGetBusData") == 0) return (void *)ntb_HalGetBusData;
        if (strcmp(name, "HalSetBusData") == 0) return (void *)ntb_HalSetBusData;
        if (strcmp(name, "HalGetBusDataByOffset") == 0) return (void *)ntb_HalGetBusDataByOffset;
        if (strcmp(name, "HalSetBusDataByOffset") == 0) return (void *)ntb_HalSetBusDataByOffset;
        if (strcmp(name, "KeStallExecutionProcessor") == 0) return (void *)ntb_KeStallExecutionProcessor;
        if (strcmp(name, "KeQueryPerformanceCounter") == 0) return (void *)ntb_KeQueryPerformanceCounter;
        return (void *)ntb_generic_stub;
    }
    if (strcasecmp(dll, "ntdll.dll") == 0 ||
        strcasecmp(dll, "kernel32.dll") == 0) {
        for (m = user_map; m->name; m++) {
            if (strcmp(m->name, name) == 0)
                return m->fn;
        }
        return NULL;
    }
    return NULL;
}

int ntbox_resolve_supported_count(void)
{
    int n = 0;
    const struct import_map *m;
    for (m = ntoskrnl_map; m->name; m++)
        n++;
    return n;
}
