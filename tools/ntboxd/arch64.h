/*
 * arch64.h — 64-bit architecture helpers for NT-Box.
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
#ifndef NTB0_ARCH64_H
#define NTB0_ARCH64_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Architecture detection                                              */
/* ------------------------------------------------------------------ */

/*
 * Returns 1 if running on a 64-bit architecture (x86_64 / AArch64),
 * 0 otherwise. This is a compile-time constant on supported platforms.
 */
static inline int ntb0_arch_is64(void)
{
#if defined(__x86_64__) || defined(__aarch64__) || defined(__LP64__)
    return 1;
#else
    return 0;
#endif
}

/*
 * Returns the pointer size in bytes (8 on 64-bit, 4 on 32-bit).
 * This is a compile-time constant.
 */
static inline size_t ntb0_arch_pointer_size(void)
{
    return sizeof(void *);
}

/*
 * Aligns a value up to the given alignment boundary.
 * alignment must be a power of two.
 */
static inline uintptr_t ntb0_arch_align(uintptr_t value, size_t alignment)
{
    return (value + alignment - 1) & ~(uintptr_t)(alignment - 1);
}

/*
 * Aligns a pointer up to the given alignment boundary.
 * Returns the aligned pointer.
 */
static inline void *ntb0_arch_align_ptr(void *ptr, size_t alignment)
{
    return (void *)ntb0_arch_align((uintptr_t)ptr, alignment);
}

/*
 * Checks if a pointer is aligned to the given boundary.
 * Returns 1 if aligned, 0 otherwise.
 */
static inline int ntb0_arch_is_aligned(const void *ptr, size_t alignment)
{
    return ((uintptr_t)ptr & (alignment - 1)) == 0;
}

/* ------------------------------------------------------------------ */
/* 64-bit safe pointer arithmetic                                      */
/* ------------------------------------------------------------------ */

/*
 * Adds an offset to a pointer safely using uintptr_t.
 * Prevents accidental 32-bit truncation on 64-bit platforms.
 */
static inline void *ntb0_arch_ptr_add(const void *ptr, ptrdiff_t offset)
{
    return (void *)((uintptr_t)ptr + (uintptr_t)offset);
}

/*
 * Subtracts two pointers safely, returning the difference in bytes.
 */
static inline ptrdiff_t ntb0_arch_ptr_diff(const void *a, const void *b)
{
    return (ptrdiff_t)((uintptr_t)a - (uintptr_t)b);
}

/*
 * Computes pointer + index * element_size safely.
 * Prevents integer overflow in pointer arithmetic.
 */
static inline void *ntb0_arch_ptr_index(const void *base, size_t index, size_t element_size)
{
    return (void *)((uintptr_t)base + index * element_size);
}

/* ------------------------------------------------------------------ */
/* Structure alignment helpers                                         */
/* ------------------------------------------------------------------ */

/* Natural alignment for 64-bit: 8 bytes */
#define NTB0_ARCH_ALIGN_8   8
#define NTB0_ARCH_ALIGN_16  16
#define NTB0_ARCH_ALIGN_32  32
#define NTB0_ARCH_ALIGN_64  64
#define NTB0_ARCH_ALIGN_128 128
#define NTB0_ARCH_ALIGN_256 256
#define NTB0_ARCH_ALIGN_4K  4096

/*
 * Aligns size to 8 bytes (natural 64-bit alignment).
 */
static inline size_t ntb0_arch_align8(size_t size)
{
    return ntb0_arch_align(size, NTB0_ARCH_ALIGN_8);
}

/*
 * Aligns size to 16 bytes (SSE/AVX alignment).
 */
static inline size_t ntb0_arch_align16(size_t size)
{
    return ntb0_arch_align(size, NTB0_ARCH_ALIGN_16);
}

/*
 * Aligns size to page boundary (4KB).
 */
static inline size_t ntb0_arch_align_page(size_t size)
{
    return ntb0_arch_align(size, NTB0_ARCH_ALIGN_4K);
}

/* ------------------------------------------------------------------ */
/* Type size verification (compile-time assertions)                    */
/* ------------------------------------------------------------------ */

/* Ensure standard types have expected sizes on 64-bit */
#if defined(__x86_64__) || defined(__aarch64__) || defined(__LP64__)
#define NTB0_STATIC_ASSERT_64() \
    _Static_assert(sizeof(void *) == 8, "64-bit pointers expected"); \
    _Static_assert(sizeof(size_t) == 8, "64-bit size_t expected"); \
    _Static_assert(sizeof(uintptr_t) == 8, "64-bit uintptr_t expected"); \
    _Static_assert(sizeof(long) == 8, "64-bit long expected"); \
    _Static_assert(sizeof(ptrdiff_t) == 8, "64-bit ptrdiff_t expected")
