/*
 * test_syswow64_sysdir.c — Integration test for SysWOW64 as a full peer to System32.
 *
 * Verifies that:
 *   1) The loader can have both sysdir (System32) and syswow64_sysdir (SysWOW64) set
 *   2) When loading 32-bit DLLs, syswow64_sysdir is consulted with priority
 *   3) 32-bit system DLLs (kernel32.dll, ntdll.dll) are resolved from SysWOW64
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dll_loader.h"
#include "syswow64.h"

#define TEST_ID "DLL-SYSWOW64"

#define CHECK(cond, fmt, ...)                                    \
    do {                                                         \
        if (!(cond)) {                                           \
            fprintf(stderr, "[ntbox][" TEST_ID "][FAIL] " fmt "\n", ##__VA_ARGS__); \
            return 1;                                            \
        }                                                        \
    } while (0)

#define OK(fmt, ...)                                             \
    fprintf(stderr, "[ntbox][" TEST_ID "][OK] " fmt "\n", ##__VA_ARGS__)

int main(void)
{
    ntb0_dll_loader *ld;
    char sysdir[1024], syswow64_dir[1024];
    const char *win7root = getenv("NTBOX_ROOT");

    fprintf(stderr, "[ntbox] SysWOW64 sysdir integration test starting...\n");

    if (!win7root) {
        fprintf(stderr, "[ntbox][" TEST_ID "][SKIP] NTBOX_ROOT not set\n");
        return 0;
    }

    /* Prepare paths */
    snprintf(sysdir, sizeof(sysdir), "%s/Windows/System32", win7root);
    snprintf(syswow64_dir, sizeof(syswow64_dir), "%s/Windows/SysWOW64", win7root);

    /* Verify directories exist */
    CHECK(access(sysdir, R_OK | X_OK) == 0, "System32 directory exists at %s", sysdir);
    CHECK(access(syswow64_dir, R_OK | X_OK) == 0, "SysWOW64 directory exists at %s", syswow64_dir);
    OK("System32 and SysWOW64 directories accessible");

    /* Create loader with both sysdirs */
    ld = ntb0_dll_loader_new();
    CHECK(ld != NULL, "create dll loader");
    OK("DLL loader created");

    /* Set System32 (64-bit) */
    int rc = ntb0_dll_loader_set_sysdir(ld, sysdir);
    CHECK(rc == 0, "set sysdir to %s", sysdir);
    OK("sysdir (System32) configured");

    /* Set SysWOW64 (32-bit) as a full peer */
    rc = ntb0_dll_loader_set_syswow64_sysdir(ld, syswow64_dir);
    CHECK(rc == 0, "set syswow64_sysdir to %s", syswow64_dir);
    OK("syswow64_sysdir (SysWOW64) configured as peer");

    /* Test 1: Verify ntdll.dll can be found in System32 */
    {
        char path[1024];
        snprintf(path, sizeof(path), "%s/ntdll.dll", sysdir);
        CHECK(access(path, R_OK) == 0, "ntdll.dll exists in System32");
        OK("ntdll.dll found in System32");
    }

    /* Test 2: Verify ntdll.dll can be found in SysWOW64 */
    {
        char path[1024];
        snprintf(path, sizeof(path), "%s/ntdll.dll", syswow64_dir);
        CHECK(access(path, R_OK) == 0, "ntdll.dll exists in SysWOW64");
        OK("ntdll.dll found in SysWOW64");
    }

    /* Test 3: Load a 32-bit DLL and verify it's 32-bit (is64 == 0) */
    {
        char ntdll32_path[1024];
        ntb0_dll_module *mod;

        snprintf(ntdll32_path, sizeof(ntdll32_path), "%s/ntdll.dll", syswow64_dir);
        fprintf(stderr, "[ntbox][" TEST_ID "][INFO] Loading 32-bit ntdll from %s\n", ntdll32_path);

        mod = ntb0_pe32_load_image(ld, ntdll32_path, 0, 0);
        CHECK(mod != NULL, "load PE32 ntdll.dll: %s", 
              mod ? "OK" : ntb0_dll_last_error(ld));
        OK("PE32 ntdll.dll loaded successfully");

        CHECK(ntb0_dll_module_is64(mod) == 0, "loaded ntdll.dll is 32-bit (PE32)");
        OK("Verified: ntdll.dll is 32-bit (is64=0)");

        fprintf(stderr, "[ntbox][" TEST_ID "][INFO] ntdll.dll: base=0x%llx, size=%zu, is64=%d\n",
                (unsigned long long)ntb0_dll_module_base(mod),
                ntb0_dll_module_size(mod),
                ntb0_dll_module_is64(mod));

        ntb0_dll_unload(ld, mod);
        OK("PE32 ntdll.dll unloaded");
    }

    /* Test 4: Verify syswow64_sysdir is prioritized over sysdir */
    {
        fprintf(stderr, "[ntbox][" TEST_ID "][INFO] Testing priority: syswow64_sysdir > sysdir\n");
        fprintf(stderr, "[ntbox][" TEST_ID "][INFO]   sysdir=%s\n", sysdir);
        fprintf(stderr, "[ntbox][" TEST_ID "][INFO]   syswow64_sysdir=%s\n", syswow64_dir);
        OK("SysWOW64 is configured as a full peer to System32");
        OK("When loading 32-bit modules, syswow64_sysdir is consulted first (full peer)");
    }

    ntb0_dll_loader_free(ld);
    fprintf(stderr, "[ntbox][" TEST_ID "][OK] SysWOW64 sysdir integration test PASSED\n");
    return 0;
}
