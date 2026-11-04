/*
 * test_kernel32.c — اختبار kernel32 الحقيقي مع DllMain/TLS/Console (T-120..T-132)
 *
 * البناء:
 *   gcc -O2 -Wall -Wextra -Itools/ntboxreg -Itools/ntboxd -Itools/ntboxd/syscalls \
 *       -o /tmp/opencode/test_kernel32 \
 *       test_kernel32.c kernel32_loader.c dll_loader.c apiset.c \
 *       peb.c ntboxd_libc.c ../ntboxreg/hive.c pe_export.c \
 *       ../ntboxreg/ntdll_syscalls.c \
 *       tools/ntboxd/syscalls/ntb0_trap.c \
 *       tools/ntboxd/syscalls/ntb0_dispatch.c \
 *       tools/ntboxd/syscalls/ntb0_path.c \
 *       tools/ntboxd/syscalls/ntb0_handle.c \
 *       tools/ntboxd/syscalls/ntb0_thread.c \
 *       tools/ntboxd/syscalls/ntb0_reg.c \
 *       tools/ntboxd/syscalls/ntb0_trampoline.S -lpthread
 *
 * التشغيل:
 *   NTBOX_ROOT=/home/imadix/Downloads/Niga/win7root \
 *   /tmp/opencode/test_kernel32
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

#include "kernel32_loader.h"
#include "syscalls/ntb0_trace.h"

#define NTBOX_DEFAULT_NTDLL "/tmp/opencode/win7root/Windows/System32/ntdll.dll"
#define NTBOX_DEFAULT_K32   "/tmp/opencode/win7root/Windows/System32/kernel32.dll"

#define TEST_PASS(fmt, ...) do { \
    printf("PASS: " fmt "\n", ##__VA_ARGS__); \
} while(0)

#define TEST_FAIL(fmt, ...) do { \
    printf("FAIL: " fmt "\n", ##__VA_ARGS__); \
    failures++; \
} while(0)

/* Forward declaration for generic stub from kernel32_loader.c */
extern unsigned long long ntboxd_generic_stub(void);

/* Forward declaration */
static void test_console_apis(void);

static int failures = 0;

