#define _GNU_SOURCE
#pragma GCC diagnostic ignored "-Wformat-truncation"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>

#include "hive.h"
#include "ntb0_trace.h"
#include "driver_install.h"

/* ------------------------------------------------------------------ */
/* constants                                                           */
/* ------------------------------------------------------------------ */
#define DRV_MANIFEST_MAX 512
#define DRV_LIST_MAX     4096

typedef struct {
    char name[128];
    char path[512];
    char type[64];
    int  load_order;
} drv_entry_t;

static char g_driver_names[DRV_LIST_MAX][256];
static int  g_driver_count = 0;

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static const char *resolve_root(const char *root)
{
    static char cand[1024];
    const char *env;
    const char *home;

    if (root && *root)
        return root;

    env = getenv("NTBOX_ROOT");
    if (env && *env)
        return env;

    if (access("/tmp/opencode/win7root/Windows/System32/config", R_OK) == 0)
        return "/tmp/opencode/win7root";

    home = getenv("HOME");
    if (home && *home) {
        snprintf(cand, sizeof(cand), "%s/win7root", home);
        if (access(cand, R_OK) == 0)
            return cand;
    }

    if (access("win7root/Windows/System32/config", R_OK) == 0)
        return "win7root";

    return NULL;
}

static int ensure_dir(const char *path)
{
    char tmp[4096];
    char *p;
    size_t len;

    if (!path || !*path)
        return -1;
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (len == 0)
        return -1;
    if (tmp[len - 1] == '/')
        tmp[len - 1] = '\0';

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int copy_file_impl(const char *src, const char *dst)
{
    int in, out;
    char buf[65536];
    ssize_t n;

    in = open(src, O_RDONLY);
    if (in < 0)
        return -1;
    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        close(in);
        return -1;
    }
    for (;;) {
        n = read(in, buf, sizeof(buf));
        if (n < 0) {
            close(in);
            close(out);
            return -1;
        }
        if (n == 0)
            break;
        {
            char *ptr = buf;
            ssize_t rem = n;
            while (rem > 0) {
                ssize_t w = write(out, ptr, (size_t)rem);
                if (w < 0) {
                    close(in);
                    close(out);
                    return -1;
                }
                ptr += w;
                rem -= w;
            }
        }
    }
    close(in);
    if (close(out) != 0)
        return -1;
    return 0;
}

/* strip trailing .sys (case-insensitive) */
static void strip_sys(char *dst, size_t dstlen, const char *src)
{
    size_t len;

    if (!dst || dstlen == 0)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dstlen, "%s", src);
    len = strlen(dst);
    if (len > 4 && strcasecmp(dst + len - 4, ".sys") == 0)
        dst[len - 4] = '\0';
}

/* ------------------------------------------------------------------ */
/* manifest JSON                                                       */
/* ------------------------------------------------------------------ */

static void manifest_path(const char *root, char *buf, size_t len)
{
    snprintf(buf, len, "%s/ntbox/drivers.json", root);
}

static int json_get_string(const char *obj, const char *key, char *out, size_t outlen)
{
    char pat[128];
    const char *p;
    size_t i = 0;

    if (!obj || !key || !out || outlen == 0)
        return -1;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(obj, pat);
    if (!p)
        return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == ':')
        p++;
    if (*p != '"')
        return -1;
    p++;
    while (*p && *p != '"' && i + 1 < outlen) {
        if (*p == '\\' && p[1]) {
            p++;
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return (i > 0) ? 0 : -1;
}

static int json_get_int(const char *obj, const char *key, int *out)
{
    char pat[128];
    const char *p;

    if (!obj || !key || !out)
        return -1;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(obj, pat);
    if (!p)
        return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == ':')
        p++;
    if (*p == '-' || *p == '+')
        p++;
    if (*p < '0' || *p > '9')
        return -1;
    /* recompute cleanly */
    {
        const char *q = strstr(obj, pat);
        q += strlen(pat);
        while (*q == ' ' || *q == '\t' || *q == ':')
            q++;
        *out = atoi(q);
    }
    return 0;
}

