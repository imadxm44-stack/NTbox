/*
 * test_svc_files.c — T-165 (امتداد): تشغيل خدمات **حقيقية** ملفاتها
 * موجودة تحت win7root، بمساراتها من خلية SYSTEM الحقيقية عبر scm.c.
 *
 * المُقاس:
 *   1) جرد كل خدمات SCM: كم ImagePath يُحلّ إلى ملف موجود فعلاً
 *      (نفس محمّل الخلية)، منها كم PE32+ (يقبلها ntboxd).
 *   2) بدء مجموعة خدمات حقيقية (حتى 6) في الصندوق عبر ntboxd:
 *      spawn ⇒ SCM RUNNING ⇒ مُلاحَظ حي ⇒ خروج ⇒ reap ⇒ SCM STOPPED.
 *   3) رفض صحيح لخدمة PE32 (ntboxd يرفض "not PE32+").
 *
 * المصابيح (stderr): SVC-002 (spawn حقيقي) · SVC-003 (خروج/رفض مُلاحَظ)
 *                    · SVC-006..SVC-010 (حسب svc_real).
 *
 * يخرج 0 فقط إذا نجح كل شيء.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

#include "hive.h"
#include "ntb0_trace.h"
#include "scm.h"
#include "svc_run.h"
#include "svc_real.h"

static int failures = 0;

static void pass(const char *what, const char *fmt, ...)
{
    va_list ap;

    printf("  PASS %s: ", what);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static void fail(const char *what, const char *fmt, ...)
{
    va_list ap;

    printf("  FAIL %s: ", what);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    failures++;
}

static void info(const char *fmt, ...)
{
    va_list ap;

    printf("  ---  ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static void ensure_root(void)
{
    if (!getenv("NTBOX_ROOT")) {
        const char *cands[] = { "./win7root", "../win7root",
                                "/home/imadix/Downloads/Niga/win7root", NULL };
        int i;

        for (i = 0; cands[i]; i++) {
            struct stat st;

            if (stat(cands[i], &st) == 0 && S_ISDIR(st.st_mode)) {
                setenv("NTBOX_ROOT", cands[i], 1);
                break;
            }
        }
    }
}

int main(int argc, char **argv)
{
    ntb0_scm *scm;
    char buf[1024];
    const char *path = NULL;
    ntb0_svc_scan_stats st;
    char chosen[8][NTB0_SCM_MAX_NAME];
    char bins[8][4096];
    int nchosen = 0;
    const int maxc = 6;
    int i, count;
    char pe32svc[NTB0_SCM_MAX_NAME] = "";

    ensure_root();
    printf("=== NT-Box svc_files self-test (W7 / T-165) ===\n");
    printf("root : %s\n", getenv("NTBOX_ROOT"));

    if (argc > 1)
        path = argv[1];

    if (path) {
        scm = ntb0_scm_open(path);
    } else {
        if (ntb0_hive_config_path("SYSTEM", buf, sizeof(buf)) < 0) {
            printf("  FAIL env: SYSTEM hive not found (NTBOX_ROOT=%s)\n",
                   getenv("NTBOX_ROOT") ? getenv("NTBOX_ROOT") : "(unset)");
            return 2;
        }
        scm = ntb0_scm_open(buf);
    }
    if (!scm) {
        printf("  FAIL env: cannot open SCM\n");
        return 2;
    }
    ntb0_svc_real_set_scm(scm);

    /* ---- الفحص 1: جرد الملفات الحقيقية من الريجستري ---- */
    {
        memset(&st, 0, sizeof(st));
        if (ntb0_svc_real_scan(scm, &st) != 0) {
            fail("scan", "ntb0_svc_real_scan failed");
            return 1;
        }
        printf("\nscan : total=%d with_image=%d exists=%d pe32plus=%d pe32=%d "
               "kernel=%d missing=%d no_image=%d\n",
               st.total, st.with_image, st.img_exists, st.img_pe32plus,
               st.img_pe32, st.img_kernel, st.img_missing, st.no_image);

        if (st.total >= 100)
            pass("scan-total", "SCM loaded: %d services from the real SYSTEM hive", st.total);
        else
            fail("scan-total", "expected >=100 services, got %d", st.total);

        if (st.img_exists > 0)
            pass("scan-exists", "%d service ImagePath(s) resolve to real files on disk",
                 st.img_exists);
        else
            fail("scan-exists", "no service binary found under win7root");

        if (st.img_pe32plus > 0)
            pass("scan-pe32plus", "%d service binary(ies) are PE32+ (runs in ntboxd)",
                 st.img_pe32plus);
        else
            fail("scan-pe32plus", "no PE32+ service binary found");
    }

    /* ---- اختيار خدمات حقيقية قابلة للتشغيل ---- */
    count = ntb0_scm_count(scm);
    /* ALG أولاً (الأنسب تاريخياً)، ثم أي WIN32_OWN بملف PE32+ وتبعيات مُستوفاة */
    for (i = 0; i < count && nchosen < maxc; i++) {
        ntb0_svc *s = ntb0_scm_at(scm, i);
        char bin[4096];
        int k, dup = 0;
        int j;

        if (!s || !s->image[0])
            continue;
        if (strcasecmp(s->name, "ALG") != 0 && nchosen == 0)
            continue;                       /* لا نبدأ إلا بـ ALG */
        if (s->type & (NTB0_SVC_TYPE_KERNEL_DRIVER | NTB0_SVC_TYPE_FILE_SYSTEM))
            continue;
        if (s->type & NTB0_SVC_TYPE_WIN32_SHARE)
            continue;                       /* مستضاف svchost (dll) — T-166 */
        if (s->start == NTB0_SVC_START_DISABLED)
            continue;
        /* تبعيات داخل SCM غير مشغّلة ⇒ سيُرفض بدءها (-4) — نختار بغيرها */
        for (j = 0; j < s->ndeps; j++) {
            ntb0_svc *d = ntb0_scm_find(scm, s->deps[j]);

            if (d && d->state != NTB0_SVC_RUNNING)
                break;
        }
        if (j != s->ndeps)
            continue;
        if (ntb0_svc_run_resolve_image(s->image, bin, sizeof(bin)) != 0)
            continue;
        if (ntb0_svc_real_probe_pe(bin) != 1)
            continue;
        for (k = 0; k < nchosen; k++) {
            if (strcasecmp(chosen[k], s->name) == 0)
                dup = 1;
        }
        if (dup)
            continue;
        snprintf(chosen[nchosen], NTB0_SCM_MAX_NAME, "%s", s->name);
        snprintf(bins[nchosen], sizeof(bins[nchosen]), "%s", bin);
        nchosen++;
    }

    if (nchosen == 0) {
        fail("choose", "no runnable real service found (ALG + peers)");
        return 1;
    }
    printf("\nchosen (%d):", nchosen);
    for (i = 0; i < nchosen; i++)
        printf(" %s", chosen[i]);
    printf("\n\n");

    /* ---- الفحص 2: بدء/خصائص الحياة/إيقاف لكل خدمة حقيقية ---- */
    for (i = 0; i < nchosen; i++) {
        const char *name = chosen[i];
        int rc = ntb0_svc_real_start(scm, name);
        pid_t p;
        int seen_alive = 0;
        int waited_us = 0;

        if (rc != NTB0_SVC_REAL_OK) {
            fail("svc-start", "start('%s') = %d (expected 0)", name, rc);
            continue;
        }
        pass("svc-start", "real service '%s' spawned in-box (image='%s')", name, bins[i]);

        p = ntb0_svc_real_get_pid(name);
        if (p > 0)
            pass("svc-pid", "pid('%s') = %d (>0, tracked)", name, (int)p);
        else
            fail("svc-pid", "pid('%s') = %d", name, (int)p);

        {
            int state = ntb0_scm_state_of(scm, name);

            if (state == NTB0_SVC_RUNNING)
                pass("svc-state-running", "SCM state of '%s' == RUNNING", name);
            else
                fail("svc-state-running", "SCM state of '%s' == %d (%s)",
                     name, state, ntb0_scm_state_name(state));
        }

        /* مراقبة الحياة حتى الخروج (الحد 5 ثوانٍ) */
        while (waited_us < 5000000) {
            if (ntb0_svc_real_is_alive(name)) {
                seen_alive = 1;
                break;
            }
            usleep(20000);
            waited_us += 20000;
        }
        if (seen_alive) {
            pass("svc-alive", "service '%s' observed alive while its real code "
                 "executed in-box", name);
        } else {
            fail("svc-alive", "service '%s' never observed alive", name);
        }

        /* انتظار الإنهاء + reap */
        {
            int reap_wait = 0;

            while (ntb0_svc_real_is_alive(name) && reap_wait < 5000000) {
                usleep(50000);
                reap_wait += 50000;
            }
            /* خروج الابن مُلاحَظ: reap_dead يختم الحالة ويصدُر SVC-003 */
            ntb0_svc_real_reap_dead(scm);
            usleep(100000);
            ntb0_svc_real_reap_dead(scm);
        }

        {
            int state = ntb0_scm_state_of(scm, name);

            if (state == NTB0_SVC_STOPPED)
                pass("svc-reap", "SCM state of '%s' == STOPPED after exit "
                     "(lifecycle tracked)", name);
            else
                fail("svc-reap", "SCM state of '%s' == %d (%s), expected STOPPED",
                     name, state, ntb0_scm_state_name(state));
        }

        /* نظافة الحالة قبل الخدمة التالية */
        ntb0_svc_real_stop(name);
        usleep(100000);
    }

    /* ---- الفحص 3: رفض PE32 (بوّة ntboxd الحقيقية) ---- */
    if (st.img_pe32 > 0) {
        for (i = 0; i < count && !pe32svc[0]; i++) {
            ntb0_svc *s = ntb0_scm_at(scm, i);
            char bin[4096];

            if (!s || !s->image[0] ||
                (s->type & (NTB0_SVC_TYPE_KERNEL_DRIVER | NTB0_SVC_TYPE_FILE_SYSTEM)))
                continue;
            if (s->start == NTB0_SVC_START_DISABLED)
                continue;
            if (ntb0_svc_run_resolve_image(s->image, bin, sizeof(bin)) != 0)
                continue;
            if (ntb0_svc_real_probe_pe(bin) == 0)
                snprintf(pe32svc, sizeof(pe32svc), "%s", s->name);
        }
        if (pe32svc[0]) {
            int rc = ntb0_svc_real_start(scm, pe32svc);

            if (rc == NTB0_SVC_REAL_ERR_NOTPE32P)
                pass("svc-refuse-pe32", "PE32 service '%s' cleanly refused "
                     "(NTB0_SVC_REAL_ERR_NOTPE32P) — real ntboxd gate", pe32svc);
            else
                fail("svc-refuse-pe32", "PE32 service '%s': rc=%d (expected NOTPE32P)",
                     pe32svc, rc);
        } else {
            info("no PE32 service found to test the refusal gate");
        }
    }

    /*
     * ملاحظة أمانة: خدمات ويندوز الحقيقية (alg.exe وغيرها) تبلغ نقطة
     * الدخول داخل الصندوق (~300-600ms) ثم تنهار (SIGSEGV) لغياب
     * kernel32/ntdll الحيّة (بوّة T-120، خارج مُلكية هذه الملفات).
     * ما يُقاس هنا: مسار الريجستري ⇒ ملف حقيقي ⇒ القفز إلى النقطة داخل
     * الصندوق ⇒ تتبع دورة الحياة كاملة في SCM (RUNNING→STOPPED).
     */

    printf("\n=== Lamps (stderr): SVC-002 real spawn | SVC-003 exit/refusal observed "
           "| SVC-006..SVC-010 lifecycle ===\n");

    printf("\nfailures: %d\n", failures);
    ntb0_scm_close(scm);
    return failures ? 1 : 0;
}
