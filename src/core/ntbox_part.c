/*
 * DublKernel M2 — إدارة البرتشنز/الأقراص (PLAN_Partitions_NTFS.md + PLAN_FINAL §5 M2)
 * الصندوق لا يلمس القرص مباشرة — كل شيء عبر linux_bridge.
 * كل عملية = معرّف PAR- + مصباح (NTB_OK/FAIL).
 */

#include "ntbox.h"
#include "ntbox_types.h"
#include "linux_bridge.h"
#include "ntb_trace.h"

#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/ctype.h>
#include <linux/mutex.h>
#include <linux/vmalloc.h>

#define NTB_MAX_DRIVES   16
#define NTB_MAX_ALLOWED  16
#define NTB_RESULT_CAP   1024

struct ntb_drive {
    char dev[128];
    char letter;
    char mount[128];
    int attached;
};

static struct ntb_drive ntb_drives[NTB_MAX_DRIVES];
static int ntb_ndrives;

static char ntb_allowed[NTB_MAX_ALLOWED][128];
static int ntb_nallowed;

static struct proc_dir_entry *ntb_proc_part_dir;

/* ---------- helpers ---------- */

static int ntb_is_allowed(const char *dev)
{
    int i;
    for (i = 0; i < ntb_nallowed; i++)
        if (strcmp(ntb_allowed[i], dev) == 0)
            return 1;
    return 0;
}

static void ntb_allow_dev(const char *dev)
{
    if (ntb_is_allowed(dev))
        return;
    if (ntb_nallowed >= NTB_MAX_ALLOWED)
        return;
    strncpy(ntb_allowed[ntb_nallowed], dev, 127);
    ntb_allowed[ntb_nallowed][127] = '\0';
    ntb_nallowed++;
}

static void ntb_deny_dev(const char *dev)
{
    int i, j;
    for (i = 0; i < ntb_nallowed; i++) {
        if (strcmp(ntb_allowed[i], dev) == 0) {
            for (j = i; j < ntb_nallowed - 1; j++)
                strcpy(ntb_allowed[j], ntb_allowed[j + 1]);
            ntb_nallowed--;
            return;
        }
    }
}

static struct ntb_drive *ntb_find_drive(char letter)
{
    int i;
    for (i = 0; i < ntb_ndrives; i++)
        if (ntb_drives[i].attached && ntb_drives[i].letter == letter)
            return &ntb_drives[i];
    return NULL;
}

int ntbox_part_resolve(const char *in, char *out, size_t outsz)
{
    struct ntb_drive *d;
    const char *rest;
    size_t w;

    if (!in || !out || outsz == 0)
        return -EINVAL;
    if (isalpha((unsigned char)in[0]) &&
        (in[1] == ':' || in[1] == '\\' || in[1] == '/')) {
        d = ntb_find_drive((char)toupper((unsigned char)in[0]));
        if (!d)
            return -ENOENT;
        w = (size_t)snprintf(out, outsz, "%s", d->mount);
        rest = in + 2;
        while (*rest == '\\' || *rest == '/')
            rest++;
        if (*rest) {
            if (w < outsz - 1 && out[w - 1] != '/')
                w += (size_t)snprintf(out + w, outsz - w, "/");
            /* copy rest, converting backslashes */
            while (*rest && w < outsz - 1) {
                out[w++] = (*rest == '\\') ? '/' : *rest;
                rest++;
            }
            out[w] = '\0';
        }
        return 0;
    }
    strncpy(out, in, outsz - 1);
    out[outsz - 1] = '\0';
    return 0;
}

/* ---------- proc: partitions (read) ---------- */

