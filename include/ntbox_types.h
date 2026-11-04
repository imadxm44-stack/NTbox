#ifndef NTBOX_TYPES_H
#define NTBOX_TYPES_H

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/completion.h>

/* Minimal NT types for Demo only. No Windows binaries included. */

typedef s32 NTSTATUS;
#define STATUS_SUCCESS ((NTSTATUS)0x00000000)
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001)
#define STATUS_NOT_IMPLEMENTED ((NTSTATUS)0xC0000002)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000D)
#define STATUS_NO_MEMORY ((NTSTATUS)0xC0000017)
#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xC0000023)
#define STATUS_TIMEOUT ((NTSTATUS)0x00000102)

#define IRP_MJ_CREATE 0x00
#define IRP_MJ_CLOSE  0x02
#define IRP_MJ_MAX    0x1B
#define IO_NO_INCREMENT 0

struct _DRIVER_OBJECT;
struct _DEVICE_OBJECT;
struct _IRP;
struct _UNICODE_STRING;

#define NTB_MSABI __attribute__((ms_abi))
typedef NTSTATUS (*PDRIVER_DISPATCH)(struct _DEVICE_OBJECT *DeviceObject, struct _IRP *Irp);
typedef NTSTATUS (*PDRIVER_ENTRY)(struct _DRIVER_OBJECT *DriverObject, struct _UNICODE_STRING *RegistryPath);

typedef struct _IO_STATUS_BLOCK {
    NTSTATUS Status;
    unsigned long Information;
} IO_STATUS_BLOCK;

typedef struct _DEVICE_OBJECT {
    int Type;
    void *DriverObject;
} DEVICE_OBJECT, *PDEVICE_OBJECT;

typedef struct _DRIVER_OBJECT {
    u16 Type;
    u16 Size;
    u8 _pad0[4];
    PDEVICE_OBJECT DeviceObject;   /* +0x08 */
    u32 Flags;                     /* +0x10 */
    u8 _pad1[4];
    void *DriverStart;             /* +0x18 */
    u32 DriverSize;                /* +0x20 */
    u8 _pad2[4];
    void *DriverSection;           /* +0x28 */
    void *DriverExtension;         /* +0x30 */
    u8 DriverName[16];             /* +0x38 (UNICODE_STRING) */
    void *HardwareDatabase;        /* +0x48 */
    void *FastIoDispatch;          /* +0x50 */
    void *DriverInit;              /* +0x58 */
    void *DriverStartIo;           /* +0x60 */
    void *DriverUnload;            /* +0x68 */
    PDRIVER_DISPATCH MajorFunction[IRP_MJ_MAX + 1]; /* +0x70 */
} DRIVER_OBJECT, *PDRIVER_OBJECT;

typedef struct _IRP {
    u8 _pad0[0x30];
    IO_STATUS_BLOCK IoStatus;   /* +0x30 */
    u8 _pad1[0x78];            /* to +0xb8 */
    void *CurrentStackLocation; /* +0xb8 */
    u8 _pad2[0x40];
} IRP, *PIRP;

typedef struct _UNICODE_STRING {
    u16 Length;
    u16 MaximumLength;
    u16 *Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

typedef struct _ANSI_STRING {
    u16 Length;
    u16 MaximumLength;
    char *Buffer;
} ANSI_STRING, *PANSI_STRING;

/* ---- Tier-1 types (PLAN_Translator_Diagnostics.md §7) ---- */

typedef void *PVOID;
typedef s32 LONG;
typedef u32 ULONG;
typedef unsigned long ULONG_PTR;
typedef u8 BOOLEAN;
typedef s64 LARGE_INTEGER;
typedef LARGE_INTEGER *PLARGE_INTEGER;

typedef u8 KIRQL;
typedef KIRQL *PKIRQL;
#define PASSIVE_LEVEL  0
#define APC_LEVEL      1
#define DISPATCH_LEVEL 2

typedef int POOL_TYPE;
#define NTB_NonPagedPool 0
#define NTB_PagedPool    1

typedef int KPROCESSOR_MODE;
#define KernelMode 0
#define UserMode   1

typedef int KWAIT_REASON;
#define Executive 0

typedef int KEVENT_TYPE;
#define NotificationEvent   0
#define SynchronizationEvent 1

typedef struct _NTB_KSPIN_LOCK {
    raw_spinlock_t raw;
} NTB_KSPIN_LOCK, *PNTB_KSPIN_LOCK;

typedef struct _KEVENT {
    atomic_t signaled;
    wait_queue_head_t wq;
    int type;
} KEVENT, *PKEVENT;

struct _KDPC;
typedef void (*PKDEFERRED_ROUTINE)(struct _KDPC *Dpc, PVOID ctx,
                                   PVOID arg1, PVOID arg2);
typedef struct _KDPC {
    struct work_struct work;
    PKDEFERRED_ROUTINE func;
    PVOID ctx;
    PVOID arg1;
    PVOID arg2;
} KDPC, *PKDPC;

typedef struct _KMUTEX {
    struct mutex m;
} KMUTEX, *PKMUTEX;

typedef struct _NTB_LOOKASIDE {
    size_t size;
} NTB_LOOKASIDE, *PNTB_LOOKASIDE;

#endif
