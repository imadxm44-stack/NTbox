#include "ntbox.h"
#include "ntbox_types.h"
#include "linux_bridge.h"
#include "nt_object.h"
#include "pe_loader.h"
#include "irp_core.h"
#include "ntapi_stubs.h"
#include "hal_mini.h"
#include "ntb_trace.h"

#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/fs.h>
#include <linux/fcntl.h>

extern int ntbox_box_init(void);
extern void ntbox_box_exit(void);

static struct proc_dir_entry *ntbox_proc_dir;
static struct proc_dir_entry *ntbox_proc_load;

/* Simulated DriverEntry for the Demo (stands in for the .sys entry point). */
static NTSTATUS ntbox_simulated_driver_entry(PDRIVER_OBJECT DriverObject,
                                             PUNICODE_STRING RegistryPath)
{
    (void)RegistryPath;
    if (!DriverObject)
        return STATUS_UNSUCCESSFUL;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = ntbox_dummy_dispatch;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = ntbox_dummy_dispatch;
    return STATUS_SUCCESS;
}

/* Shared analysis (used by /proc/ntbox/load and by M2 open-in-place). */
int ntbox_analyze_pe(const char *buf, size_t size, const char *path)
{
    unsigned long entry;
    PDRIVER_OBJECT drv;
    NTSTATUS status;
    int ret;

    (void)path;
    ret = ntbox_pe_check((void *)buf, size);
    if (ret) {
        return ret;
    }
    entry = ntbox_pe_get_entry((void *)buf);
    (void)entry;

    drv = ntbox_create_driver_object();
    if (!drv) {
        NTBOX_ERR("obj_mgr: failed to create DRIVER_OBJECT\n");
        return -ENOMEM;
    }

    NTBOX_LOG("calling DriverEntry\n");
    status = ntbox_simulated_driver_entry(drv, NULL);
    NTBOX_LOG("returned STATUS_SUCCESS (0x%08x)\n", (unsigned int)status);
    if (status != STATUS_SUCCESS) {
        ntbox_destroy_driver_object(drv);
        return -EIO;
    }

    ret = ntbox_test_create_close(drv);
    if (ret != 0) {
        NTBOX_ERR("irp_core: test failed %d\n", ret);
        ntbox_destroy_driver_object(drv);
        return ret;
    }

    NTBOX_LOG("NT-Box Demo Test: PASSED\n");
    ntbox_destroy_driver_object(drv);
    return 0;
}

static int ntbox_load_driver(const char *path)
{
    struct file *f;
    loff_t pos = 0;
    ssize_t n;
    size_t size;
    char *buf;
    int ret;

    NTBOX_LOG("load: received path\n");

    f = filp_open(path, O_RDONLY, 0);
    if (IS_ERR(f)) {
        NTBOX_ERR("load: cannot open %s (err=%ld)\n", path, PTR_ERR(f));
        return PTR_ERR(f);
    }
    size = (size_t)i_size_read(file_inode(f));
    if (size == 0 || size > (16 * 1024 * 1024)) {
        NTBOX_ERR("load: bad file size %lu\n", (unsigned long)size);
        filp_close(f, NULL);
        return -EINVAL;
    }
    buf = vmalloc(size);
    if (!buf) {
        filp_close(f, NULL);
        return -ENOMEM;
    }
    n = kernel_read(f, buf, size, &pos);
    filp_close(f, NULL);
    if (n != (ssize_t)size) {
        NTBOX_ERR("load: short read %ld/%lu\n", (long)n, (unsigned long)size);
        vfree(buf);
        return -EIO;
    }

    ret = ntbox_analyze_pe(buf, size, path);
    vfree(buf);
    return ret;
}

static ssize_t ntbox_load_write(struct file *file, const char __user *ubuf,
                                size_t count, loff_t *ppos)
{
    char *kbuf;
    size_t len = count;
    int ret;

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
    /* strip trailing newline */
    while (len > 0 && (kbuf[len - 1] == '\n' || kbuf[len - 1] == '\r')) {
        kbuf[len - 1] = '\0';
        len--;
    }

    ret = ntbox_load_driver(kbuf);
    kfree(kbuf);
    if (ret)
        return ret;
    return count;
}

static const struct proc_ops ntbox_load_proc_ops = {
    .proc_write = ntbox_load_write,
};

static int __init ntbox_init(void)
{
    int ret;

    ret = ntbox_box_init();
    if (ret)
        return ret;
    ret = ntbox_hal_init();
    if (ret) {
        ntbox_box_exit();
        return ret;
    }

    ntbox_proc_dir = proc_mkdir("ntbox", NULL);
    if (!ntbox_proc_dir) {
        ntbox_box_exit();
        return -ENOMEM;
    }
    ntbox_proc_load = proc_create("load", 0666, ntbox_proc_dir,
                                  &ntbox_load_proc_ops);
    if (!ntbox_proc_load) {
        remove_proc_entry("ntbox", NULL);
        ntbox_box_exit();
        return -ENOMEM;
    }

    ret = ntb_diag_init(ntbox_proc_dir);
    if (ret) {
        remove_proc_entry("load", ntbox_proc_dir);
        remove_proc_entry("ntbox", NULL);
        ntbox_box_exit();
        return ret;
    }

    /* M2: partitions + file access in place */
    ret = ntbox_part_init(ntbox_proc_dir);
    if (ret) {
        ntb_diag_exit(ntbox_proc_dir);
        remove_proc_entry("load", ntbox_proc_dir);
        remove_proc_entry("ntbox", NULL);
        ntbox_box_exit();
        return ret;
    }
    ret = ntbox_file_init(ntbox_proc_dir);
    if (ret) {
        ntbox_part_exit(ntbox_proc_dir);
        ntb_diag_exit(ntbox_proc_dir);
        remove_proc_entry("load", ntbox_proc_dir);
        remove_proc_entry("ntbox", NULL);
        ntbox_box_exit();
        return ret;
    }
    return 0;
}

static void __exit ntbox_exit(void)
{
    extern void ntb_threads_exit(void);
    ntb_threads_exit();
    ntbox_file_exit(ntbox_proc_dir);
    ntbox_part_exit(ntbox_proc_dir);
    ntb_diag_exit(ntbox_proc_dir);
    if (ntbox_proc_load)
        remove_proc_entry("load", ntbox_proc_dir);
    if (ntbox_proc_dir)
        remove_proc_entry("ntbox", NULL);
    ntbox_box_exit();
}

module_init(ntbox_init);
module_exit(ntbox_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("DublKernel (Local Research)");
MODULE_DESCRIPTION("NT-Box minimal demo for feasibility test");
MODULE_VERSION("0.1.0-demo");