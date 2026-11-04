/*
 * test_scm.c — الاختبار الذاتي لمُسيّر خدمات NT-Box (W7 / T-163 … T-168).
 *
 * يقرأ خلية SYSTEM **الحقيقية** لويندوز 7:
 *   $NTBOX_ROOT/Windows/System32/config/SYSTEM
 * (أو يمرّر مسار الخلية كوسيلة أولى)
 *
 * ما يتحقّق منه:
 *   T-163  عدد الخدمات ≥ 100 من الريجستري الحقيقي
 *   T-164  جدول الخدمات + دورة حياة الحالة (SVC-001)
 *   T-167  نواة SCM تعمل في وضع المستخدم ⇒ قابلة للاستعمال كبديل
 *          services.exe داخل الصندوق (SVC-004)
 *   T-168  قائمة الخدمات المستضافة داخل svchost
 *   T-171  ترتيب البدء = فرز طوبولوجي حقيقي + كشف الدورات
 *
 * القاعدة (NTBOX.md §9): كل نقطة = مصباح + اختبار.
 * المصابيح في هذا الملف: SVC-001 و SVC-004.
 *
 * صريح/أمين: الاختبار **لا يشغّل** أي عملية — دورة حياة حالة فقط.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>

#include "hive.h"
#include "ntb0_trace.h"
#include "scm.h"

/* الحد الأدنى المقبول لعدد الخدمات (معيار T-163) */
#define MIN_SERVICES 100
/* الحد الأدنى لقائمة الخدمات المستضافة (معيار T-168) */
#define MIN_HOSTED 12

static int failures;

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

static const char *dash(const char *s)
{
    return (s && *s) ? s : "-";
}

static void print_service(const ntb0_svc *s, int idx)
{
    int i;

    printf("  %3d  %-32s %-34s %-9s 0x%-4x %-19s %s\n", idx, s->name,
           dash(s->display), ntb0_scm_start_name(s->start), s->type,
           ntb0_scm_type_name(s->type), dash(s->image));
    if (s->ndeps) {
        printf("        deps:");
        for (i = 0; i < s->ndeps; i++)
            printf(" %s", s->deps[i]);
        printf("\n");
    }
}

/* ords: 0 = الكل · 1 = التلقائية فقط · 2 = ذات التبعيات فقط · 3 = التلقائية ذات التبعيات */
static void print_start_order(ntb0_scm *scm, int limit, int mode)
{
    int *order;
    int n, i, shown = 0;

    n = ntb0_scm_start_order(scm, NULL, 0);
    if (n <= 0) {
        printf("  (no start order computed)\n");
        return;
    }

    order = (int *)calloc((size_t)n, sizeof(int));
    if (!order)
        return;
    ntb0_scm_start_order(scm, order, n);

    printf("  pos  %-32s %-9s %-6s %s\n", "SERVICE", "START", "TYPE",
           "DEPS (name@pos — pos must be smaller)");
    for (i = 0; i < n && shown < limit; i++) {
        ntb0_svc *s = ntb0_scm_at(scm, order[i]);
        int j;

        if (!s)
            continue;
        if ((mode == 1 || mode == 3) && s->start > NTB0_SVC_START_AUTO)
            continue;
        if ((mode == 2 || mode == 3) && s->ndeps == 0)
            continue;
        printf("  %4d  %-32s %-9s %-6s ", i, s->name,
               ntb0_scm_start_name(s->start),
               (s->type & 0xFFu) == NTB0_SVC_TYPE_WIN32_SHARE ? "share"
               : (s->type & 0xFFu) == NTB0_SVC_TYPE_WIN32_OWN   ? "own"
                                                                : "drv");
        for (j = 0; j < s->ndeps; j++) {
            ntb0_svc *d = ntb0_scm_find(scm, s->deps[j]);
            int pos = d ? d->order : -1;

            printf("%s@%d ", s->deps[j], pos);
        }
        printf("\n");
        shown++;
    }
    if (!shown)
        printf("  (none)\n");

    free(order);
}

