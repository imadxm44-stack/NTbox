/*
 * peb.c — طبقة PEB/TEB/RTL_USER_PROCESS_PARAMETERS لـ NT-Box (W6).
 *
 * أمين: SEH "unwinding" real غير مُنفَّذ (见 peb.h). كل شيء آخر
 * مدعوم (malloc/mmap/pthread).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <errno.h>
#include <asm/prctl.h>

#include "peb.h"

/* ------------------------------------------------------------------ */
/* حالة thread-local                                                   */
/* ------------------------------------------------------------------ */
static _Thread_local TEB *peb_tls_current_teb = NULL;

TEB *peb_get_current_teb(void) { return peb_tls_current_teb; }
void peb_set_current_teb(TEB *teb) { peb_tls_current_teb = teb; }

/* ------------------------------------------------------------------ */
/* helpers: ASCII -> UTF-16                                            */
/* ------------------------------------------------------------------ */
static uint16_t *ascii_to_wide_dup(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    uint16_t *b = calloc(n + 1, sizeof(uint16_t));
    if (!b)
        return NULL;
    for (size_t i = 0; i < n; i++)
        b[i] = (unsigned char)s[i];
    return b;
}

/* ------------------------------------------------------------------ */
/* TLS (process-global bitmap)                                        */
/* ------------------------------------------------------------------ */
#define NTB0_TLS_SLOTS 64
static unsigned long tls_bitmap = 0; /* 1 = used */

unsigned long TlsAlloc(void)
{
    for (int i = 0; i < NTB0_TLS_SLOTS; i++) {
        if (!((tls_bitmap >> i) & 1u)) {
            tls_bitmap |= (1u << i);
            return (unsigned long)i;
        }
    }
    return 0xFFFFFFFFu;
}

int TlsFree(unsigned long idx)
{
    if (idx >= NTB0_TLS_SLOTS)
        return 0;
    tls_bitmap &= ~(1u << idx);
    TEB *t = peb_get_current_teb();
    if (t && t->ThreadLocalStoragePointer) {
        void **arr = (void **)t->ThreadLocalStoragePointer;
        arr[idx] = NULL;
    }
    return 1;
}

void *TlsGetValue(unsigned long idx)
{
    if (idx >= NTB0_TLS_SLOTS)
        return NULL;
    TEB *t = peb_get_current_teb();
    if (!t || !t->ThreadLocalStoragePointer)
        return NULL;
    void **arr = (void **)t->ThreadLocalStoragePointer;
    return arr[idx];
}

int TlsSetValue(unsigned long idx, void *value)
{
    if (idx >= NTB0_TLS_SLOTS)
        return 0;
    TEB *t = peb_get_current_teb();
    if (!t) {
        /* create implicit TEB? no — require one */
        return 0;
    }
    if (!t->ThreadLocalStoragePointer)
        t->ThreadLocalStoragePointer = calloc(NTB0_TLS_SLOTS, sizeof(void *));
    if (!t->ThreadLocalStoragePointer)
        return 0;
    void **arr = (void **)t->ThreadLocalStoragePointer;
    arr[idx] = value;
    return 1;
}