static int read_manifest(const char *root, drv_entry_t *entries, int *count)
{
    char path[4096];
    FILE *f;
    char *buf;
    long sz;
    int n = 0;
    const char *p, *arr, *end;

    if (count)
        *count = 0;
    manifest_path(root, path, sizeof(path));
    f = fopen(path, "r");
    if (!f)
        return 0;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return -1;
    }
    rewind(f);
    buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return -1;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return -1;
    }
    buf[sz] = '\0';
    fclose(f);

    arr = strstr(buf, "\"drivers\"");
    if (!arr) {
        free(buf);
        return 0;
    }
    arr = strchr(arr, '[');
    if (!arr) {
        free(buf);
        return 0;
    }
    end = strchr(arr, ']');
    if (!end)
        end = buf + strlen(buf);

    p = arr;
    while (n < DRV_MANIFEST_MAX) {
        const char *obj = strchr(p, '{');
        const char *objend;

        if (!obj || obj >= end)
            break;
        objend = strchr(obj, '}');
        if (!objend || objend >= end)
            break;
        {
            char tmp[2048];
            size_t len = (size_t)(objend - obj + 1);

            if (len >= sizeof(tmp))
                len = sizeof(tmp) - 1;
            memcpy(tmp, obj, len);
            tmp[len] = '\0';

            if (json_get_string(tmp, "name", entries[n].name,
                                sizeof(entries[n].name)) == 0) {
                json_get_string(tmp, "path", entries[n].path,
                                sizeof(entries[n].path));
                json_get_string(tmp, "type", entries[n].type,
                                sizeof(entries[n].type));
                if (json_get_int(tmp, "load_order", &entries[n].load_order) != 0)
                    entries[n].load_order = n + 1;
                n++;
            }
        }
        p = objend + 1;
    }

    free(buf);
    if (count)
        *count = n;
    return 0;
}

