/*
 * DublKernel M2 — فتح ملفات من البرتشن في مكانها (PLAN_Partitions_NTFS.md)
 * كل شيء عبر linux_bridge (filp_open/kernel_read) — لا نسخ.
 */

#include "ntbox.h"
#include "ntbox_types.h"
#include "linux_bridge.h"
#include "ntb_trace.h"
#include "pe_loader.h"

#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/ctype.h>

#define NTB_RESULT_CAP 1024

#define NTB_FB_W 320
#define NTB_FB_H 240
#define NTB_FB_SIZE (NTB_FB_W * NTB_FB_H * 4)

static u32 *ntb_fb;

static char ntb_result_buf[NTB_RESULT_CAP];
static size_t ntb_result_len;
static DEFINE_MUTEX(ntb_result_lock);

void ntbox_result_set(const char *fmt, ...)
{
    va_list ap;
    mutex_lock(&ntb_result_lock);
    va_start(ap, fmt);
    ntb_result_len = (size_t)vsnprintf(ntb_result_buf, NTB_RESULT_CAP, fmt, ap);
    va_end(ap);
    if (ntb_result_len < NTB_RESULT_CAP - 1) {
        ntb_result_buf[ntb_result_len] = '\n';
        ntb_result_buf[ntb_result_len + 1] = '\0';
        ntb_result_len++;
    }
    mutex_unlock(&ntb_result_lock);
}

/* ---------- proc: result (read) ---------- */

static ssize_t ntb_result_read(struct file *file, char __user *ubuf,
                               size_t count, loff_t *ppos)
{
    size_t n;
    char *tmp;

    if (*ppos == 0) {
        /* nothing to reset */
    }
    mutex_lock(&ntb_result_lock);
    tmp = kmalloc(ntb_result_len + 1, GFP_KERNEL);
    if (tmp) {
        memcpy(tmp, ntb_result_buf, ntb_result_len + 1);
    }
    n = ntb_result_len;
    mutex_unlock(&ntb_result_lock);
    if (!tmp)
        return -ENOMEM;
    if (*ppos >= (loff_t)n) {
        kfree(tmp);
        return 0;
    }
    if ((size_t)*ppos + count > n)
        count = n - (size_t)*ppos;
    if (copy_to_user(ubuf, tmp + *ppos, count)) {
        kfree(tmp);
        return -EFAULT;
    }
    *ppos += count;
    kfree(tmp);
    NTB_OK(NTB_TID_PAR_011, "result read %zu bytes", count);
    return (ssize_t)count;
}

/* ---------- proc: open (write) ---------- */