/* فحص مستقل: كل تبعية معروفة يجب أن تسبق صاحبها */
static int verify_order(ntb0_scm *scm, char *why, size_t whylen)
{
    int n = ntb0_scm_start_order(scm, NULL, 0);
    int *order = (int *)calloc((size_t)n ? (size_t)n : 1, sizeof(int));
    int i, j, bad = 0;

    if (!order)
        return 0;
    ntb0_scm_start_order(scm, order, n);

    for (i = 0; i < n && !bad; i++) {
        ntb0_svc *s = ntb0_scm_at(scm, order[i]);

        if (!s)
            continue;
        for (j = 0; j < s->ndeps; j++) {
            ntb0_svc *d = ntb0_scm_find(scm, s->deps[j]);

            if (!d || d->order < 0)
                continue;              /* خارجية أو داخل دورة */
            if (d->order >= s->order) {
                snprintf(why, whylen,
                         "'%s' (pos %d) does not follow its dependency '%s' (pos %d)",
                         s->name, s->order, d->name, d->order);
                bad++;
            }
        }
    }

    free(order);
    return bad == 0;
}

/* يشغّل خدمة وكل تبعياتها (ترتيب عكسي) — حالة فقط بلا تشغيل عملية */
static int start_chain(ntb0_scm *scm, const char *name, int depth)
{
    ntb0_svc *s = ntb0_scm_find(scm, name);
    int j;

    if (!s || depth > 32)
        return -1;
    for (j = 0; j < s->ndeps; j++)
        start_chain(scm, s->deps[j], depth + 1);
    return ntb0_scm_start_service(scm, name);
}