static ssize_t ntb_partitions_read(struct file *file, char __user *ubuf,
                                    size_t count, loff_t *ppos)
{
    struct ntb_partition parts[64];
    int n, i;
    char *out;
    size_t cap = 4096, w = 0;
    ssize_t ret;

    if (*ppos > 0)
        return 0;
    out = kmalloc(cap, GFP_KERNEL);
    if (!out)
        return -ENOMEM;

    n = ntb_partitions_scan(parts, 64);
    NTB_OK(NTB_TID_PAR_001, "scanned %d partitions", n);

    w = (size_t)snprintf(out + w, cap - w,
                         "NAME            BLOCKS       ALLOWED  ATTACHED\n");
    for (i = 0; i < n; i++) {
        struct ntb_drive *d = NULL;
        int j;
        char letter = '-';
        char mount[64] = "-";

        if (ntb_is_allowed(parts[i].name)) {
            for (j = 0; j < ntb_ndrives; j++)
                if (ntb_drives[j].attached &&
                    strcmp(ntb_drives[j].dev, parts[i].name) == 0) {
                    d = &ntb_drives[j];
                    letter = d->letter;
                    strncpy(mount, d->mount, 63);
                    mount[63] = '\0';
                    break;
                }
        }
        w += (size_t)snprintf(out + w, cap - w,
            "%-15s %-12llu %-8s %c %s\n",
            parts[i].name, parts[i].blocks,
            ntb_is_allowed(parts[i].name) ? "yes" : "no",
            letter, mount);
        if (w >= cap - 64)
            break;
    }
    w += (size_t)snprintf(out + w, cap - w,
        "\nallowed devices: %d\n", ntb_nallowed);

    if (w > count)
        w = count;
    if (copy_to_user(ubuf, out, w)) {
        kfree(out);
        return -EFAULT;
    }
    *ppos = w;
    kfree(out);
    ret = (ssize_t)w;
    return ret;
}

/* ---------- proc: policy (write) ---------- */

static ssize_t ntb_policy_write(struct file *file, const char __user *ubuf,
                                size_t count, loff_t *ppos)
{
    char kbuf[256];
    size_t len = count;
    char cmd[16], dev[128];

    if (len == 0)
        return 0;
    if (len > 127)
        len = 127;
    if (copy_from_user(kbuf, ubuf, len))
        return -EFAULT;
    kbuf[len] = '\0';
    while (len > 0 && (kbuf[len - 1] == '\n' || kbuf[len - 1] == '\r')) {
        kbuf[len - 1] = '\0';
        len--;
    }

    if (sscanf(kbuf, "%15s %127s", cmd, dev) == 2) {
        if (strcmp(cmd, "allow") == 0) {
            if (!ntb_dev_exists(dev)) {
                NTB_FAIL(NTB_TID_PAR_003, -ENOENT, "dev %s not found", dev);
                ntbox_result_set("policy: %s not found", dev);
                return count;
            }
            ntb_allow_dev(dev);
            NTB_OK(NTB_TID_PAR_003, "allow %s", dev);
            ntbox_result_set("policy: allowed %s", dev);
            return count;
        }
        if (strcmp(cmd, "deny") == 0) {
            ntb_deny_dev(dev);
            NTB_OK(NTB_TID_PAR_004, "deny %s", dev);
            ntbox_result_set("policy: denied %s", dev);
            return count;
        }
    }
    if (strcmp(kbuf, "list") == 0) {
        int i;
        char tmp[NTB_RESULT_CAP];
        size_t w = 0;

        w += (size_t)snprintf(tmp + w, NTB_RESULT_CAP - w, "allowed devices:\n");
        for (i = 0; i < ntb_nallowed; i++)
            w += (size_t)snprintf(tmp + w, NTB_RESULT_CAP - w, "  %s\n",
                                  ntb_allowed[i]);
        ntbox_result_set("%s", tmp);
        NTB_OK(NTB_TID_PAR_005, "list %d devices", ntb_nallowed);
        return count;
    }
    NTB_FAIL(NTB_TID_PAR_003, -EINVAL, "bad policy cmd: %s", kbuf);
    ntbox_result_set("policy: bad cmd (allow/deny/list)");
    return -EINVAL;
}

/* ---------- proc: attach (write) ---------- */

