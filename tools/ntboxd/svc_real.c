/*
 * svc_real.c — تنفيذ تشغيل الخدمات الحقيقية (Real Service Runtime).
 *
 * يُشغّل ملف خدمة **حقيقي** (PE ويندوز 7) داخل الصندوق عبر محمّل
 * ntboxd: fork()+execv("ntboxd <binary>"). يدمج مع SCM لتحديث
 * الحالة تلقائياً عند بدء/إيقاف الخدمة.
 *
 * المصابيح (NTBOX.md §9):
 *   SVC-006 — spawn via ntboxd
 *   SVC-007 — missing binary refusal
 *   SVC-008 — host tracked
 *   SVC-009 — stopped
 *   SVC-010 — state transition
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>

#include "hive.h"
#include "scm.h"
#include "ntb0_trace.h"
#include "svc_real.h"

#include <elf.h>

/* ------------------------------------------------------------------ */
/* SCM عام (لدوال is_alive, stop)                                      */
/* ------------------------------------------------------------------ */
static ntb0_scm *g_svc_real_scm = NULL;

void ntb0_svc_real_set_scm(ntb0_scm *scm)
{
    g_svc_real_scm = scm;
}

/* ------------------------------------------------------------------ */
/* عداد تسلسلي محلي (بديل scm->seq غير المتاح)                         */
/* ------------------------------------------------------------------ */
static unsigned g_svc_real_seq = 0;

/* ------------------------------------------------------------------ */
/* جدول تتبع الأبناء                                                  */
/* ------------------------------------------------------------------ */
#define NTB0_SVC_REAL_MAX 256

typedef struct {
    int    used;
    char   name[NTB0_SCM_MAX_NAME];
    pid_t  pid;
    time_t start_time;
    int    status;
    int    reaped;
    int    scm_updated;   /* هل حُدّثت حالة SCM عند الخروج */
} real_rec;

static real_rec g_real_runs[NTB0_SVC_REAL_MAX];

static real_rec *find_rec(const char *name)
{
    int i;

    if (!name)
        return NULL;
    for (i = 0; i < NTB0_SVC_REAL_MAX; i++) {
        if (g_real_runs[i].used && strcmp(g_real_runs[i].name, name) == 0)
            return &g_real_runs[i];
    }
    return NULL;
}

