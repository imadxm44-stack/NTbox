/*
 * test_runtime_win32.c — اختبار بيئة تشغيل Win32 (RTW-001..RTW-010).
 *
 * يحمّل kernel32.dll، يهيئ بيئة Win32، ويتحقق من واجهات الكونسول.
 *
 * المصابيح: RTW-001..RTW-010
 *   RTW-001: ntb0_runtime_win32_init — PEB/TEB/DllMain/TLS
 *   RTW-002: ntb0_runtime_win32_console_init — واجهات الكونسول
 *   RTW-003: ntb0_runtime_win32_seh_init — معالج SEH
 *   RTW-004: ntb0_runtime_win32_loader_fixups — TLS/delay-load/bound imports
 *   RTW-005: تحميل kernel32.dll بنجاح
 *   RTW-006: تحميل ntdll.dll بنجاح
 *   RTW-007: تحميل cmd.exe بنجاح
 *   RTW-008: واجهات الكونسول تعمل (GetStdHandle, WriteFile, WriteConsoleA, etc.)
 *   RTW-009: التقاط stdout يعمل
 *   RTW-010: جميع الاختبارات تمر
 *
 * البناء:
 *   gcc -O2 -Wall -Wextra -Itools/ntboxreg -Itools/ntboxd \
 *       -Itools/ntboxd/syscalls -o /tmp/opencode/test_runtime_win32 \
 *       tools/ntboxd/test_runtime_win32.c tools/ntboxd/runtime_win32.c \
 *       tools/ntboxd/dll_loader.c tools/ntboxd/apiset.c \
 *       tools/ntboxd/peb.c tools/ntboxd/ntboxd_libc.c \
 *       tools/ntboxreg/hive.c tools/ntboxd/pe_export.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <strings.h>

#include "dll_loader.h"
#include "apiset.h"
#include "runtime_win32.h"
#include "peb.h"
#include "ntboxd_libc.h"

#define DEFAULT_ROOT "/home/imadix/Downloads/Niga/win7root"

static int g_fail = 0;

#define CHECK(cond, fmt, ...)                                                 \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "[ntbox][RTW][FAIL] " fmt "\n", ##__VA_ARGS__);   \
            g_fail = 1;                                                       \
        } else {                                                              \
            fprintf(stderr, "[ntbox][RTW][OK] " fmt "\n", ##__VA_ARGS__);     \
        }                                                                     \
    } while (0)

/* case-insensitive string compare */
static int ci_eq(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = *a++;
        int cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return 0;
    }
    return *a == '\0' && *b == '\0';
}

/* ------------------------------------------------------------------ */
/* محلِّل الاستيراد: يربط أسماء دوال Win32 بتنفيذاتنا               */
/* ------------------------------------------------------------------ */
struct resolve_ctx {
    unsigned calls;
};

static uint64_t stub_addr(const char *dll, const char *func)
{
    uint64_t h = 1469598103934665603ULL;
    while (dll && *dll) {
        h ^= (unsigned char)*dll++;
        h *= 1099511628211ULL;
    }
    h ^= 0xff;
    while (func && *func) {
        h ^= (unsigned char)*func++;
        h *= 1099511628211ULL;
    }
    h &= 0xffffffULL;
    return 0x00007f0000000000ULL + h * 16;
}

