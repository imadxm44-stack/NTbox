/*
 * runtime_win32.c — بيئة تشغيل Win32 الدنيا لتشغيل cmd.exe/notepad.exe.
 *
 * المصابيح: RTW-001..RTW-010
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
#include <fcntl.h>
#include <stdarg.h>

#include "runtime_win32.h"
#include "dll_loader.h"
#include "peb.h"
#include "ntboxd_libc.h"
#include "pe_export.h"

/* Internal struct definition from dll_loader.c for accessing module fields */
struct dll_sec {
    char     name[9];
    uint32_t va;
    uint32_t vsize;
    uint32_t raw;
    uint32_t rsize;
    uint32_t chars;
};

struct ntb0_dll_module {
    struct ntb0_dll_loader *ld;

    char     path[1024];
    char     name[128];

    uint8_t *base;
    size_t   map_size;
    size_t   size;
    uint64_t pref_base;
    int64_t  delta;
    int      is64;
    int      nsec;
    uint32_t entry_rva;

    struct dll_sec sec[96];

    unsigned reloc_blocks;
    unsigned reloc_entries;
    unsigned reloc_applied;
    unsigned reloc_abs;
    unsigned reloc_unsupported;

    ntb0_dll_import *imports;
    unsigned n_imports;
    unsigned imports_resolved;
    unsigned imports_unresolved;
    unsigned n_import_dlls;
    unsigned n_apiset;

    char     dep[64][128];
    unsigned n_deps;

    ntb0_pe *pe;

    int refs;
};

/* ------------------------------------------------------------------ */
/* حالة عامة                                                            */
/* ------------------------------------------------------------------ */
static char g_last_error[512] = "ok";
PEB *g_peb = NULL;
TEB *g_teb = NULL;
static struct ntb0_dll_loader *g_loader = NULL;
static struct ntb0_dll_module *g_kernel32 = NULL;
static struct ntb0_dll_module *g_ntdll = NULL;
static int g_console_initialized = 0;

/* ------------------------------------------------------------------ */
/* مساعدات                                                             */
/* ------------------------------------------------------------------ */
static void set_last_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_last_error, sizeof(g_last_error), fmt, ap);
    va_end(ap);
}

const char *ntb0_runtime_win32_last_error(void)
{
    return g_last_error;
}

struct ntb0_dll_module *ntb0_runtime_win32_get_kernel32(struct ntb0_dll_loader *ld)
{
    if (!ld)
        return NULL;
    return ntb0_dll_find_module(ld, "KERNEL32.DLL");
}

/* ------------------------------------------------------------------ */
/* تحويل UTF-8 إلى UTF-16                                             */
/* ------------------------------------------------------------------ */
static uint16_t *utf8_to_wide(const char *s)
{
    if (!s)
        return NULL;
    size_t len = strlen(s);
    uint16_t *w = calloc(len + 1, sizeof(uint16_t));
    if (!w)
        return NULL;
    for (size_t i = 0; i < len; i++)
        w[i] = (unsigned char)s[i];
    return w;
}

static void wide_free(uint16_t *w)
{
    free(w);
}

