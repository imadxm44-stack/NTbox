#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/eventfd.h>

#include "ntb0_handle.h"
#include "ntb0_async_io.h"
#include "ntb0_trace.h"
#include "ntb0_nt.h"

static int failures = 0;

static void lamp_ok(const char *id, const char *msg) {
    fprintf(stderr, "[ntbox][%s][OK] %s\n", id, msg);
}
static void lamp_fail(const char *id, const char *msg) {
    fprintf(stderr, "[ntbox][%s][FAIL] %s\n", id, msg);
    failures++;
}

/* ---- test state ---- */
static volatile int read_cb_n = 0;
static size_t read_cb_bytes = 0;
static long read_cb_status = 0;

static void read_cb(uint64_t h, int type, long status, size_t bytes, void *ctx) {
    (void)h; (void)type; (void)ctx;
    read_cb_n++;
    read_cb_bytes = bytes;
    read_cb_status = status;
}

static volatile int write_cb_n = 0;
static size_t write_cb_bytes = 0;

static void write_cb(uint64_t h, int type, long status, size_t bytes, void *ctx) {
    (void)h; (void)type; (void)status; (void)ctx;
    write_cb_n++;
    write_cb_bytes = bytes;
}

/* قراءة حقول IO_STATUS_BLOCK (تخطيط Win64) عبر memcpy (بدون type-puning) */
static long iosb_status(const void *iosb) {
    int32_t s;
    memcpy(&s, iosb, sizeof(s));
    return (long)s;
}
static size_t iosb_info(const void *iosb) {
    uint64_t v;
    memcpy(&v, (const char *)iosb + 8, sizeof(v));
    return (size_t)v;
}

/* مقياس حالة NTSTATUS مخزّن int32 (مُمَدَّد بالإشارة) مقابل ثابت 64-بت */
static int iosb_status_is(const void *iosb, long expected) {
    return iosb_status(iosb) == (long)(int32_t)expected;
}

/* --------------------------------------------------------------------- */
/* APC حقيقي عبر ntb0_call_win64 (T-072)                                  */
/*                                                                        */
/* الـAPC يُنادى بأصطلاح ويندوز x64:  rcx=ApcContext  rdx=IoStatusBlock  r8= */
/* لكن جسم الاستقبال C بصطلاح SysV (rdi/rsi/rdx). الدالة التجميعية التالية  */
/* جسر صغير يقلب المسجلات الموضّع تمهيداً لنداء SysV عبر قفزة ذيلية تحافظ     */
/* على محاذاة 16-بايت (مطابقة كلا الاصطلاحين في نقطة الدخول).               */
/* --------------------------------------------------------------------- */
void apc_sink(void *ctx, void *iosb, int reserved);
void ntb0test_apc_shim(void);   /* معرّفة بـasm أدناه في هذا الملف */

void *volatile apc_sink_ptr;

__asm__(".text\n\t"
        ".globl ntb0test_apc_shim\n\t"
        ".type ntb0test_apc_shim,@function\n"
        "ntb0test_apc_shim:\n\t"
        "  mov %rcx, %rdi\n\t"
        "  mov %rdx, %rsi\n\t"
        "  mov %r8d, %edx\n\t"
        "  jmp *apc_sink_ptr(%rip)\n");

static volatile uint64_t apc_ctx_seen = 0;
static void *apc_iosb_seen = NULL;
static volatile int apc_reserved = -1;
static volatile long apc_status_seen = -1;
static volatile size_t apc_info_seen = (size_t)-1;
static volatile int apc_called = 0;

void apc_sink(void *ctx, void *iosb, int reserved)
{
    apc_ctx_seen = (uint64_t)(uintptr_t)ctx;
    apc_iosb_seen = iosb;
    apc_reserved = reserved;
    if (iosb) {
        /* تخطيط IO_STATUS_BLOCK (Win64): [0..3]=Status [8..15]=Information */
        apc_status_seen = (long)(*(volatile int32_t *)iosb);
        apc_info_seen = *(volatile size_t *)(void *)((uintptr_t)iosb + 8);
    }
    apc_called++;
}

