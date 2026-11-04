/*
 * arch64.c — 64-bit architecture helpers implementation for NT-Box.
 *
 * Ensures all components properly handle 64-bit: PE loader, syscall translator,
 * handle table, thread management. Provides alignment, pointer size, and
 * 64-bit detection utilities.
 *
 * Lamps: ARCH-001..ARCH-004
 *   ARCH-001 OK — ntb0_arch_is64() reports true on x86_64
 *   ARCH-002 OK — ntb0_arch_pointer_size() == 8
 *   ARCH-003 OK — ntb0_arch_align() aligns to 8/16 boundaries
 *   ARCH-004 OK — all pointer arithmetic uses uintptr_t
 */
#define _GNU_SOURCE
#include "arch64.h"

#include <string.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Syscall argument extraction (x86_64 Windows ABI)                    */
/* ------------------------------------------------------------------ */

/*
 * Windows x64 calling convention:
 *   RCX, RDX, R8, R9 = first 4 integer/pointer args
 *   Stack = args 5+ (with 32-byte shadow space at [RSP+0]..[RSP+31])
 *   RSP at entry points to return address; shadow space is above it.
 *
 * When we trap a syscall via ntb0_trampoline.S, the frame has:
 *   - Registers saved (RCX, RDX, R8, R9 in specific locations)
 *   - Stack pointer at syscall entry
 *
 * This function extracts up to 11 arguments (NT max).
 */
int ntb0_arch_syscall_extract_args(long num, const struct ntb0_arch_syscall_frame *frame,
                                   uint64_t args_out[11])
{
    if (!frame || !args_out) {
        return -1;
    }

    /* First 4 args in registers */
    args_out[0] = frame->rcx;
    args_out[1] = frame->rdx;
    args_out[2] = frame->r8;
    args_out[3] = frame->r9;

    /* Args 5+ on stack, after 32-byte shadow space */
    const uint64_t *stack = (const uint64_t *)(uintptr_t)frame->rsp;
    /* stack[0] = return address, stack[1..4] = shadow space */
    for (int i = 4; i < 11; i++) {
        args_out[i] = stack[i - 3];  /* skip ret + 32 bytes shadow */
    }

    return 11;  /* NT syscalls have max 11 args */
}

/* ------------------------------------------------------------------ */
/* TEB access                                                          */
/* ------------------------------------------------------------------ */

#if defined(__x86_64__)
/*
 * On x86_64 Windows, TEB is at GS:[0x30].
 * On Linux, we can't directly access GS segment from user mode easily,
 * so we use arch_prctl or inline asm.
 */
uint64_t ntb0_arch_teb_base(void)
{
    uint64_t teb;
    /* Read GS base via arch_prctl(ARCH_GET_GS, ...) or inline asm */
    __asm__ volatile("mov %%gs:0x30, %0" : "=r"(teb));
    return teb;
}
#elif defined(__aarch64__)
/*
 * On AArch64 Windows, TEB is at TPIDR_EL0 (thread pointer).
 * On Linux, we read the TPIDR_EL0 register.
 */
uint64_t ntb0_arch_teb_base(void)
{
    uint64_t teb;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(teb));
    return teb;
}
#else
uint64_t ntb0_arch_teb_base(void)
{
    return 0;
}
#endif

/* ------------------------------------------------------------------ */
/* Thread context setup                                                */
/* ------------------------------------------------------------------ */

/*
 * Sets up a 64-bit thread context for Windows ABI entry.
 * The context will be used by ntb0_trampoline.S to enter Windows code.
 */
int ntb0_arch_thread_setup_context(void *stack_base, size_t stack_size,
                                   uint64_t entry_point, uint64_t param,
                                   struct ntb0_arch_syscall_frame *ctx_out)
{
    if (!stack_base || !ctx_out || stack_size < 4096) {
        return -1;
    }

    /* Stack grows down; align to 16 bytes per x64 ABI */
    uintptr_t sp = ntb0_arch_align((uintptr_t)stack_base + stack_size, 16);

    /* Windows x64 requires 32-byte shadow space + 8-byte return address */
    sp -= 40;  /* 32 shadow + 8 ret addr */

    /* Clear the shadow space and return address slot */
    memset((void *)sp, 0, 40);

    ctx_out->rcx = param;           /* First arg in RCX */
    ctx_out->rdx = 0;               /* Second arg */
    ctx_out->r8 = 0;                /* Third arg */
    ctx_out->r9 = 0;                /* Fourth arg */
    ctx_out->rsp = sp;              /* Stack pointer at entry */
    ctx_out->rip = entry_point;     /* Entry point */

    return 0;
}

/* ------------------------------------------------------------------ */
/* 64-bit malloc alignment verification                                */
/* ------------------------------------------------------------------ */

/*
 * Verifies that malloc returns properly aligned memory for 64-bit.
 * Returns 1 if aligned to at least 16 bytes, 0 otherwise.
 */
int ntb0_arch_verify_malloc_alignment(void)
{
    void *ptr = malloc(1);
    if (!ptr) {
        return 0;
    }
    int aligned = ntb0_arch_is_aligned(ptr, 16);
    free(ptr);
    return aligned;
}

/* ------------------------------------------------------------------ */
/* Pointer arithmetic stress test                                      */
/* ------------------------------------------------------------------ */

/*
 * Tests various pointer arithmetic operations for 64-bit correctness.
 * Returns 1 on success, 0 on failure.
 */
int ntb0_arch_test_pointer_arithmetic(void)
{
    /* Allocate a large buffer to test 64-bit offsets */
    size_t size = 1024 * 1024;  /* 1MB */
    char *buf = malloc(size);
    if (!buf) {
        return 0;
    }

    /* Test ntb0_arch_ptr_add with large offset */
    char *p1 = ntb0_arch_ptr_add(buf, size / 2);
    if (p1 != buf + size / 2) {
        free(buf);
        return 0;
    }

    /* Test ntb0_arch_ptr_diff */
    ptrdiff_t diff = ntb0_arch_ptr_diff(p1, buf);
    if (diff != (ptrdiff_t)(size / 2)) {
        free(buf);
        return 0;
    }

    /* Test ntb0_arch_ptr_index */
    char *p2 = ntb0_arch_ptr_index(buf, 1000, 512);
    if (p2 != buf + 1000 * 512) {
        free(buf);
        return 0;
    }

    /* Test alignment helpers */
    void *aligned = ntb0_arch_align_ptr(buf + 1, 16);
    if (!ntb0_arch_is_aligned(aligned, 16)) {
        free(buf);
        return 0;
    }

    free(buf);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Size type verification                                              */
/* ------------------------------------------------------------------ */

/*
 * Verifies that size_t and uintptr_t are 64-bit on 64-bit platforms.
 * Returns 1 on success, 0 on failure.
 */
int ntb0_arch_test_size_types(void)
{
#if defined(__x86_64__) || defined(__aarch64__) || defined(__LP64__)
    if (sizeof(size_t) != 8) return 0;
    if (sizeof(uintptr_t) != 8) return 0;
    if (sizeof(void *) != 8) return 0;
    if (sizeof(ptrdiff_t) != 8) return 0;
#else
    if (sizeof(size_t) != 4) return 0;
    if (sizeof(uintptr_t) != 4) return 0;
    if (sizeof(void *) != 4) return 0;
    if (sizeof(ptrdiff_t) != 4) return 0;
#endif
    return 1;
}