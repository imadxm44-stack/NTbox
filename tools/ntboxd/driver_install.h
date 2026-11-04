/*
 * driver_install.h — مثبّت drivers لويندوز 7 لِ NT-Box.
 *
 * ينسخ ملفات .sys إلى <root>/Windows/System32/drivers/ ويُسجّل manifest
 * at <root>/ntbox/drivers.json (مصدر الحقيقة؛ الريجستري للقراءة فقط).
 *
 * Lamps: DRV-001 (list), DRV-002 (install), DRV-003 (load),
 *        DRV-004 (uninstall), DRV-005 (audio bridge — STUB).
 */
#ifndef NTB0_DRIVER_INSTALL_H
#define NTB0_DRIVER_INSTALL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* enumerate .sys in <root>/Windows/System32/drivers. Returns count (>=0) or -1. */
int ntb0_driver_list(const char *root);

/* name of driver at index from last list call (with .sys), or NULL. */
const char *ntb0_driver_list_name(int index);

/* install: copy sys_path -> drivers/<name>.sys + manifest entry. 0/-1. */
int ntb0_driver_install(const char *root, const char *name, const char *sys_path);

/* uninstall: remove entry + file. 0/-1. */
int ntb0_driver_uninstall(const char *root, const char *name);

/* 1 if installed (+file present), else 0. */
int ntb0_driver_is_installed(const char *root, const char *name);

/* invoke ntboxd on the .sys; returns exit code or -1 on launch failure. */
int ntb0_driver_load(const char *root, const char *name);

/* audio bridge: not yet translated — honest STUB. returns -1. */
int ntb0_driver_audio_bridge(const char *root);

#ifdef __cplusplus
}
#endif

#endif /* NTB0_DRIVER_INSTALL_H */
