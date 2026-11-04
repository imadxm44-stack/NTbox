/*
 * peb.h — طبقة PEB/TEB/RTL_USER_PROCESS_PARAMETERS لـ NT-Box (W6).
 *
 * تخطيطات NT الحقيقية (Windows x64 / Win7) — إزاحات مُؤكَّدة
 * بـ _Static_assert. كل الدوال هنا "مستوى مستخدم" (user-mode).
 *
 * أمانة:
 *   - SEH: نسجل مرشّح استثناء غير معالَج (SetUnhandledExceptionFilter)
 *     فقط. لا "unwinding" حقيقي (setjmp/longjmp أو libunwind) — موثَّق.
 *   - VirtualAlloc/Protect/Free: مدعومة بـ mmap/mprotect/munmap.
 *   - Heap: مدعوم بـ malloc مع تتبّع الحجم في ترويسة.
 */
#ifndef NTB0_PEB_H
#define NTB0_PEB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* typedefs على طراز Win64                                            */
/* ------------------------------------------------------------------ */
typedef void      *HANDLE;
typedef uint32_t   DWORD;
typedef unsigned char BYTE;
typedef int        BOOL;
typedef int32_t    LONG;
typedef uint32_t   ULONG;
typedef uint16_t   USHORT;
typedef uint32_t   UINT;
typedef size_t     SIZE_T;

/* ------------------------------------------------------------------ */
/* UNICODE_STRING (Win64: 0x10)                                       */
/* ------------------------------------------------------------------ */
typedef struct _UNICODE_STRING {
    USHORT  Length;          /* 0x00 — طول بالبايت (بدون NUL) */
    USHORT  MaximumLength;   /* 0x02 */
    USHORT  _pad[2];         /* 0x04 */
    void   *Buffer;          /* 0x08 — UTF-16LE */
} UNICODE_STRING;

_Static_assert(sizeof(UNICODE_STRING) == 16, "UNICODE_STRING size");
_Static_assert(offsetof(UNICODE_STRING, Buffer) == 8, "UNICODE_STRING.Buffer");

/* ------------------------------------------------------------------ */
/* CLIENT_ID (Win64: 0x10)                                            */
/* ------------------------------------------------------------------ */
typedef struct _CLIENT_ID {
    HANDLE UniqueProcess;    /* 0x00 */
    HANDLE UniqueThread;     /* 0x08 */
} CLIENT_ID;

_Static_assert(sizeof(CLIENT_ID) == 16, "CLIENT_ID size");
_Static_assert(offsetof(CLIENT_ID, UniqueThread) == 8, "CLIENT_ID.UniqueThread");

/* ------------------------------------------------------------------ */
/* NT_TIB (Win64: 0x38)                                              */
/* ------------------------------------------------------------------ */
typedef struct _NT_TIB {
    void               *ExceptionList;        /* 0x00 */
    void               *StackBase;            /* 0x08 */
    void               *StackLimit;           /* 0x10 */
    void               *SubSystemTib;         /* 0x18 */
    union {
        void           *FiberData;            /* 0x20 */
        ULONG           Version;
    };
    void               *ArbitraryUserPointer; /* 0x28 */
    struct _NT_TIB     *Self;                 /* 0x30 */
} NT_TIB;

_Static_assert(sizeof(NT_TIB) == 0x38, "NT_TIB size");
_Static_assert(offsetof(NT_TIB, StackBase) == 0x08, "NT_TIB.StackBase");
_Static_assert(offsetof(NT_TIB, StackLimit) == 0x10, "NT_TIB.StackLimit");
_Static_assert(offsetof(NT_TIB, Self) == 0x30, "NT_TIB.Self");

/* ------------------------------------------------------------------ */
/* LIST_ENTRY / PEB_LDR_DATA (minimal, for PEB.Ldr)                  */
/* ------------------------------------------------------------------ */
typedef struct _LIST_ENTRY {
    struct _LIST_ENTRY *Flink;
    struct _LIST_ENTRY *Blink;
} LIST_ENTRY;