/* ------------------------------------------------------------------ */
/* create_peb                                                         */
/* ------------------------------------------------------------------ */
PEB *create_peb(const char *image_path, int argc, char **argv, char **envp)
{
    (void)image_path;

    PEB *peb = calloc(1, sizeof *peb);
    if (!peb)
        return NULL;

    RTL_USER_PROCESS_PARAMETERS *pp = calloc(1, sizeof *pp);
    if (!pp) {
        free(peb);
        return NULL;
    }
    pp->MaximumLength = sizeof *pp;
    pp->Length = sizeof *pp;
    pp->Flags = 1; /* NORMALIZED */

    /* CommandLine from argv */
    size_t cmd_narrow_len = 0;
    for (int i = 0; i < argc; i++) {
        if (i)
            cmd_narrow_len += 1; /* space */
        if (argv && argv[i])
            cmd_narrow_len += strlen(argv[i]);
    }
    char *cmd_narrow = calloc(cmd_narrow_len + 1, 1);
    if (cmd_narrow) {
        size_t pos = 0;
        for (int i = 0; i < argc; i++) {
            if (i) {
                cmd_narrow[pos++] = ' ';
            }
            if (argv && argv[i]) {
                size_t l = strlen(argv[i]);
                memcpy(cmd_narrow + pos, argv[i], l);
                pos += l;
            }
        }
        cmd_narrow[pos] = '\0';
    }
    uint16_t *cmd_wide = ascii_to_wide_dup(cmd_narrow ? cmd_narrow : "");
    free(cmd_narrow);
    pp->CommandLine.Buffer = cmd_wide;
    pp->CommandLine.Length = (USHORT)(cmd_narrow_len * 2);
    if (cmd_wide) {
        pp->CommandLine.MaximumLength = (USHORT)((cmd_narrow_len + 1) * 2);
    }

    /* CurrentDirectory */
    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd))
        strcpy(cwd, "/");
    uint16_t *cwd_wide = ascii_to_wide_dup(cwd);
    pp->CurrentDirectory.DosPath.Buffer = cwd_wide;
    if (cwd_wide) {
        size_t wl = 0;
        while (cwd_wide[wl])
            wl++;
        pp->CurrentDirectory.DosPath.Length = (USHORT)(wl * 2);
        pp->CurrentDirectory.DosPath.MaximumLength = (USHORT)((wl + 1) * 2);
    }

    /* ImagePathName */
    if (image_path) {
        uint16_t *ip = ascii_to_wide_dup(image_path);
        pp->ImagePathName.Buffer = ip;
        if (ip) {
            size_t wl = 0;
            while (ip[wl])
                wl++;
            pp->ImagePathName.Length = (USHORT)(wl * 2);
            pp->ImagePathName.MaximumLength = (USHORT)((wl + 1) * 2);
        }
    }

    /* Environment block (UTF-16, double-NUL terminated) */
    size_t total_chars = 0;
    int nenv = 0;
    if (envp) {
        for (; envp[nenv]; nenv++)
            total_chars += strlen(envp[nenv]) + 1;
    }
    total_chars += 1; /* final NUL */
    uint16_t *env_block = calloc(total_chars, sizeof(uint16_t));
    if (env_block) {
        size_t cursor = 0;
        for (int i = 0; i < nenv; i++) {
            size_t l = strlen(envp[i]);
            for (size_t j = 0; j < l; j++)
                env_block[cursor + j] = (unsigned char)envp[i][j];
            cursor += l + 1; /* NUL separator already zero */
        }
        /* final NUL already zero */
    }
    pp->Environment = env_block;

    /* Ldr: minimal empty PEB_LDR_DATA with self-linked lists */
    PEB_LDR_DATA *ldr = calloc(1, sizeof *ldr);
    if (ldr) {
        ldr->Length = sizeof *ldr;
        ldr->Initialized = 1;
        ldr->InLoadOrderModuleList.Flink = &ldr->InLoadOrderModuleList;
        ldr->InLoadOrderModuleList.Blink = &ldr->InLoadOrderModuleList;
        ldr->InMemoryOrderModuleList.Flink = &ldr->InMemoryOrderModuleList;
        ldr->InMemoryOrderModuleList.Blink = &ldr->InMemoryOrderModuleList;
        ldr->InInitializationOrderModuleList.Flink = &ldr->InInitializationOrderModuleList;
        ldr->InInitializationOrderModuleList.Blink = &ldr->InInitializationOrderModuleList;
    }
    peb->Ldr = ldr;

    peb->ProcessParameters = pp;
    peb->ImageBaseAddress = (void *)(uintptr_t)0x00400000ULL;
    peb->ProcessHeap = GetProcessHeap();
    peb->BeingDebugged = 0;
    peb->Environment = pp->Environment;

    return peb;
}

/* ------------------------------------------------------------------ */
/* create_teb                                                         */
/* ------------------------------------------------------------------ */
TEB *create_teb(PEB *peb, void *stack_base)
{
    TEB *teb = calloc(1, sizeof *teb);
    if (!teb)
        return NULL;

    teb->NtTib.ExceptionList = NULL;
    teb->NtTib.StackBase = stack_base;
    teb->NtTib.StackLimit = stack_base ? (void *)((char *)stack_base - (1024 * 1024)) : NULL;
    teb->NtTib.SubSystemTib = NULL;
    teb->NtTib.FiberData = NULL;
    teb->NtTib.ArbitraryUserPointer = NULL;
    teb->NtTib.Self = (NT_TIB *)teb;

    teb->EnvironmentPointer = NULL;
    teb->ClientId.UniqueProcess = (HANDLE)(uintptr_t)getpid();
    teb->ClientId.UniqueThread = (HANDLE)(uintptr_t)gettid();
    teb->RpcHandle = NULL;
    teb->ThreadLocalStoragePointer = calloc(NTB0_TLS_SLOTS, sizeof(void *));
    teb->ProcessEnvironmentBlock = peb;
    teb->LastErrorValue = 0;
    teb->CountOfOwnedCriticalSections = 0;

    if (!peb_get_current_teb())
        peb_set_current_teb(teb);

    return teb;
}

