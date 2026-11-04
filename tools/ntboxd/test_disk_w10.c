/*
 * test_disk_w10.c — اختبارات W10 المتبقّية (T-242 / T-243-done / T-245 /
 * T-246 / T-249) على طبقة disk.c حصراً.
 *
 * المصابيح (تتمة الترقيم DISK-001..009 في test_disk.c):
 *   DISK-010  فرز مدخلات المجلد بأسلوب `dir` (T-242)
 *   DISK-011  صفات + أحجام المدخلات كما تعرضها `dir` (T-242)
 *   DISK-012  مجسّ O_DIRECT/posix_fadvise — نجاح أو رفض صادق (T-245)
 *   DISK-013  أشكال مسار الجذر \??\C: / \??\C:\ (T-246)
 *   DISK-014  \??\UNC\ ⇒ رفض صادق NOT_SUPPORTED (T-246)
 *   DISK-015  حماية نواة الحجم: لا rename/delete على \??\C:\ (T-246)
 *   DISK-016  قياس لا-نسخ: مسار القراءة ينسخ 0 بايت عبر الصندوق (T-249)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

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

/* تنظيف بقايا الجولة السابقة (اجعل الاختبار idempotent) */
static void wipe_sandbox(void)
{
    static const char *kids[] = {
        "ntbox_w10dir/B.txt", "ntbox_w10dir/a.txt",
        "ntbox_w10dir/.hidden", "ntbox_w10dir/ex0.txt",
    };
    size_t i;

    for (i = 0; i < sizeof(kids) / sizeof(kids[0]); i++)
        unlinkat(ntb0_path_dirfd(), kids[i], 0);
    unlinkat(ntb0_path_dirfd(), "ntbox_w10dir/Csub", AT_REMOVEDIR);
    unlinkat(ntb0_path_dirfd(), "ntbox_w10dir", AT_REMOVEDIR);
    unlinkat(ntb0_path_dirfd(), "ntbox_copy_measure.bin", 0);
    unlinkat(ntb0_path_dirfd(), ".ntbox_odirect_probe.tmp", 0);
}

/* فتح/إنشاء ملف داخل مجلد اختبار، كتابة n بايت من نمط، إغلاقه */
static long make_file(const char *ntpath, const char *pattern, size_t n)
{
    ntb0_unicode_string us = make_us(ntpath);
    uint64_t h = 0;
    long st = ntb0_disk_create_file(&us, NTB0_GENERIC_WRITE | NTB0_FILE_WRITE_DATA,
                                    NTB0_FILE_SHARE_READ | NTB0_FILE_SHARE_WRITE,
                                    NTB0_FILE_CREATE, 0, &h);
    if (st == NTB0_ST_SUCCESS && h) {
        int fd = ntb0_handle_fd(h);

        if (write(fd, pattern, n) != (ssize_t)n)
            st = -1;
        ntb0_disk_close(h);
    }
    free_us(&us);
    return st;
}