static int write_manifest(const char *root, drv_entry_t *entries, int count)
{
    char path[4096];
    char dir[4096];
    FILE *f;
    int i;

    snprintf(dir, sizeof(dir), "%s/ntbox", root);
    ensure_dir(dir);
    manifest_path(root, path, sizeof(path));
    f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "{\n  \"drivers\": [\n");
    for (i = 0; i < count; i++) {
        fprintf(f, "    {\"name\":\"%s\",\"path\":\"%s\",\"type\":\"%s\",\"load_order\":%d}%s\n",
                entries[i].name, entries[i].path, entries[i].type,
                entries[i].load_order, (i + 1 < count) ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    if (fclose(f) != 0)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* find_ntboxd                                                         */
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
/* public API                                                          */
/* ------------------------------------------------------------------ */

int ntb0_driver_list(const char *root)
{
    const char *r = resolve_root(root);
    char dirpath[4096];
    DIR *d;
    struct dirent *e;

    g_driver_count = 0;
    if (!r) {
        NTB0_FAIL("DRV-001", "win7 root not found");
        return -1;
    }
    snprintf(dirpath, sizeof(dirpath), "%s/Windows/System32/drivers", r);
    d = opendir(dirpath);
    if (!d) {
        NTB0_FAIL("DRV-001", "cannot open drivers dir: %s", dirpath);
        return -1;
    }
    while ((e = readdir(d)) != NULL && g_driver_count < DRV_LIST_MAX) {
        size_t len = strlen(e->d_name);
        struct stat st;
        char child[4096];

        if (len <= 4 || strcasecmp(e->d_name + len - 4, ".sys") != 0)
            continue;
        snprintf(child, sizeof(child), "%s/%s", dirpath, e->d_name);
        if (stat(child, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        snprintf(g_driver_names[g_driver_count], sizeof(g_driver_names[0]),
                 "%s", e->d_name);
        g_driver_count++;
    }
    closedir(d);
    NTB0_OK("DRV-001", "driver list: %d .sys in %s", g_driver_count, dirpath);
    return g_driver_count;
}

const char *ntb0_driver_list_name(int index)
{
    if (index < 0 || index >= g_driver_count)
        return NULL;
    return g_driver_names[index];
}

int ntb0_driver_install(const char *root, const char *name, const char *sys_path)
{
    const char *r = resolve_root(root);
    char base[128];
    char ddir[4096], dst[4096];
    drv_entry_t entries[DRV_MANIFEST_MAX];
    int count = 0, i, found = -1, maxorder = 0;

    if (!r || !name || !*name || !sys_path || !*sys_path)
        return -1;
    if (access(sys_path, R_OK) != 0)
        return -1;

    strip_sys(base, sizeof(base), name);
    if (!*base)
        return -1;

    snprintf(ddir, sizeof(ddir), "%s/Windows/System32/drivers", r);
    ensure_dir(ddir);
    snprintf(dst, sizeof(dst), "%s/%s.sys", ddir, base);

    if (copy_file_impl(sys_path, dst) != 0)
        return -1;

    if (read_manifest(r, entries, &count) != 0)
        count = 0;

    for (i = 0; i < count; i++) {
        if (entries[i].load_order > maxorder)
            maxorder = entries[i].load_order;
        if (strcmp(entries[i].name, base) == 0)
            found = i;
    }

    if (found >= 0) {
        snprintf(entries[found].path, sizeof(entries[found].path),
                 "Windows/System32/drivers/%s.sys", base);
        snprintf(entries[found].type, sizeof(entries[found].type), "%s", "kernel");
        if (entries[found].load_order <= 0)
            entries[found].load_order = maxorder + 1;
    } else {
        if (count >= DRV_MANIFEST_MAX)
            return -1;
        snprintf(entries[count].name, sizeof(entries[count].name), "%s", base);
        snprintf(entries[count].path, sizeof(entries[count].path),
                 "Windows/System32/drivers/%s.sys", base);
        snprintf(entries[count].type, sizeof(entries[count].type), "%s", "kernel");
        entries[count].load_order = maxorder + 1;
        count++;
    }

    if (write_manifest(r, entries, count) != 0)
        return -1;

    NTB0_OK("DRV-002", "installed driver '%s' from %s", base, sys_path);
    return 0;
}

int ntb0_driver_is_installed(const char *root, const char *name)
{
    const char *r = resolve_root(root);
    drv_entry_t entries[DRV_MANIFEST_MAX];
    int count = 0, i;
    char base[128], fpath[4096];

    if (!r || !name || !*name)
        return 0;
    strip_sys(base, sizeof(base), name);
    if (read_manifest(r, entries, &count) != 0)
        return 0;
    for (i = 0; i < count; i++) {
        if (strcmp(entries[i].name, base) == 0) {
            snprintf(fpath, sizeof(fpath), "%s/Windows/System32/drivers/%s.sys", r, base);
            if (access(fpath, F_OK) == 0)
                return 1;
            return 0;
        }
    }
    return 0;
}

int ntb0_driver_uninstall(const char *root, const char *name)
{
    const char *r = resolve_root(root);
    drv_entry_t entries[DRV_MANIFEST_MAX];
    int count = 0, i, j;
    char base[128], fpath[4096];

    if (!r || !name || !*name)
        return -1;
    strip_sys(base, sizeof(base), name);

    if (read_manifest(r, entries, &count) != 0)
        count = 0;

    for (i = 0; i < count; i++) {
        if (strcmp(entries[i].name, base) == 0) {
            for (j = i; j < count - 1; j++)
                entries[j] = entries[j + 1];
            count--;
            i--;
        }
    }

    if (write_manifest(r, entries, count) != 0)
        return -1;

    snprintf(fpath, sizeof(fpath), "%s/Windows/System32/drivers/%s.sys", r, base);
    unlink(fpath);

    NTB0_OK("DRV-004", "uninstalled driver '%s'", base);
    return 0;
}

int ntb0_driver_load(const char *root, const char *name)
{
    const char *r = resolve_root(root);
    char base[128], path[4096];
    const char *ntboxd_path;
    pid_t pid;
    int status;

    if (!r || !name || !*name)
        return -1;
    strip_sys(base, sizeof(base), name);
    snprintf(path, sizeof(path), "%s/Windows/System32/drivers/%s.sys", r, base);
    if (access(path, R_OK) != 0)
        return -1;

    if (!getenv("NTBOX_ROOT"))
        setenv("NTBOX_ROOT", r, 1);

    ntboxd_path = find_ntboxd();
    if (!ntboxd_path) {
        NTB0_FAIL("DRV-003", "ntboxd host binary not found — cannot load '%s'", base);
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        NTB0_FAIL("DRV-003", "fork failed for '%s': %s", base, strerror(errno));
        return -1;
    }
    if (pid == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        char *argv[] = { (char *)ntboxd_path, path, NULL };
        execv(ntboxd_path, argv);
        _exit(127);
    }

    if (waitpid(pid, &status, 0) < 0) {
        NTB0_FAIL("DRV-003", "waitpid failed for '%s': %s", base, strerror(errno));
        return -1;
    }

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        NTB0_OK("DRV-003", "loaded driver '%s' via ntboxd (exit=%d)", base, code);
        return code;
    }
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        NTB0_OK("DRV-003", "loaded driver '%s' via ntboxd (signal=%d)", base, sig);
        return 128 + sig;
    }
    NTB0_FAIL("DRV-003", "driver '%s' terminated abnormally", base);
    return -1;
}

int ntb0_driver_audio_bridge(const char *root)
{
    (void)root;
    NTB0_STUB("DRV-005", "audio bridge not yet translated (no audio translation layer)");
    return -1;
}
