#define _GNU_SOURCE
#pragma GCC diagnostic ignored "-Wformat-truncation"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#include "runtime_install.h"

/* ------------------------------------------------------------------ */
/* قائمة بيئة التشغيل المعروفة                                         */
/* ------------------------------------------------------------------ */
static const ntb0_runtime_t g_runtimes[] = {
    { "java",     "Java Runtime (JRE/JDK)",      "java",   "jre-*-windows-*.exe" },
    { "vcpp2005", "Visual C++ 2005 Redist",     "vcpp",   "vcredist*2005*.exe" },
    { "vcpp2008", "Visual C++ 2008 Redist",     "vcpp",   "vcredist*2008*.exe" },
    { "vcpp2010", "Visual C++ 2010 Redist",     "vcpp",   "vcredist*2010*.exe" },
    { "vcpp2012", "Visual C++ 2012 Redist",     "vcpp",   "vcredist*2012*.exe" },
    { "vcpp2013", "Visual C++ 2013 Redist",     "vcpp",   "vcredist*2013*.exe" },
    { "vcpp2015", "Visual C++ 2015-2022 Redist","vcpp",   "vc_redist*.exe" },
    { "dotnet20", ".NET Framework 2.0",         "dotnet", "dotNetFx2*.exe" },
    { "dotnet35", ".NET Framework 3.5",         "dotnet", "dotNetFx35*.exe" },
    { "dotnet4x", ".NET Framework 4.x",         "dotnet", "dotNetFx4*.exe" },
};

static const size_t g_runtime_count =
    sizeof(g_runtimes) / sizeof(g_runtimes[0]);

const ntb0_runtime_t *ntb0_runtime_list(size_t *count_out)
{
    if (count_out)
        *count_out = g_runtime_count;
    NTB0_OK("RT-001", "runtime list: %zu known runtimes", g_runtime_count);
    return g_runtimes;
}

int ntb0_runtime_count(void)
{
    return (int)g_runtime_count;
}

