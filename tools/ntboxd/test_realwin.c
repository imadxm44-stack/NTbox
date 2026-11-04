/*
 * test_realwin.c — W3 T-068..T-070
 *
 * Runs REAL Windows 7 binaries (notepad.exe, cmd.exe) inside ntboxd and
 * reports honestly how far they get.  No edits to ntboxd or its headers.
 *
 * Build:
 *   gcc -O2 -Wall -Wextra -Itools/ntboxd/syscalls -Itools/ntboxd \
 *       -o /tmp/opencode/test_realwin tools/ntboxd/test_realwin.c
 * Run:
 *   NTBOX_ROOT=/home/imadix/Downloads/Niga/win7root /tmp/opencode/test_realwin
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>

#include "ntb0_trace.h"

#define TIMEOUT_MS 8000

typedef struct {
    char tag[64];
    char bin[64];
    int  entry_reached;
    size_t ntdll_real;
    size_t kernel32_real;
    size_t stdout_bytes;
    int    signaled;
    int    signal_no;
    int    timed_out;
    int    printed;
    char   first_fail[256];
} Result;

static size_t count_substr(const char *buf, const char *needle) {
    size_t c = 0;
    const char *p = buf;
    if (!buf || !needle) return 0;
    while ((p = strstr(p, needle)) != NULL) {
        c++;
        p += strlen(needle);
    }
    return c;
}

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = '\0';
    fclose(f);
    if (out_len) *out_len = rd;
    return buf;
}

static void append_file(const char *src, FILE *dst) {
    size_t len;
    char *buf = read_file(src, &len);
    if (!buf) return;
    if (dst) fwrite(buf, 1, len, dst);
    free(buf);
}

static int run_case(const char *ntboxd_path, const char *winroot,
                    const char *bin, const char **extra, int nextra,
                    Result *res, const char *tag) {
    memset(res, 0, sizeof(*res));
    snprintf(res->tag, sizeof(res->tag), "%s", tag);
    snprintf(res->bin, sizeof(res->bin), "%s", bin);

    char binpath[1024], logpath[1024], outpath[1024], errpath[1024];
    snprintf(binpath, sizeof(binpath), "%s/Windows/System32/%s", winroot, bin);
    snprintf(logpath,  sizeof(logpath),  "/tmp/opencode/realwin_%s.log", tag);
    snprintf(outpath,  sizeof(outpath),  "/tmp/opencode/realwin_%s.out", tag);
    snprintf(errpath,  sizeof(errpath),  "/tmp/opencode/realwin_%s.err", tag);

    int outfd = open(outpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int errfd = open(errpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (outfd < 0 || errfd < 0) {
        if (outfd >= 0) close(outfd);
        if (errfd >= 0) close(errfd);
        snprintf(res->first_fail, sizeof(res->first_fail), "cannot open log files");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(outfd); close(errfd);
        snprintf(res->first_fail, sizeof(res->first_fail), "fork failed");
        return -1;
    }
    if (pid == 0) {
        dup2(outfd, STDOUT_FILENO);
        dup2(errfd, STDERR_FILENO);
        close(outfd);
        close(errfd);
        char *argv[32];
        int ac = 0;
        argv[ac++] = (char *)ntboxd_path;
        argv[ac++] = binpath;
        for (int i = 0; i < nextra && ac < 30; i++)
            argv[ac++] = (char *)extra[i];
        argv[ac] = NULL;
        execv(ntboxd_path, argv);
        perror("execv");
        _exit(127);
    }
    close(outfd);
    close(errfd);

    int status = 0;
    int waited = 0;
    int done = 0;
    while (waited < TIMEOUT_MS) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) { done = 1; break; }
        if (r < 0) { done = 1; break; }
        usleep(100000);
        waited += 100;
    }
    if (!done) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        res->timed_out = 1;
    }

    /* parse stderr log */
    size_t elen = 0;
    char *ebuf = read_file(errpath, &elen);
    if (ebuf) {
        res->entry_reached = (strstr(ebuf, "calling entry") != NULL);
        res->ntdll_real = count_substr(ebuf, "[ntboxd] real ntdll!");
        res->kernel32_real = count_substr(ebuf, "[ntboxd] real kernel32!");

        /* first failure line */
        char *copy = strdup(ebuf);
        if (copy) {
            char *save = NULL;
            char *line = strtok_r(copy, "\n", &save);
            while (line) {
                if (strstr(line, "UNIMPLEMENTED") || strstr(line, "UNRESOLVED") ||
                    strstr(line, "Segmentation") || strstr(line, "core dumped")) {
                    snprintf(res->first_fail, sizeof(res->first_fail), "%.200s", line);
                    break;
                }
                line = strtok_r(NULL, "\n", &save);
            }
            free(copy);
        }

        if (res->first_fail[0] == '\0') {
            if (WIFSIGNALED(status))
                snprintf(res->first_fail, sizeof(res->first_fail),
                         "signal %d", WTERMSIG(status));
            else if (res->timed_out)
                snprintf(res->first_fail, sizeof(res->first_fail), "timeout");
            else
                snprintf(res->first_fail, sizeof(res->first_fail), "none");
        } else if (WIFSIGNALED(status)) {
            size_t l = strlen(res->first_fail);
            if (l + 24 < sizeof(res->first_fail))
                snprintf(res->first_fail + l, sizeof(res->first_fail) - l,
                         "; terminated by signal %d", WTERMSIG(status));
        }
        free(ebuf);
    }

    /* stdout bytes */
    struct stat st;
    if (stat(outpath, &st) == 0)
        res->stdout_bytes = (size_t)st.st_size;

    res->signaled = WIFSIGNALED(status) ? 1 : 0;
    res->signal_no = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    res->printed = (res->stdout_bytes > 0);

    /* combined log file */
    FILE *lg = fopen(logpath, "wb");
    if (lg) {
        append_file(errpath, lg);
        append_file(outpath, lg);
        fclose(lg);
    }

    return 0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    const char *ntboxd_path = "tools/ntboxd/ntboxd";
    if (access(ntboxd_path, X_OK) != 0)
        ntboxd_path = "/home/imadix/Downloads/Niga/tools/ntboxd/ntboxd";
    if (access(ntboxd_path, X_OK) != 0) {
        fprintf(stderr, "[ntbox][REAL-001][FAIL] ntboxd binary not found\n");
        return 1;
    }

    const char *winroot = getenv("NTBOX_ROOT");
    if (!winroot || !*winroot)
        winroot = "/home/imadix/Downloads/Niga/win7root";
    if (access(winroot, R_OK) != 0) {
        fprintf(stderr, "[ntbox][REAL-001][FAIL] win7 root not found: %s\n", winroot);
        return 1;
    }

    /* notepad.exe */
    Result r_np;
    run_case(ntboxd_path, winroot, "notepad.exe", NULL, 0, &r_np, "notepad");

    /* cmd.exe with dir-like invocation (argv) */
    const char *cmd_args[] = { "/c", "dir" };
    Result r_cmd;
    run_case(ntboxd_path, winroot, "cmd.exe", cmd_args, 2, &r_cmd, "cmd");

    int any = 0;

    /* lamp lines */
    if (r_np.entry_reached && r_np.printed) {
        NTB0_OK("REAL-001", "notepad.exe: entry reached; real-ntdll=%zu real-kernel32=%zu; stdout=%zu bytes",
                r_np.ntdll_real, r_np.kernel32_real, r_np.stdout_bytes);
        any = 1;
    } else {
        NTB0_FAIL("REAL-001", "notepad.exe: entry=%d real-ntdll=%zu real-kernel32=%zu stdout=%zu first_failure=%s",
                  r_np.entry_reached, r_np.ntdll_real, r_np.kernel32_real,
                  r_np.stdout_bytes, r_np.first_fail);
    }

    if (r_cmd.entry_reached && r_cmd.printed) {
        NTB0_OK("REAL-001", "cmd.exe: entry reached; real-ntdll=%zu real-kernel32=%zu; stdout=%zu bytes",
                r_cmd.ntdll_real, r_cmd.kernel32_real, r_cmd.stdout_bytes);
        any = 1;
    } else {
        NTB0_FAIL("REAL-001", "cmd.exe: entry=%d real-ntdll=%zu real-kernel32=%zu stdout=%zu first_failure=%s",
                  r_cmd.entry_reached, r_cmd.ntdll_real, r_cmd.kernel32_real,
                  r_cmd.stdout_bytes, r_cmd.first_fail);
    }

    /* human-readable summary on stdout */
    printf("REALWIN notepad.exe: entry=%d real_ntdll=%zu real_kernel32=%zu stdout_bytes=%zu signaled=%d first=%s\n",
           r_np.entry_reached, r_np.ntdll_real, r_np.kernel32_real,
           r_np.stdout_bytes, r_np.signaled, r_np.first_fail);
    printf("REALWIN cmd.exe:     entry=%d real_ntdll=%zu real_kernel32=%zu stdout_bytes=%zu signaled=%d first=%s\n",
           r_cmd.entry_reached, r_cmd.ntdll_real, r_cmd.kernel32_real,
           r_cmd.stdout_bytes, r_cmd.signaled, r_cmd.first_fail);

    return any ? 0 : 1;
}
