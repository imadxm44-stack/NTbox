#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "hive.h"
#include "ntb0_trace.h"
#include "scm.h"
#include "svc_run.h"

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

int main(int argc, char **argv)
{
    ntb0_scm *scm;
    char buf[1024];
    const char *path = NULL;
    /* ntb0_svc *svc = NULL; */
    char chosen[NTB0_SCM_MAX_NAME];
    int i, count;
    char imgbuf[4096];

    printf("=== NT-Box svc_run self-test (W7 / T-165..T-172) ===\n");

    if (argc > 1)
        path = argv[1];

    if (path) {
        scm = ntb0_scm_open(path);
    } else {
        if (ntb0_hive_config_path("SYSTEM", buf, sizeof(buf)) < 0) {
            fprintf(stderr, "ERROR: SYSTEM hive not found (set NTBOX_ROOT)\n");
            return 2;
        }
        scm = ntb0_scm_open(buf);
    }
    if (!scm) {
        fprintf(stderr, "ERROR: cannot load SCM\n");
        return 2;
    }

    ntb0_svc_run_set_scm(scm);

    /* اختر خدمة whose binary exists */
    count = ntb0_scm_count(scm);
    chosen[0] = '\0';
    for (i = 0; i < count; i++) {
        ntb0_svc *s = ntb0_scm_at(scm, i);

        if (!s)
            continue;
        if (ntb0_svc_run_resolve_image(s->image, imgbuf, sizeof(imgbuf)) == 0) {
            snprintf(chosen, sizeof(chosen), "%s", s->name);
            break;
        }
    }

    if (chosen[0] == '\0') {
        fprintf(stderr, "ERROR: no service with existing binary found\n");
        return 2;
    }
    printf("chosen service : %s (image resolves)\n", chosen);

    /* ---- Test 1: start existing binary ---- */
    {
        int rc = ntb0_svc_run_start(chosen);

        check(rc == 0, "SVC-start", "ntb0_svc_run_start('%s') == 0 (got %d)",
              chosen, rc);

        /* alive immediately (child still loading) */
        /* small delay to let child exec */
        usleep(200000);
        {
            int alive = ntb0_svc_run_is_alive(chosen);
            check(alive == 1, "SVC-alive", "service '%s' is alive after start (got %d)",
                  chosen, alive);
        }

        /* pid */
        {
            pid_t p = ntb0_svc_run_pid(chosen);
            check(p > 0, "SVC-pid", "pid('%s') = %d (>0)", chosen, (int)p);
        }
    }

    /* ---- Test 2: stop ---- */
    {
        ntb0_svc_run_stop(chosen);
        /* ensure reaped */
        usleep(200000);
        {
            int alive = ntb0_svc_run_is_alive(chosen);
            check(alive == 0, "SVC-stopped", "service '%s' is stopped after stop (got %d)",
                  chosen, alive);
        }
        {
            int st = ntb0_scm_state_of(scm, chosen);
            check(st == NTB0_SVC_STOPPED, "SVC-state",
                  "SCM state of '%s' == STOPPED (got %d)", chosen, st);
        }
    }

    /* ---- Test 3: missing binary -> clean refusal ---- */
    {
        const char *missing_name = "NTB0MissingSvc";

        ntb0_scm_create_service(scm, missing_name, "missing", "C:\\Windows\\System32\\definitely_not_there_xyz.exe",
                                NTB0_SVC_TYPE_WIN32_OWN, NTB0_SVC_START_DEMAND,
                                0, NULL, 0);

        int rc = ntb0_svc_run_start(missing_name);
        check(rc == NTB0_SVC_RUN_ERR_NOBINARY, "SVC-nobinary",
              "missing binary returns NTB0_SVC_RUN_ERR_NOBINARY (got %d)", rc);

        int st = ntb0_scm_state_of(scm, missing_name);
        check(st != NTB0_SVC_RUNNING, "SVC-nobinary-state",
              "missing binary does not mark RUNNING (state=%d)", st);
    }

    printf("\nfailures: %d\n", failures);
    ntb0_scm_close(scm);
    return failures ? 1 : 0;
}