static int resolve_fn(void *ctx, const char *dll, const char *func, uint64_t *out_addr)
{
    struct resolve_ctx *c = (struct resolve_ctx *)ctx;
    c->calls++;

    /* دوال الكونسول التي ننفذها في runtime_win32.c */
    if (ci_eq(func, "GetStdHandle") ||
        ci_eq(func, "WriteFile") ||
        ci_eq(func, "ReadFile") ||
        ci_eq(func, "WriteConsoleA") ||
        ci_eq(func, "WriteConsoleW") ||
        ci_eq(func, "ReadConsoleA") ||
        ci_eq(func, "ReadConsoleW") ||
        ci_eq(func, "GetConsoleMode") ||
        ci_eq(func, "SetConsoleMode") ||
        ci_eq(func, "AllocConsole") ||
        ci_eq(func, "FreeConsole") ||
        ci_eq(func, "AttachConsole") ||
        ci_eq(func, "GetConsoleScreenBufferInfo") ||
        ci_eq(func, "SetConsoleCursorPosition")) {
        *out_addr = stub_addr(dll, func);
        return 0;
    }

    /* دوال PEB/TEB/Heap/Virtual/SEH الموجودة في peb.c */
    if (ci_eq(func, "TlsAlloc") || ci_eq(func, "TlsFree") ||
        ci_eq(func, "TlsGetValue") || ci_eq(func, "TlsSetValue") ||
        ci_eq(func, "InitializeCriticalSection") ||
        ci_eq(func, "DeleteCriticalSection") ||
        ci_eq(func, "EnterCriticalSection") ||
        ci_eq(func, "LeaveCriticalSection") ||
        ci_eq(func, "TryEnterCriticalSection") ||
        ci_eq(func, "InitializeSRWLock") ||
        ci_eq(func, "AcquireSRWLockExclusive") ||
        ci_eq(func, "ReleaseSRWLockExclusive") ||
        ci_eq(func, "AcquireSRWLockShared") ||
        ci_eq(func, "ReleaseSRWLockShared") ||
        ci_eq(func, "GetProcessHeap") ||
        ci_eq(func, "HeapCreate") || ci_eq(func, "HeapDestroy") ||
        ci_eq(func, "RtlAllocateHeap") || ci_eq(func, "RtlFreeHeap") ||
        ci_eq(func, "RtlSizeHeap") ||
        ci_eq(func, "VirtualAlloc") || ci_eq(func, "VirtualProtect") ||
        ci_eq(func, "VirtualFree") ||
        ci_eq(func, "SetUnhandledExceptionFilter") ||
        ci_eq(func, "GetUnhandledExceptionFilter") ||
        ci_eq(func, "IsDebuggerPresent") ||
        ci_eq(func, "SetLastError") || ci_eq(func, "GetLastError") ||
        ci_eq(func, "NtSetInformationThread") ||
        ci_eq(func, "CreateThreadWithTeb") ||
        ci_eq(func, "ExitThread") || ci_eq(func, "ExitProcess") ||
        ci_eq(func, "WaitForSingleObject")) {
        *out_addr = stub_addr(dll, func);
        return 0;
    }

    /* دوال C runtime من ntboxd_libc.c */
    void *libc_fn = ntboxd_libc_lookup(func);
    if (libc_fn) {
        *out_addr = (uint64_t)(uintptr_t)libc_fn;
        return 0;
    }

    /* باقي الدوال: stub عام */
    *out_addr = stub_addr(dll, func);
    return 0;
}