int main(void) {
    char path1[] = "/tmp/opencode/async_read.txt";
    char path2[] = "/tmp/opencode/async_write.txt";
    int fd;
    FILE *f;

    apc_sink_ptr = (void *)apc_sink;

    /* prepare read file: "HelloAsync!" (11 chars) */
    f = fopen(path1, "w");
    fputs("HelloAsync!", f);
    fclose(f);

    /* ---- ASY-001: async read (legacy cb API) ---- */
    {
        int r;
        uint64_t fh = 0, eh = 0;
        int efd;
        char buf[64];
        memset(buf, 0, sizeof(buf));

        fd = open(path1, O_RDONLY);
        if (fd < 0) { lamp_fail("ASY-001", "open read file failed"); goto done; }
        if (ntb0_handle_create(fd, NTB0_FILE_READ_DATA | NTB0_GENERIC_READ,
                               NTB0_FILE_SHARE_READ, path1, &fh) != NTB0_ST_SUCCESS) {
            close(fd); lamp_fail("ASY-001", "handle create failed"); goto done;
        }
        efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (efd < 0 || ntb0_handle_create_event(efd, &eh) != NTB0_ST_SUCCESS) {
            lamp_fail("ASY-001", "event handle create failed"); goto done;
        }

        read_cb_n = 0;
        r = (int)ntb0_async_submit_read(fh, buf, 10, eh, read_cb, NULL);
        if (r != NTB0_ASYNC_PENDING) {
            lamp_fail("ASY-001", "submit did not return PENDING");
        } else {
            /* wait for completion */
            ntb0_async_wait_one(fh, 2000);
            /* ensure drained */
            ntb0_async_poll();
            /* check buffer */
            int buf_ok = (memcmp(buf, "HelloAsync", 10) == 0);
            /* check event signaled */
            uint64_t v = 0;
            int ev_ok = (read(efd, &v, sizeof(v)) == (ssize_t)sizeof(v) && v >= 1);
            /* check callback */
            int cb_ok = (read_cb_n == 1 && read_cb_bytes == 10 &&
                         read_cb_status == NTB0_ST_SUCCESS);
            if (buf_ok && ev_ok && cb_ok)
                lamp_ok("ASY-001", "read pending->complete, event signaled, buffer filled");
            else
                lamp_fail("ASY-001", "read completion checks failed");
        }

        ntb0_handle_close(eh);
        ntb0_handle_close(fh);
    }

    /* ---- ASY-002: async write (legacy cb API) ---- */
    {
        int r;
        uint64_t fh = 0;
        const char *data = "WriteAsync123";
        size_t dlen = strlen(data);
        char outbuf[64];

        fd = open(path2, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { lamp_fail("ASY-002", "open write file failed"); goto done; }
        if (ntb0_handle_create(fd, NTB0_FILE_WRITE_DATA | NTB0_GENERIC_WRITE,
                               NTB0_FILE_SHARE_WRITE, path2, &fh) != NTB0_ST_SUCCESS) {
            close(fd); lamp_fail("ASY-002", "handle create failed"); goto done;
        }

        write_cb_n = 0;
        r = (int)ntb0_async_submit_write(fh, data, dlen, 0, write_cb, NULL);
        if (r != NTB0_ASYNC_PENDING) {
            lamp_fail("ASY-002", "submit did not return PENDING");
        } else {
            ntb0_async_wait_one(fh, 2000);
            ntb0_async_poll();
            /* read back from disk */
            int rfd = open(path2, O_RDONLY);
            memset(outbuf, 0, sizeof(outbuf));
            ssize_t n = read(rfd, outbuf, sizeof(outbuf) - 1);
            close(rfd);
            int data_ok = (n == (ssize_t)dlen && memcmp(outbuf, data, dlen) == 0);
            int cb_ok = (write_cb_n == 1 && write_cb_bytes == dlen);
            if (data_ok && cb_ok)
                lamp_ok("ASY-002", "write pending->complete, bytes on disk");
            else
                lamp_fail("ASY-002", "write completion checks failed");
        }

        ntb0_handle_close(fh);
    }

    /* ---- ASY-003: cancel ---- */
    {
        uint64_t fh = 0, eh = 0;
        int efd;
        char buf[64];
        memset(buf, 0, sizeof(buf));

        fd = open(path1, O_RDONLY);
        if (fd < 0) { lamp_fail("ASY-003", "open failed"); goto done; }
        if (ntb0_handle_create(fd, NTB0_FILE_READ_DATA | NTB0_GENERIC_READ,
                               NTB0_FILE_SHARE_READ, path1, &fh) != NTB0_ST_SUCCESS) {
            close(fd); lamp_fail("ASY-003", "handle create failed"); goto done;
        }
        efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        ntb0_handle_create_event(efd, &eh);

        (void)ntb0_async_submit_read(fh, buf, 10, eh, read_cb, NULL);
        ntb0_async_cancel(fh);
        if (ntb0_async_pending_count() == 0)
            lamp_ok("ASY-003", "pending op removed by cancel");
        else
            lamp_fail("ASY-003", "pending op still in queue after cancel");

        /* cleanup */
        ntb0_async_wait_one(fh, 500);
        ntb0_async_poll();
        ntb0_handle_close(eh);
        ntb0_handle_close(fh);
    }

    /* T-057 (overlap · ByteOffset) + T-072 (APC عبر ntb0_call_win64):
     * قراءة غير متزامنة عند إزاحة 5، IOSB يُعبّأ بالبايتات الفعلية، الـAPC
     * يُنادى بالاصطلاح الحقيقي لويندوز x64 مع (ApcContext, IoStatusBlock, 0). */
    {
        uint64_t fh = 0, eh = 0;
        int efd;
        char buf[32];
        long iosb[2];               /* 16 bytes — تخطيط IO_STATUS_BLOCK */
        long st = -1;
        size_t inf = 0;
        uint64_t ev = 0;
        int r;

        memset(buf, 0, sizeof(buf));
        memset(iosb, 0, sizeof(iosb));
        apc_called = 0;

        fd = open(path1, O_RDONLY);
        if (fd < 0) { lamp_fail("SYS-047", "open failed"); goto done; }
        if (ntb0_handle_create(fd, NTB0_FILE_READ_DATA | NTB0_GENERIC_READ,
                               NTB0_FILE_SHARE_READ, path1, &fh) != NTB0_ST_SUCCESS) {
            close(fd); lamp_fail("SYS-047", "handle create failed"); goto done;
        }
        efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (efd < 0 || ntb0_handle_create_event(efd, &eh) != NTB0_ST_SUCCESS) {
            lamp_fail("SYS-047", "event create failed"); goto done;
        }

        r = (int)ntb0_async_submit_read_ex(fh, eh, buf, 6, (void *)iosb, 5,
                                           (void *)&ntb0test_apc_shim,
                                           0xA11CEULL, NULL, NULL);
        int pending_ok = (r == (int)NTB0_ASYNC_PENDING);
        if (!pending_ok)
            lamp_fail("SYS-047", "immediate STATUS_PENDING missing");

        ntb0_async_wait_one(fh, 2000);
        int reaped = ntb0_async_next_done(fh, &st, &inf);

        /* IOSB مملوء فعلياً بالبايتات (كتبه worker قبل التوصيل) */
        int iosb_ok = (iosb_status_is(iosb, NTB0_ST_SUCCESS) &&
                       iosb_info(iosb) == 6);
        int buf_ok = (memcmp(buf, "Async!", 6) == 0);
        int reap_ok = (reaped == 1 && st == NTB0_ST_SUCCESS && inf == 6);

        /* الـAPC وصل عبر الجسر win64: ctx، مؤشر IOSB نفسه، القيم الموافقة */
        int apc_ok = (apc_called == 1 &&
                      apc_ctx_seen == 0xA11CEULL &&
                      apc_iosb_seen == (void *)iosb &&
                      apc_status_seen == NTB0_ST_SUCCESS &&
                      apc_info_seen == 6 &&
                      apc_reserved == 0);

        int ev_ok = (read(efd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev) && ev >= 1);

        if (pending_ok && iosb_ok && buf_ok && reap_ok && apc_ok && ev_ok) {
            lamp_ok("SYS-047",
                    "T-057/T-072: overlapped read@5 -> IOSB{SUCCESS,6}, "
                    "APC via ntb0_call_win64 (win64 ABI), event signaled");
        } else {
            fprintf(stderr, "[ntbox][SYS-047][DBG] pending=%d iosb=%d buf=%d "
                    "reap=%d apc=%d ev=%d (apc_called=%d ctx=%llx iosb=%p "
                    "st=%lx info=%zx rsvd=%d)\n",
                    pending_ok, iosb_ok, buf_ok, reap_ok, apc_ok, ev_ok,
                    apc_called, (unsigned long long)apc_ctx_seen,
                    apc_iosb_seen, (unsigned long)apc_status_seen,
                    apc_info_seen, apc_reserved);
            lamp_fail("SYS-047", "async read APC/IoS/overlap checks failed");
        }

        ntb0_handle_close(eh);
        ntb0_handle_close(fh);
    }

    /* T-073 — كتابة غير متزامنة بـAPC + IOSB بحجم فعلي */
    {
        uint64_t fh = 0;
        const char *data = "WriteAPC!";
        size_t dlen = strlen(data);
        char outbuf[64];
        long iosb[2];
        long st = -1;
        size_t inf = 0;
        int r;
        int apc_before = apc_called;

        memset(outbuf, 0, sizeof(outbuf));
        memset(iosb, 0, sizeof(iosb));

        fd = open(path2, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { lamp_fail("SYS-054", "open failed"); goto done; }
        if (ntb0_handle_create(fd, NTB0_FILE_WRITE_DATA | NTB0_GENERIC_WRITE,
                               NTB0_FILE_SHARE_WRITE, path2, &fh) != NTB0_ST_SUCCESS) {
            close(fd); lamp_fail("SYS-054", "handle create failed"); goto done;
        }

        r = (int)ntb0_async_submit_write_ex(fh, 0, data, dlen, (void *)iosb, -1,
                                            (void *)&ntb0test_apc_shim,
                                            0xBEEFULL, NULL, NULL);
        int pending_ok = (r == (int)NTB0_ASYNC_PENDING);

        ntb0_async_wait_one(fh, 2000);
        int reaped = ntb0_async_next_done(fh, &st, &inf);

        int iosb_ok = (iosb_status_is(iosb, NTB0_ST_SUCCESS) &&
                       iosb_info(iosb) == dlen);
        int reap_ok = (reaped == 1 && st == NTB0_ST_SUCCESS && inf == dlen);

        int rfd = open(path2, O_RDONLY);
        ssize_t n = read(rfd, outbuf, sizeof(outbuf) - 1);
        close(rfd);
        int data_ok = (n == (ssize_t)dlen && memcmp(outbuf, data, dlen) == 0);

        int apc_ok = (apc_called == apc_before + 1 &&
                      apc_ctx_seen == 0xBEEFULL &&
                      apc_iosb_seen == (void *)iosb &&
                      apc_status_seen == NTB0_ST_SUCCESS &&
                      apc_info_seen == dlen);

        /* الكتابة بموضع fd (byte_offset=-1) تقدّم مؤشر المفتاح فعلياً */
        int pos_ok = (lseek(fd, 0, SEEK_CUR) == (off_t)dlen);

        if (pending_ok && iosb_ok && reap_ok && data_ok && apc_ok && pos_ok) {
            lamp_ok("SYS-054",
                    "T-073: async write -> IOSB{SUCCESS,len}, APC invoked, "
                    "bytes on disk, fd pos advanced");
        } else {
            fprintf(stderr, "[ntbox][SYS-054][DBG] pending=%d iosb=%d reap=%d "
                    "data=%d apc=%d pos=%d\n",
                    pending_ok, iosb_ok, reap_ok, data_ok, apc_ok, pos_ok);
            lamp_fail("SYS-054", "async write APC/IoS checks failed");
        }

        ntb0_handle_close(fh);
    }

    /* قائمة إتمام لكل FileHandle (T-057) + NtCancelIoFile:
     * طلبان متزامنان على نفس المقبض بإزاحتين (overlap)، ثم طلب ثالث لاغٍ —
     * الإلغاء = 0 pending، IOSB بـSTATUS_CANCELLED، eventfd يُنبَّه. */
    {
        uint64_t fh = 0, eh = 0;
        int efd;
        char buf1[32], buf2[32];
        long iosb1[2], iosb2[2], iosb3[2];
        long st;
        size_t inf;
        uint64_t ev = 0;
        int r1, r2, r3, pair_ok, cancel_iosb_ok, cancel_ev_ok;

        memset(buf1, 0, sizeof(buf1));
        memset(buf2, 0, sizeof(buf2));
        memset(iosb1, 0, sizeof(iosb1));
        memset(iosb2, 0, sizeof(iosb2));
        memset(iosb3, 0, sizeof(iosb3));

        fd = open(path1, O_RDONLY);
        if (fd < 0) { lamp_fail("SYS-055", "open failed"); goto done; }
        if (ntb0_handle_create(fd, NTB0_FILE_READ_DATA | NTB0_GENERIC_READ,
                               NTB0_FILE_SHARE_READ, path1, &fh) != NTB0_ST_SUCCESS) {
            close(fd); lamp_fail("SYS-055", "handle create failed"); goto done;
        }
        efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (efd < 0 || ntb0_handle_create_event(efd, &eh) != NTB0_ST_SUCCESS) {
            lamp_fail("SYS-055", "event create failed"); goto done;
        }

        /* طلبان متراكبان على نفس المقبض (overlap حقيقي) */
        r1 = (int)ntb0_async_submit_read_ex(fh, 0, buf1, 4, (void *)iosb1, 0,
                                            NULL, 0, NULL, NULL);
        r2 = (int)ntb0_async_submit_read_ex(fh, 0, buf2, 8, (void *)iosb2, 3,
                                            NULL, 0, NULL, NULL);
        int overlap_ok = (r1 == (int)NTB0_ASYNC_PENDING &&
                          r2 == (int)NTB0_ASYNC_PENDING);

        ntb0_async_wait_one(fh, 2000);
        ntb0_async_wait_one(fh, 2000);

        /* كل IOSB عبّأ عينه (مستقلاً عن ترتيب السحب) */
        int iosb1_ok = (iosb_status_is(iosb1, NTB0_ST_SUCCESS) &&
                        iosb_info(iosb1) == 4 &&
                        memcmp(buf1, "Hell", 4) == 0);
        int iosb2_ok = (iosb_status_is(iosb2, NTB0_ST_SUCCESS) &&
                        iosb_info(iosb2) == 8 &&
                        memcmp(buf2, "loAsync!", 8) == 0);

        /* تُسحَب النتائج واحدة واحدة من قائمة إتمام المقبض — كل عنصر مرة واحدة */
        int drain_ok = (ntb0_async_next_done(fh, &st, &inf) == 1 &&
                        ntb0_async_next_done(fh, &st, &inf) == 1 &&
                        ntb0_async_next_done(fh, &st, &inf) == 0);
        pair_ok = (iosb1_ok && iosb2_ok && drain_ok);

        /* NtCancelIoFile: طلب ثالث في طابور ثم إلغاء */
        memset(buf1, 0, sizeof(buf1));
        r3 = (int)ntb0_async_submit_read_ex(fh, eh, buf1, 4, (void *)iosb3, 0,
                                            NULL, 0, NULL, NULL);
        ntb0_async_cancel(fh);
        int cancel_pending_ok = (ntb0_async_pending_count() == 0);
        cancel_iosb_ok = iosb_status_is(iosb3, NTB0_ST_CANCELLED);
        cancel_ev_ok = (read(efd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev) &&
                        ev >= 1);

        ntb0_async_poll();          /* تنظيف أي بقايا إتمام */

        if (overlap_ok && pair_ok && cancel_pending_ok && cancel_iosb_ok &&
            cancel_ev_ok && r3 == (int)NTB0_ASYNC_PENDING) {
            lamp_ok("SYS-055",
                    "T-057: 2 overlapping reads on one handle, per-handle "
                    "completion queue drained once each; NtCancelIoFile -> "
                    "0 pending, IOSB=STATUS_CANCELLED, event signaled");
        } else {
            fprintf(stderr, "[ntbox][SYS-055][DBG] overlap=%d pair=%d "
                    "cancel_pending=%d cancel_iosb=%d cancel_ev=%d "
                    "(iosb1_ok=%d iosb2_ok=%d drain=%d) "
                    "i1{st=%lx inf=%zx b1='%.4s'} i2{st=%lx inf=%zx b2='%.8s'} "
                    "i3{st=%lx}\n",
                    overlap_ok, pair_ok, cancel_pending_ok, cancel_iosb_ok,
                    cancel_ev_ok, iosb1_ok, iosb2_ok, drain_ok,
                    iosb_status(iosb1), iosb_info(iosb1), buf1,
                    iosb_status(iosb2), iosb_info(iosb2), buf2,
                    iosb_status(iosb3));
            lamp_fail("SYS-055", "overlap/cancel checks failed");
        }

        ntb0_handle_close(eh);
        ntb0_handle_close(fh);
    }

done:
    unlink(path1);
    unlink(path2);

    if (failures == 0) {
        fprintf(stderr, "[ntbox][ASY-SUMMARY][OK] all async io lamps passed\n");
        return 0;
    }
    fprintf(stderr, "[ntbox][ASY-SUMMARY][FAIL] %d failure(s)\n", failures);
    return 1;
}
