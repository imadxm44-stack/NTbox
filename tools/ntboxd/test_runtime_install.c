#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdarg.h>

#include "hive.h"
#include "ntb0_trace.h"
#include "runtime_install.h"

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
    size_t count = 0;
    const ntb0_runtime_t *list;
    int rc;

    printf("=== NT-Box runtime_install self-test (W5/W6) ===\n");

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

    /* RT-001: list runtimes (>= 10) */
    list = ntb0_runtime_list(&count);
    check(list != NULL && count >= 10, "RT-001 list",
          "count=%zu (need >=10)", count);

    /* clean slate */
    ntb0_runtime_uninstall(root, "test");

    /* RT-002: install a small test binary (/bin/true as fake installer) */
    rc = ntb0_runtime_install(root, "test", "/bin/true");
    check(rc == 0, "RT-002 install", "rc=%d", rc);
    check(ntb0_runtime_is_installed(root, "test") == 1, "RT-002 is_installed",
          "installed=%d", ntb0_runtime_is_installed(root, "test"));

    /* RT-003: run */
    rc = ntb0_runtime_run(root, "test");
    check(rc != -1, "RT-003 run", "rc=%d", rc);

    /* RT-004: uninstall + verify removed */
    rc = ntb0_runtime_uninstall(root, "test");
    check(rc == 0, "RT-004 uninstall", "rc=%d", rc);
    check(ntb0_runtime_is_installed(root, "test") == 0, "RT-004 removed",
          "installed=%d", ntb0_runtime_is_installed(root, "test"));

    /* verify directory removed */
    {
        char tdir[4096];
        snprintf(tdir, sizeof(tdir), "%s/ntbox/runtimes/test", root);
        struct stat st;
        check(stat(tdir, &st) != 0, "RT-004 dir removed", "stat=%d", (int)stat(tdir, &st));
    }

    printf("\n=== RESULT: %d failure(s) ===\n", failures);
    return failures == 0 ? 0 : 1;
}