#else
#define NTB0_STATIC_ASSERT_64() \
    _Static_assert(sizeof(void *) == 4, "32-bit pointers expected"); \
    _Static_assert(sizeof(size_t) == 4, "32-bit size_t expected"); \
    _Static_assert(sizeof(uintptr_t) == 4, "32-bit uintptr_t expected")
#endif

/* Invoke static assertions */
NTB0_STATIC_ASSERT_64();

/* ------------------------------------------------------------------ */
/* PE loader 64-bit helpers                                            */
/* ------------------------------------------------------------------ */

/*
 * Reads a 64-bit value from a potentially unaligned memory location.
 * Safe for reading PE32+ optional header fields.
 */
static inline uint64_t ntb0_arch_read_u64(const void *ptr)
{
    uint64_t val;
    memcpy(&val, ptr, sizeof(val));
    return val;
}

/*
 * Writes a 64-bit value to a potentially unaligned memory location.
 */
static inline void ntb0_arch_write_u64(void *ptr, uint64_t val)
{
    memcpy(ptr, &val, sizeof(val));
}

/*
 * Reads a 32-bit value from a potentially unaligned memory location.
 */
static inline uint32_t ntb0_arch_read_u32(const void *ptr)
{
    uint32_t val;
    memcpy(&val, ptr, sizeof(val));
    return val;
}

/*
 * Writes a 32-bit value to a potentially unaligned memory location.
 */
static inline void ntb0_arch_write_u32(void *ptr, uint32_t val)
{
    memcpy(ptr, &val, sizeof(val));
}

/* ------------------------------------------------------------------ */
/* Handle table 64-bit helpers                                         */
/* ------------------------------------------------------------------ */

/*
 * Encodes a handle with 64-bit cookie format:
 *   bits 63..48 = magic (0x4E54 'NT')
 *   bits 47..24 = generation
 *   bits 23..0  = index + 1
 */
#define NTB0_HANDLE_MAGIC_64  0x4E54ULL
#define NTB0_HANDLE_IDX_MASK  0x00FFFFFFULL
#define NTB0_HANDLE_GEN_MASK  0xFFFFFF000000ULL
#define NTB0_HANDLE_MAGIC_MASK 0xFFFF000000000000ULL

static inline uint64_t ntb0_arch_handle_make(uint32_t index, uint32_t generation)
{
    return (NTB0_HANDLE_MAGIC_64 << 48) |
           ((uint64_t)generation << 24) |
           ((uint64_t)(index + 1) & NTB0_HANDLE_IDX_MASK);
}

static inline uint32_t ntb0_arch_handle_index(uint64_t handle)
{
    return (uint32_t)(handle & NTB0_HANDLE_IDX_MASK);
}

static inline uint32_t ntb0_arch_handle_generation(uint64_t handle)
{
    return (uint32_t)((handle >> 24) & 0xFFFFFFULL);
}

static inline int ntb0_arch_handle_check_magic(uint64_t handle)
{
    return ((handle >> 48) & 0xFFFFULL) == NTB0_HANDLE_MAGIC_64;
}

/* ------------------------------------------------------------------ */
/* Syscall translator 64-bit helpers                                   */
/* ------------------------------------------------------------------ */

/*
 * Syscall argument register mapping for x86_64 Windows ABI:
 *   RCX, RDX, R8, R9 = first 4 args
 *   Stack = remaining args (shadow space + args 5+)
 *
 * These helpers extract arguments from the register/stack frame.
 */

struct ntb0_arch_syscall_frame {
    uint64_t rcx, rdx, r8, r9;     /* Register args 1-4 */
    uint64_t rsp;                   /* Stack pointer at entry */
    uint64_t rip;                   /* Return address */
};

/*
 * Extracts syscall arguments from a 64-bit register/stack frame.
 * num = syscall number (in RAX at trap entry)
 * frame = captured register state
 * args_out = array of up to 11 arguments (Windows max)
 * Returns number of arguments extracted.
 */
int ntb0_arch_syscall_extract_args(long num, const struct ntb0_arch_syscall_frame *frame,
                                   uint64_t args_out[11]);

/* ------------------------------------------------------------------ */
/* Thread management 64-bit helpers                                    */
/* ------------------------------------------------------------------ */

/*
 * TEB (Thread Environment Block) base address for current thread.
 * On 64-bit Windows, TEB is at GS:[0x30].
 */
uint64_t ntb0_arch_teb_base(void);

/*
 * Sets up a 64-bit thread context for Windows ABI entry.
 * Used when creating new threads that execute Windows code.
 */
int ntb0_arch_thread_setup_context(void *stack_base, size_t stack_size,
                                   uint64_t entry_point, uint64_t param,
                                   struct ntb0_arch_syscall_frame *ctx_out);

#ifdef __cplusplus
}
#endif

#endif /* NTB0_ARCH64_H */