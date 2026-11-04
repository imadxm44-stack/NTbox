#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdarg.h>

#include "hive.h"
#include "ntb0_trace.h"
#include "ntb0_nt.h"
#include "ntb0_handle.h"
#include "ntb0_path.h"
#include "disk.h"

static int failures = 0;

static ntb0_unicode_string make_us(const char *s)
{
    ntb0_unicode_string us;
    size_t n = strlen(s);
    size_t i;

    memset(&us, 0, sizeof(us));
    us.length = (uint16_t)(n * 2);
    us.maximum_length = (uint16_t)(n * 2 + 2);
    us.buffer = calloc(n + 1, 2);
    for (i = 0; i < n; i++)
        us.buffer[i] = (uint16_t)(unsigned char)s[i];
    return us;
}

static void free_us(ntb0_unicode_string *us)
{
    free(us->buffer);
    us->buffer = NULL;
}

int main(void)
{
    ntb0_disk_dir_entry *ents;
    uint64_t h = 0, dh = 0, th = 0, h1 = 0, h2 = 0;
    char buf[4];
    ssize_t nr;
    long st;
    ntb0_unicode_string us, usdir, ustmp, usd;

    printf("=== NT-Box disk self-test (W10: T-241..T-249) ===\n");

    /* DISK-001: init / open root */
    if (ntb0_disk_init(NULL) == 0) {
        NTB0_OK("DISK-001", "disk root opened: %s", ntb0_path_root());
    } else {
        NTB0_FAIL("DISK-001", "cannot open disk root");
        failures++;
    }

    /* DISK-002: open ntdll.dll by NT path */
    us = make_us("\\??\\C:\\Windows\\System32\\ntdll.dll");
    st = ntb0_disk_create_file(&us, NTB0_GENERIC_READ | NTB0_FILE_READ_DATA,
                               NTB0_FILE_SHARE_READ, NTB0_FILE_OPEN, 0, &h);
    if (st == NTB0_ST_SUCCESS && h != 0) {
        NTB0_OK("DISK-002", "NtCreateFile opened ntdll.dll (handle=0x%llx)",
                (unsigned long long)h);
    } else {
        NTB0_FAIL("DISK-002", "NtCreateFile failed: status=0x%lx", st);
        failures++;
    }

    /* DISK-003: read first 4 bytes (MZ) */
    if (h != 0) {
        int fd = ntb0_handle_fd(h);
        memset(buf, 0, sizeof(buf));
        nr = read(fd, buf, 4);
        if (nr == 4 && buf[0] == 'M' && buf[1] == 'Z') {
            NTB0_OK("DISK-003", "read first 4 bytes: %.4s", buf);
        } else {
            NTB0_FAIL("DISK-003", "unexpected header (n=%zd, bytes=%02x %02x %02x %02x)",
                      nr, (unsigned char)buf[0], (unsigned char)buf[1],
                      (unsigned char)buf[2], (unsigned char)buf[3]);
            failures++;
        }
    } else {
        NTB0_FAIL("DISK-003", "no handle for ntdll.dll");
        failures++;
    }

    /* DISK-004: query directory (>= 100 entries) */
    usdir = make_us("\\??\\C:\\Windows\\System32");
    st = ntb0_disk_create_file(&usdir, NTB0_GENERIC_READ | NTB0_FILE_READ_DATA,
                               NTB0_FILE_SHARE_READ, NTB0_FILE_OPEN,
                               NTB0_FILE_DIRECTORY_FILE, &dh);
    ents = calloc(4096, sizeof(*ents));
    if (st == NTB0_ST_SUCCESS && dh != 0 && ents) {
        size_t n = 0;
        long qr = ntb0_disk_query_directory(dh, ents, 4096, &n);
        if (qr == NTB0_ST_SUCCESS && n >= 100) {
            NTB0_OK("DISK-004", "NtQueryDirectoryFile listed %zu entries (>=100)",
                    n);
        } else {
            NTB0_FAIL("DISK-004", "directory query failed: status=0x%lx count=%zu",
                      qr, n);
            failures++;
        }
    } else {
        NTB0_FAIL("DISK-004", "cannot open System32 directory: status=0x%lx", st);
        failures++;
    }

    /* DISK-005 + DISK-006: rename temp file then delete it */
    ustmp = make_us("\\??\\C:\\ntbox_disk_tmp.txt");
    st = ntb0_disk_create_file(&ustmp, NTB0_GENERIC_WRITE | NTB0_FILE_WRITE_DATA,
                               NTB0_FILE_SHARE_READ | NTB0_FILE_SHARE_WRITE,
                               NTB0_FILE_CREATE, 0, &th);
    if (st == NTB0_ST_SUCCESS && th != 0) {
        long rr = ntb0_disk_rename(th, "ntbox_disk_tmp2.txt");
        if (rr == NTB0_ST_SUCCESS) {
            NTB0_OK("DISK-005", "rename succeeded: %s",
                    ntb0_handle_path(th));
        } else {
            NTB0_FAIL("DISK-005", "rename failed: status=0x%lx", rr);
            failures++;
        }

        long dr = ntb0_disk_set_disposition(th, 1);
        if (dr == NTB0_ST_SUCCESS) {
            NTB0_OK("DISK-006", "delete (disposition) succeeded");
        } else {
            NTB0_FAIL("DISK-006", "delete failed: status=0x%lx", dr);
            failures++;
        }
    } else {
        NTB0_FAIL("DISK-005", "cannot create temp file: status=0x%lx", st);
        failures++;
        NTB0_FAIL("DISK-006", "skipped (no temp file)");
        failures++;
    }

    /* DISK-007: \\??\\D:\\foo ⇒ DEVICE_NOT_CONNECTED */
    usd = make_us("\\??\\D:\\foo");
    {
        uint64_t hd = 0;
        long sr = ntb0_disk_create_file(&usd, NTB0_GENERIC_READ | NTB0_FILE_READ_DATA,
                                        NTB0_FILE_SHARE_READ, NTB0_FILE_OPEN, 0, &hd);
        if (sr == NTB0_ST_DEVICE_NOT_CONNECTED) {
            NTB0_OK("DISK-007", "D: drive reported DEVICE_NOT_CONNECTED (honest)");
        } else {
            NTB0_FAIL("DISK-007", "expected DEVICE_NOT_CONNECTED, got 0x%lx", sr);
            failures++;
            if (hd)
                ntb0_disk_close(hd);
        }
    }

    /* DISK-008: O_DIRECT (FILE_NO_INTERMEDIATE_BUFFERING) */
    {
        uint64_t ho = 0;
        long so = ntb0_disk_create_file(&us, NTB0_GENERIC_READ | NTB0_FILE_READ_DATA,
                                        NTB0_FILE_SHARE_READ, NTB0_FILE_OPEN,
                                        NTB0_FILE_NO_INTERMEDIATE_BUFFERING, &ho);
        if (so == NTB0_ST_SUCCESS && ho != 0) {
            NTB0_OK("DISK-008", "opened with FILE_NO_INTERMEDIATE_BUFFERING (O_DIRECT)");
            ntb0_disk_close(ho);
        } else {
            NTB0_FAIL("DISK-008", "O_DIRECT open failed: status=0x%lx", so);
            failures++;
        }
    }

    /* DISK-009: open-file table sharing (same path opened twice) */
    {
        long a = ntb0_disk_create_file(&us, NTB0_GENERIC_READ | NTB0_FILE_READ_DATA,
                                       NTB0_FILE_SHARE_READ, NTB0_FILE_OPEN, 0, &h1);
        long b = ntb0_disk_create_file(&us, NTB0_GENERIC_READ | NTB0_FILE_READ_DATA,
                                       NTB0_FILE_SHARE_READ, NTB0_FILE_OPEN, 0, &h2);
        if (a == NTB0_ST_SUCCESS && b == NTB0_ST_SUCCESS && h1 && h2) {
            NTB0_OK("DISK-009", "open-file table shared: count=%zu (handles 0x%llx, 0x%llx)",
                    ntb0_disk_open_file_count(),
                    (unsigned long long)h1, (unsigned long long)h2);
        } else {
            NTB0_FAIL("DISK-009", "second open failed: %ld / %ld", a, b);
            failures++;
        }
    }

    /* cleanup */
    if (h)  ntb0_disk_close(h);
    if (dh) ntb0_disk_close(dh);
    if (th) ntb0_disk_close(th);
    if (h1) ntb0_disk_close(h1);
    if (h2) ntb0_disk_close(h2);

    free(ents);
    free_us(&us);
    free_us(&usdir);
    free_us(&ustmp);
    free_us(&usd);

    printf("\n=== result: %s (%d failure(s)) ===\n",
           failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}