int main(void)
{
    ntb0_disk_dir_entry *ents = NULL;
    ntb0_unicode_string us, ud, f;
    uint64_t h = 0;
    long st;
    size_t i;
    int have_dir = 0;

    (void)have_dir;

    printf("=== NT-Box disk W10 remainder test (T-242/245/246/249) ===\n");

    if (ntb0_disk_init("./win7root") == 0) {
        printf("[init] disk root: %s\n", ntb0_path_root());
    } else {
        printf("[init] FAIL: cannot open ./win7root\n");
        return 1;
    }
    wipe_sandbox();

    /* -------------------------------------------------------------- */
    /* مجلد اختبار معزول داخل الجذر                                    */
    /* -------------------------------------------------------------- */
    ud = make_us("\\??\\C:\\ntbox_w10dir");
    st = ntb0_disk_create_file(&ud, NTB0_FILE_READ_DATA | NTB0_FILE_WRITE_DATA,
                               NTB0_FILE_SHARE_READ | NTB0_FILE_SHARE_WRITE,
                               NTB0_FILE_OPEN_IF,
                               NTB0_FILE_DIRECTORY_FILE, &h);
    if (st == NTB0_ST_SUCCESS && h != 0) {
        printf("[setup] sandbox dir opened (handle=0x%llx, path=%s)\n",
               (unsigned long long)h, ntb0_handle_path(h));
    } else {
        printf("[setup] FAIL: cannot open sandbox dir: 0x%lx\n", st);
        return 1;
    }
    ntb0_disk_close(h);
    h = 0;

    if (make_file("\\??\\C:\\ntbox_w10dir\\B.txt", "xxx", 3) != 0 ||
        make_file("\\??\\C:\\ntbox_w10dir\\a.txt", "yy", 2) != 0 ||
        make_file("\\??\\C:\\ntbox_w10dir\\.hidden", "z", 1) != 0) {
        printf("[setup] FAIL: cannot create test files\n");
        return 1;
    }
    /* ex0.txt: ملف فقط-قراءة (READONLY كما يعرضها dir) */
    {
        f = make_us("\\??\\C:\\ntbox_w10dir\\ex0.txt");

        if (make_file("\\??\\C:\\ntbox_w10dir\\ex0.txt", "rrrrr", 5) == 0) {
            uint64_t hx = 0;

            if (ntb0_disk_create_file(&f, NTB0_FILE_WRITE_DATA, 0,
                                      NTB0_FILE_OPEN, 0, &hx) == 0 && hx) {
                if (fchmod(ntb0_handle_fd(hx), 0444) != 0)
                    printf("[setup] warn: fchmod ex0.txt failed\n");
                ntb0_disk_close(hx);
            }
        }
        free_us(&f);
    }
    /* Csub: مجلد فرعي داخل مجلد الاختبار */
    f = make_us("\\??\\C:\\ntbox_w10dir\\Csub");
    if (ntb0_disk_create_file(&f, NTB0_FILE_READ_DATA, 0, NTB0_FILE_CREATE,
                              NTB0_FILE_DIRECTORY_FILE, &h) == 0 && h) {
        ntb0_disk_close(h);
        h = 0;
    } else {
        printf("[setup] FAIL: cannot create Csub subdirectory\n");
        return 1;
    }
    free_us(&f);

    /* -------------------------------------------------------------- */
    /* DISK-010 + DISK-011: فرز وصفات وأحجام بأسلوب `dir`              */
    /* -------------------------------------------------------------- */
    ents = calloc(64, sizeof(*ents));
    {
        uint64_t dh = 0;

        st = ntb0_disk_create_file(&ud, NTB0_FILE_READ_DATA,
                                   NTB0_FILE_SHARE_READ, NTB0_FILE_OPEN,
                                   NTB0_FILE_DIRECTORY_FILE, &dh);
        if (st == NTB0_ST_SUCCESS && dh) {
            size_t n = 0;
            int sorted = 1, attrs_ok = 1, sizes_ok = 1;
            const ntb0_disk_dir_entry *e;
            const char *expect[5] = { ".hidden", "a.txt", "B.txt",
                                      "Csub", "ex0.txt" };

            if (ntb0_disk_query_directory(dh, ents, 64, &n) ==
                NTB0_ST_SUCCESS) {
                /* T-242: الفرز — حساسية حالة صفرية، dots أولاً */
                for (i = 0; i < n; i++)
                    printf("[dir    ] %-10s attr=0x%06x size=%llu\n",
                           ents[i].name, ents[i].attributes,
                           (unsigned long long)ents[i].size);
                if (n != 5) {
                    printf("[DISK-010] FAIL: expected 5 sorted entries, got %zu\n", n);
                    failures++;
                }
                for (i = 0; i < n; i++) {
                    const char *nm = ents[i].name;

                    if (i > 0 && strcasecmp(ents[i - 1].name, nm) > 0)
                        sorted = 0;
                    if (i >= sizeof(expect) / sizeof(expect[0]) ||
                        strcmp(nm, expect[i]) != 0) {
                        printf("[DISK-010] mismatch at %zu: got '%s'\n",
                               i, nm);
                        sorted = 0;
                    }
                }
                if (sorted && n == 5) {
                    NTB0_OK("DISK-010",
                            "dir sort verified: .hidden,a.txt,B.txt,Csub,ex0.txt (5 entries)");
                } else {
                    NTB0_FAIL("DISK-010", "dir sort mismatch (sorted=%d)", sorted);
                    failures++;
                }

                /* T-242: الصفات والأحجام */
                {
                    const uint64_t sizes[5] = { 1, 2, 3, 0, 5 };
                    uint32_t a;

                    for (i = 0; i < n; i++) {
                        e = &ents[i];
                        a = e->attributes;
                        if (e->size != sizes[i])
                            sizes_ok = 0;
                        if ((a & NTB0_FILE_ATTRIBUTE_DIRECTORY) &&
                            e->size != 0)
                            sizes_ok = 0;
                        if (strcmp(e->name, ".hidden") == 0) {
                            /* ويندوز: الملف المخفي يحمل A+H (dir /a:h) */
                            if (!(a & NTB0_FILE_ATTRIBUTE_HIDDEN))
                                attrs_ok = 0;
                        } else if (strcmp(e->name, "Csub") == 0) {
                            if (!(a & NTB0_FILE_ATTRIBUTE_DIRECTORY) ||
                                (a & NTB0_FILE_ATTRIBUTE_ARCHIVE))
                                attrs_ok = 0;
                        } else if (strcmp(e->name, "ex0.txt") == 0) {
                            if (!(a & NTB0_FILE_ATTRIBUTE_READONLY))
                                attrs_ok = 0;
                        } else { /* a.txt / B.txt: ملفات عادية */
                            if (!(a & NTB0_FILE_ATTRIBUTE_ARCHIVE) ||
                                (a & NTB0_FILE_ATTRIBUTE_HIDDEN) ||
                                (a & NTB0_FILE_ATTRIBUTE_DIRECTORY))
                                attrs_ok = 0;
                        }
                    }
                }
                if (attrs_ok)
                    NTB0_OK("DISK-011",
                            "dir attributes verified: HIDDEN/.hidden, "
                            "DIR/Csub(size 0), READONLY/ex0.txt, ARCHIVE files");
                else {
                    NTB0_FAIL("DISK-011", "attribute mapping mismatch");
                    failures++;
                }
                if (!sizes_ok) {
                    NTB0_FAIL("DISK-011", "size mapping mismatch");
                    failures++;
                }
            } else {
                NTB0_FAIL("DISK-010", "query_directory failed: 0x%lx", st);
                failures++;
            }
            ntb0_disk_close(dh);
        } else {
            NTB0_FAIL("DISK-010", "cannot reopen sandbox dir: 0x%lx", st);
            failures++;
        }
    }

    /* -------------------------------------------------------------- */
    /* DISK-012: مجسّ O_DIRECT / posix_fadvise (T-245)                */
    /* -------------------------------------------------------------- */
    {
        ntb0_disk_odirect_report rep;
        long pr = ntb0_disk_odirect_probe(&rep);

        if (pr == NTB0_ST_SUCCESS && rep.direct_ok == 1 &&
            rep.probe_bytes == 4096 && rep.fadvise_ok == 1 && !rep.refused) {
            NTB0_OK("DISK-012",
                    "O_DIRECT probe measured: open+write+readback 4096/4096 "
                    "bytes verified, posix_fadvise(DONTNEED)=0");
        } else if (pr == NTB0_ST_SUCCESS && rep.refused == 1 &&
                   (rep.refused_errno == EINVAL ||
                    rep.refused_errno == EOPNOTSUPP)) {
            NTB0_OK("DISK-012",
                    "O_DIRECT probe measured: honest refusal by fs "
                    "(errno=%d) — still a PASS (probe measured)",
                    rep.refused_errno);
        } else {
            NTB0_FAIL("DISK-012",
                      "probe inconclusive: rc=0x%lx direct_ok=%d refused=%d"
                      " errno=%d bytes=%llu fadvise=%d",
                      pr, rep.direct_ok, rep.refused, rep.refused_errno,
                      (unsigned long long)rep.probe_bytes, rep.fadvise_ok);
            failures++;
        }
    }

    /* -------------------------------------------------------------- */
    /* DISK-013: أشكال مسار الجذر (T-246)                             */
    /* -------------------------------------------------------------- */
    {
        const char *roots[4] = { "\\??\\C:\\", "\\??\\C:", "\\??\\c:\\",
                                 "\\??\\C:\\\\" };
        /* الأشكال: أصلي، بلا فاصلة خلفية، حرف صغير، فاصلتان خلفيتان */

        for (i = 0; i < 4; i++) {
            uint64_t hr = 0;
            struct stat stx;

            us = make_us(roots[i]);
            st = ntb0_disk_create_file(&us, NTB0_FILE_READ_DATA,
                                       NTB0_FILE_SHARE_READ, NTB0_FILE_OPEN,
                                       NTB0_FILE_DIRECTORY_FILE, &hr);
            if (st == NTB0_ST_SUCCESS && hr &&
                fstat(ntb0_handle_fd(hr), &stx) == 0 && S_ISDIR(stx.st_mode)) {
                if (i == 0)
                    NTB0_OK("DISK-013", "\\??\\C:\\ ⇒ volume root opened");
                ntb0_disk_close(hr);
            } else {
                NTB0_FAIL("DISK-013", "root variant '%s' failed: 0x%lx",
                          roots[i], st);
                failures++;
            }
            free_us(&us);
        }
    }

    /* -------------------------------------------------------------- */
    /* DISK-014: UNC رفض صادق (T-246)                                 */
    /* -------------------------------------------------------------- */
    {
        uint64_t hu = 0;

        us = make_us("\\??\\UNC\\server\\share\\file.txt");
        st = ntb0_disk_create_file(&us, NTB0_FILE_READ_DATA,
                                   NTB0_FILE_SHARE_READ, NTB0_FILE_OPEN,
                                   0, &hu);
        if (st == NTB0_ST_NOT_SUPPORTED && hu == 0) {
            NTB0_OK("DISK-014", "\\??\\UNC\\ refused: NOT_SUPPORTED (honest)");
        } else {
            NTB0_FAIL("DISK-014", "expected NOT_SUPPORTED, got 0x%lx", st);
            failures++;
            if (hu)
                ntb0_disk_close(hu);
        }
        free_us(&us);
    }

    /* -------------------------------------------------------------- */
    /* DISK-015: لا rename/delete على نواة الحجم (T-246)              */
    /* -------------------------------------------------------------- */
    {
        uint64_t hr = 0;

        us = make_us("\\??\\C:\\");
        st = ntb0_disk_create_file(&us, NTB0_FILE_READ_DATA,
                                   NTB0_FILE_SHARE_READ, NTB0_FILE_OPEN,
                                   NTB0_FILE_DIRECTORY_FILE, &hr);
        if (st == NTB0_ST_SUCCESS && hr) {
            long rr = ntb0_disk_rename(hr, "renamed_root");
            long dr = (rr != NTB0_ST_ACCESS_DENIED)
                          ? ntb0_disk_set_disposition(hr, 1)
                          : NTB0_ST_ACCESS_DENIED;
            struct stat stx;

            if (rr == NTB0_ST_ACCESS_DENIED &&
                dr == NTB0_ST_ACCESS_DENIED &&
                fstat(ntb0_handle_fd(hr), &stx) == 0 &&
                S_ISDIR(stx.st_mode)) {
                NTB0_OK("DISK-015",
                        "volume root rename/delete refused, root intact");
            } else {
                NTB0_FAIL("DISK-015",
                          "root rename=0x%lx delete=0x%lx — must be"
                          " ACCESS_DENIED twice", rr, dr);
                failures++;
            }
            ntb0_disk_close(hr);
        } else {
            NTB0_FAIL("DISK-015", "cannot reopen root: 0x%lx", st);
            failures++;
        }
        free_us(&us);
    }

    /* -------------------------------------------------------------- */
    /* DISK-016: قياس لا-نسخ — القراءة تنسخ 0 بايت عبر الصندوق  */
    /* -------------------------------------------------------------- */
    {
        uint64_t hw = 0, hr2 = 0;
        uint8_t *pattern = NULL, *got = NULL;
        size_t sz = 1u << 20; /* 1 MiB */
        uint64_t before, after;
        struct stat sb, sr;
        int plain;
        int ok = 0;

        if (posix_memalign((void **)&pattern, 4096, sz) != 0 ||
            posix_memalign((void **)&got, 4096, sz) != 0) {
            printf("[DISK-016] FAIL: alloc\n");
            return 1;
        }
        for (i = 0; i < sz; i++)
            pattern[i] = (uint8_t)(i * 7 + 13);

        us = make_us("\\??\\C:\\ntbox_copy_measure.bin");
        st = ntb0_disk_create_file(
            &us, NTB0_GENERIC_WRITE | NTB0_FILE_WRITE_DATA,
            NTB0_FILE_SHARE_READ | NTB0_FILE_SHARE_WRITE, NTB0_FILE_CREATE, 0,
            &hw);
        if (st == NTB0_ST_SUCCESS && hw) {
            int wfd = ntb0_handle_fd(hw);

            if (write(wfd, pattern, sz) == (ssize_t)sz &&
                fstat(wfd, &sb) == 0) {
                /* نغلق مقبض الكتابة قبل القراءة: جدول الملفات المفتوحة
                 * يحرّر fd الكتابة (O_WRONLY) ⇒ الفتح الثاني = whence جديد
                 * O_RDONLY حقيقي من النواة (وليس dup قديم بلا قراءة). */
                ntb0_disk_close(hw);
                hw = 0;

                if (ntb0_disk_create_file(
                        &us, NTB0_FILE_READ_DATA,
                        NTB0_FILE_SHARE_READ | NTB0_FILE_SHARE_WRITE,
                        NTB0_FILE_OPEN, 0, &hr2) == NTB0_ST_SUCCESS &&
                    hr2) {

                    before = ntb0_disk_copy_bytes();
                    {
                        int rdfd = ntb0_handle_fd(hr2);
                        size_t off = 0;

                        while (off < sz) {
                            ssize_t k = read(rdfd, got + off, sz - off);

                            if (k <= 0)
                                break;
                            off += (size_t)k;
                        }
                    }
                    after = ntb0_disk_copy_bytes();

                    plain = open("win7root/ntbox_copy_measure.bin",
                                 O_RDONLY | O_CLOEXEC);
                    ok = (after == before) && /* 0 بايت منسوخة عبر الصندوق */
                         memcmp(pattern, got, sz) == 0 &&
                         plain >= 0 &&
                         fstat(ntb0_handle_fd(hr2), &sb) == 0 &&
                         fstat(plain, &sr) == 0 &&
                         sb.st_dev == sr.st_dev && sb.st_ino == sr.st_ino;

                    if (ok) {
                        NTB0_OK("DISK-016",
                                "zero-copy measured: read path copied "
                                "0 bytes through the box (counter delta=%llu"
                                " over 1 MiB read), fd inode==real file "
                                "(ino=%llu)",
                                (unsigned long long)(after - before),
                                (unsigned long long)sb.st_ino);
                    } else {
                        NTB0_FAIL("DISK-016",
                                  "measure failed: delta=%llu memcmp=%d "
                                  "inode match=%d",
                                  (unsigned long long)(after - before),
                                  memcmp(pattern, got, sz) == 0,
                                  plain >= 0 && sb.st_dev == sr.st_dev &&
                                      sb.st_ino == sr.st_ino);
                        failures++;
                    }
                    if (plain >= 0)
                        close(plain);
                } else {
                    NTB0_FAIL("DISK-016", "second open failed: 0x%lx", st);
                    failures++;
                }
            } else {
                NTB0_FAIL("DISK-016", "pattern write failed");
                failures++;
            }
        } else {
            NTB0_FAIL("DISK-016", "cannot create measure file: 0x%lx", st);
            failures++;
        }

        /* تنظيف ملف القياس */
        if (hw) ntb0_disk_close(hw);
        if (hr2) ntb0_disk_close(hr2);
        hw = hr2 = 0;
        if (ntb0_disk_create_file(&us, NTB0_FILE_WRITE_DATA, 0,
                                  NTB0_FILE_OPEN, 0, &hw) == 0 && hw) {
            ntb0_disk_set_disposition(hw, 1);
            ntb0_disk_close(hw);
        }
        free_us(&us);
        free(pattern);
        free(got);
    }

    /* -------------------------------------------------------------- */
    /* الترجمة                                                         */
    /* -------------------------------------------------------------- */
    wipe_sandbox(); /* لا بقايا: ملفات/مجلد الباكتَرة ومقاييس القياس */
    if (h) ntb0_disk_close(h);
    free(ents);
    free_us(&ud);

    printf("\n=== result: %s (%d failure(s)) ===\n",
           failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