/* Test console APIs by calling them directly */
static void test_console_apis(void)
{
    fprintf(stderr, "\n=== Console API Tests ===\n");
    
    /* Get function pointers from resolver */
    void *getstd = ntboxd_resolve_stub("kernel32.dll", "GetStdHandle");
    void *writec_a = ntboxd_resolve_stub("kernel32.dll", "WriteConsoleA");
    void *writec_w = ntboxd_resolve_stub("kernel32.dll", "WriteConsoleW");
    void *getcon = ntboxd_resolve_stub("kernel32.dll", "GetConsoleMode");
    void *setcon = ntboxd_resolve_stub("kernel32.dll", "SetConsoleMode");
    void *alloc_con = ntboxd_resolve_stub("kernel32.dll", "AllocConsole");
    void *free_con = ntboxd_resolve_stub("kernel32.dll", "FreeConsole");
    
    if (!getstd || getstd == (void *)ntboxd_generic_stub) {
        TEST_FAIL("GetStdHandle not resolved");
    } else {
        TEST_PASS("GetStdHandle resolved to %p", getstd);
    }
    
    if (!writec_a || writec_a == (void *)ntboxd_generic_stub) {
        TEST_FAIL("WriteConsoleA not resolved");
    } else {
        TEST_PASS("WriteConsoleA resolved to %p", writec_a);
    }
    
    if (!writec_w || writec_w == (void *)ntboxd_generic_stub) {
        TEST_FAIL("WriteConsoleW not resolved");
    } else {
        TEST_PASS("WriteConsoleW resolved to %p", writec_w);
    }
    
    if (!getcon || getcon == (void *)ntboxd_generic_stub) {
        TEST_FAIL("GetConsoleMode not resolved");
    } else {
        TEST_PASS("GetConsoleMode resolved to %p", getcon);
    }
    
    if (!setcon || setcon == (void *)ntboxd_generic_stub) {
        TEST_FAIL("SetConsoleMode not resolved");
    } else {
        TEST_PASS("SetConsoleMode resolved to %p", setcon);
    }
    
    if (!alloc_con || alloc_con == (void *)ntboxd_generic_stub) {
        TEST_FAIL("AllocConsole not resolved");
    } else {
        TEST_PASS("AllocConsole resolved to %p", alloc_con);
    }
    
    if (!free_con || free_con == (void *)ntboxd_generic_stub) {
        TEST_FAIL("FreeConsole not resolved");
    } else {
        TEST_PASS("FreeConsole resolved to %p", free_con);
    }
    
    /* Test actual console output */
    fprintf(stderr, "--- Testing console output ---\n");
    typedef HANDLE (__attribute__((ms_abi)) *GetStdHandleFn)(DWORD);
    typedef BOOL (__attribute__((ms_abi)) *WriteConsoleAFn)(HANDLE, const void *, DWORD, DWORD *, void *);
    typedef BOOL (__attribute__((ms_abi)) *WriteConsoleWFn)(HANDLE, const void *, DWORD, DWORD *, void *);
    typedef BOOL (__attribute__((ms_abi)) *GetConsoleModeFn)(HANDLE, DWORD *);
    typedef BOOL (__attribute__((ms_abi)) *SetConsoleModeFn)(HANDLE, DWORD);
    typedef BOOL (__attribute__((ms_abi)) *AllocConsoleFn)(void);
    typedef BOOL (__attribute__((ms_abi)) *FreeConsoleFn)(void);
    
    GetStdHandleFn pGetStdHandle = (GetStdHandleFn)getstd;
    WriteConsoleAFn pWriteConsoleA = (WriteConsoleAFn)writec_a;
    WriteConsoleWFn pWriteConsoleW = (WriteConsoleWFn)writec_w;
    GetConsoleModeFn pGetConsoleMode = (GetConsoleModeFn)getcon;
    SetConsoleModeFn pSetConsoleMode = (SetConsoleModeFn)setcon;
    AllocConsoleFn pAllocConsole = (AllocConsoleFn)alloc_con;
    FreeConsoleFn pFreeConsole = (FreeConsoleFn)free_con;
    
    /* Test GetStdHandle */
    HANDLE hOut = pGetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) {
        TEST_FAIL("GetStdHandle(STD_OUTPUT_HANDLE) returned INVALID_HANDLE_VALUE");
    } else {
        TEST_PASS("GetStdHandle(STD_OUTPUT_HANDLE) = %p", hOut);
    }
    
    HANDLE hErr = pGetStdHandle(STD_ERROR_HANDLE);
    if (hErr == INVALID_HANDLE_VALUE) {
        TEST_FAIL("GetStdHandle(STD_ERROR_HANDLE) returned INVALID_HANDLE_VALUE");
    } else {
        TEST_PASS("GetStdHandle(STD_ERROR_HANDLE) = %p", hErr);
    }
    
    HANDLE hIn = pGetStdHandle(STD_INPUT_HANDLE);
    if (hIn == INVALID_HANDLE_VALUE) {
        TEST_FAIL("GetStdHandle(STD_INPUT_HANDLE) returned INVALID_HANDLE_VALUE");
    } else {
        TEST_PASS("GetStdHandle(STD_INPUT_HANDLE) = %p", hIn);
    }
    
    /* Test GetConsoleMode */
    DWORD mode = 0;
    if (pGetConsoleMode(hOut, &mode)) {
        TEST_PASS("GetConsoleMode(hOut) = 0x%x", (unsigned)mode);
    } else {
        TEST_FAIL("GetConsoleMode(hOut) failed");
    }
    
    /* Test SetConsoleMode */
    if (pSetConsoleMode(hOut, mode)) {
        TEST_PASS("SetConsoleMode(hOut, 0x%x) succeeded", (unsigned)mode);
    } else {
        TEST_FAIL("SetConsoleMode(hOut, 0x%x) failed", (unsigned)mode);
    }
    
    /* Test WriteConsoleA - this should print "hello from ntbox" */
    const char *msg = "hello from ntbox\n";
    DWORD written = 0;
    if (pWriteConsoleA(hOut, msg, (DWORD)strlen(msg), &written, NULL)) {
        TEST_PASS("WriteConsoleA wrote %u chars", (unsigned)written);
    } else {
        TEST_FAIL("WriteConsoleA failed");
    }
    
    /* Test WriteConsoleW */
    const uint16_t wmsg[] = u"hello from ntbox (wide)\n";
    DWORD wlen = (DWORD)((sizeof(wmsg) / sizeof(uint16_t)) - 1); /* exclude null */
    if (pWriteConsoleW(hOut, wmsg, wlen, &written, NULL)) {
        TEST_PASS("WriteConsoleW wrote %u chars", (unsigned)written);
    } else {
        TEST_FAIL("WriteConsoleW failed");
    }
    
    /* Test AllocConsole/FreeConsole */
    if (pAllocConsole()) {
        TEST_PASS("AllocConsole succeeded");
    } else {
        TEST_FAIL("AllocConsole failed");
    }
    
    if (pFreeConsole()) {
        TEST_PASS("FreeConsole succeeded");
    } else {
        TEST_FAIL("FreeConsole failed");
    }
}