static ssize_t ntb_open_write(struct file *file, const char __user *ubuf,
                              size_t count, loff_t *ppos)
{
    char *kbuf, *rpath;
    size_t len = count;
    int ret;
    char *buf = NULL;
    size_t size = 0;

    if (len == 0)
        return 0;
    if (len > 4096)
        len = 4096;
    kbuf = kmalloc(len + 1, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;
    if (copy_from_user(kbuf, ubuf, len)) {
        kfree(kbuf);
        return -EFAULT;
    }
    kbuf[len] = '\0';
    while (len > 0 && (kbuf[len - 1] == '\n' || kbuf[len - 1] == '\r')) {
        kbuf[len - 1] = '\0';
        len--;
    }

    rpath = kmalloc(512, GFP_KERNEL);
    if (!rpath) {
        kfree(kbuf);
        return -ENOMEM;
    }
    ret = ntbox_part_resolve(kbuf, rpath, 512);
    if (ret) {
        NTB_FAIL(NTB_TID_PAR_008, ret, "resolve %s", kbuf);
        ntbox_result_set("open: cannot resolve %s", kbuf);
        kfree(rpath);
        kfree(kbuf);
        return ret;
    }

    NTB_OK(NTB_TID_PAR_008, "resolved %s -> %s", kbuf, rpath);

    ret = ntb_file_open_read(rpath, &buf, &size);
    if (ret) {
        NTB_FAIL(NTB_TID_PAR_008, ret, "open %s", rpath);
        ntbox_result_set("open: cannot open %s (err=%d)", rpath, ret);
        kfree(rpath);
        kfree(kbuf);
        return ret;
    }

    /* PE؟ */
    if (size >= 2 && buf[0] == 'M' && buf[1] == 'Z') {
        int pr = ntbox_run_real_pe(buf, size, rpath);
        if (pr == 0) {
            NTB_OK(NTB_TID_PAR_009, "real PE in place PASSED (%zu bytes)", size);
            ntbox_result_set("open: %s real PE PASSED in place (%zu bytes)",
                             kbuf, size);
        } else {
            NTB_FAIL(NTB_TID_PAR_009, pr, "real PE %s", rpath);
            ntbox_result_set("open: %s real PE failed (%d) — see dmesg", kbuf, pr);
        }
    } else {
        /* ملف آخر: معلومات + أول 16 بايت */
        char hex[64];
        size_t i, hn = (size < 16) ? size : 16;
        size_t w = 0;

        for (i = 0; i < hn; i++)
            w += (size_t)snprintf(hex + w, sizeof(hex) - w, "%02x ", (unsigned char)buf[i]);
        hex[w] = '\0';
        NTB_OK(NTB_TID_PAR_010, "opened non-PE %s (%zu bytes)", rpath, size);
        ntbox_result_set("open: %s (%zu bytes) first bytes: %s", kbuf, size, hex);
    }

    ntb_file_close_buf(buf);
    kfree(rpath);
    kfree(kbuf);
    return count;
}

/* ---------- proc: runuser (write) ---------- */

static ssize_t ntb_runuser_write(struct file *file, const char __user *ubuf,
                                 size_t count, loff_t *ppos)
{
    char *kbuf, *rpath;
    size_t len = count;
    int ret;
    char *buf = NULL;
    size_t size = 0;

    if (len == 0)
        return 0;
    if (len > 4096)
        len = 4096;
    kbuf = kmalloc(len + 1, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;
    if (copy_from_user(kbuf, ubuf, len)) {
        kfree(kbuf);
        return -EFAULT;
    }
    kbuf[len] = '\0';
    while (len > 0 && (kbuf[len - 1] == '\n' || kbuf[len - 1] == '\r')) {
        kbuf[len - 1] = '\0';
        len--;
    }

    rpath = kmalloc(512, GFP_KERNEL);
    if (!rpath) {
        kfree(kbuf);
        return -ENOMEM;
    }
    ret = ntbox_part_resolve(kbuf, rpath, 512);
    if (ret) {
        ntbox_result_set("runuser: cannot resolve %s", kbuf);
        kfree(rpath);
        kfree(kbuf);
        return ret;
    }

    ret = ntb_file_open_read(rpath, &buf, &size);
    if (ret) {
        ntbox_result_set("runuser: cannot open %s (err=%d)", rpath, ret);
        kfree(rpath);
        kfree(kbuf);
        return ret;
    }

    ret = ntbox_run_user_pe(buf, size, rpath);
    if (ret == 0) {
        ntbox_result_set("runuser: %s PASSED (%zu bytes)", kbuf, size);
    } else {
        ntbox_result_set("runuser: %s failed (%d)", kbuf, ret);
    }

    ntb_file_close_buf(buf);
    kfree(rpath);
    kfree(kbuf);
    return (ret == 0) ? count : ret;
}

/* ---------- proc: runexe (write) — أمر موحد لتشغيل أي ملفexe (32-bit أو 64-bit) ---------- */

static ssize_t ntb_runexe_write(struct file *file, const char __user *ubuf,
                                size_t count, loff_t *ppos)
{
    /* runexe هو الأمر الموحد: يجري أي ملف 실행ية (PE32 x86 أو PE32+ x64)
     * عبر ntbox_run_user_pe الذي يكشف العمارة تلقائياً من رأس الملف. */
    return ntb_runuser_write(file, ubuf, count, ppos);
}

static const struct proc_ops ntb_runexe_ops = { .proc_write = ntb_runexe_write };

/* ---------- proc: gpu_driver (read/write) — مسار تعريف كرت الشاشة ---------- */

#define NTB_GPU_PATH_CAP 512
static char ntb_gpu_driver_path[NTB_GPU_PATH_CAP];

static ssize_t ntb_gpu_driver_read(struct file *file, char __user *ubuf,
                                   size_t count, loff_t *ppos)
{
    size_t len = strnlen(ntb_gpu_driver_path, NTB_GPU_PATH_CAP);
    if (*ppos >= (loff_t)len)
        return 0;
    if ((size_t)*ppos + count > len)
        count = len - (size_t)*ppos;
    if (copy_to_user(ubuf, ntb_gpu_driver_path + *ppos, count))
        return -EFAULT;
    *ppos += count;
    return (ssize_t)count;
}

static ssize_t ntb_gpu_driver_write(struct file *file, const char __user *ubuf,
                                    size_t count, loff_t *ppos)
{
    char *kbuf;
    size_t len = count;

    if (len == 0)
        return 0;
    if (len > NTB_GPU_PATH_CAP - 1)
        len = NTB_GPU_PATH_CAP - 1;
    kbuf = kmalloc(len + 1, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;
    if (copy_from_user(kbuf, ubuf, len)) {
        kfree(kbuf);
        return -EFAULT;
    }
    kbuf[len] = '\0';
    while (len > 0 && (kbuf[len - 1] == '\n' || kbuf[len - 1] == '\r')) {
        kbuf[len - 1] = '\0';
        len--;
    }
    strncpy(ntb_gpu_driver_path, kbuf, NTB_GPU_PATH_CAP - 1);
    ntb_gpu_driver_path[NTB_GPU_PATH_CAP - 1] = '\0';
    kfree(kbuf);

    NTB_OK(NTB_TID_GPU_008, "gpu_driver path set: %s", ntb_gpu_driver_path);
    ntbox_result_set("gpu_driver: %s", ntb_gpu_driver_path);
    return count;
}

static const struct proc_ops ntb_gpu_driver_ops = {
    .proc_read = ntb_gpu_driver_read,
    .proc_write = ntb_gpu_driver_write,
};

/* ---------- proc: fb (read) ---------- */
static ssize_t ntb_fb_read(struct file *file, char __user *ubuf,
                           size_t count, loff_t *ppos)
{
    size_t total = NTB_FB_SIZE;
    if (*ppos >= (loff_t)total)
        return 0;
    if ((size_t)*ppos + count > total)
        count = total - (size_t)*ppos;
    if (copy_to_user(ubuf, (char *)ntb_fb + *ppos, count))
        return -EFAULT;
    *ppos += count;
    return (ssize_t)count;
}

static const struct proc_ops ntb_fb_ops = { .proc_read = ntb_fb_read };

/* ---------- init/exit ---------- */

static const struct proc_ops ntb_open_ops = { .proc_write = ntb_open_write };
static const struct proc_ops ntb_result_ops = { .proc_read = ntb_result_read };

static const struct proc_ops ntb_runuser_ops = { .proc_write = ntb_runuser_write };

int ntbox_file_init(struct proc_dir_entry *dir)
{
    size_t i;
    if (!proc_create("open", 0666, dir, &ntb_open_ops))
        return -ENOMEM;
    if (!proc_create("runuser", 0666, dir, &ntb_runuser_ops)) {
        remove_proc_entry("open", dir);
        return -ENOMEM;
    }
    if (!proc_create("runexe", 0666, dir, &ntb_runexe_ops)) {
        remove_proc_entry("runuser", dir);
        remove_proc_entry("open", dir);
        return -ENOMEM;
    }
    if (!proc_create("gpu_driver", 0666, dir, &ntb_gpu_driver_ops)) {
        remove_proc_entry("runexe", dir);
        remove_proc_entry("runuser", dir);
        remove_proc_entry("open", dir);
        return -ENOMEM;
    }
    if (!proc_create("result", 0444, dir, &ntb_result_ops)) {
        remove_proc_entry("runexe", dir);
        remove_proc_entry("runuser", dir);
        remove_proc_entry("open", dir);
        return -ENOMEM;
    }
    if (!proc_create("fb", 0444, dir, &ntb_fb_ops)) {
        remove_proc_entry("result", dir);
        remove_proc_entry("runuser", dir);
        remove_proc_entry("open", dir);
        return -ENOMEM;
    }
    ntb_fb = vzalloc(NTB_FB_SIZE);
    if (!ntb_fb) {
        remove_proc_entry("fb", dir);
        remove_proc_entry("result", dir);
        remove_proc_entry("runuser", dir);
        remove_proc_entry("open", dir);
        return -ENOMEM;
    }
    for (i = 0; i < NTB_FB_W * NTB_FB_H; i++) {
        u8 r = (i % NTB_FB_W) * 255 / NTB_FB_W;
        u8 g = (i / NTB_FB_W) * 255 / NTB_FB_H;
        ntb_fb[i] = 0xFF000000u | (r << 16) | (g << 8) | 0x40;
    }
    ntbox_result_set("ready");
    NTB_OK(NTB_TID_PAR_008, "file module ready");
    return 0;
}

void ntbox_file_exit(struct proc_dir_entry *dir)
{
    if (ntb_fb) {
        vfree(ntb_fb);
        ntb_fb = NULL;
    }
    remove_proc_entry("fb", dir);
    remove_proc_entry("result", dir);
    remove_proc_entry("gpu_driver", dir);
    remove_proc_entry("runexe", dir);
    remove_proc_entry("runuser", dir);
    remove_proc_entry("open", dir);
}