void peb_destroy(PEB *peb)
{
    if (!peb)
        return;
    free(peb->Ldr);
    if (peb->ProcessParameters) {
        RTL_USER_PROCESS_PARAMETERS *pp = peb->ProcessParameters;
        free(pp->CommandLine.Buffer);
        free(pp->CurrentDirectory.DosPath.Buffer);
        free(pp->ImagePathName.Buffer);
        free(pp->Environment);
        free(pp);
    }
    free(peb);
}

/* ------------------------------------------------------------------ */
/* Critical sections                                                  */
/* ------------------------------------------------------------------ */
void InitializeCriticalSection(CRITICAL_SECTION *cs)
{
    if (!cs)
        return;
    memset(cs, 0, sizeof *cs);
    cs->LockSemaphore = calloc(1, sizeof(pthread_mutex_t));
    if (cs->LockSemaphore) {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init((pthread_mutex_t *)cs->LockSemaphore, &attr);
        pthread_mutexattr_destroy(&attr);
    }
}

void DeleteCriticalSection(CRITICAL_SECTION *cs)
{
    if (!cs)
        return;
    if (cs->LockSemaphore) {
        pthread_mutex_destroy((pthread_mutex_t *)cs->LockSemaphore);
        free(cs->LockSemaphore);
        cs->LockSemaphore = NULL;
    }
}

void EnterCriticalSection(CRITICAL_SECTION *cs)
{
    if (!cs)
        return;
    if (!cs->LockSemaphore)
        InitializeCriticalSection(cs);
    pthread_mutex_lock((pthread_mutex_t *)cs->LockSemaphore);
    cs->RecursionCount++;
    cs->OwningThread = (HANDLE)(uintptr_t)pthread_self();
    cs->LockCount++;
    TEB *t = peb_get_current_teb();
    if (t)
        t->CountOfOwnedCriticalSections++;
}

void LeaveCriticalSection(CRITICAL_SECTION *cs)
{
    if (!cs)
        return;
    if (cs->LockSemaphore) {
        if (cs->RecursionCount > 0)
            cs->RecursionCount--;
        if (cs->LockCount > 0)
            cs->LockCount--;
        TEB *t = peb_get_current_teb();
        if (t && t->CountOfOwnedCriticalSections > 0)
            t->CountOfOwnedCriticalSections--;
        pthread_mutex_unlock((pthread_mutex_t *)cs->LockSemaphore);
    }
}

