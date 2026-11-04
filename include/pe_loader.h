#ifndef PE_LOADER_H
#define PE_LOADER_H

#include <linux/types.h>

int ntbox_pe_check(const void *data, size_t len);
unsigned long ntbox_pe_get_entry(const void *data);
int ntbox_run_user_pe(const void *filedata, size_t filesize, const char *path);

/* M3: import resolver (Windows 7 base) */
void *ntbox_resolve_import(const char *dll, const char *name);
int ntbox_resolve_supported_count(void);

#endif
