/*
 * syswow64.h — SysWOW64 / PE32 loader & translation layer for NT-Box.
 *
 * Handles PE32 (32-bit) headers, 32-bit import directory parsing,
 * 32-bit base relocations (IMAGE_REL_BASED_HIGHLOW), 32-bit TEB/PEB32
 * layout structures, and 32-bit argument/stub translation.
 */
#ifndef NTB0_SYSWOW64_H
#define NTB0_SYSWOW64_H

#include <stddef.h>
#include <stdint.h>
#include "dll_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* PEB32 / TEB32 Layout Structures (Windows x86 / SysWOW64)           */
/* ------------------------------------------------------------------ */
typedef struct _NT_TIB32 {
    uint32_t ExceptionList;        /* 0x00 */
    uint32_t StackBase;            /* 0x04 */
    uint32_t StackLimit;           /* 0x08 */
    uint32_t SubSystemTib;         /* 0x0C */
    uint32_t FiberData;            /* 0x10 */
    uint32_t ArbitraryUserPointer; /* 0x14 */
    uint32_t Self;                 /* 0x18 */
} NT_TIB32;

_Static_assert(sizeof(NT_TIB32) == 0x1c, "NT_TIB32 size");

typedef struct _PEB32 {
    uint8_t  InheritedAddressSpace;          /* 0x00 */
    uint8_t  ReadImageFileExecOptions;        /* 0x01 */
    uint8_t  BeingDebugged;                  /* 0x02 */
    uint8_t  BitField;                       /* 0x03 */
    uint32_t Mutant;                         /* 0x04 */
    uint32_t ImageBaseAddress;               /* 0x08 */
    uint32_t Ldr;                            /* 0x0C */
    uint32_t ProcessParameters;              /* 0x10 */
    uint32_t SubSystemData;                  /* 0x14 */
    uint32_t ProcessHeap;                    /* 0x18 */
    uint32_t FastPebLock;                    /* 0x1C */
} PEB32;

_Static_assert(offsetof(PEB32, BeingDebugged) == 0x02, "PEB32.BeingDebugged");
_Static_assert(offsetof(PEB32, ImageBaseAddress) == 0x08, "PEB32.ImageBaseAddress");
_Static_assert(offsetof(PEB32, Ldr) == 0x0C, "PEB32.Ldr");
_Static_assert(offsetof(PEB32, ProcessParameters) == 0x10, "PEB32.ProcessParameters");
_Static_assert(offsetof(PEB32, ProcessHeap) == 0x18, "PEB32.ProcessHeap");

typedef struct _TEB32 {
    NT_TIB32 NtTib;                          /* 0x000 */
    uint32_t EnvironmentPointer;             /* 0x01C */
    uint32_t ClientId[2];                    /* 0x020 */
    uint32_t RpcHandle;                      /* 0x028 */
    uint32_t ThreadLocalStoragePointer;      /* 0x02C */
    uint32_t ProcessEnvironmentBlock;        /* 0x030 */
    uint32_t LastErrorValue;                 /* 0x034 */
} TEB32;

_Static_assert(offsetof(TEB32, NtTib) == 0x000, "TEB32.NtTib");
_Static_assert(offsetof(TEB32, ProcessEnvironmentBlock) == 0x030, "TEB32.ProcessEnvironmentBlock");

/* ------------------------------------------------------------------ */
/* PE32 Loader Functions                                              */
/* ------------------------------------------------------------------ */

/* Dedicated 32-bit PE loader function */
ntb0_dll_module *ntb0_pe32_load_image(ntb0_dll_loader *ld, const char *path,
                                       uint64_t base, unsigned flags);

/* 32-bit base relocations (IMAGE_REL_BASED_HIGHLOW) */
int ntb0_syswow64_apply_relocs(ntb0_dll_loader *ld, struct ntb0_dll_module *mod,
                               const void *im);

/* 32-bit import directory parsing */
int ntb0_syswow64_resolve_imports(ntb0_dll_loader *ld, struct ntb0_dll_module *mod,
                                   const void *im, unsigned flags);

/* ------------------------------------------------------------------ */
/* Translation Layer & Dispatch Mapping                               */
/* ------------------------------------------------------------------ */
typedef uint32_t (*ntb0_syswow64_stub_fn)(uint32_t *stack_args);

int ntb0_syswow64_register_stub(const char *dll, const char *func,
                                ntb0_syswow64_stub_fn stub);
uint32_t ntb0_syswow64_dispatch(const char *dll, const char *func,
                                uint32_t *stack_args);

#ifdef __cplusplus
}
#endif

#endif /* NTB0_SYSWOW64_H */