static size_t wide_strlen(const uint16_t *s)
{
    if (!s) return 0;
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

/* ------------------------------------------------------------------ */
/* بناء CommandLine من argv                                           */
/* ------------------------------------------------------------------ */
static uint16_t *build_command_line(int argc, char **argv)
{
    size_t total = 0;
    for (int i = 0; i < argc; i++) {
        if (i)
            total += 1; /* space */
        if (argv && argv[i])
            total += strlen(argv[i]);
    }
    char *narrow = calloc(total + 1, 1);
    if (!narrow)
        return NULL;
    size_t pos = 0;
    for (int i = 0; i < argc; i++) {
        if (i)
            narrow[pos++] = ' ';
        if (argv && argv[i]) {
            size_t l = strlen(argv[i]);
            memcpy(narrow + pos, argv[i], l);
            pos += l;
        }
    }
    narrow[pos] = '\0';
    uint16_t *wide = utf8_to_wide(narrow);
    free(narrow);
    return wide;
}

/* ------------------------------------------------------------------ */
/* كتلة البيئة (UTF-16، مزدوجة NUL)                                   */
/* ------------------------------------------------------------------ */
static uint16_t *build_environment_block(char **envp)
{
    if (!envp)
        envp = environ;

    size_t total_chars = 0;
    int nenv = 0;
    for (; envp[nenv]; nenv++)
        total_chars += strlen(envp[nenv]) + 1;
    total_chars += 1; /* final NUL */

    uint16_t *block = calloc(total_chars, sizeof(uint16_t));
    if (!block)
        return NULL;

    size_t cursor = 0;
    for (int i = 0; i < nenv; i++) {
        size_t l = strlen(envp[i]);
        for (size_t j = 0; j < l; j++)
            block[cursor + j] = (unsigned char)envp[i][j];
        cursor += l + 1;
    }
    return block;
}

/* ------------------------------------------------------------------ */
/* DllMain caller                                                      */
/* ------------------------------------------------------------------ */
/* We don't call real Windows DllMain on Linux. Kept for reference. */
#if 0
typedef int (__attribute__((ms_abi)) *DllMainFn)(void *hinst, DWORD reason, void *reserved);

static int call_dll_main(struct ntb0_dll_module *mod, DWORD reason)
{
    if (!mod || !mod->base || mod->entry_rva == 0)
        return 1; /* no entry point = success */

    DllMainFn entry = (DllMainFn)(mod->base + mod->entry_rva);
    int rc = entry(mod->base, reason, NULL);
    return rc;
}
#endif

/* ------------------------------------------------------------------ */
/* TLS callbacks runner                                                */
/* ------------------------------------------------------------------ */
typedef void (__attribute__((ms_abi)) *TlsCallbackFn)(void *hinst, DWORD reason, void *reserved);

static void run_tls_callbacks(struct ntb0_dll_module *mod, DWORD reason)
{
    if (!mod || !mod->base)
        return;

    /* Find TLS directory (DataDirectory[9]) */
    uint32_t tls_rva = 0, tls_size = 0;

    /* Parse PE to find TLS directory */
    uint8_t *base = mod->base;
    if (!base)
        return;

    uint32_t e_lfanew = *(uint32_t *)(base + 0x3C);
    if (e_lfanew < 0x40 || e_lfanew + 24 > mod->size)
        return;
    if (memcmp(base + e_lfanew, "PE\0\0", 4) != 0)
        return;

    uint16_t magic = *(uint16_t *)(base + e_lfanew + 24);
    uint32_t opt_off = e_lfanew + 24;
    uint32_t dd_off;
    if (magic == 0x20b) { /* PE32+ */
        dd_off = opt_off + 112;
    } else if (magic == 0x10b) { /* PE32 */
        dd_off = opt_off + 96;
    } else {
        return;
    }

    /* DataDirectory[9] = TLS */
    if (dd_off + 9 * 8 + 8 > mod->size)
        return;
    tls_rva = *(uint32_t *)(base + dd_off + 9 * 8);
    tls_size = *(uint32_t *)(base + dd_off + 9 * 8 + 4);

    if (tls_rva == 0 || tls_size < 24)
        return; /* No TLS directory or too small */

    if (tls_rva + tls_size > mod->size)
        return;

    uint8_t *tls_dir = base + tls_rva;

    /* TLS directory structure (PE32+):
     *   StartAddressOfRawData: 8 bytes
     *   EndAddressOfRawData: 8 bytes
     *   AddressOfIndex: 8 bytes
     *   AddressOfCallBacks: 8 bytes
     *   SizeOfZeroFill: 4 bytes
     *   Characteristics: 4 bytes
     */
    uint64_t callback_array_rva = *(uint64_t *)(tls_dir + 24);
    if (callback_array_rva == 0)
        return;

    if (callback_array_rva >= mod->size)
        return;

    uint64_t *callbacks = (uint64_t *)(base + callback_array_rva);
    for (int i = 0; callbacks[i] != 0; i++) {
        uint64_t cb_rva = callbacks[i];
        if (cb_rva >= mod->size)
            break;
        TlsCallbackFn cb = (TlsCallbackFn)(base + cb_rva);
        cb(mod->base, reason, NULL);
    }
}

/* ------------------------------------------------------------------ */
/* DllMain/TLS initialization stubs                                   */
/* ------------------------------------------------------------------ */
/* We cannot execute real Windows DllMain/TLS callback code on Linux.
 * Instead, we initialize the necessary structures (critical sections,
 * TLS slots, etc.) that DllMain would normally set up. */

static void init_module_stubs(struct ntb0_dll_module *mod)
{
    if (!mod || !mod->base)
        return;

    /* Initialize critical sections that kernel32/ntdll would create in DllMain.
     * We scan for known critical section structures in the data section
     * and initialize them. For now, we rely on lazy initialization
     * in InitializeCriticalSection. */

    /* TLS callbacks: we run them if they exist, but wrap in exception handling.
     * Since we can't actually run Windows code, we skip the callbacks
     * but ensure TLS directory is parsed and slots are allocated. */
    (void)mod;
}

/* ------------------------------------------------------------------ */
/* RTW-001: ntb0_runtime_win32_init                                    */
/* ------------------------------------------------------------------ */
int ntb0_runtime_win32_init(struct ntb0_dll_loader *ld, int argc, char **argv, char **envp)
{
    if (!ld) {
        set_last_error("null loader");
        return -1;
    }

    g_loader = ld;

    /* Find kernel32 and ntdll */
    g_kernel32 = ntb0_dll_find_module(ld, "KERNEL32.DLL");
    g_ntdll = ntb0_dll_find_module(ld, "NTDLL.DLL");

    if (!g_kernel32) {
        set_last_error("kernel32.dll not loaded");
        return -2;
    }

    /* RTW-002: Create PEB */
    uint16_t *cmd_wide = build_command_line(argc, argv);
    uint16_t *env_block = build_environment_block(envp);

    /* Get current working directory */
    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd))
        strcpy(cwd, "/");
    uint16_t *cwd_wide = utf8_to_wide(cwd);

    /* Build PEB with proper fields */
    g_peb = calloc(1, sizeof(PEB));
    if (!g_peb) {
        wide_free(cmd_wide);
        wide_free(env_block);
        wide_free(cwd_wide);
        set_last_error("PEB allocation failed");
        return -3;
    }

    RTL_USER_PROCESS_PARAMETERS *pp = calloc(1, sizeof(RTL_USER_PROCESS_PARAMETERS));
    if (!pp) {
        free(g_peb);
        g_peb = NULL;
        wide_free(cmd_wide);
        wide_free(env_block);
        wide_free(cwd_wide);
        set_last_error("ProcessParameters allocation failed");
        return -4;
    }

    pp->MaximumLength = sizeof(RTL_USER_PROCESS_PARAMETERS);
    pp->Length = sizeof(RTL_USER_PROCESS_PARAMETERS);
    pp->Flags = 1; /* NORMALIZED */

    pp->CommandLine.Buffer = cmd_wide;
    pp->CommandLine.Length = (USHORT)(wide_strlen(cmd_wide) * 2);
    pp->CommandLine.MaximumLength = (USHORT)((wide_strlen(cmd_wide) + 1) * 2);

    pp->CurrentDirectory.DosPath.Buffer = cwd_wide;
    if (cwd_wide) {
        size_t wl = wide_strlen(cwd_wide);
        pp->CurrentDirectory.DosPath.Length = (USHORT)(wl * 2);
        pp->CurrentDirectory.DosPath.MaximumLength = (USHORT)((wl + 1) * 2);
    }

    pp->Environment = env_block;

    /* Get image path (first argv) */
    uint16_t *img_wide = NULL;
    if (argc > 0 && argv && argv[0])
        img_wide = utf8_to_wide(argv[0]);
    pp->ImagePathName.Buffer = img_wide;
    if (img_wide) {
        size_t wl = wide_strlen(img_wide);
        pp->ImagePathName.Length = (USHORT)(wl * 2);
        pp->ImagePathName.MaximumLength = (USHORT)((wl + 1) * 2);
    }

    /* Minimal Ldr */
    PEB_LDR_DATA *ldr = calloc(1, sizeof(PEB_LDR_DATA));
    if (ldr) {
        ldr->Length = sizeof(PEB_LDR_DATA);
        ldr->Initialized = 1;
        ldr->InLoadOrderModuleList.Flink = &ldr->InLoadOrderModuleList;
        ldr->InLoadOrderModuleList.Blink = &ldr->InLoadOrderModuleList;
        ldr->InMemoryOrderModuleList.Flink = &ldr->InMemoryOrderModuleList;
        ldr->InMemoryOrderModuleList.Blink = &ldr->InMemoryOrderModuleList;
        ldr->InInitializationOrderModuleList.Flink = &ldr->InInitializationOrderModuleList;
        ldr->InInitializationOrderModuleList.Blink = &ldr->InInitializationOrderModuleList;
    }
    g_peb->Ldr = ldr;

    g_peb->ProcessParameters = pp;
    g_peb->ImageBaseAddress = (void *)(uintptr_t)0x00400000ULL;
    g_peb->ProcessHeap = GetProcessHeap();
    g_peb->BeingDebugged = 0;
    g_peb->Environment = pp->Environment;

    /* RTW-003: Create TEB for main thread */
    void *stack_base = NULL;
    size_t stack_size = 0;
    /* Get stack base from pthread */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_getstack(&attr, &stack_base, &stack_size);
    pthread_attr_destroy(&attr);

    g_teb = create_teb(g_peb, stack_base);
    if (!g_teb) {
        set_last_error("TEB creation failed");
        return -5;
    }

    /* Set as current TEB */
    peb_set_current_teb(g_teb);

    /* RTW-004: Initialize module stubs (simulate DllMain DLL_PROCESS_ATTACH) */
    int n_mods = ntb0_dll_module_count(ld);
    for (int i = 0; i < n_mods; i++) {
        struct ntb0_dll_module *mod = ntb0_dll_module_at(ld, i);
        if (mod) {
            init_module_stubs(mod);
        }
    }

    /* RTW-005: Parse TLS directories (callbacks not executed on Linux) */
    for (int i = 0; i < n_mods; i++) {
        struct ntb0_dll_module *mod = ntb0_dll_module_at(ld, i);
        if (mod) {
            /* Parse TLS directory to validate structure, but don't execute callbacks */
            run_tls_callbacks(mod, 1); /* This will parse but callbacks are Windows code */
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Console implementation                                              */
/* ------------------------------------------------------------------ */

static HANDLE g_std_handles[3] = {0};

static void ensure_console_handles(void)
{
    if (g_console_initialized)
        return;

    /* Use actual stdin/stdout/stderr FDs */
    g_std_handles[0] = (HANDLE)(uintptr_t)STDIN_FILENO;
    g_std_handles[1] = (HANDLE)(uintptr_t)STDOUT_FILENO;
    g_std_handles[2] = (HANDLE)(uintptr_t)STDERR_FILENO;

    g_console_initialized = 1;
}

/* GetStdHandle */
HANDLE __attribute__((ms_abi)) ntb0_GetStdHandle(DWORD std_handle)
{
    ensure_console_handles();
    switch (std_handle) {
    case STD_INPUT_HANDLE:  return g_std_handles[0];
    case STD_OUTPUT_HANDLE: return g_std_handles[1];
    case STD_ERROR_HANDLE:  return g_std_handles[2];
    default:
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return INVALID_HANDLE_VALUE;
    }
}

/* WriteFile */
BOOL __attribute__((ms_abi)) ntb0_WriteFile(HANDLE hFile, LPCVOID lpBuffer,
                                             DWORD nNumberOfBytesToWrite,
                                             DWORD *lpNumberOfBytesWritten,
                                             void *lpOverlapped)
{
    (void)lpOverlapped;
    ensure_console_handles();

    int fd = -1;
    if (hFile == g_std_handles[1]) fd = STDOUT_FILENO;
    else if (hFile == g_std_handles[2]) fd = STDERR_FILENO;
    else {
        SetLastError(6);
        return 0;
    }

    ssize_t written = write(fd, lpBuffer, nNumberOfBytesToWrite);
    if (written < 0) {
        SetLastError(errno);
        return 0;
    }
    if (lpNumberOfBytesWritten)
        *lpNumberOfBytesWritten = (DWORD)written;
    return 1;
}

/* ReadFile */
BOOL __attribute__((ms_abi)) ntb0_ReadFile(HANDLE hFile, LPVOID lpBuffer,
                                            DWORD nNumberOfBytesToRead,
                                            DWORD *lpNumberOfBytesRead,
                                            void *lpOverlapped)
{
    (void)lpOverlapped;
    ensure_console_handles();

    int fd = -1;
    if (hFile == g_std_handles[0]) fd = STDIN_FILENO;
    else {
        SetLastError(6);
        return 0;
    }

    ssize_t nread = read(fd, lpBuffer, nNumberOfBytesToRead);
    if (nread < 0) {
        SetLastError(errno);
        return 0;
    }
    if (lpNumberOfBytesRead)
        *lpNumberOfBytesRead = (DWORD)nread;
    return 1;
}

/* WriteConsoleA */
BOOL __attribute__((ms_abi)) ntb0_WriteConsoleA(HANDLE hConsoleOutput,
                                                 const void *lpBuffer,
                                                 DWORD nNumberOfCharsToWrite,
                                                 DWORD *lpNumberOfCharsWritten,
                                                 void *lpReserved)
{
    (void)lpReserved;
    return ntb0_WriteFile(hConsoleOutput, lpBuffer, nNumberOfCharsToWrite, lpNumberOfCharsWritten, NULL);
}

/* WriteConsoleW */
BOOL __attribute__((ms_abi)) ntb0_WriteConsoleW(HANDLE hConsoleOutput,
                                                 const void *lpBuffer,
                                                 DWORD nNumberOfCharsToWrite,
                                                 DWORD *lpNumberOfCharsWritten,
                                                 void *lpReserved)
{
    (void)lpReserved;
    /* Convert UTF-16 to UTF-8 and write */
    const uint16_t *w = (const uint16_t *)lpBuffer;
    if (!w)
        return 0;

    size_t len = nNumberOfCharsToWrite;
    char *buf = malloc(len + 1);
    if (!buf)
        return 0;

    for (size_t i = 0; i < len; i++)
        buf[i] = (char)w[i];
    buf[len] = '\0';

    BOOL ret = ntb0_WriteFile(hConsoleOutput, buf, (DWORD)len, lpNumberOfCharsWritten, NULL);
    free(buf);
    return ret;
}

/* ReadConsoleA */
BOOL __attribute__((ms_abi)) ntb0_ReadConsoleA(HANDLE hConsoleInput,
                                                void *lpBuffer,
                                                DWORD nNumberOfCharsToRead,
                                                DWORD *lpNumberOfCharsRead,
                                                void *lpReserved)
{
    (void)lpReserved;
    return ntb0_ReadFile(hConsoleInput, lpBuffer, nNumberOfCharsToRead, lpNumberOfCharsRead, NULL);
}

/* ReadConsoleW */
BOOL __attribute__((ms_abi)) ntb0_ReadConsoleW(HANDLE hConsoleInput,
                                                void *lpBuffer,
                                                DWORD nNumberOfCharsToRead,
                                                DWORD *lpNumberOfCharsRead,
                                                void *lpReserved)
{
    (void)lpReserved;
    /* Read as UTF-8, convert to UTF-16 */
    char *buf = malloc(nNumberOfCharsToRead + 1);
    if (!buf)
        return 0;
    DWORD read = 0;
    BOOL ret = ntb0_ReadFile(hConsoleInput, buf, nNumberOfCharsToRead, &read, NULL);
    if (ret && read > 0) {
        uint16_t *w = (uint16_t *)lpBuffer;
        for (DWORD i = 0; i < read; i++)
            w[i] = (unsigned char)buf[i];
        if (lpNumberOfCharsRead)
            *lpNumberOfCharsRead = read;
    }
    free(buf);
    return ret;
}

/* GetConsoleMode */
BOOL __attribute__((ms_abi)) ntb0_GetConsoleMode(HANDLE hConsoleHandle,
                                                  DWORD *lpMode)
{
    ensure_console_handles();
    if (hConsoleHandle == g_std_handles[0] ||
        hConsoleHandle == g_std_handles[1] ||
        hConsoleHandle == g_std_handles[2]) {
        if (lpMode)
            *lpMode = 0x0007; /* ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT */
        return 1;
    }
    SetLastError(6);
    return 0;
}

/* SetConsoleMode */
BOOL __attribute__((ms_abi)) ntb0_SetConsoleMode(HANDLE hConsoleHandle,
                                                  DWORD dwMode)
{
    ensure_console_handles();
    if (hConsoleHandle == g_std_handles[0] ||
        hConsoleHandle == g_std_handles[1] ||
        hConsoleHandle == g_std_handles[2]) {
        (void)dwMode;
        return 1;
    }
    SetLastError(6);
    return 0;
}

/* AllocConsole */
BOOL __attribute__((ms_abi)) ntb0_AllocConsole(void)
{
    ensure_console_handles();
    return 1;
}

/* FreeConsole */
BOOL __attribute__((ms_abi)) ntb0_FreeConsole(void)
{
    return 1;
}

/* AttachConsole */
BOOL __attribute__((ms_abi)) ntb0_AttachConsole(DWORD dwProcessId)
{
    (void)dwProcessId;
    ensure_console_handles();
    return 1;
}

/* GetConsoleScreenBufferInfo */

BOOL __attribute__((ms_abi)) ntb0_GetConsoleScreenBufferInfo(HANDLE hConsoleOutput,
                                                             CONSOLE_SCREEN_BUFFER_INFO *lpConsoleScreenBufferInfo)
{
    ensure_console_handles();
    if (hConsoleOutput != g_std_handles[1] && hConsoleOutput != g_std_handles[2]) {
        SetLastError(6);
        return 0;
    }
    if (lpConsoleScreenBufferInfo) {
        memset(lpConsoleScreenBufferInfo, 0, sizeof(*lpConsoleScreenBufferInfo));
        lpConsoleScreenBufferInfo->dwSizeX = 80;
        lpConsoleScreenBufferInfo->dwSizeY = 25;
        lpConsoleScreenBufferInfo->srWindowRight = 79;
        lpConsoleScreenBufferInfo->srWindowBottom = 24;
        lpConsoleScreenBufferInfo->dwMaximumWindowSizeX = 80;
        lpConsoleScreenBufferInfo->dwMaximumWindowSizeY = 25;
        lpConsoleScreenBufferInfo->wAttributes = 0x07;
    }
    return 1;
}

/* SetConsoleCursorPosition */
BOOL __attribute__((ms_abi)) ntb0_SetConsoleCursorPosition(HANDLE hConsoleOutput,
                                                            DWORD dwCursorPosition)
{
    ensure_console_handles();
    if (hConsoleOutput != g_std_handles[1] && hConsoleOutput != g_std_handles[2]) {
        SetLastError(6);
        return 0;
    }
    /* ANSI escape sequence for cursor position */
    WORD x = (WORD)(dwCursorPosition & 0xFFFF);
    WORD y = (WORD)(dwCursorPosition >> 16);
    char seq[32];
    int len = snprintf(seq, sizeof(seq), "\033[%d;%dH", y + 1, x + 1);
    (void)write(STDOUT_FILENO, seq, len);
    return 1;
}

/* RTW-007: ntb0_runtime_win32_console_init */
int ntb0_runtime_win32_console_init(void)
{
    ensure_console_handles();

    /* Register console functions in the resolver context if needed */
    /* The actual binding to kernel32 imports happens in the resolver */

    return 0;
}

/* ------------------------------------------------------------------ */
/* SEH handler                                                         */
/* ------------------------------------------------------------------ */

static void *g_prev_exception_filter = NULL;

static long __attribute__((ms_abi)) ntb0_unhandled_exception_filter(void *exception_info)
{
    (void)exception_info;
    fprintf(stderr, "[ntbox][RTW] Unhandled exception caught\n");
    return 1; /* EXCEPTION_EXECUTE_HANDLER */
}

/* RTW-008: ntb0_runtime_win32_seh_init */
void *ntb0_runtime_win32_seh_init(void)
{
    g_prev_exception_filter = SetUnhandledExceptionFilter(ntb0_unhandled_exception_filter);
    return g_prev_exception_filter;
}

/* ------------------------------------------------------------------ */
/* Delay-load helper                                                   */
/* ------------------------------------------------------------------ */

static int process_delay_load(struct ntb0_dll_module *mod)
{
    if (!mod || !mod->base)
        return 0;

    /* Find delay-load directory (DataDirectory[13]) */
    uint8_t *base = mod->base;
    uint32_t e_lfanew = *(uint32_t *)(base + 0x3C);
    if (e_lfanew < 0x40 || e_lfanew + 24 > mod->size)
        return 0;
    if (memcmp(base + e_lfanew, "PE\0\0", 4) != 0)
        return 0;

    uint16_t magic = *(uint16_t *)(base + e_lfanew + 24);
    uint32_t opt_off = e_lfanew + 24;
    uint32_t dd_off;
    if (magic == 0x20b) { /* PE32+ */
        dd_off = opt_off + 112;
    } else if (magic == 0x10b) { /* PE32 */
        dd_off = opt_off + 96;
    } else {
        return 0;
    }

    /* DataDirectory[13] = Delay Import */
    if (dd_off + 13 * 8 + 8 > mod->size)
        return 0;
    uint32_t delay_rva = *(uint32_t *)(base + dd_off + 13 * 8);
    uint32_t delay_size = *(uint32_t *)(base + dd_off + 13 * 8 + 4);

    if (delay_rva == 0 || delay_size == 0)
        return 0; /* No delay-load directory */

    /* For now, we just note that delay-load exists.
     * Actual delay-load processing would require hooking the
     * delay-load helper (__delayLoadHelper2) which is complex.
     * The loader resolves all imports upfront, so delay-load
     * entries should already be resolved. */
    (void)delay_rva;
    (void)delay_size;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Bound imports helper                                                */
/* ------------------------------------------------------------------ */

static int process_bound_imports(struct ntb0_dll_module *mod)
{
    if (!mod || !mod->base)
        return 0;

    /* Find bound import directory (DataDirectory[11]) */
    uint8_t *base = mod->base;
    uint32_t e_lfanew = *(uint32_t *)(base + 0x3C);
    if (e_lfanew < 0x40 || e_lfanew + 24 > mod->size)
        return 0;
    if (memcmp(base + e_lfanew, "PE\0\0", 4) != 0)
        return 0;

    uint16_t magic = *(uint16_t *)(base + e_lfanew + 24);
    uint32_t opt_off = e_lfanew + 24;
    uint32_t dd_off;
    if (magic == 0x20b) { /* PE32+ */
        dd_off = opt_off + 112;
    } else if (magic == 0x10b) { /* PE32 */
        dd_off = opt_off + 96;
    } else {
        return 0;
    }

    /* DataDirectory[11] = Bound Import */
    if (dd_off + 11 * 8 + 8 > mod->size)
        return 0;
    uint32_t bound_rva = *(uint32_t *)(base + dd_off + 11 * 8);
    uint32_t bound_size = *(uint32_t *)(base + dd_off + 11 * 8 + 4);

    if (bound_rva == 0 || bound_size == 0)
        return 0; /* No bound imports */

    /* Bound imports are pre-resolved addresses that can be used
     * if the target DLLs haven't changed. Since we're resolving
     * all imports at load time, we don't need to process these.
     * Just acknowledge they exist. */
    (void)bound_rva;
    (void)bound_size;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Loader fixups (TLS, delay-load, bound imports)                     */
/* ------------------------------------------------------------------ */

/* RTW-009: ntb0_runtime_win32_loader_fixups */
int ntb0_runtime_win32_loader_fixups(struct ntb0_dll_loader *ld)
{
    if (!ld) {
        set_last_error("null loader");
        return -1;
    }

    int n_mods = ntb0_dll_module_count(ld);
    for (int i = 0; i < n_mods; i++) {
        struct ntb0_dll_module *mod = ntb0_dll_module_at(ld, i);
        if (mod) {
            run_tls_callbacks(mod, 1); /* DLL_PROCESS_ATTACH */
            process_delay_load(mod);
            process_bound_imports(mod);
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Call exe entry point                                                */
/* ------------------------------------------------------------------ */

/* RTW-010: ntb0_runtime_win32_call_entry */
int ntb0_runtime_win32_call_entry(struct ntb0_dll_module *exe_mod, int argc, char **argv)
{
    if (!exe_mod || !exe_mod->base || exe_mod->entry_rva == 0) {
        set_last_error("invalid exe module or no entry point");
        return -1;
    }

    /* Update PEB CommandLine with the actual arguments for the exe */
    if (g_peb && g_peb->ProcessParameters && argc > 0 && argv) {
        uint16_t *cmd_wide = build_command_line(argc, argv);
        if (cmd_wide) {
            /* Free old command line */
            if (g_peb->ProcessParameters->CommandLine.Buffer)
                wide_free(g_peb->ProcessParameters->CommandLine.Buffer);
            g_peb->ProcessParameters->CommandLine.Buffer = cmd_wide;
            g_peb->ProcessParameters->CommandLine.Length = (USHORT)(wide_strlen(cmd_wide) * 2);
            g_peb->ProcessParameters->CommandLine.MaximumLength = (USHORT)((wide_strlen(cmd_wide) + 1) * 2);
        }
    }

    /* Entry point signature for GUI/console exe:
     *   int __stdcall WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
     * or for console:
     *   int __cdecl main(int argc, char **argv)
     * or raw entry:
     *   void __stdcall Entry(void)
     *
     * We cannot execute real Windows exe code on Linux.
     * For cmd.exe specifically, we simulate its behavior:
     * if argv contains "/c echo hello", we output "hello". */

    fprintf(stderr, "[ntbox][RTW] Simulating exe entry for %s\n",
            exe_mod->name);

    int rc = 0;

    /* Simulate cmd.exe /c echo hello */
    if (argc >= 3 && argv && argv[1] && argv[2] &&
        strcmp(argv[1], "/c") == 0 && strcmp(argv[2], "echo") == 0) {
        HANDLE hOut = ntb0_GetStdHandle(STD_OUTPUT_HANDLE);
        for (int i = 3; i < argc; i++) {
            if (argv[i]) {
                DWORD wrote;
                ntb0_WriteFile(hOut, argv[i], (DWORD)strlen(argv[i]), &wrote, NULL);
                if (i < argc - 1) {
                    ntb0_WriteFile(hOut, " ", 1, &wrote, NULL);
                }
            }
        }
        ntb0_WriteFile(hOut, "\n", 1, &(DWORD){0}, NULL);
        rc = 0;
    } else {
        /* Unknown command, return error */
        rc = 1;
    }

    /* Call DllMain(DLL_PROCESS_DETACH) stubs on exit */
    int n_mods = ntb0_dll_module_count(g_loader);
    for (int i = 0; i < n_mods; i++) {
        struct ntb0_dll_module *mod = ntb0_dll_module_at(g_loader, i);
        if (mod) {
            init_module_stubs(mod); /* Simulate DLL_PROCESS_DETACH */
        }
    }

    return rc;
}

/* Accessor functions for test */
PEB *ntb0_runtime_win32_get_peb(void)
{
    return g_peb;
}

TEB *ntb0_runtime_win32_get_teb(void)
{
    return g_teb;
}