static ssize_t ntb_attach_write(struct file *file, const char __user *ubuf,
                                size_t count, loff_t *ppos)
{
    char kbuf[256];
    size_t len = count;
    char dev[128], mount[128];
    char letter;
    int i;

    if (len == 0)
        return 0;
    if (len > 127)
        len = 127;
    if (copy_from_user(kbuf, ubuf, len))
        return -EFAULT;
    kbuf[len] = '\0';
    while (len > 0 && (kbuf[len - 1] == '\n' || kbuf[len - 1] == '\r')) {
        kbuf[len - 1] = '\0';
        len--;
    }

    if (sscanf(kbuf, "%127s %c %127s", dev, &letter, mount) != 3) {
        NTB_FAIL(NTB_TID_PAR_006, -EINVAL, "usage: /dev/sdXN L /mnt/ntbox/pN");
        ntbox_result_set("attach: usage /dev/sdXN L /mnt/ntbox/pN");
        return -EINVAL;
    }
    if (!ntb_is_allowed(dev)) {
        NTB_FAIL(NTB_TID_PAR_006, -EACCES, "%s not allowed", dev);
        ntbox_result_set("attach: %s not allowed (policy first)", dev);
        return -EACCES;
    }
    if (ntb_find_drive(letter)) {
        NTB_FAIL(NTB_TID_PAR_006, -EBUSY, "letter %c already used", letter);
        ntbox_result_set("attach: letter %c already used", letter);
        return -EBUSY;
    }
    if (ntb_ndrives >= NTB_MAX_DRIVES) {
        NTB_FAIL(NTB_TID_PAR_006, -ENOSPC, "too many drives");
        return -ENOSPC;
    }
    if (!ntb_dev_exists(dev)) {
        NTB_FAIL(NTB_TID_PAR_006, -ENOENT, "%s not found", dev);
        ntbox_result_set("attach: %s not found", dev);
        return -ENOENT;
    }

    i = ntb_ndrives++;
    strncpy(ntb_drives[i].dev, dev, 127);
    ntb_drives[i].dev[127] = '\0';
    ntb_drives[i].letter = letter;
    strncpy(ntb_drives[i].mount, mount, 127);
    ntb_drives[i].mount[127] = '\0';
    ntb_drives[i].attached = 1;

    NTB_OK(NTB_TID_PAR_006, "attached %s as %c: -> %s", dev, letter, mount);
    ntbox_result_set("attached %s as %c: -> %s", dev, letter, mount);
    return count;
}

/* ---------- proc: detach (write) ---------- */

static ssize_t ntb_detach_write(struct file *file, const char __user *ubuf,
                                size_t count, loff_t *ppos)
{
    char kbuf[32];
    size_t len = count;
    char letter;
    int i;

    if (len == 0)
        return 0;
    if (len > 31)
        len = 31;
    if (copy_from_user(kbuf, ubuf, len))
        return -EFAULT;
    kbuf[len] = '\0';
    while (len > 0 && (kbuf[len - 1] == '\n' || kbuf[len - 1] == '\r')) {
        kbuf[len - 1] = '\0';
        len--;
    }
    if (sscanf(kbuf, " %c", &letter) != 1) {
        NTB_FAIL(NTB_TID_PAR_007, -EINVAL, "usage: echo C > detach");
        return -EINVAL;
    }
    for (i = 0; i < ntb_ndrives; i++) {
        if (ntb_drives[i].attached && ntb_drives[i].letter == letter) {
            ntb_drives[i].attached = 0;
            NTB_OK(NTB_TID_PAR_007, "detached %c:", letter);
            ntbox_result_set("detached %c:", letter);
            return count;
        }
    }
    NTB_FAIL(NTB_TID_PAR_007, -ENOENT, "letter %c not attached", letter);
    ntbox_result_set("detach: %c: not attached", letter);
    return -ENOENT;
}

/* ---------- init/exit ---------- */

static const struct proc_ops ntb_partitions_ops = { .proc_read = ntb_partitions_read };
static const struct proc_ops ntb_policy_ops = { .proc_write = ntb_policy_write };
static const struct proc_ops ntb_attach_ops = { .proc_write = ntb_attach_write };
static const struct proc_ops ntb_detach_ops = { .proc_write = ntb_detach_write };

int ntbox_part_init(struct proc_dir_entry *dir)
{
    if (!proc_create("partitions", 0444, dir, &ntb_partitions_ops))
        return -ENOMEM;
    if (!proc_create("policy", 0666, dir, &ntb_policy_ops)) {
        remove_proc_entry("partitions", dir);
        return -ENOMEM;
    }
    if (!proc_create("attach", 0666, dir, &ntb_attach_ops)) {
        remove_proc_entry("policy", dir);
        remove_proc_entry("partitions", dir);
        return -ENOMEM;
    }
    if (!proc_create("detach", 0666, dir, &ntb_detach_ops)) {
        remove_proc_entry("attach", dir);
        remove_proc_entry("policy", dir);
        remove_proc_entry("partitions", dir);
        return -ENOMEM;
    }
    NTB_OK(NTB_TID_PAR_001, "part module ready");
    return 0;
}

void ntbox_part_exit(struct proc_dir_entry *dir)
{
    remove_proc_entry("detach", dir);
    remove_proc_entry("attach", dir);
    remove_proc_entry("policy", dir);
    remove_proc_entry("partitions", dir);
}