int TryEnterCriticalSection(CRITICAL_SECTION *cs)
{
    if (!cs)
        return 0;
    if (!cs->LockSemaphore)
        InitializeCriticalSection(cs);
    if (pthread_mutex_trylock((pthread_mutex_t *)cs->LockSemaphore) == 0) {
        cs->RecursionCount++;
        cs->OwningThread = (HANDLE)(uintptr_t)pthread_self();
        cs->LockCount++;
        TEB *t = peb_get_current_teb();
        if (t)
            t->CountOfOwnedCriticalSections++;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* SRW locks                                                          */
/* ------------------------------------------------------------------ */
void InitializeSRWLock(SRWLOCK *lock)
{
    if (!lock)
        return;
    if (!lock->Ptr) {
        lock->Ptr = calloc(1, sizeof(pthread_rwlock_t));
        if (lock->Ptr)
            pthread_rwlock_init((pthread_rwlock_t *)lock->Ptr, NULL);
    }
}

static pthread_rwlock_t *srw_get(SRWLOCK *lock)
{
    if (!lock)
        return NULL;
    if (!lock->Ptr) {
        lock->Ptr = calloc(1, sizeof(pthread_rwlock_t));
        if (lock->Ptr)
            pthread_rwlock_init((pthread_rwlock_t *)lock->Ptr, NULL);
    }
    return (pthread_rwlock_t *)lock->Ptr;
}

void AcquireSRWLockExclusive(SRWLOCK *lock)
{
    pthread_rwlock_t *rw = srw_get(lock);
    if (rw)
        pthread_rwlock_wrlock(rw);
}

void ReleaseSRWLockExclusive(SRWLOCK *lock)
{
    pthread_rwlock_t *rw = lock ? (pthread_rwlock_t *)lock->Ptr : NULL;
    if (rw)
        pthread_rwlock_unlock(rw);
}

void AcquireSRWLockShared(SRWLOCK *lock)
{
    pthread_rwlock_t *rw = srw_get(lock);
    if (rw)
        pthread_rwlock_rdlock(rw);
}

void ReleaseSRWLockShared(SRWLOCK *lock)
{
    pthread_rwlock_t *rw = lock ? (pthread_rwlock_t *)lock->Ptr : NULL;
    if (rw)
        pthread_rwlock_unlock(rw);
}

/* ------------------------------------------------------------------ */
/* Heap                                                               */
/* ------------------------------------------------------------------ */
static int process_heap_marker = 0;

HANDLE GetProcessHeap(void)
{
    return (HANDLE)&process_heap_marker;
}

HANDLE HeapCreate(ULONG flags, SIZE_T initial_size, SIZE_T maximum_size)
{
    (void)flags;
    (void)initial_size;
    (void)maximum_size;
    /* distinct non-null handle */
    char *h = calloc(1, 1);
    return (HANDLE)h;
}

int HeapDestroy(HANDLE heap)
{
    free(heap);
    return 1;
}

void *RtlAllocateHeap(HANDLE heap, ULONG flags, SIZE_T size)
{
    (void)heap;
    if (size == 0)
        size = 1;
    size_t *p = malloc(size + sizeof(size_t));
    if (!p)
        return NULL;
    *p = size;
    void *user = p + 1;
    if (flags & HEAP_ZERO_MEMORY)
        memset(user, 0, size);
    return user;
}

int RtlFreeHeap(HANDLE heap, ULONG flags, void *ptr)
{
    (void)heap;
    (void)flags;
    if (!ptr)
        return 1;
    size_t *p = (size_t *)ptr - 1;
    free(p);
    return 1;
}

SIZE_T RtlSizeHeap(HANDLE heap, ULONG flags, void *ptr)
{
    (void)heap;
    (void)flags;
    if (!ptr)
        return (SIZE_T)-1;
    size_t *p = (size_t *)ptr - 1;
    return *p;
}

/* ------------------------------------------------------------------ */
/* Virtual memory                                                     */
/* ------------------------------------------------------------------ */
#define NTB0_VA_MAX 1024
typedef struct { void *base; size_t size; } ntb0_va_rec_t;
static ntb0_va_rec_t va_table[NTB0_VA_MAX];
static int va_count = 0;

static int prot_from_page(DWORD p)
{
    if (p == PAGE_NOACCESS)
        return PROT_NONE;
    int prot = 0;
    if (p & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
             PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
        prot |= PROT_READ;
    if (p & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE |
             PAGE_EXECUTE_WRITECOPY))
        prot |= PROT_WRITE;
    if (p & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
             PAGE_EXECUTE_WRITECOPY))
        prot |= PROT_EXEC;
    if (prot == 0)
        prot = PROT_READ | PROT_WRITE;
    return prot;
}

static void va_record(void *base, size_t size)
{
    if (va_count < NTB0_VA_MAX) {
        va_table[va_count].base = base;
        va_table[va_count].size = size;
        va_count++;
    }
}

static size_t va_lookup(void *base)
{
    for (int i = 0; i < va_count; i++) {
        if (va_table[i].base == base)
            return va_table[i].size;
    }
    return 0;
}

static void va_forget(void *base)
{
    for (int i = 0; i < va_count; i++) {
        if (va_table[i].base == base) {
            for (int j = i; j < va_count - 1; j++)
                va_table[j] = va_table[j + 1];
            va_count--;
            return;
        }
    }
}

void *VirtualAlloc(void *address, SIZE_T size, DWORD type, DWORD protect)
{
    (void)type;
    if (size == 0)
        return NULL;
    size_t pages = (size + 4095) & ~(size_t)4095;
    int prot = prot_from_page(protect);
    /* ensure RWX-ish default for 0 */
    if (protect == 0)
        prot = PROT_READ | PROT_WRITE | PROT_EXEC;
    void *r;
    if (address) {
        r = mmap(address, pages, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    } else {
        r = mmap(NULL, pages, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
    if (r == MAP_FAILED)
        return NULL;
    va_record(r, pages);
    return r;
}

int VirtualProtect(void *address, SIZE_T size, DWORD new_protect, DWORD *old_protect)
{
    if (old_protect)
        *old_protect = PAGE_EXECUTE_READWRITE;
    if (size == 0)
        size = 4096;
    size_t pages = (size + 4095) & ~(size_t)4095;
    void *aligned = (void *)((uintptr_t)address & ~(uintptr_t)4095);
    size_t extra = (uintptr_t)address - (uintptr_t)aligned;
    size_t total = pages + extra;
    if (total == 0)
        total = 4096;
    int prot = prot_from_page(new_protect);
    if (mprotect(aligned, total, prot) != 0)
        return 0;
    return 1;
}

int VirtualFree(void *address, SIZE_T size, DWORD type)
{
    if (!address)
        return 0;
    if (size == 0) {
        size_t rec = va_lookup(address);
        if (rec)
            size = rec;
        else
            size = 4096;
    }
    if (!(type & MEM_RELEASE)) {
        /* MEM_DECOMMIT: keep mapping but (no-op) */
        if (size == 0)
            size = 4096;
        size_t pages = (size + 4095) & ~(size_t)4095;
        /* best effort: mprotect NOACCESS */
        mprotect(address, pages, PROT_NONE);
        return 1;
    }
    size_t pages = (size + 4095) & ~(size_t)4095;
    if (munmap(address, pages) != 0)
        return 0;
    va_forget(address);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Exception / debugger / last error                                  */
/* ------------------------------------------------------------------ */
static void *unhandled_filter = NULL;

void *SetUnhandledExceptionFilter(void *filter)
{
    void *prev = unhandled_filter;
    unhandled_filter = filter;
    return prev;
}

void *GetUnhandledExceptionFilter(void)
{
    return unhandled_filter;
}

int IsDebuggerPresent(void)
{
    TEB *t = peb_get_current_teb();
    if (t && t->ProcessEnvironmentBlock)
        return t->ProcessEnvironmentBlock->BeingDebugged ? 1 : 0;
    return 0;
}

void SetLastError(ULONG err)
{
    TEB *t = peb_get_current_teb();
    if (t)
        t->LastErrorValue = err;
}

ULONG GetLastError(void)
{
    TEB *t = peb_get_current_teb();
    if (t)
        return t->LastErrorValue;
    return 0;
}

/* ------------------------------------------------------------------ */
/* NtSetInformationThread (subset)                                    */
/* ------------------------------------------------------------------ */
long NtSetInformationThread(void *thread_handle, ULONG info_class, void *info, ULONG length)
{
    (void)thread_handle;
    if (!info)
        return -1;
    if (info_class == NTB0_TIC_TEB_POINTER) {
        if (length < sizeof(void *))
            return -1;
        TEB *t = *(TEB **)info;
        peb_set_current_teb(t);
        return 0;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Thread helpers                                                     */
/* ------------------------------------------------------------------ */
typedef struct {
    void *start;
    void *param;
    TEB  *teb;
} peb_thread_ctx_t;

static void *peb_thread_wrapper(void *p)
{
    peb_thread_ctx_t *c = (peb_thread_ctx_t *)p;
    peb_set_current_teb(c->teb);
    if (c->start)
        ((void (*)(void *))c->start)(c->param);
    free(c);
    return NULL;
}

void *CreateThreadWithTeb(void *start_routine, void *param, TEB **teb_out)
{
    PEB *peb = NULL;
    TEB *cur = peb_get_current_teb();
    if (cur)
        peb = cur->ProcessEnvironmentBlock;

    TEB *teb = create_teb(peb, NULL);
    if (!teb)
        return NULL;

    pthread_t *th = malloc(sizeof(pthread_t));
    if (!th) {
        free(teb);
        return NULL;
    }

    peb_thread_ctx_t *c = calloc(1, sizeof *c);
    if (!c) {
        free(th);
        free(teb);
        return NULL;
    }
    c->start = start_routine;
    c->param = param;
    c->teb = teb;

    if (pthread_create(th, NULL, peb_thread_wrapper, c) != 0) {
        free(c);
        free(th);
        free(teb);
        return NULL;
    }
    if (teb_out)
        *teb_out = teb;
    return th;
}

void ExitThread(ULONG code)
{
    (void)code;
    pthread_exit(NULL);
}

void ExitProcess(ULONG code)
{
    _exit((int)code);
}

unsigned long WaitForSingleObject(void *handle, unsigned long timeout_ms)
{
    (void)timeout_ms;
    if (!handle)
        return 0;
    pthread_t *th = (pthread_t *)handle;
    pthread_join(*th, NULL);
    free(th);
    return 0; /* WAIT_OBJECT_0 */
}