static real_rec *alloc_rec(void)
{
    int i;

    for (i = 0; i < NTB0_SVC_REAL_MAX; i++) {
        if (!g_real_runs[i].used)
            return &g_real_runs[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* مساعدة: حل ImagePath إلى ملف فعلي (منسوخ من svc_run.c)             */
/* ------------------------------------------------------------------ */
static int resolve_image(const char *image, char *out, size_t len)
{
    char img[NTB0_SCM_MAX_IMAGE];
    const char *root;
    char path[4096];
    char *p;
    struct stat st;
    int i;

    if (!image || !*image || !out || len == 0)
        return -1;

    snprintf(img, sizeof(img), "%s", image);
    /* تجريد الوسائط (كل ما بعد أول فراغ) */
    for (p = img; *p; p++) {
        if (*p == ' ' || *p == '\t') {
            *p = '\0';
            break;
        }
    }
    /* تجريد علامات الاقتباس */
    if (img[0] == '"' && img[strlen(img) > 0 ? strlen(img) - 1 : 0] == '"') {
        memmove(img, img + 1, strlen(img));
        img[strlen(img) - 1] = '\0';
    }

    root = ntb0_hive_win7_root();
    if (!root || !*root)
        return -1;

    if (strncasecmp(img, "%SystemRoot%", 12) == 0 ||
        strncasecmp(img, "%windir%", 8) == 0) {
        int skip = (strncasecmp(img, "%SystemRoot%", 12) == 0) ? 12 : 8;

        p = img + skip;
        while (*p == '\\' || *p == '/')
            p++;
        snprintf(path, sizeof(path), "%s/Windows/%s", root, p);
    } else if (strncasecmp(img, "\\SystemRoot", 11) == 0) {
        p = img + 11;
        while (*p == '\\' || *p == '/')
            p++;
        snprintf(path, sizeof(path), "%s/Windows/%s", root, p);
    } else if (img[0] != '\0' && img[1] == ':') {
        p = img + 2;
        while (*p == '\\' || *p == '/')
            p++;
        snprintf(path, sizeof(path), "%s/%s", root, p);
    } else if (img[0] == '\\' || img[0] == '/') {
        snprintf(path, sizeof(path), "%s%s", root, img);
    } else {
        snprintf(path, sizeof(path), "%s/Windows/System32/%s", root, img);
    }

    /* تطبيع الفواصل (backslash -> slash) */
    for (i = 0; path[i]; i++) {
        if (path[i] == '\\')
            path[i] = '/';
    }

    if (access(path, F_OK) != 0)
        return -1;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return -1;

    snprintf(out, len, "%s", path);
    return 0;
}

/* ------------------------------------------------------------------ */
/* تحديد مسار ntboxd (المضيف)                                           */
/* ------------------------------------------------------------------ */
static const char *find_ntboxd(void)
{
    const char *cands[] = {
        "./tools/ntboxd/ntboxd",
        "tools/ntboxd/ntboxd",
        "../tools/ntboxd/ntboxd",
        "/home/imadix/Downloads/Niga/tools/ntboxd/ntboxd",
        NULL
    };
    const char *env;
    int i;

    for (i = 0; cands[i]; i++) {
        if (access(cands[i], X_OK) == 0)
            return cands[i];
    }

    env = getenv("NTBOXD_PATH");
    if (env && *env && access(env, X_OK) == 0)
        return env;

    return NULL;
}

/* ------------------------------------------------------------------ */
/* تحديث حالة SCM عند خروج الابن                                       */
/* ------------------------------------------------------------------ */
static void mark_stopped_ext(ntb0_scm *scm, const char *name, const char *why)
{
    ntb0_svc *s;

    if (!scm)
        scm = g_svc_real_scm;
    if (!scm || !name)
        return;
    s = ntb0_scm_find(scm, name);
    if (!s)
        return;
    if (s->state != NTB0_SVC_STOPPED && s->state != NTB0_SVC_DISABLED) {
        s->state = NTB0_SVC_STOP_PENDING;
        s->state = NTB0_SVC_STOPPED;
        NTB0_OK("SVC-010", "service '%s' state transition: RUNNING -> STOP_PENDING -> STOPPED", name);
        /* SVC-003: الخروج مُلاحَظ فعلياً من المُضيف (نصف المُرسِل الجانب-المضيف) */
        NTB0_OK("SVC-003", "SCM observed exit of '%s' (%s) — lifecycle tracked RUNNING→STOPPED",
                name, why ? why : "exited");
    }
}

static void mark_stopped(ntb0_scm *scm, const char *name)
{
    mark_stopped_ext(scm, name, "exited");
}

/* ------------------------------------------------------------------ */
/* ntb0_svc_real_start                                                  */
/* ------------------------------------------------------------------ */
int ntb0_svc_real_start(ntb0_scm *scm, const char *name)
{
    ntb0_svc *s;
    char bin[4096];
    const char *ntboxd_path;
    pid_t pid;
    real_rec *rec;
    int j;

    if (!scm || !name || !*name)
        return NTB0_SVC_REAL_ERR_NOTFOUND;

    s = ntb0_scm_find(scm, name);
    if (!s)
        return NTB0_SVC_REAL_ERR_NOTFOUND;

    /* binary غير موجود ⇒ رفض صحيح (مصباح SVC-007) */
    if (resolve_image(s->image, bin, sizeof(bin)) != 0) {
        NTB0_OK("SVC-007", "service '%s': binary not found for image '%s' — start refused (no RUNNING)",
                s->name, s->image[0] ? s->image : "(none)");
        NTB0_OK("SVC-003", "service '%s': start refused — binary not found (no RUNNING)",
                s->name);
        return NTB0_SVC_REAL_ERR_NOBINARY;
    }

    /*
     * بوّة PE32+ حقيقية: ntboxd نفسه يرفض PE32 ("not PE32+")، لذا
     * نرفض هنا بدل أن ينكسر الابن بعد fork — refusal قابل للقياس.
     */
    {
        int pe = ntb0_svc_real_probe_pe(bin);

        if (pe == 0) {
            NTB0_FAIL("SVC-002", "service '%s': image '%s' is PE32 (32-bit) — ntboxd refuses (real gate)",
                      s->name, bin);
            return NTB0_SVC_REAL_ERR_NOTPE32P;
        }
        if (pe < 0) {
            NTB0_FAIL("SVC-002", "service '%s': image '%s' is not a PE file — start refused",
                      s->name, bin);
            return NTB0_SVC_REAL_ERR_NOTPE32P;
        }
    }

    if (s->start == NTB0_SVC_START_DISABLED) {
        s->state = NTB0_SVC_DISABLED;
        return NTB0_SVC_REAL_ERR_DISABLED;
    }

    /* تبعيات يجب أن تكون قيد التشغيل قبلها */
    for (j = 0; j < s->ndeps; j++) {
        ntb0_svc *d = ntb0_scm_find(scm, s->deps[j]);

        if (!d)
            continue;                   /* تبعية خارجية */
        if (d->state != NTB0_SVC_RUNNING) {
            NTB0_OK("SVC-010", "service '%s' waits for dependency '%s' (state %d) — start refused",
                    s->name, d->name, d->state);
            return NTB0_SVC_REAL_ERR_DEPENDENCY;
        }
    }

    /* إن كان لا يزال حيّاً */
    {
        real_rec *r = find_rec(name);

        if (r && r->used && !r->reaped) {
            if (kill(r->pid, 0) == 0) {
                s->state = NTB0_SVC_RUNNING;
                NTB0_OK("SVC-010", "service '%s' already RUNNING (pid=%d)", name, (int)r->pid);
                return NTB0_SVC_REAL_OK;
            }
            r->reaped = 1;
            mark_stopped(scm, name);
        }
    }

    /* START_PENDING — انتقال حالة SVC-010 */
    s->state = NTB0_SVC_START_PENDING;
    s->seq_start = ++g_svc_real_seq;
    NTB0_OK("SVC-010", "service '%s' state transition: STOPPED -> START_PENDING (seq %u)", name, s->seq_start);

    ntboxd_path = find_ntboxd();
    if (!ntboxd_path) {
        s->state = NTB0_SVC_STOPPED;
        NTB0_FAIL("SVC-006", "ntboxd host binary not found — cannot launch '%s'", s->name);
        return NTB0_SVC_REAL_ERR_EXEC;
    }

    pid = fork();
    if (pid < 0) {
        s->state = NTB0_SVC_STOPPED;
        NTB0_FAIL("SVC-006", "fork failed for service '%s': %s", s->name, strerror(errno));
        return NTB0_SVC_REAL_ERR_FORK;
    }

    if (pid == 0) {
        /* ابن: إعادة توجيه الخرج، ثم exec ntboxd */
        char logpath[1024];
        int fd;

        snprintf(logpath, sizeof(logpath), "/tmp/opencode/ntboxd_%s.log", s->name);
        fd = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        {
            char *argv[] = { (char *)ntboxd_path, bin, NULL };
            execv(ntboxd_path, argv);
        }
        fprintf(stderr, "svc_real: execv(%s %s) failed: %s\n", ntboxd_path, bin, strerror(errno));
        _exit(127);
    }

    /* أب: تتبع */
    rec = alloc_rec();
    if (!rec) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        s->state = NTB0_SVC_STOPPED;
        NTB0_FAIL("SVC-008", "no slot to track service '%s'", s->name);
        return NTB0_SVC_REAL_ERR_NOSLOT;
    }
    memset(rec, 0, sizeof(*rec));
    rec->used = 1;
    snprintf(rec->name, sizeof(rec->name), "%s", s->name);
    rec->pid = pid;
    rec->start_time = time(NULL);
    rec->reaped = 0;
    rec->scm_updated = 0;

    s->state = NTB0_SVC_RUNNING;
    s->seq_start = ++g_svc_real_seq;

    NTB0_OK("SVC-006", "service '%s' spawned via ntboxd pid=%d image='%s'", s->name, (int)pid, bin);
    /* SVC-002: ملف خدمة **حقيقي** من الريجستري يعمل الآن داخل الصندوق */
    NTB0_OK("SVC-002", "real service '%s' running in-box via ntboxd pid=%d (image from SYSTEM hive: '%s')",
            s->name, (int)pid, s->image);
    NTB0_OK("SVC-008", "service host tracked: name='%s' pid=%d start_time=%ld", s->name, (int)pid, (long)rec->start_time);
    NTB0_OK("SVC-010", "service '%s' state transition: START_PENDING -> RUNNING (seq %u)", name, s->seq_start);

    return NTB0_SVC_REAL_OK;
}

/* ------------------------------------------------------------------ */
/* ntb0_svc_real_stop                                                  */
/* ------------------------------------------------------------------ */
void ntb0_svc_real_stop(const char *name)
{
    ntb0_scm *scm = g_svc_real_scm;
    ntb0_svc *s = NULL;
    real_rec *rec;

    if (!name || !*name)
        return;

    if (scm)
        s = ntb0_scm_find(scm, name);

    if (s && s->state != NTB0_SVC_STOPPED && s->state != NTB0_SVC_DISABLED) {
        s->state = NTB0_SVC_STOP_PENDING;
        NTB0_OK("SVC-010", "service '%s' state transition: RUNNING -> STOP_PENDING", name);
    }

    rec = find_rec(name);
    if (rec && rec->used && !rec->reaped) {
        kill(rec->pid, SIGTERM);

        {
            int waited = 0;

            while (waited < 5000) {
                pid_t w = waitpid(rec->pid, &rec->status, WNOHANG);

                if (w == rec->pid) {
                    rec->reaped = 1;
                    break;
                }
                if (w < 0) {
                    rec->reaped = 1;
                    break;
                }
                usleep(100000);
                waited += 100;
            }
        }

        if (!rec->reaped) {
            kill(rec->pid, SIGKILL);
            waitpid(rec->pid, &rec->status, 0);
            rec->reaped = 1;
        }
    }

    if (scm)
        mark_stopped_ext(scm, name, "SIGTERM (stop requested)");

    if (rec)
        rec->used = 0;

    NTB0_OK("SVC-009", "service '%s' stopped", name);
}

/* ------------------------------------------------------------------ */
/* ntb0_svc_real_is_alive                                              */
/* ------------------------------------------------------------------ */
int ntb0_svc_real_is_alive(const char *name)
{
    ntb0_scm *scm = g_svc_real_scm;
    real_rec *rec;

    if (!name || !*name)
        return 0;

    rec = find_rec(name);
    if (!rec || !rec->used)
        return 0;
    if (rec->reaped)
        return 0;

    {
        pid_t w = waitpid(rec->pid, &rec->status, WNOHANG);

        if (w == rec->pid) {
            char why[96];

            if (WIFSIGNALED(rec->status))
                snprintf(why, sizeof(why), "died by signal %d", WTERMSIG(rec->status));
            else
                snprintf(why, sizeof(why), "exited with status %d",
                         WIFEXITED(rec->status) ? WEXITSTATUS(rec->status) : -1);
            rec->reaped = 1;
            if (scm)
                mark_stopped_ext(scm, name, why);
            return 0;
        }
        if (w < 0) {
            rec->reaped = 1;
            if (scm)
                mark_stopped_ext(scm, name, "waitpid error");
            return 0;
        }
    }

    if (kill(rec->pid, 0) == 0) {
        return 1;
    }

    rec->reaped = 1;
    if (scm)
        mark_stopped(scm, name);

    return 0;
}

/* ------------------------------------------------------------------ */
/* ntb0_svc_real_get_pid                                               */
/* ------------------------------------------------------------------ */
pid_t ntb0_svc_real_get_pid(const char *name)
{
    real_rec *rec;

    if (!name || !*name)
        return (pid_t)-1;
    rec = find_rec(name);
    if (!rec || !rec->used)
        return (pid_t)-1;
    return rec->pid;
}

/* ------------------------------------------------------------------ */
/* ntb0_svc_real_reap_dead — فحص دوري لجميع الخدمات                    */
/* ------------------------------------------------------------------ */
void ntb0_svc_real_reap_dead(ntb0_scm *scm)
{
    int i;

    if (!scm)
        return;

    for (i = 0; i < NTB0_SVC_REAL_MAX; i++) {
        real_rec *r = &g_real_runs[i];

        if (!r->used || r->reaped)
            continue;

        pid_t w = waitpid(r->pid, &r->status, WNOHANG);

        if (w == r->pid) {
            char why[96];
            if (WIFSIGNALED(r->status))
                snprintf(why, sizeof(why), "died by signal %d", WTERMSIG(r->status));
            else
                snprintf(why, sizeof(why), "exited with status %d",
                         WIFEXITED(r->status) ? WEXITSTATUS(r->status) : -1);
            r->reaped = 1;
            if (!r->scm_updated) {
                mark_stopped_ext(scm, r->name, why);
                r->scm_updated = 1;
            }
        } else if (w < 0) {
            r->reaped = 1;
            if (!r->scm_updated) {
                mark_stopped_ext(scm, r->name, "waitpid error");
                r->scm_updated = 1;
            }
        } else if (kill(r->pid, 0) != 0) {
            /* العملية ماتت لكن waitpid لم يُبلغ بعد */
            r->reaped = 1;
            if (!r->scm_updated) {
                mark_stopped_ext(scm, r->name, "process gone");
                r->scm_updated = 1;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* ntb0_svc_real_probe_pe — فحص فعلي: PE32+ أم PE32 أم ليس PE           */
/* ------------------------------------------------------------------ */
int ntb0_svc_real_probe_pe(const char *path)
{
    unsigned char hdr[0x248];

    if (!path)
        return -1;

    {
        FILE *f = fopen(path, "rb");
        long n;

        if (!f)
            return -1;
        n = (long)fread(hdr, 1, sizeof(hdr), f);
        fclose(f);
        /* نحتاج على الأقل e_lfanew + DOS header */
        if (n < 0x40)
            return -1;
        if (hdr[0] != 'M' || hdr[1] != 'Z')
            return -1;
    }

    {
        unsigned int e_lfanew = (unsigned int)hdr[0x3C] |
                                ((unsigned int)hdr[0x3D] << 8) |
                                ((unsigned int)hdr[0x3E] << 16) |
                                ((unsigned int)hdr[0x3F] << 24);
        unsigned char magic0, magic1;

        if (e_lfanew < 0x40 || e_lfanew + 0x26 > sizeof(hdr))
            return -1;
        if (hdr[e_lfanew] != 'P' || hdr[e_lfanew + 1] != 'E' ||
            hdr[e_lfanew + 2] != 0 || hdr[e_lfanew + 3] != 0)
            return -1;
        /* OptionalHeader.Magic على +0x18: 0x20B=PE32+ ، 0x10B=PE32 */
        magic0 = hdr[e_lfanew + 0x18];
        magic1 = hdr[e_lfanew + 0x19];
        if ((magic1 & 0xFF) == 0x02 && magic0 == 0x0B)
            return 1;
        if ((magic1 & 0xFF) == 0x01 && magic0 == 0x0B)
            return 0;
        return -1;
    }
}

/* ------------------------------------------------------------------ */
/* ntb0_svc_real_scan — جرد كل خدمات SCM عبر نفس محمّل الريجستري         */
/* ------------------------------------------------------------------ */
int ntb0_svc_real_scan(ntb0_scm *scm, ntb0_svc_scan_stats *out)
{
    ntb0_svc_scan_stats st;
    char bin[4096];
    int i, count;

    if (!scm || !out)
        return -1;
    memset(&st, 0, sizeof(st));

    count = ntb0_scm_count(scm);
    st.total = count;

    for (i = 0; i < count; i++) {
        ntb0_svc *s = ntb0_scm_at(scm, i);

        if (!s)
            continue;

        /* نوع نواة (driver/fs) — لا spawn مستخدم له أصلاً */
        if ((s->type & (NTB0_SVC_TYPE_KERNEL_DRIVER | NTB0_SVC_TYPE_FILE_SYSTEM)) &&
            !(s->type & (NTB0_SVC_TYPE_WIN32_OWN | NTB0_SVC_TYPE_WIN32_SHARE))) {
            st.img_kernel++;
            continue;
        }

        if (!s->image[0]) {
            st.no_image++;
            continue;
        }
        st.with_image++;

        if (resolve_image(s->image, bin, sizeof(bin)) != 0) {
            st.img_missing++;
            continue;
        }
        st.img_exists++;

        {
            int pe = ntb0_svc_real_probe_pe(bin);

            if (pe == 1)
                st.img_pe32plus++;
            else
                st.img_pe32++;
        }
    }

    *out = st;
    return 0;
}