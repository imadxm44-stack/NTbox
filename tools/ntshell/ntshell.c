/*
 * ntshell — طرفية NT-Box بأسلوب PowerShell (M2)
 * يترجم أوامر إلى /proc/ntbox/ ويعرض النتيجة.
 * Requires root (mount/proc).
 *
 * Build: gcc -O2 -o ntshell ntshell.c
 * Usage: ntshell <command> [args...]
 *
 * Commands:
 *   partitions                 Get-Partition
 *   policy allow|deny|list DEV  policy
 *   attach DEV LETTER MOUNT    Initialize-Disk + حرف
 *   mount  DEV MOUNT           mount -t ntfs3 (helper)
 *   detach LETTER              Dismount-Volume
 *   open PATH                  open in place + run if PE (.sys kernel-mode)
 *   runuser PATH               run user-mode .exe (32-bit or 64-bit)
 *   runexe  PATH               الأمر الموحد: run any .exe (x86 PE32 or x64 PE32+)
 *   gpu   PATH                 تعيين مسار تعريف كرت الشاشة (مثلا AMD RX 580 exe)
 *   result                     show last result
 *   selftest                   run self-test matrix
 *   stats                      show counters
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define PROC_DIR "/proc/ntbox"

static int write_proc(const char *name, const char *value)
{
    char path[256];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", PROC_DIR, name);
    f = fopen(path, "w");
    if (!f) {
        perror(path);
        return -1;
    }
    fputs(value, f);
    fputc('\n', f);
    fclose(f);
    return 0;
}

static void read_proc(const char *name)
{
    char path[256];
    char line[1024];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", PROC_DIR, name);
    f = fopen(path, "r");
    if (!f) {
        perror(path);
        return;
    }
    while (fgets(line, sizeof(line), f))
        fputs(line, stdout);
    fclose(f);
}

static void show_help(void)
{
    puts("ntshell — NT-Box PowerShell-style shell (M2)");
    puts("Commands:");
    puts("  partitions                      Get-Partition");
    puts("  policy allow|deny|list DEV      policy");
    puts("  attach DEV LETTER MOUNT         attach drive");
    puts("  mount  DEV MOUNT                mount -t ntfs3 (helper)");
    puts("  detach LETTER                   Dismount-Volume");
    puts("  open PATH                       open in place + info");
    puts("  loadsys PATH.sys                load a Windows service/driver (.sys)");
    puts("  runexe PATH.exe                 run any Windows app (.exe, x86 or x64)");
    puts("  runuser PATH.exe                same as runexe (kept for compatibility)");
    puts("  gpu   PATH                      Set GPU driver path (e.g. AMD RX 580 exe)");
    puts("  gpu                             Show current GPU driver path");
    puts("  result                          show last result");
    puts("  selftest                        run self-test matrix");
    puts("  stats                           show counters");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        show_help();
        return 0;
    }

    if (strcmp(argv[1], "partitions") == 0) {
        read_proc("partitions");
    } else if (strcmp(argv[1], "policy") == 0) {
        if (argc < 4) { fprintf(stderr, "usage: policy allow|deny|list DEV\n"); return 1; }
        char val[128];
        snprintf(val, sizeof(val), "%s %s", argv[2], argv[3]);
        write_proc("policy", val);
        read_proc("result");
    } else if (strcmp(argv[1], "attach") == 0) {
        if (argc < 5) { fprintf(stderr, "usage: attach DEV LETTER MOUNT\n"); return 1; }
        char val[256];
        snprintf(val, sizeof(val), "%s %s %s", argv[2], argv[3], argv[4]);
        write_proc("attach", val);
        read_proc("result");
    } else if (strcmp(argv[1], "mount") == 0) {
        if (argc < 4) { fprintf(stderr, "usage: mount DEV MOUNT\n"); return 1; }
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "mkdir -p %s && mount -t ntfs3 %s %s", argv[3], argv[2], argv[3]);
        system(cmd);
    } else if (strcmp(argv[1], "detach") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: detach LETTER\n"); return 1; }
        write_proc("detach", argv[2]);
        read_proc("result");
    } else if (strcmp(argv[1], "open") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: open PATH\n"); return 1; }
        write_proc("open", argv[2]);
        read_proc("result");
    } else if (strcmp(argv[1], "imports") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: imports PATH\n"); return 1; }
        char cmd[1024];
        const char *pe = "peimports";
        /* prefer project build, then PATH */
        if (access("/home/imadix/Downloads/Niga/tools/peimports/peimports", X_OK) == 0)
            pe = "/home/imadix/Downloads/Niga/tools/peimports/peimports";
        snprintf(cmd, sizeof(cmd), "%s %s", pe, argv[2]);
        system(cmd);
    } else if (strcmp(argv[1], "runuser") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: runuser PATH\n"); return 1; }
        write_proc("runuser", argv[2]);
        read_proc("result");
    } else if (strcmp(argv[1], "runexe") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: runexe PATH\n"); return 1; }
        write_proc("runexe", argv[2]);
        read_proc("result");
    } else if (strcmp(argv[1], "gpu") == 0) {
        if (argc < 3) {
            /* إذا لم يُمرر مسار، اعرض المسار الحالي المخزن */
            read_proc("gpu_driver");
        } else {
            write_proc("gpu_driver", argv[2]);
            read_proc("result");
        }
    } else if (strcmp(argv[1], "loadsys") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: loadsys PATH.sys\n"); return 1; }
        write_proc("load", argv[2]);
        read_proc("result");
    } else if (strcmp(argv[1], "run") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: run PATH\n"); return 1; }
        write_proc("open", argv[2]);
        read_proc("result");
        system("dmesg | tail -n 20");
    } else if (strcmp(argv[1], "result") == 0) {
        read_proc("result");
    } else if (strcmp(argv[1], "selftest") == 0) {
        read_proc("selftest");
    } else if (strcmp(argv[1], "stats") == 0) {
        read_proc("stats");
    } else {
        fprintf(stderr, "unknown command: %s\n", argv[1]);
        show_help();
        return 1;
    }
    return 0;
}
