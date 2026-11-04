#ifndef NTBOX_H
#define NTBOX_H

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/err.h>
#include <linux/utsname.h>

int ntbox_analyze_pe(const char *buf, size_t size, const char *path);
int ntbox_run_real_pe(const void *buf, size_t size, const char *path);
int ntbox_part_init(struct proc_dir_entry *dir);
void ntbox_part_exit(struct proc_dir_entry *dir);
int ntbox_part_resolve(const char *in, char *out, size_t outsz);
int ntbox_file_init(struct proc_dir_entry *dir);
void ntbox_file_exit(struct proc_dir_entry *dir);
void ntbox_result_set(const char *fmt, ...);

#endif