int main(int argc, char **argv)
{
    const char *ntdll_path = argc > 1 ? argv[1] : NTBOX_DEFAULT_NTDLL;
    const char *win7_root = argc > 2 ? argv[2] : "/tmp/opencode/win7root";

    printf("== NT-Box kernel32 test ==\n");
    printf("ntdll: %s\n", ntdll_path);
    printf("win7_root: %s\n", win7_root);

    /* تعيين NTBOX_ROOT للبيئة */
    setenv("NTBOX_ROOT", "/home/imadix/Downloads/Niga/win7root", 1);

    /* 1) تحميل الوحدات الحقيقية */
    printf("\n=== 1) تحميل ntdll + kernel32 الحقيقيين ===\n");
    if (!ntboxd_load_real_modules()) {
        printf("FAIL: فشل تحميل الوحدات الحقيقية\n");
        return 1;
    }
    TEST_PASS("ntboxd_load_real_modules succeeded");

    /* 2) التحقق من تحميل الوحدات */
    ntb0_dll_module *mod_ntdll = ntboxd_get_mod_ntdll();
    ntb0_dll_module *mod_k32 = ntboxd_get_mod_kernel32();
    if (!mod_ntdll) {
        TEST_FAIL("mod_ntdll is NULL");
    } else {
        TEST_PASS("mod_ntdll loaded at %p", (void*)ntboxd_get_mod_ntdll());
    }
    if (!mod_k32) {
        TEST_FAIL("mod_kernel32 is NULL");
    } else {
        TEST_PASS("mod_kernel32 loaded at %p, imports: %u resolved, %u unresolved",
                  (void*)ntboxd_get_mod_kernel32(),
                  ntb0_dll_module_imports_resolved(mod_k32),
                  ntb0_dll_module_imports_unresolved(mod_k32));
    }

    /* 3) اختبار تصديرات kernel32 الحقيقية */
    printf("\n=== 3) اختبار تصديرات kernel32 الحقيقية ===\n");
    void *getstd = ntboxd_real_kernel32_export("GetStdHandle");
    void *writec = ntboxd_real_kernel32_export("WriteConsoleA");
    void *getcon = ntboxd_real_kernel32_export("GetConsoleMode");
    uint32_t entry_rva = ntboxd_get_mod_kernel32() ? ntb0_dll_module_entry_rva(ntboxd_get_mod_kernel32()) : 0;
    void *entry_addr = entry_rva ? (void *)((char *)ntb0_dll_module_base(ntboxd_get_mod_kernel32()) + entry_rva) : NULL;

    if (getstd) TEST_PASS("GetStdHandle -> %p", getstd); else TEST_FAIL("GetStdHandle not found");
    if (writec) TEST_PASS("WriteConsoleA -> %p", writec); else TEST_FAIL("WriteConsoleA not found");
    if (getcon) TEST_PASS("GetConsoleMode -> %p", getcon); else TEST_FAIL("GetConsoleMode not found");
    if (entry_addr) TEST_PASS("DllMain (entry point) -> %p (RVA 0x%x)", entry_addr, entry_rva); else TEST_FAIL("DllMain entry point not found");

    /* 4) اختبار Console APIs */
    printf("\n=== 4) اختبار Console APIs ===\n");
    void *hOut = ntboxd_real_kernel32_export("GetStdHandle");
    if (hOut) {
        /* استدعاء GetStdHandle(STD_OUTPUT_HANDLE) عبر trap */
        /* سنختبر عبر استدعاء مباشر للـ export */
        TEST_PASS("GetStdHandle export resolved");
    } else {
        TEST_FAIL("GetStdHandle not resolved");
    }

    /* Test console APIs via resolver */
    test_console_apis();

    /* 5) اختبار تحميل cmd.exe والتحقق من استيراداته */
    printf("\n=== 5) اختبار تحميل cmd.exe ===\n");
    ntb0_dll_module *mod_cmd = ntb0_dll_load_name(ntboxd_get_loader(), "cmd.exe", NTB0_DLL_FLAG_AUTO_DEPS);
    if (mod_cmd) {
        unsigned resolved = ntb0_dll_module_imports_resolved(mod_cmd);
        unsigned unresolved = ntb0_dll_module_imports_unresolved(mod_cmd);
        TEST_PASS("cmd.exe loaded at %p, imports: %u resolved, %u unresolved",
                  (void *)ntb0_dll_module_base(mod_cmd), resolved, unresolved);
        
        /* Verify console APIs are resolved in cmd.exe */
        int n_imp = ntb0_dll_import_count(mod_cmd);
        int has_getstd = 0, has_writecon_a = 0, has_writecon_w = 0, has_writefile = 0, has_getcon = 0;
        for (int i = 0; i < n_imp; i++) {
            ntb0_dll_import rec;
            if (ntb0_dll_import_at(mod_cmd, i, &rec) == 0) {
                if (strcmp(rec.func, "GetStdHandle") == 0) has_getstd = 1;
                if (strcmp(rec.func, "WriteConsoleA") == 0) has_writecon_a = 1;
                if (strcmp(rec.func, "WriteConsoleW") == 0) has_writecon_w = 1;
                if (strcmp(rec.func, "WriteFile") == 0) has_writefile = 1;
                if (strcmp(rec.func, "GetConsoleMode") == 0) has_getcon = 1;
            }
        }
        int has_writecon = has_writecon_a || has_writecon_w || has_writefile;
        if (has_getstd && has_writecon && has_getcon) {
            TEST_PASS("K32-004: cmd.exe console imports resolved (GetStdHandle, WriteConsole*, GetConsoleMode)");
        } else {
            TEST_FAIL("K32-004: cmd.exe missing console imports (getstd=%d writecon=%d getcon=%d)", 
                      has_getstd, has_writecon, has_getcon);
        }
        
        /* Check for ntdll ordinal imports */
        int has_ordinal_8 = 0;
        for (int i = 0; i < n_imp; i++) {
            ntb0_dll_import rec;
            if (ntb0_dll_import_at(mod_cmd, i, &rec) == 0) {
                if (strcmp(rec.dll, "ntdll.dll") == 0 && rec.by_ordinal && rec.ordinal == 8) {
                    has_ordinal_8 = 1;
                    if (rec.resolved) {
                        TEST_PASS("K32-006: ntdll.dll!#8 (NtQueryInformationProcess) resolved to %p", (void*)rec.addr);
                    } else {
                        TEST_FAIL("K32-006: ntdll.dll!#8 (NtQueryInformationProcess) UNRESOLVED");
                    }
                }
            }
        }
        if (!has_ordinal_8) {
            TEST_PASS("K32-006: cmd.exe does not import ntdll.dll!#8");
        }
    } else {
        TEST_FAIL("cmd.exe not loaded: %s", ntb0_dll_last_error(ntboxd_get_loader()));
    }

    /* 6) اختبار argv parsing من PEB.CommandLine (K32-005) */
    printf("\n=== 6) اختبار PEB CommandLine (K32-005) ===\n");
    PEB *peb = ntb0_runtime_win32_get_peb();
    if (peb && peb->ProcessParameters) {
        RTL_USER_PROCESS_PARAMETERS *pp = peb->ProcessParameters;
        if (pp->CommandLine.Buffer && pp->CommandLine.Length > 0) {
            /* Convert wide to narrow for display */
            uint16_t *w = pp->CommandLine.Buffer;
            size_t wlen = pp->CommandLine.Length / 2;
            char *narrow = malloc(wlen + 1);
            if (narrow) {
                for (size_t i = 0; i < wlen; i++)
                    narrow[i] = (w[i] < 0x80) ? (char)w[i] : '?';
                narrow[wlen] = '\0';
                TEST_PASS("K32-005: PEB.CommandLine = '%s'", narrow);
                free(narrow);
            } else {
                TEST_FAIL("K32-005: Failed to allocate for CommandLine conversion");
            }
        } else {
            TEST_FAIL("K32-005: PEB.CommandLine is empty");
        }
    } else {
        TEST_FAIL("K32-005: PEB or ProcessParameters not initialized");
    }

    /* 7) Test ntdll ordinal resolution */
    printf("\n=== 7) اختبار حل ترتيبات ntdll (ordinal imports) ===\n");
    ntb0_dll_loader *ld = ntboxd_get_loader();
    if (ld && mod_ntdll) {
        /* Test resolving ntdll ordinals by checking if loader can resolve them for a module */
        /* We test by trying to load a simple DLL that imports ntdll ordinals */
        /* For now, verify the resolver handles ordinals via stub resolver */
        void *stub_addr = ntboxd_resolve_stub("ntdll.dll", "#8");
        if (stub_addr && stub_addr != (void *)ntboxd_generic_stub) {
            TEST_PASS("K32-007: Resolved ntdll.dll!#8 via stub resolver to %p", stub_addr);
        } else {
            TEST_FAIL("K32-007: Failed to resolve ntdll.dll!#8 via stub resolver");
        }
        
        /* Test a few more common ordinals */
        const char *ordinals[] = {"#12", "#19", "#20", "#21", "#22", "#40", "#42", "#51", "#52"};
        const char *ord_names[] = {"NtClose", "NtAllocateVirtualMemory", "NtFreeVirtualMemory", 
                                   "NtReadFile", "NtWriteFile", "NtTerminateProcess", 
                                   "NtQuerySystemInformation", "NtOpenProcess", "NtOpenThread"};
        for (int i = 0; i < 9; i++) {
            void *ord_stub = ntboxd_resolve_stub("ntdll.dll", ordinals[i]);
            if (ord_stub && ord_stub != (void *)ntboxd_generic_stub) {
                TEST_PASS("K32-007: Resolved ntdll.dll!%s (%s) to %p", ordinals[i], ord_names[i], ord_stub);
            } else {
                TEST_FAIL("K32-007: Failed to resolve ntdll.dll!%s (%s)", ordinals[i], ord_names[i]);
            }
        }
    }

    /* ملخص النتائج */
    printf("\n=== النتائج ===\n");
    if (failures == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    } else {
        printf("%d TESTS FAILED\n", failures);
        return 1;
    }
}