typedef struct _PEB_LDR_DATA {
    ULONG      Length;
    unsigned char Initialized;
    void      *SsHandle;
    LIST_ENTRY InLoadOrderModuleList;
    LIST_ENTRY InMemoryOrderModuleList;
    LIST_ENTRY InInitializationOrderModuleList;
} PEB_LDR_DATA;

/* ------------------------------------------------------------------ */
/* forward declarations                                               */
/* ------------------------------------------------------------------ */
struct _PEB;
struct _RTL_USER_PROCESS_PARAMETERS;

/* ------------------------------------------------------------------ */
/* CURDIR inside RTL_USER_PROCESS_PARAMETERS                          */
/* ------------------------------------------------------------------ */
typedef struct _CURDIR {
    UNICODE_STRING DosPath;  /* 0x00 within CURDIR */
    HANDLE         Handle;   /* 0x10 */
} CURDIR;

/* ------------------------------------------------------------------ */
/* RTL_USER_PROCESS_PARAMETERS (Win64)                                */
/* ------------------------------------------------------------------ */
typedef struct _RTL_USER_PROCESS_PARAMETERS {
    ULONG   MaximumLength;          /* 0x00 */
    ULONG   Length;                 /* 0x04 */
    ULONG   Flags;                  /* 0x08 */
    ULONG   DebugFlags;             /* 0x0C */
    HANDLE  ConsoleHandle;          /* 0x10 */
    ULONG   ConsoleFlags;           /* 0x18 */
    HANDLE  StandardInput;          /* 0x20 */
    HANDLE  StandardOutput;         /* 0x28 */
    HANDLE  StandardError;          /* 0x30 */
    CURDIR  CurrentDirectory;       /* 0x38 */
    UNICODE_STRING DllPath;         /* 0x50 */
    UNICODE_STRING ImagePathName;   /* 0x60 */
    UNICODE_STRING CommandLine;     /* 0x70 */
    void   *Environment;            /* 0x80 — كتلة البيئة (UTF-16) */
    ULONG   StartingX;              /* 0x88 */
    ULONG   StartingY;              /* 0x8C */
    ULONG   CountX;                 /* 0x90 */
    ULONG   CountY;                 /* 0x94 */
    ULONG   CountCharsX;            /* 0x98 */
    ULONG   CountCharsY;            /* 0x9C */
    ULONG   FillAttribute;          /* 0xA0 */
    ULONG   WindowFlags;            /* 0xA4 */
    ULONG   ShowWindowFlags;        /* 0xA8 */
    UNICODE_STRING WindowTitle;     /* 0xB0 */
    UNICODE_STRING DesktopInfo;     /* 0xC0 */
    UNICODE_STRING ShellInfo;       /* 0xD0 */
    UNICODE_STRING RuntimeData;     /* 0xE0 */
} RTL_USER_PROCESS_PARAMETERS;

_Static_assert(offsetof(RTL_USER_PROCESS_PARAMETERS, MaximumLength) == 0x00, "RUPP.MaximumLength");
_Static_assert(offsetof(RTL_USER_PROCESS_PARAMETERS, CommandLine) == 0x70, "RUPP.CommandLine");
_Static_assert(offsetof(RTL_USER_PROCESS_PARAMETERS, Environment) == 0x80, "RUPP.Environment");

/* ------------------------------------------------------------------ */
/* TEB (Win64) — offsets مُؤكَّدة                                     */
/* ------------------------------------------------------------------ */
typedef struct _TEB {
    NT_TIB NtTib;                          /* 0x000 */
    void  *EnvironmentPointer;             /* 0x038 */
    CLIENT_ID ClientId;                    /* 0x040 */
    void  *RpcHandle;                      /* 0x050 */
    void  *ThreadLocalStoragePointer;      /* 0x058 */
    struct _PEB *ProcessEnvironmentBlock;  /* 0x060 */
    ULONG  LastErrorValue;                 /* 0x068 */
    ULONG  CountOfOwnedCriticalSections;   /* 0x06C */
    /* بقية الحقول (اختيارية) — لا نحتاجها */
} TEB;