/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    const char *root = getenv("NTBOX_ROOT");
    if (!root || !*root)
        root = DEFAULT_ROOT;

    char sysdir[1024];
    snprintf(sysdir, sizeof(sysdir), "%s/Windows/System32", root);

    fprintf(stderr, "NTBOX_ROOT: %s\n", root);

    /* --- Create loader --- */
    struct resolve_ctx rctx = {0};
    ntb0_dll_loader *ld = ntb0_dll_loader_new();
    CHECK(ld != NULL, "RTW-005: dll_loader_new() succeeded");

    ntb0_dll_loader_set_resolver(ld, resolve_fn, &rctx);
    ntb0_dll_loader_set_sysdir(ld, sysdir);

    /* --- Setup api-set --- */
    ntb0_apiset *as = ntb0_apiset_new();
    CHECK(as != NULL, "RTW-005: apiset_new() succeeded");
    ntb0_apiset_set_sysdir(as, sysdir);
    ntb0_dll_loader_set_apiset(ld, as);

    /* --- RTW-005: Load kernel32.dll --- */
    struct ntb0_dll_module *k32 = ntb0_dll_load_name(ld, "kernel32.dll", 0);
    CHECK(k32 != NULL, "RTW-005: kernel32.dll loaded at 0x%llx",
          (unsigned long long)ntb0_dll_module_base(k32));
    CHECK(ntb0_dll_module_imports_resolved(k32) > 0,
          "RTW-005: kernel32 imports resolved (%u)",
          ntb0_dll_module_imports_resolved(k32));
    fprintf(stderr, "  kernel32: base=0x%llx, size=%zu, imports=%u resolved, %u unresolved\n",
            (unsigned long long)ntb0_dll_module_base(k32),
            ntb0_dll_module_size(k32),
            ntb0_dll_module_imports_resolved(k32),
            ntb0_dll_module_imports_unresolved(k32));

    /* --- RTW-006: Load ntdll.dll --- */
    struct ntb0_dll_module *ntdll = ntb0_dll_load_name(ld, "ntdll.dll", 0);
    CHECK(ntdll != NULL, "RTW-006: ntdll.dll loaded at 0x%llx",
          (unsigned long long)ntb0_dll_module_base(ntdll));
    fprintf(stderr, "  ntdll: base=0x%llx, size=%zu, imports=%u resolved\n",
            (unsigned long long)ntb0_dll_module_base(ntdll),
            ntb0_dll_module_size(ntdll),
            ntb0_dll_module_imports_resolved(ntdll));

    /* --- RTW-001: Initialize Win32 runtime (PEB/TEB/DllMain/TLS) --- */
    int rt = ntb0_runtime_win32_init(ld, 1, (char*[]){"cmd.exe"}, NULL);
    CHECK(rt == 0, "RTW-001: ntb0_runtime_win32_init() = %d", rt);

    PEB *peb = ntb0_runtime_win32_get_peb();
    TEB *teb = ntb0_runtime_win32_get_teb();

    CHECK(peb != NULL, "RTW-001: PEB created");
    CHECK(teb != NULL, "RTW-001: TEB created");
    CHECK(peb->ProcessParameters != NULL, "RTW-001: ProcessParameters set");
    CHECK(peb->ProcessParameters->CommandLine.Buffer != NULL, "RTW-001: CommandLine set");
    CHECK(peb->ImageBaseAddress != NULL, "RTW-001: ImageBaseAddress set");
    CHECK(peb->Ldr != NULL, "RTW-001: Ldr set");
    CHECK(teb->ProcessEnvironmentBlock == peb, "RTW-001: TEB->PEB linked");
    CHECK(teb->NtTib.Self == (NT_TIB *)teb, "RTW-001: TEB->NtTib.Self set");
    CHECK(teb->ThreadLocalStoragePointer != NULL, "RTW-001: TLS pointer allocated");
    fprintf(stderr, "  PEB: ImageBase=0x%llx, CommandLine=%p\n",
            (unsigned long long)peb->ImageBaseAddress,
            peb->ProcessParameters->CommandLine.Buffer);
    fprintf(stderr, "  TEB: Self=0x%llx, TLS=0x%llx, PID=0x%llx, TID=0x%llx\n",
            (unsigned long long)teb->NtTib.Self,
            (unsigned long long)teb->ThreadLocalStoragePointer,
            (unsigned long long)teb->ClientId.UniqueProcess,
            (unsigned long long)teb->ClientId.UniqueThread);

    /* --- RTW-002: Initialize console APIs --- */
    rt = ntb0_runtime_win32_console_init();
    CHECK(rt == 0, "RTW-002: ntb0_runtime_win32_console_init() = %d", rt);

    /* Test GetStdHandle */
    HANDLE hOut = ntb0_GetStdHandle(STD_OUTPUT_HANDLE);
    CHECK(hOut != INVALID_HANDLE_VALUE, "RTW-002: GetStdHandle(STD_OUTPUT_HANDLE) works");
    HANDLE hErr = ntb0_GetStdHandle(STD_ERROR_HANDLE);
    CHECK(hErr != INVALID_HANDLE_VALUE, "RTW-002: GetStdHandle(STD_ERROR_HANDLE) works");
    HANDLE hIn = ntb0_GetStdHandle(STD_INPUT_HANDLE);
    CHECK(hIn != INVALID_HANDLE_VALUE, "RTW-002: GetStdHandle(STD_INPUT_HANDLE) works");

    /* Test WriteFile */
    DWORD written;
    const char *test_msg = "Hello from WriteFile\n";
    BOOL ok = ntb0_WriteFile(hOut, test_msg, (DWORD)strlen(test_msg), &written, NULL);
    CHECK(ok && written == strlen(test_msg), "RTW-002: WriteFile works (wrote %u bytes)", written);

    /* Test WriteConsoleA */
    ok = ntb0_WriteConsoleA(hOut, "Hello from WriteConsoleA\n", 25, &written, NULL);
    CHECK(ok && written == 25, "RTW-002: WriteConsoleA works");

    /* Test GetConsoleMode / SetConsoleMode */
    DWORD mode;
    ok = ntb0_GetConsoleMode(hOut, &mode);
    CHECK(ok, "RTW-002: GetConsoleMode works (mode=0x%x)", mode);
    ok = ntb0_SetConsoleMode(hOut, mode);
    CHECK(ok, "RTW-002: SetConsoleMode works");

    /* Test GetConsoleScreenBufferInfo */
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    ok = ntb0_GetConsoleScreenBufferInfo(hOut, &csbi);
    CHECK(ok, "RTW-002: GetConsoleScreenBufferInfo works (size=%ux%u)",
          csbi.dwSizeX, csbi.dwSizeY);

    /* Test SetConsoleCursorPosition */
    ok = ntb0_SetConsoleCursorPosition(hOut, (0 << 16) | 0);
    CHECK(ok, "RTW-002: SetConsoleCursorPosition works");

    /* Test AllocConsole/FreeConsole/AttachConsole */
    ok = ntb0_AllocConsole();
    CHECK(ok, "RTW-002: AllocConsole works");
    ok = ntb0_FreeConsole();
    CHECK(ok, "RTW-002: FreeConsole works");
    ok = ntb0_AttachConsole(0);
    CHECK(ok, "RTW-002: AttachConsole works");

    /* Test WriteConsoleW and ReadConsoleW (basic) */
    ok = ntb0_WriteConsoleW(hOut, L"Hello from WriteConsoleW\n", 25, &written, NULL);
    CHECK(ok && written == 25, "RTW-002: WriteConsoleW works");

    /* --- RTW-003: Initialize SEH --- */
    void *prev_filter = ntb0_runtime_win32_seh_init();
    CHECK(prev_filter == NULL || prev_filter != (void*)-1,
          "RTW-003: ntb0_runtime_win32_seh_init() registered filter");

    /* --- RTW-004: Loader fixups --- */
    rt = ntb0_runtime_win32_loader_fixups(ld);
    CHECK(rt == 0, "RTW-004: ntb0_runtime_win32_loader_fixups() = %d", rt);

    /* --- RTW-007: Load cmd.exe --- */
    struct ntb0_dll_module *cmd_exe = ntb0_dll_load_name(ld, "cmd.exe",
                                                          NTB0_DLL_FLAG_AUTO_DEPS);
    CHECK(cmd_exe != NULL, "RTW-007: cmd.exe loaded at 0x%llx",
          (unsigned long long)ntb0_dll_module_base(cmd_exe));
    fprintf(stderr, "  cmd.exe: base=0x%llx, entry_rva=0x%x, size=%zu\n",
            (unsigned long long)ntb0_dll_module_base(cmd_exe),
            ntb0_dll_module_entry_rva(cmd_exe), ntb0_dll_module_size(cmd_exe));

    /* --- RTW-008/009/010: Run cmd.exe with "/c echo hello" and capture stdout --- */
    {
        int pipefd[2];
        CHECK(pipe(pipefd) == 0, "RTW-009: pipe created for stdout capture");

        int saved_stdout = dup(STDOUT_FILENO);
        int saved_stderr = dup(STDERR_FILENO);

        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        /* Run cmd.exe with "/c echo hello" - this simulates cmd.exe behavior */
        char *cmd_argv[] = {"cmd.exe", "/c", "echo", "hello", NULL};
        int rc = ntb0_runtime_win32_call_entry(cmd_exe, 4, cmd_argv);
        CHECK(rc == 0, "RTW-008: cmd.exe simulation exited with code %d", rc);
        fprintf(stderr, "  cmd.exe simulation exited with code %d\n", rc);

        /* Restore stdout/stderr */
        dup2(saved_stdout, STDOUT_FILENO);
        dup2(saved_stderr, STDERR_FILENO);
        close(saved_stdout);
        close(saved_stderr);

        /* Read from pipe */
        char buf[1024];
        ssize_t nread = read(pipefd[0], buf, sizeof(buf) - 1);
        close(pipefd[0]);
        CHECK(nread > 0, "RTW-009: captured stdout (%zd bytes)", nread);
        buf[nread] = '\0';

        fprintf(stderr, "  Captured output:\n%s", buf);

        int found_hello = 0;
        if (strstr(buf, "hello") != NULL) {
            found_hello = 1;
        }
        CHECK(found_hello, "RTW-010: 'hello' found in captured output");
    }

    /* --- Cleanup --- */
    ntb0_apiset_free(as);
    ntb0_dll_loader_free(ld);

    if (g_fail) {
        fprintf(stderr, "\n[ntbox][RTW][FAIL] test_runtime_win32 FAILED\n");
        return 1;
    }

    fprintf(stderr, "\n[ntbox][RTW][OK] All RTW-001..RTW-010 tests passed\n");
    return 0;
}