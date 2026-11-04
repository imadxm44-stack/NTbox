#ifndef NTBOX_KERNEL32_LOADER_H
#define NTBOX_KERNEL32_LOADER_H

#include <stdint.h>
#include "dll_loader.h"
#include "peb.h"

/* Windows Console API constants */
#define STD_INPUT_HANDLE  ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE  ((DWORD)-12)
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)

/* أنواع ويندوز للـ DllMain */
#define NTBOX_DLL_PROCESS_ATTACH   1
#define NTBOX_DLL_THREAD_ATTACH    2
#define NTBOX_DLL_THREAD_DETACH    3
#define NTBOX_DLL_PROCESS_DETACH   0

typedef int (__attribute__((ms_abi)) *ntboxd_dllmain_fn)(void *hinst, unsigned reason, void *reserved);

/* --- API --- */

/* يحمّل ntdll + kernel32 الحقيقيين، يعدّل الستوبات، ينفّذ DllMain/TLS/Console */
int ntboxd_load_real_modules(void);

/* يحل اسم تصدير من kernel32 الحقيقي */
void *ntboxd_real_kernel32_export(const char *name);

/* يحل اسم تصدير من ntdll الحقيقي */
void *ntboxd_real_ntdll_export(const char *name);

/* تهيئة runtime Win32 (DllMain + TLS + PEB/TEB + Console) */
int ntboxd_runtime_win32_init_local(ntb0_dll_loader *ld);
void ntboxd_runtime_win32_console_init(void);
void ntboxd_runtime_win32_seh_init(void);
void ntboxd_runtime_win32_loader_fixups(ntb0_dll_loader *ld);

/* تعيين وسائط سطر الأوامر لتهيئة PEB */
void ntboxd_runtime_win32_set_args(int argc, char **argv, char **envp);

/* دالة حل stubs الصندوق (Console APIs، إلخ) */
void *ntboxd_resolve_stub(const char *dll, const char *name);

/* Accessors للـ test */
ntb0_dll_module *ntboxd_get_mod_ntdll(void);
ntb0_dll_module *ntboxd_get_mod_kernel32(void);
ntb0_dll_loader *ntboxd_get_loader(void);
PEB *ntb0_runtime_win32_get_peb(void);
TEB *ntb0_runtime_win32_get_teb(void);

/* نقطة الدخول الرئيسية */
void ntboxd_init_real_modules(void);

#endif /* NTBOX_KERNEL32_LOADER_H */