_Static_assert(offsetof(TEB, NtTib) == 0x000, "TEB.NtTib");
_Static_assert(offsetof(TEB, EnvironmentPointer) == 0x038, "TEB.EnvironmentPointer");
_Static_assert(offsetof(TEB, ClientId) == 0x040, "TEB.ClientId");
_Static_assert(offsetof(TEB, RpcHandle) == 0x050, "TEB.RpcHandle");
_Static_assert(offsetof(TEB, ThreadLocalStoragePointer) == 0x058, "TEB.ThreadLocalStoragePointer");
_Static_assert(offsetof(TEB, ProcessEnvironmentBlock) == 0x060, "TEB.ProcessEnvironmentBlock");
_Static_assert(offsetof(TEB, LastErrorValue) == 0x068, "TEB.LastErrorValue");

/* ------------------------------------------------------------------ */
/* PEB (Win64) — offsets مُؤكَّدة                                     */
/* ------------------------------------------------------------------ */
typedef struct _PEB {
    BYTE   InheritedAddressSpace;          /* 0x00 */
    BYTE   ReadImageFileExecOptions;        /* 0x01 */
    BYTE   BeingDebugged;                  /* 0x02 */
    union {
        BYTE BitField;                     /* 0x03 */
        struct {
            BYTE ImageUsesLargePages : 1;
            BYTE IsProtectedProcess : 1;
            BYTE IsImageDynamicallyRelocated : 1;
            BYTE SkipPatchingUser32Forwarders : 1;
            BYTE IsPackagedProcess : 1;
            BYTE IsAppContainer : 1;
            BYTE IsProtectedProcessLight : 1;
            BYTE SpareBits : 1;
        };
    };
    HANDLE Mutant;                         /* 0x08 */
    void  *ImageBaseAddress;               /* 0x10 */
    void  *Ldr;                            /* 0x18 */
    RTL_USER_PROCESS_PARAMETERS *ProcessParameters; /* 0x20 */
    void  *SubSystemData;                  /* 0x28 */
    void  *ProcessHeap;                    /* 0x30 */
    void  *FastPebLock;                    /* 0x38 */
    void  *Environment;                    /* extension: mirrors ProcessParameters->Environment */
} PEB;

_Static_assert(offsetof(PEB, InheritedAddressSpace) == 0x00, "PEB.InheritedAddressSpace");
_Static_assert(offsetof(PEB, BeingDebugged) == 0x02, "PEB.BeingDebugged");
_Static_assert(offsetof(PEB, Mutant) == 0x08, "PEB.Mutant");
_Static_assert(offsetof(PEB, ImageBaseAddress) == 0x10, "PEB.ImageBaseAddress");
_Static_assert(offsetof(PEB, Ldr) == 0x18, "PEB.Ldr");
_Static_assert(offsetof(PEB, ProcessParameters) == 0x20, "PEB.ProcessParameters");
_Static_assert(offsetof(PEB, SubSystemData) == 0x28, "PEB.SubSystemData");
_Static_assert(offsetof(PEB, ProcessHeap) == 0x30, "PEB.ProcessHeap");
_Static_assert(offsetof(PEB, FastPebLock) == 0x38, "PEB.FastPebLock");

/* ------------------------------------------------------------------ */
/* CRITICAL_SECTION / SRWLOCK (Win64)                                 */
/* ------------------------------------------------------------------ */
typedef struct _RTL_CRITICAL_SECTION {
    void   *DebugInfo;       /* 0x00 */
    LONG    LockCount;       /* 0x08 */
    LONG    RecursionCount;  /* 0x0C */
    HANDLE  OwningThread;    /* 0x10 */
    HANDLE  LockSemaphore;   /* 0x18 */
    SIZE_T  SpinCount;       /* 0x20 */
} RTL_CRITICAL_SECTION;

_Static_assert(sizeof(RTL_CRITICAL_SECTION) == 0x28, "RTL_CRITICAL_SECTION size");

typedef RTL_CRITICAL_SECTION CRITICAL_SECTION;

typedef struct _RTL_SRWLOCK {
    void *Ptr;               /* 0x00 */
} RTL_SRWLOCK;

typedef RTL_SRWLOCK SRWLOCK;

