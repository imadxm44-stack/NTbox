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
#include "svc_run.h"

/* ------------------------------------------------------------------ */
/* حالة عامة                                                           */
/* ------------------------------------------------------------------ */
static ntb0_scm *g_scm = NULL;

void ntb0_svc_run_set_scm(ntb0_scm *scm)
{
    g_scm = scm;
}

static ntb0_scm *get_scm(void)
{
    if (g_scm)
        return g_scm;
    g_scm = ntb0_scm_open_default();
    return g_scm;
}

/* ------------------------------------------------------------------ */
/* جدول تتبع الأبناء                                                  */
/* ------------------------------------------------------------------ */
#define NTB0_SVC_RUN_MAX 256

typedef struct {
    int    used;
    char   name[NTB0_SCM_MAX_NAME];
    pid_t  pid;
    time_t start_time;
    int    status;
    int    reaped;
} run_rec;

static run_rec g_runs[NTB0_SVC_RUN_MAX];

static run_rec *find_rec(const char *name)
{
    int i;

    if (!name)
        return NULL;
    for (i = 0; i < NTB0_SVC_RUN_MAX; i++) {
        if (g_runs[i].used && strcmp(g_runs[i].name, name) == 0)
            return &g_runs[i];
    }
    return NULL;
}

static run_rec *alloc_rec(void)
{
    int i;

    for (i = 0; i < NTB0_SVC_RUN_MAX; i++) {
        if (!g_runs[i].used)
            return &g_runs[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* مساعدة: حل ImagePath إلى ملف فعلي                                    */
/* ------------------------------------------------------------------ */
int ntb0_svc_run_resolve_image(const char *image, char *out, size_t len)
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
/* تحديث حالة SCM عند خروج الابن                                         */
/* ------------------------------------------------------------------ */
static void mark_stopped(ntb0_scm *scm, const char *name)
{
    ntb0_svc *s;

    if (!scm || !name)
        return;
    s = ntb0_scm_find(scm, name);
    if (!s)
        return;
    if (s->state != NTB0_SVC_STOPPED && s->state != NTB0_SVC_DISABLED) {
        s->state = NTB0_SVC_STOP_PENDING;
        s->state = NTB0_SVC_STOPPED;
    }
}

/* ------------------------------------------------------------------ */
/* ntb0_svc_run_start                                                  */
/* ------------------------------------------------------------------ */
int ntb0_svc_run_start(const char *name)
{
    ntb0_scm *scm;
    ntb0_svc *s;
    char bin[4096];
    const char *ntboxd_path;
    pid_t pid;
    run_rec *rec;
    int j;

    if (!name || !*name)
        return -1;

    /* ضمان NTBOX_ROOT للابن */
    if (!getenv("NTBOX_ROOT")) {
        const char *r = ntb0_hive_win7_root();

        if (r && *r)
            setenv("NTBOX_ROOT", r, 1);
    }

    scm = get_scm();
    if (!scm)
        return -1;

    s = ntb0_scm_find(scm, name);
    if (!s)
        return -1;

    /* binary غير موجود ⇒ رفض صحيح (مصباح OK) */
    if (ntb0_svc_run_resolve_image(s->image, bin, sizeof(bin)) != 0) {
        NTB0_OK("SVC-003",
                "service '%s': binary not found for image '%s' — start refused (no RUNNING)",
                s->name, s->image[0] ? s->image : "(none)");
        return NTB0_SVC_RUN_ERR_NOBINARY;
    }

    if (s->start == NTB0_SVC_START_DISABLED) {
        s->state = NTB0_SVC_DISABLED;
        return -1;
    }

    /* تبعيات */
    for (j = 0; j < s->ndeps; j++) {
        ntb0_svc *d = ntb0_scm_find(scm, s->deps[j]);

        if (!d)
            continue;
        if (d->state != NTB0_SVC_RUNNING)
            return -1;
    }

    /* إن كان لا يزال حيّاً */
    {
        run_rec *r = find_rec(name);

        if (r && r->used && !r->reaped) {
            if (kill(r->pid, 0) == 0) {
                s->state = NTB0_SVC_RUNNING;
                return 0;
            }
            r->reaped = 1;
            mark_stopped(scm, name);
        }
    }

    /* START_PENDING */
    s->state = NTB0_SVC_START_PENDING;

    ntboxd_path = find_ntboxd();
    if (!ntboxd_path) {
        s->state = NTB0_SVC_STOPPED;
        NTB0_FAIL("SVC-002", "ntboxd host binary not found — cannot launch '%s'",
                  s->name);
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        s->state = NTB0_SVC_STOPPED;
        NTB0_FAIL("SVC-002", "fork failed for service '%s': %s", s->name,
                  strerror(errno));
        return -1;
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
        fprintf(stderr, "svc_run: execv(%s) failed: %s\n", ntboxd_path,
                strerror(errno));
        _exit(127);
    }

    /* أب: تتبع */
    rec = alloc_rec();
    if (!rec) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        s->state = NTB0_SVC_STOPPED;
        NTB0_FAIL("SVC-002", "no slot to track service '%s'", s->name);
        return -1;
    }
    memset(rec, 0, sizeof(*rec));
    rec->used = 1;
    snprintf(rec->name, sizeof(rec->name), "%s", s->name);
    rec->pid = pid;
    rec->start_time = time(NULL);
    rec->reaped = 0;

    s->state = NTB0_SVC_RUNNING;

    NTB0_OK("SVC-002", "service '%s' spawned via ntboxd pid=%d image='%s'",
            s->name, (int)pid, bin);
    NTB0_OK("SVC-004", "service host tracked: name='%s' pid=%d start_time=%ld",
            s->name, (int)pid, (long)rec->start_time);

    return 0;
}

/* ------------------------------------------------------------------ */
/* ntb0_svc_run_stop                                                  */
/* ------------------------------------------------------------------ */
void ntb0_svc_run_stop(const char *name)
{
    ntb0_scm *scm = get_scm();
    ntb0_svc *s = NULL;
    run_rec *rec;

    if (!name || !*name)
        return;

    if (scm)
        s = ntb0_scm_find(scm, name);

    if (s && s->state != NTB0_SVC_STOPPED && s->state != NTB0_SVC_DISABLED) {
        s->state = NTB0_SVC_STOP_PENDING;
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
        mark_stopped(scm, name);

    if (rec)
        rec->used = 0;

    NTB0_OK("SVC-005", "service '%s' stopped", name);
}

/* ------------------------------------------------------------------ */
/* ntb0_svc_run_is_alive                                              */
/* ------------------------------------------------------------------ */
int ntb0_svc_run_is_alive(const char *name)
{
    ntb0_scm *scm = get_scm();
    run_rec *rec;

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
            rec->reaped = 1;
            if (scm)
                mark_stopped(scm, name);
            return 0;
        }
        if (w < 0) {
            rec->reaped = 1;
            if (scm)
                mark_stopped(scm, name);
            return 0;
        }
    }

    if (kill(rec->pid, 0) == 0)
        return 1;

    rec->reaped = 1;
    if (scm)
        mark_stopped(scm, name);
    return 0;
}

/* ------------------------------------------------------------------ */
/* ntb0_svc_run_pid                                                  */
/* ------------------------------------------------------------------ */
pid_t ntb0_svc_run_pid(const char *name)
{
    run_rec *rec;

    if (!name || !*name)
        return (pid_t)-1;
    rec = find_rec(name);
    if (!rec || !rec->used)
        return (pid_t)-1;
    return rec->pid;
}