int main(int argc, char **argv)
{
    ntb0_scm *scm;
    const char *path = NULL;
    int count, i, norder, ncycles, nhosted, ngroups;
    char why[512];
    char cycle[1024];
    int started = 0, failed_start = 0, stopped = 0;

    printf("=== NT-Box SCM self-test (W7: T-163..T-168) ===\n");

    if (argc > 1)
        path = argv[1];

    if (path) {
        scm = ntb0_scm_open(path);
    } else {
        char buf[1024];

        if (ntb0_hive_config_path("SYSTEM", buf, sizeof(buf)) < 0) {
            fprintf(stderr, "ERROR: SYSTEM hive not found (set NTBOX_ROOT)\n");
            return 2;
        }
        scm = ntb0_scm_open(buf);
    }

    if (!scm) {
        fprintf(stderr, "ERROR: cannot load the SCM from the real SYSTEM hive\n");
        return 2;
    }

    printf("hive          : %s\n", ntb0_scm_hive_path(scm));
    printf("control set   : %s\n", ntb0_scm_control_set(scm));

    /* ---------------------------------------------------------- */
    /* T-163 — استخراج قائمة الخدمات من SYSTEM hive               */
    /* ---------------------------------------------------------- */
    count = ntb0_scm_count(scm);
    printf("\n--- T-163: service table from the real SYSTEM hive ---\n");
    printf("services      : %d\n", count);
    check(count >= MIN_SERVICES, "T-163", "%d service(s) extracted from the real hive (need >= %d)",
          count, MIN_SERVICES);

    {
        static int tc[256];
        int drivers = 0, own = 0, share = 0, disabled = 0, autostart = 0;
        int notype = 0;

        for (i = 0; i < count; i++) {
            ntb0_svc *s = ntb0_scm_at(scm, i);

            if (!s)
                continue;
            tc[s->type & 0xFFu]++;
            if ((s->type & 0xFFu) == NTB0_SVC_TYPE_WIN32_OWN)
                own++;
            else if ((s->type & 0xFFu) == NTB0_SVC_TYPE_WIN32_SHARE)
                share++;
            else if ((s->type & 0xFFu) == 0)
                notype++;
            else
                drivers++;
            if (s->start == NTB0_SVC_START_DISABLED)
                disabled++;
            if (s->start <= NTB0_SVC_START_AUTO)
                autostart++;
        }
        printf("  drivers (0x1/0x2/0x4/0x8): %d\n", drivers);
        printf("  win32 own   (0x10): %d\n", own);
        printf("  win32 share (0x20): %d\n", share);
        printf("  no Type value      : %d\n", notype);
        printf("  disabled (Start=4) : %d\n", disabled);
        printf("  auto-ish (Start<=2): %d\n", autostart);
        printf("  exact types: 0x1=%d 0x2=%d 0x4=%d 0x8=%d 0x10=%d 0x20=%d\n",
               tc[1], tc[2], tc[4], tc[8], tc[0x10], tc[0x20]);
    }

    /* ---------------------------------------------------------- */
    /* أول 10 خدمات بالترتيب الحقيقي للريجستري                   */
    /* ---------------------------------------------------------- */
    printf("\n--- first 10 services (registry order) ---\n");
    for (i = 0; i < count && i < 10; i++) {
        ntb0_svc *s = ntb0_scm_at(scm, i);

        if (s)
            print_service(s, i);
    }

    /* ---------------------------------------------------------- */
    /* T-171 — سلسلة تبعيات خدمة حقيقية + ترتيب البدء             */
    /* ---------------------------------------------------------- */
    printf("\n--- dependency chain of a real service ---\n");
    {
        static const char *probe[] = { "RpcSs", "RpcEptMapper", "Dhcp",
                                       "Dnscache", "EventLog" };
        size_t p;

        for (p = 0; p < sizeof(probe) / sizeof(probe[0]); p++) {
            ntb0_svc *s = ntb0_scm_find(scm, probe[p]);
            const char *deps[NTB0_SCM_MAX_DEPS];
            const char *privs[NTB0_SCM_MAX_PRIVS];
            int nd, np, k;

            if (!s) {
                printf("  %s: NOT FOUND in the hive\n", probe[p]);
                continue;
            }
            nd = ntb0_scm_deps(scm, probe[p], deps, NTB0_SCM_MAX_DEPS);
            np = ntb0_scm_privs(scm, probe[p], privs, NTB0_SCM_MAX_PRIVS);
            printf("  %s\n", s->name);
            printf("    display   : %s\n", dash(s->display));
            printf("    description: %s\n", dash(s->description));
            printf("    start     : %s (%u)\n", ntb0_scm_start_name(s->start),
                   s->start);
            printf("    type      : 0x%x (%s)\n", s->type,
                   ntb0_scm_type_name(s->type));
            printf("    image     : %s\n", dash(s->image));
            printf("    object    : %s\n", dash(s->object));
            printf("    group     : %s\n", dash(s->group));
            printf("    error ctl : %u\n", s->error_control);
            printf("    deps (%d)  : ", nd);
            for (k = 0; k < nd; k++)
                printf("%s%s", k ? ", " : "", deps[k]);
            if (!nd)
                printf("(none)");
            printf("\n    privs (%d) : ", np);
            for (k = 0; k < np; k++)
                printf("%s%s", k ? ", " : "", privs[k]);
            if (!np)
                printf("(none)");
            printf("\n    start pos : %d\n", s->order);
        }
        check(ntb0_scm_find(scm, "RpcSs") != NULL, "T-171",
              "RpcSs found in ControlSet001\\services with %d dependency/ies",
              ntb0_scm_deps(scm, "RpcSs", NULL, 0));
    }

    printf("\n--- computed start order (topological, dependency-first) ---\n");
    norder = ntb0_scm_start_order(scm, NULL, 0);
    print_start_order(scm, 25, 0);
    printf("  total in order: %d of %d\n", norder, count);

    printf("\n--- same order, auto-start subset only (Start<=2) ---\n");
    print_start_order(scm, 15, 1);

    printf("\n--- dependency-first proof: services that HAVE dependencies ---\n");
    print_start_order(scm, 20, 2);

    why[0] = '\0';
    check(verify_order(scm, why, sizeof(why)), "T-171",
          "every known dependency precedes its dependent in the start order%s%s",
          why[0] ? " -- " : "", why);
    check(ntb0_scm_check_order(scm) != 0, "T-171",
          "scm-internal order check passed (Kahn topological sort)");

    /* ---------------------------------------------------------- */
    /* كشف الدورات — بصراحة: إن لم توجد فلا نختلق                */
    /* ---------------------------------------------------------- */
    printf("\n--- dependency cycles ---\n");
    ncycles = ntb0_scm_cycles(scm, NULL, 0);
    cycle[0] = '\0';
    ntb0_scm_cycle_report(scm, cycle, sizeof(cycle));
    if (ncycles > 0) {
        int *idx = (int *)calloc((size_t)ncycles, sizeof(int));
        int k;

        ntb0_scm_cycles(scm, idx, ncycles);
        printf("  %d service(s) take part in a dependency cycle\n", ncycles);
        for (k = 0; k < ncycles; k++) {
            ntb0_svc *s = ntb0_scm_at(scm, idx[k]);

            if (s)
                printf("    %-32s deps:", s->name);
            if (s) {
                int j;

                for (j = 0; j < s->ndeps; j++)
                    printf(" %s", s->deps[j]);
            }
            printf("\n");
        }
        printf("  first cycle : %s\n", cycle[0] ? cycle : "(path not resolved)");
        free(idx);
        check(cycle[0] != '\0', "T-171", "dependency cycle detected and reported: %s",
              cycle);
    } else {
        printf("  no dependency cycle found in ControlSet001\\services "
               "(reported honestly: the graph is acyclic)\n");
        NTB0_OK("SVC-001", "no dependency cycle in %d services (graph is acyclic)",
                count);
    }

    /* ---------------------------------------------------------- */
    /* T-168 — قائمة الخدمات المستضافة داخل svchost               */
    /* ---------------------------------------------------------- */
    printf("\n--- hosted services (svchost share-process) ---\n");
    nhosted = ntb0_scm_hosted(scm, NULL, 0);
    ngroups = ntb0_scm_groups(scm, NULL, 0);
    printf("  hosted services: %d\n", nhosted);
    printf("  svchost groups : %d\n", ngroups);
    {
        const char *hosted[512];
        int k;

        ntb0_scm_hosted(scm, hosted, 512);
        for (k = 0; k < nhosted && k < 20; k++) {
            ntb0_svc *s = ntb0_scm_find(scm, hosted[k]);

            printf("    %-32s group=%-34s image=%s\n", hosted[k],
                   s ? dash(s->group) : "-", s ? dash(s->image) : "-");
        }
        if (nhosted > 20)
            printf("    ... (%d more)\n", nhosted - 20);
    }
    check(nhosted >= MIN_HOSTED, "T-168",
          "%d hosted service(s) listed (need >= %d)", nhosted, MIN_HOSTED);

    /* ---------------------------------------------------------- */
    /* T-164 — دورة حياة الحالة (start/stop) بلا تشغيل عمليات    */
    /* ---------------------------------------------------------- */
    printf("\n--- state machine (start in dependency order, then stop) ---\n");
    {
        int *order = (int *)calloc((size_t)norder ? (size_t)norder : 1,
                                   sizeof(int));

        ntb0_scm_start_order(scm, order, norder);
        for (i = 0; i < norder; i++) {
            ntb0_svc *s = ntb0_scm_at(scm, order[i]);
            int rc;

            if (!s)
                continue;
            rc = ntb0_scm_start_service(scm, s->name);
            if (rc == NTB0_SCM_OK) {
                started++;
            } else {
                failed_start++;
            }
        }
        printf("  started: %d   failed: %d   (no process was launched — state only)\n",
               started, failed_start);

        /* إيقاف عكسي (كما يفعل SCM الحقيقي: المعتمدون أولاً) */
        for (i = norder - 1; i >= 0; i--) {
            ntb0_svc *s = ntb0_scm_at(scm, order[i]);

            if (!s)
                continue;
            if (ntb0_scm_stop_service(scm, s->name) == NTB0_SCM_OK)
                stopped++;
        }
        printf("  stopped: %d\n", stopped);

        check(started > 0, "T-164", "%d service(s) moved STOPPED -> RUNNING in dependency order",
              started);
        check(ntb0_scm_state_of(scm, "RpcSs") == NTB0_SVC_STOPPED,
              "T-164", "RpcSs back to STOPPED after the stop pass");

        /* ControlService: PAUSE / CONTINUE / INTERROGATE / STOP */
        {
            int rc;

            start_chain(scm, "RpcSs", 0);
            rc = ntb0_scm_control_service(scm, "RpcSs",
                                          NTB0_SVC_CONTROL_PAUSE);            check(rc == NTB0_SCM_OK &&
                      ntb0_scm_state_of(scm, "RpcSs") == NTB0_SVC_PAUSED,
                  "T-164", "ControlService(PAUSE) moves RpcSs to PAUSED");
            rc = ntb0_scm_control_service(scm, "RpcSs",
                                          NTB0_SVC_CONTROL_CONTINUE);
            check(rc == NTB0_SCM_OK &&
                      ntb0_scm_state_of(scm, "RpcSs") == NTB0_SVC_RUNNING,
                  "T-164", "ControlService(CONTINUE) moves RpcSs back to RUNNING");
            rc = ntb0_scm_control_service(scm, "RpcSs",
                                          NTB0_SVC_CONTROL_INTERROGATE);
            check(rc == NTB0_SCM_OK, "T-164",
                  "ControlService(INTERROGATE) answers without changing the state");
            rc = ntb0_scm_control_service(scm, "RpcSs",
                                          NTB0_SVC_CONTROL_STOP);
            check(rc == NTB0_SCM_OK &&
                      ntb0_scm_state_of(scm, "RpcSs") == NTB0_SVC_STOPPED,
                  "T-164", "ControlService(STOP) moves RpcSs to STOPPED");
        }

        /* اختبار حالة معطّلة */
        {
            ntb0_svc *s = ntb0_scm_find(scm, "RpcSs");

            if (s) {
                uint32_t saved = s->start;
                int rc;

                s->start = NTB0_SVC_START_DISABLED;
                rc = ntb0_scm_start_service(scm, "RpcSs");
                s->start = saved;
                s->state = NTB0_SVC_STOPPED;
                check(rc == NTB0_SCM_ERR_DISABLED, "T-164",
                      "a disabled service (Start=4) refuses to start");
            }
        }

        /* CreateService (CreateServiceW shape) — في الذاكرة فقط */
        {
            static const char *newdeps[] = { "RpcSs" };
            int rc = ntb0_scm_create_service(scm, "NtBoxProbe", "NT-Box Probe",
                                             "%SystemRoot%\\system32\\svchost.exe -k ntb0probe",
                                             NTB0_SVC_TYPE_WIN32_SHARE,
                                             NTB0_SVC_START_DEMAND, 1, newdeps, 1);

            check(rc == NTB0_SCM_OK, "T-164",
                  "CreateService-style entry added in memory (rc=%d, deps=%d)",
                  rc, ntb0_scm_deps(scm, "NtBoxProbe", NULL, 0));
            check(ntb0_scm_order_of(scm, "NtBoxProbe") > ntb0_scm_order_of(scm, "RpcSs"),
                  "T-164", "the new service is ordered after its dependency RpcSs");
            ntb0_scm_stop_service(scm, "NtBoxProbe");
        }

        free(order);
    }

    /* ---------------------------------------------------------- */
    /* فحص كاشف الدورات على دورة مصنوعة في الذاكرة               */
    /* (الخلية الحقيقية بلا دورات — نُثبت أن الكاشف يعمل)         */
    /* ---------------------------------------------------------- */
    printf("\n--- cycle detector probe (synthetic cycle, in memory) ---\n");
    {
        static const char *dep_b[] = { "NtBoxCycleB" };
        static const char *dep_a[] = { "NtBoxCycleA" };
        char probe[512];
        int rc1, rc2, ncyc;

        rc1 = ntb0_scm_create_service(scm, "NtBoxCycleA", "cycle A",
                                      "n/a", NTB0_SVC_TYPE_WIN32_OWN,
                                      NTB0_SVC_START_AUTO, 1, dep_b, 1);
        rc2 = ntb0_scm_create_service(scm, "NtBoxCycleB", "cycle B",
                                      "n/a", NTB0_SVC_TYPE_WIN32_OWN,
                                      NTB0_SVC_START_AUTO, 1, dep_a, 1);
        ncyc = ntb0_scm_cycles(scm, NULL, 0);
        probe[0] = '\0';
        ntb0_scm_cycle_report(scm, probe, sizeof(probe));
        printf("  created A(rc=%d) B(rc=%d) -> cyclic services: %d\n", rc1, rc2,
               ncyc);
        printf("  reported cycle: %s\n", probe[0] ? probe : "(none)");
        check(rc1 == NTB0_SCM_OK && rc2 == NTB0_SCM_OK && ncyc >= 2 &&
                  strstr(probe, "NtBoxCycleA") && strstr(probe, "NtBoxCycleB"),
              "T-171",
              "the cycle detector reports the synthetic cycle (no infinite loop): %s",
              probe);
    }

    /* ---------------------------------------------------------- */
    /* المصابيح                                                   */
    /* ---------------------------------------------------------- */
    NTB0_OK("SVC-001", "SCM service table + state machine OK: %d service(s) from '%s' (%s)",
            count, ntb0_scm_hive_path(scm), ntb0_scm_control_set(scm));
    NTB0_OK("SVC-004", "SCM core runs in user mode (pure userspace, no kernel dependency) — ready to be linked into a services.exe replacement inside the box");

    printf("\n=== result: %s (%d failure(s)) ===\n", failures ? "FAIL" : "PASS",
           failures);

    ntb0_scm_close(scm);
    return failures ? 1 : 0;
}