/* ------------------------------------------------------------------ */
/* ثوابت                                                              */
/* ------------------------------------------------------------------ */
#define HEAP_ZERO_MEMORY       0x00000008u
#define MEM_COMMIT             0x00001000u
#define MEM_RESERVE            0x00002000u
#define MEM_RELEASE            0x00008000u
#define PAGE_NOACCESS          0x01u
#define PAGE_READONLY          0x02u
#define PAGE_READWRITE         0x04u
#define PAGE_EXECUTE           0x10u
#define PAGE_EXECUTE_READ      0x20u
#define PAGE_EXECUTE_READWRITE 0x40u
#define PAGE_WRITECOPY         0x08u
#define PAGE_EXECUTE_WRITECOPY 0x80u

#define NTB0_TIC_TEB_POINTER   5u

/* ------------------------------------------------------------------ */
/* PEB/TEB                                                            */
/* ------------------------------------------------------------------ */
PEB  *create_peb(const char *image_path, int argc, char **argv, char **envp);
TEB  *create_teb(PEB *peb, void *stack_base);
void  peb_set_current_teb(TEB *teb);
TEB  *peb_get_current_teb(void);
void  peb_destroy(PEB *peb);

/* ------------------------------------------------------------------ */
/* TLS                                                                */
/* ------------------------------------------------------------------ */
unsigned long TlsAlloc(void);
void         *TlsGetValue(unsigned long idx);
int           TlsSetValue(unsigned long idx, void *value);
int           TlsFree(unsigned long idx);

/* ------------------------------------------------------------------ */
/* Critical sections                                                  */
/* ------------------------------------------------------------------ */
void  InitializeCriticalSection(CRITICAL_SECTION *cs);
void  EnterCriticalSection(CRITICAL_SECTION *cs);
void  LeaveCriticalSection(CRITICAL_SECTION *cs);
int   TryEnterCriticalSection(CRITICAL_SECTION *cs);
void  DeleteCriticalSection(CRITICAL_SECTION *cs);

/* ------------------------------------------------------------------ */
/* SRW locks                                                          */
/* ------------------------------------------------------------------ */
void  InitializeSRWLock(SRWLOCK *lock);
void  AcquireSRWLockExclusive(SRWLOCK *lock);
void  ReleaseSRWLockExclusive(SRWLOCK *lock);
void  AcquireSRWLockShared(SRWLOCK *lock);
void  ReleaseSRWLockShared(SRWLOCK *lock);

/* ------------------------------------------------------------------ */
/* Heap                                                               */
/* ------------------------------------------------------------------ */
HANDLE GetProcessHeap(void);
HANDLE HeapCreate(ULONG flags, SIZE_T initial_size, SIZE_T maximum_size);
int    HeapDestroy(HANDLE heap);
void  *RtlAllocateHeap(HANDLE heap, ULONG flags, SIZE_T size);
int    RtlFreeHeap(HANDLE heap, ULONG flags, void *ptr);
SIZE_T RtlSizeHeap(HANDLE heap, ULONG flags, void *ptr);

/* ------------------------------------------------------------------ */
/* Virtual memory                                                     */
/* ------------------------------------------------------------------ */
void  *VirtualAlloc(void *address, SIZE_T size, DWORD type, DWORD protect);
int    VirtualProtect(void *address, SIZE_T size, DWORD new_protect, DWORD *old_protect);
int    VirtualFree(void *address, SIZE_T size, DWORD type);

/* ------------------------------------------------------------------ */
/* Exception / debugger / last error                                  */
/* ------------------------------------------------------------------ */
void  *SetUnhandledExceptionFilter(void *filter);
void  *GetUnhandledExceptionFilter(void);
int    IsDebuggerPresent(void);
void   SetLastError(ULONG err);
ULONG  GetLastError(void);

/* ------------------------------------------------------------------ */
/* NtSetInformationThread (subset: TEB pointer)                       */
/* ------------------------------------------------------------------ */
long   NtSetInformationThread(void *thread_handle, ULONG info_class,
                              void *info, ULONG length);

/* ------------------------------------------------------------------ */
/* Thread helpers (documented subset)                                 */
/* ------------------------------------------------------------------ */
void  *CreateThreadWithTeb(void *start_routine, void *param, TEB **teb_out);
void   ExitThread(ULONG code);
void   ExitProcess(ULONG code);
unsigned long WaitForSingleObject(void *handle, unsigned long timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* NTB0_PEB_H */
