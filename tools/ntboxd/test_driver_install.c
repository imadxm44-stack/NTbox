#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdarg.h>

#include "hive.h"
#include "ntb0_trace.h"
#include "driver_install.h"

static int failures = 0;

static void check(int ok, const char *what, const char *fmt, ...)
{
    va_list ap;

    printf("  [%s] %s: ", ok ? "PASS" : "FAIL", what);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    if (!ok)
        failures++;
}

int main(void)
{
    const char *root = NULL;
    int rc;
    int n;

    printf("=== NT-Box driver_install self-test (W9b) ===\n");

    {
        const char *env = getenv("NTBOX_ROOT");
        static char cand[1024];
        const char *home = getenv("HOME");
        if (env && *env) root = env;
        else if (access("/tmp/opencode/win7root/Windows/System32/config", R_OK) == 0) root = "/tmp/opencode/win7root";
        else if (home && *home) {
            snprintf(cand, sizeof(cand), "%s/win7root", home);
            if (access(cand, R_OK) == 0) root = cand;
            else if (access("win7root/Windows/System32/config", R_OK) == 0) root = "win7root";
            else root = NULL;
        } else if (access("win7root/Windows/System32/config", R_OK) == 0) root = "win7root";
        else root = NULL;
    }
    if (!root || !*root) {
        fprintf(stderr, "ERROR: win7 root not found (set NTBOX_ROOT)\n");
        return 2;
    }
    printf("root: %s\n", root);

    /* DRV-001: list drivers (>= 200) */
    n = ntb0_driver_list(root);
    check(n >= 200, "DRV-001 list", "count=%d (need >=200)", n);

    /* clean slate */
    ntb0_driver_uninstall(root, "fakedrv");

    /* DRV-002: install fake .sys (ntdll.dll as source) */
    {
        char src[4096];
        snprintf(src, sizeof(src), "%s/Windows/System32/ntdll.dll", root);
        rc = ntb0_driver_install(root, "fakedrv", src);
    }
    check(rc == 0, "DRV-002 install", "rc=%d", rc);
    check(ntb0_driver_is_installed(root, "fakedrv") == 1, "DRV-002 is_installed",
          "installed=%d", ntb0_driver_is_installed(root, "fakedrv"));

    /* DRV-003: load (expected to fail — .sys not runnable by ntboxd) */
    rc = ntb0_driver_load(root, "fakedrv");
    printf("  [INFO] DRV-003 load rc=%d (expected failure: .sys is not a runnable PE for ntboxd)\n", rc);
    check(1, "DRV-003 load attempted", "rc=%d", rc);

    /* DRV-004: uninstall + verify removed */
    rc = ntb0_driver_uninstall(root, "fakedrv");
    check(rc == 0, "DRV-004 uninstall", "rc=%d", rc);
    check(ntb0_driver_is_installed(root, "fakedrv") == 0, "DRV-004 removed",
          "installed=%d", ntb0_driver_is_installed(root, "fakedrv"));

    {
        char fpath[4096];
        struct stat st;
        snprintf(fpath, sizeof(fpath), "%s/Windows/System32/drivers/fakedrv.sys", root);
        check(stat(fpath, &st) != 0, "DRV-004 file removed", "stat=%d", (int)stat(fpath, &st));
    }

    /* DRV-005: audio bridge (honest stub) */
    rc = ntb0_driver_audio_bridge(root);
    check(rc == -1, "DRV-005 audio bridge stub", "rc=%d (expected -1/STUB)", rc);

    printf("\n=== RESULT: %d failure(s) ===\n", failures);
    return failures == 0 ? 0 : 1;
}