const ntb0_runtime_t *ntb0_runtime_find(const char *name)
{
    size_t i;

    if (!name || !*name)
        return NULL;
    for (i = 0; i < g_runtime_count; i++) {
        if (strcmp(g_runtimes[i].name, name) == 0)
            return &g_runtimes[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* مساعدة: جذر + مسارات                                                */
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
    /* أزل الفاصلة النهائية */
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

/* نسخة ملف */
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

/* ------------------------------------------------------------------ */
/* manifest JSON                                                       */
/* ------------------------------------------------------------------ */
#define MANIFEST_MAX_ENTRIES 128

typedef struct {
    char name[128];
    char installer[256];
} manifest_entry_t;

static void manifest_path(const char *root, char *buf, size_t len)
{
    snprintf(buf, len, "%s/ntbox/runtimes.json", root);
}

/* استخراج قيمة نصية من كائن JSON بسيط */
static int json_get_string(const char *obj, const char *key, char *out, size_t outlen)
{
    char pat[128];
    const char *p, *q;
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
    (void)q;
    return (i > 0) ? 0 : -1;
}

static int read_manifest(const char *root, manifest_entry_t *entries, int *count)
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
        return 0; /* لا manifest ⇒ فارغ */
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

    arr = strstr(buf, "\"runtimes\"");
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
    while (n < MANIFEST_MAX_ENTRIES) {
        const char *obj = strchr(p, '{');
        const char *objend;

        if (!obj || obj >= end)
            break;
        objend = strchr(obj, '}');
        if (!objend || objend >= end)
            break;
        {
            char tmp[1024];
            size_t len = (size_t)(objend - obj + 1);

            if (len >= sizeof(tmp))
                len = sizeof(tmp) - 1;
            memcpy(tmp, obj, len);
            tmp[len] = '\0';

            if (json_get_string(tmp, "name", entries[n].name,
                                sizeof(entries[n].name)) == 0) {
                json_get_string(tmp, "installer", entries[n].installer,
                                sizeof(entries[n].installer));
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

static int write_manifest(const char *root, manifest_entry_t *entries, int count)
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
    fprintf(f, "{\n  \"runtimes\": [\n");
    for (i = 0; i < count; i++) {
        fprintf(f, "    {\"name\":\"%s\",\"installer\":\"%s\"}%s\n",
                entries[i].name, entries[i].installer,
                (i + 1 < count) ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    if (fclose(f) != 0)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* helpers: remove directory recursively                               */
/* ------------------------------------------------------------------ */
static int remove_tree(const char *path)
{
    struct stat st;
    DIR *d;
    struct dirent *e;
    char child[4096];

    if (lstat(path, &st) != 0)
        return 0; /* already gone */
    if (!S_ISDIR(st.st_mode)) {
        return unlink(path) == 0 ? 0 : -1;
    }
    d = opendir(path);
    if (!d)
        return -1;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        remove_tree(child);
    }
    closedir(d);
    return rmdir(path) == 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

int ntb0_runtime_install(const char *root, const char *name,
                         const char *installer_path)
{
    const char *r = resolve_root(root);
    char base[4096], rdir[4096], tdir[4096], dst[4096];
    const char *base_name;
    manifest_entry_t entries[MANIFEST_MAX_ENTRIES];
    int count = 0;
    int i, found = -1;

    if (!r || !name || !*name || !installer_path || !*installer_path)
        return -1;
    if (access(installer_path, R_OK) != 0)
        return -1;

    base_name = strrchr(installer_path, '/');
    base_name = base_name ? base_name + 1 : installer_path;

    snprintf(base, sizeof(base), "%s/ntbox", r);
    ensure_dir(base);
    snprintf(rdir, sizeof(rdir), "%s/ntbox/runtimes", r);
    ensure_dir(rdir);
    snprintf(tdir, sizeof(tdir), "%s/ntbox/runtimes/%s", r, name);
    ensure_dir(tdir);
    snprintf(dst, sizeof(dst), "%s/%s", tdir, base_name);

    if (copy_file_impl(installer_path, dst) != 0)
        return -1;

    if (read_manifest(r, entries, &count) != 0)
        count = 0;

    for (i = 0; i < count; i++) {
        if (strcmp(entries[i].name, name) == 0) {
            found = i;
            break;
        }
    }
    if (found >= 0) {
        snprintf(entries[found].installer, sizeof(entries[found].installer),
                 "%s", base_name);
    } else {
        if (count >= MANIFEST_MAX_ENTRIES)
            return -1;
        snprintf(entries[count].name, sizeof(entries[count].name), "%s", name);
        snprintf(entries[count].installer, sizeof(entries[count].installer),
                 "%s", base_name);
        count++;
    }

    if (write_manifest(r, entries, count) != 0)
        return -1;

    NTB0_OK("RT-002", "installed runtime '%s' from %s", name, installer_path);
    return 0;
}

int ntb0_runtime_is_installed(const char *root, const char *name)
{
    const char *r = resolve_root(root);
    manifest_entry_t entries[MANIFEST_MAX_ENTRIES];
    int count = 0, i;
    char tdir[4096];

    if (!r || !name || !*name)
        return 0;
    if (read_manifest(r, entries, &count) != 0)
        return 0;
    for (i = 0; i < count; i++) {
        if (strcmp(entries[i].name, name) == 0) {
            /* also require directory present */
            snprintf(tdir, sizeof(tdir), "%s/ntbox/runtimes/%s", r, name);
            if (access(tdir, F_OK) == 0)
                return 1;
            return 0;
        }
    }
    return 0;
}

int ntb0_runtime_uninstall(const char *root, const char *name)
{
    const char *r = resolve_root(root);
    manifest_entry_t entries[MANIFEST_MAX_ENTRIES];
    int count = 0, i, j;
    char tdir[4096];

    if (!r || !name || !*name)
        return -1;

    if (read_manifest(r, entries, &count) != 0)
        count = 0;

    /* remove entry */
    for (i = 0; i < count; i++) {
        if (strcmp(entries[i].name, name) == 0) {
            for (j = i; j < count - 1; j++)
                entries[j] = entries[j + 1];
            count--;
            i--;
        }
    }

    if (write_manifest(r, entries, count) != 0)
        return -1;

    snprintf(tdir, sizeof(tdir), "%s/ntbox/runtimes/%s", r, name);
    remove_tree(tdir);

    NTB0_OK("RT-004", "uninstalled runtime '%s'", name);
    return 0;
}

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

int ntb0_runtime_run(const char *root, const char *name)
{
    const char *r = resolve_root(root);
    manifest_entry_t entries[MANIFEST_MAX_ENTRIES];
    int count = 0, i;
    char path[4096], tdir[4096];
    const char *ntboxd_path;
    pid_t pid;
    int status;

    if (!r || !name || !*name)
        return -1;
    if (!ntb0_runtime_is_installed(r, name))
        return -1;

    if (read_manifest(r, entries, &count) != 0)
        return -1;
    for (i = 0; i < count; i++) {
        if (strcmp(entries[i].name, name) == 0) {
            snprintf(tdir, sizeof(tdir), "%s/ntbox/runtimes/%s", r, name);
            snprintf(path, sizeof(path), "%s/%s", tdir, entries[i].installer);
            break;
        }
    }
    if (i >= count)
        return -1;
    if (access(path, R_OK) != 0)
        return -1;

    if (!getenv("NTBOX_ROOT"))
        setenv("NTBOX_ROOT", r, 1);

    ntboxd_path = find_ntboxd();
    if (!ntboxd_path) {
        NTB0_FAIL("RT-003", "ntboxd host binary not found — cannot run '%s'", name);
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        NTB0_FAIL("RT-003", "fork failed for '%s': %s", name, strerror(errno));
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
        NTB0_FAIL("RT-003", "waitpid failed for '%s': %s", name, strerror(errno));
        return -1;
    }

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        NTB0_OK("RT-003", "ran runtime '%s' (exit=%d)", name, code);
        return code;
    }
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        NTB0_OK("RT-003", "ran runtime '%s' (signal=%d)", name, sig);
        return 128 + sig;
    }
    NTB0_FAIL("RT-003", "runtime '%s' terminated abnormally", name);
    return -1;
}

