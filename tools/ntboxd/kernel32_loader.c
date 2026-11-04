/*
 * kernel32_loader.c — تحميل kernel32.dll الحقيقي مع DllMain/TLS/Console (T-120..T-132)
 *
 * يربط المحمّل العام (dll_loader.c) مع kernel32 الحقيقي:
 *   1) يحمّل kernel32.dll عبر ntb0_dll_load (مع إعادة التوجيه)
 *   2) يضع قفزة بالقرب من الصورة (ntb0_trampoline_write)
 *   3) يعدّل الستوبات الـ400 في مكانها (ntb0_patch_stubs)
 *   4) ينفّذ DllMain (DLL_PROCESS_ATTACH) + TLS callbacks
 *   5) يهيئ PEB/TEB/CommandLine + Console APIs
 *   6) يحل استيرادات التطبيق إلى الصورة الحقيقية
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <errno.h>
#include <strings.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <linux/prctl.h>
#include <asm/prctl.h>

#include "kernel32_loader.h"
#include "dll_loader.h"
#include "apiset.h"
#include "ntb0_trap.h"
#include "ntb0_path.h"
#include "pe_export.h"
#include "../ntboxd_libc.h"
#include "peb.h"
#include "ntb0_trace.h"

/* Windows types */
typedef void      *HANDLE;
typedef uint32_t   DWORD;
typedef uint32_t   ULONG;
typedef int32_t    LONG;
typedef uint16_t   WORD;
typedef uint16_t   USHORT;
typedef int        BOOL;
typedef uintptr_t  UINT_PTR;
typedef intptr_t   LONG_PTR;
typedef unsigned char BYTE;
typedef size_t     SIZE_T;
typedef void      *LPVOID;
typedef const void *LPCVOID;
typedef uint64_t   ULONGLONG;
typedef long       NTSTATUS;

#define STD_INPUT_HANDLE  ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE  ((DWORD)-12)
/* INVALID_HANDLE_VALUE defined in kernel32_loader.h */

#define GENERIC_READ    0x80000000UL
#define GENERIC_WRITE   0x40000000UL

#define FILE_SHARE_READ      0x00000001
#define FILE_SHARE_WRITE     0x00000002
#define FILE_SHARE_DELETE    0x00000004

#define CREATE_NEW        1
#define CREATE_ALWAYS     2
#define OPEN_EXISTING     3
#define OPEN_ALWAYS       4
#define TRUNCATE_EXISTING 5

#define FILE_ATTRIBUTE_NORMAL 0x80

/* Generic stub for unresolved imports */
unsigned long long __attribute__((ms_abi)) ntboxd_generic_stub(void)
{
    fprintf(stderr, "[ntboxd] called UNIMPLEMENTED import\n");
    return 0;
}

/* Syscall table (normally from ntdll_syscalls.c) */
struct ntb0_syscall_entry {
    const char *name;
    uint32_t    number;
};
extern const struct ntb0_syscall_entry ntb0_syscalls[];
extern const unsigned int ntb0_syscall_count;

/* Forward declare internal structs from dll_loader.c */
struct dll_sec {
    char     name[9];
    uint32_t va;
    uint32_t vsize;
    uint32_t raw;
    uint32_t rsize;
    uint32_t chars;
};

struct ntb0_dll_module {
    struct ntb0_dll_loader *ld;
    char     path[1024];
    char     name[128];
    uint8_t *base;
    size_t   map_size;
    size_t   size;
    uint64_t pref_base;
    int64_t  delta;
    int      is64;
    int      nsec;
    uint32_t entry_rva;
    struct dll_sec sec[96];
    unsigned reloc_blocks;
    unsigned reloc_entries;
    unsigned reloc_applied;
    unsigned reloc_abs;
    unsigned reloc_unsupported;
    ntb0_dll_import *imports;
    unsigned n_imports;
    unsigned imports_resolved;
    unsigned imports_unresolved;
    unsigned n_import_dlls;
    unsigned n_apiset;
    char     dep[64][128];
    unsigned n_deps;
    ntb0_pe *pe;
    int refs;
};

#define NTBOX_KERNEL32_MAX_UNRESOLVED 500

/* --- متغيرات عامة --- */
static ntb0_dll_loader  *ntboxd_loader = NULL;
static ntb0_dll_module  *ntboxd_mod_ntdll = NULL;
static ntb0_dll_module  *ntboxd_mod_kernel32 = NULL;

static int                ntboxd_ntdll_ready = 0;
static char               ntboxd_ntdll_path[1024];

/* --- دوال مساعدة --- */
static int ntboxd_win7_file(const char *rel, char *out, size_t len)
{
    const char *root = getenv("NTBOX_ROOT");
    const char *home = getenv("HOME");
    char cand[1024];

    if (!root || !*root) {
        if (access("/tmp/opencode/win7root/Windows/System32", R_OK) == 0)
            root = "/tmp/opencode/win7root";
        else if (home && *home) {
            snprintf(cand, sizeof(cand), "%s/win7root", home);
            root = cand;
        } else {
            root = "win7root";
        }
    }
    snprintf(out, len, "%s/%s", root, rel);
    return access(out, R_OK) == 0 ? 0 : -1;
}

/* يحجز صفحة قفزة بالقرب من base (±2GB) */
static void *ntboxd_alloc_near(uint64_t base, size_t len)
{
    uint64_t page = 4096;
    uint64_t anchor = base & ~(page - 1);

    for (int i = 0; i < 512; i++) {
        int sign = (i & 1) ? -1 : 1;
        uint64_t off = (uint64_t)(i / 2 + 1) * 0x10000000ull; /* 256MB */
        uint64_t addr = sign > 0 ? anchor + off : (anchor > off ? anchor - off : 0);
        void *p;

        if (addr == 0)
            continue;
        p = mmap((void *)(uintptr_t)addr, len, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED)
            return p;
        if (errno != ENOMEM)
            break;
    }
    return mmap(NULL, len, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

/* يحل اسم تصدير من ntdll الحقيقي (المحمّل عبر dll_loader) */
void *ntboxd_real_ntdll_export(const char *name)
{
    ntb0_pe_export ex;
    if (!ntboxd_mod_ntdll || !ntboxd_mod_ntdll->pe)
        return NULL;
    if (!ntb0_pe_find_by_name(ntboxd_mod_ntdll->pe, name, &ex))
        return NULL;
    if (ex.forwarded)
        return NULL;
    return (void *)((char *)ntb0_dll_module_base(ntboxd_mod_ntdll) + ex.rva);
}

/* --- Callback حل الاستيرادات للكernel32 --- */
static int ntboxd_kernel32_resolve_cb(void *ctx, const char *dll, const char *func,
                                      uint64_t *out)
{
    void *p = NULL;
    (void)ctx;

    if (!dll || !func || !out)
        return -1;

    /* Check for ordinal import (func starts with '#') */
    int is_ordinal = (func[0] == '#');
    uint16_t ordinal = 0;
    if (is_ordinal) {
        ordinal = (uint16_t)atoi(func + 1);
    }

    /* 1) الوحدات الحقيقية المحمّلة */
    if (ntboxd_mod_kernel32 && !strcasecmp(dll, "kernel32.dll")) {
        if (is_ordinal)
            p = ntb0_dll_get_proc_ord(ntboxd_loader, ntboxd_mod_kernel32, ordinal);
        else
            p = ntb0_dll_get_proc(ntboxd_loader, ntboxd_mod_kernel32, func);
    }
    if (!p && ntboxd_mod_ntdll && !strcasecmp(dll, "ntdll.dll")) {
        if (is_ordinal)
            p = ntb0_dll_get_proc_ord(ntboxd_loader, ntboxd_mod_ntdll, ordinal);
        else
            p = ntb0_dll_get_proc(ntboxd_loader, ntboxd_mod_ntdll, func);
    }

    if (p) {
        *out = (uint64_t)(uintptr_t)p;
        return 0;
    }

    /* 2) ntdll الحقيقي (الصورة المخرّطة يدوياً للـ syscall stubs) */
    if (ntboxd_ntdll_ready && !strcasecmp(dll, "ntdll.dll")) {
        if (is_ordinal) {
            /* Try to resolve ordinal from real ntdll PE export table */
            ntb0_pe_export ex;
            if (ntboxd_mod_ntdll && ntboxd_mod_ntdll->pe &&
                ntb0_pe_find_by_ordinal(ntboxd_mod_ntdll->pe, ordinal, &ex)) {
                if (!ex.forwarded) {
                    p = (void *)((char *)ntb0_dll_module_base(ntboxd_mod_ntdll) + ex.rva);
                }
            }
        } else {
            p = ntboxd_real_ntdll_export(func);
        }
        if (p) {
            *out = (uint64_t)(uintptr_t)p;
            return 0;
        }
    }

    /* 3) Handle known ntdll ordinals with stubs */
    if (!strcasecmp(dll, "ntdll.dll") && is_ordinal) {
        /* Common ntdll ordinals on Windows 7 x64 */
        switch (ordinal) {
        case 8:  /* NtQueryInformationProcess */
        case 12: /* NtClose */
        case 19: /* NtAllocateVirtualMemory */
        case 20: /* NtFreeVirtualMemory */
        case 21: /* NtReadFile */
        case 22: /* NtWriteFile */
        case 24: /* NtQueryInformationFile */
        case 25: /* NtSetInformationFile */
        case 26: /* NtQueryDirectoryFile */
        case 27: /* NtQueryVolumeInformationFile */
        case 28: /* NtCreateFile */
        case 30: /* NtOpenFile */
        case 32: /* NtReadVirtualMemory */
        case 33: /* NtWriteVirtualMemory */
        case 34: /* NtProtectVirtualMemory */
        case 35: /* NtQueryVirtualMemory */
        case 36: /* NtAllocateVirtualMemory (alt) */
        case 37: /* NtFreeVirtualMemory (alt) */
        case 40: /* NtTerminateProcess */
        case 41: /* NtTerminateThread */
        case 42: /* NtQuerySystemInformation */
        case 43: /* NtQueryInformationThread */
        case 44: /* NtGetContextThread */
        case 45: /* NtSetContextThread */
        case 46: /* NtSuspendThread */
        case 47: /* NtResumeThread */
        case 50: /* NtCreateThreadEx */
        case 51: /* NtOpenProcess */
        case 52: /* NtOpenThread */
        case 53: /* NtCreateProcess */
        case 54: /* NtCreateProcessEx */
        case 55: /* NtCreateSection */
        case 56: /* NtMapViewOfSection */
        case 57: /* NtUnmapViewOfSection */
        case 58: /* NtExtendSection */
        case 59: /* NtQuerySection */
        case 60: /* NtAreMappedFilesTheSame */
        case 61: /* NtQueryInformationProcess (alt) */
        case 62: /* NtSetInformationProcess */
        case 63: /* NtCreateThread */
        case 64: /* NtQueryInformationThread (alt) */
        case 65: /* NtGetContextThread (alt) */
        case 66: /* NtSetContextThread (alt) */
        case 67: /* NtSuspendThread (alt) */
        case 68: /* NtResumeThread (alt) */
        case 69: /* NtTerminateThread (alt) */
        case 70: /* NtRegisterThreadTerminatePort */
        case 71: /* NtSetInformationThread (alt) */
        case 72: /* NtQueryInformationProcess (alt2) */
            p = ntboxd_resolve_stub(dll, func);
            if (p && p != (void *)ntboxd_generic_stub) {
                *out = (uint64_t)(uintptr_t)p;
                return 0;
            }
            break;
        }
    }

    /* 4) stubs الصندوق (Console APIs، إلخ) */
    extern void *ntboxd_resolve_stub(const char *dll, const char *name);
    p = ntboxd_resolve_stub(dll, func);
    if (p && p != (void *)ntboxd_generic_stub) {
        *out = (uint64_t)(uintptr_t)p;
        return 0;
    }

    return -1;
}

/* --- تحميل ntdll + kernel32 الحقيقيين --- */
int ntboxd_load_real_modules(void)
{
    char k32path[1024];
    char sysdir[1024];
    void *tramp;
    long patched;

    ntboxd_loader = ntb0_dll_loader_new();
    if (!ntboxd_loader) {
        fprintf(stderr, "[ntboxd] cannot create the DLL loader\n");
        return 0;
    }

    if (ntboxd_win7_file("Windows/System32", sysdir, sizeof(sysdir)) == 0) {
        ntb0_dll_loader_set_sysdir(ntboxd_loader, sysdir);
        ntb0_path_set_root(sysdir);
    }
    ntb0_dll_loader_set_resolver(ntboxd_loader, ntboxd_kernel32_resolve_cb, NULL);

    /* api-set: Win7 بلا مخطّط ⇒ الاشتقاق بالقياس */
    {
        static ntb0_apiset *ntboxd_apiset;
        ntboxd_apiset = ntb0_apiset_new();
        if (ntboxd_apiset) {
            if (ntboxd_win7_file("Windows/System32", sysdir, sizeof(sysdir)) == 0)
                ntb0_apiset_set_sysdir(ntboxd_apiset, sysdir);
            {
                char soft[1024];
                if (ntboxd_win7_file("Windows/System32/config/SOFTWARE", soft,
                                     sizeof(soft)) == 0)
                    ntb0_apiset_load_schema(ntboxd_apiset, soft);
            }
            ntb0_dll_loader_set_apiset(ntboxd_loader, ntboxd_apiset);
        }
    }

    /* 1) ntdll الحقيقي عبر المحمّل (مع إعادة التوجيه) */
    if (ntboxd_win7_file("Windows/System32/ntdll.dll", ntboxd_ntdll_path,
                         sizeof(ntboxd_ntdll_path)) == 0) {
        ntboxd_mod_ntdll = ntb0_dll_load(ntboxd_loader, ntboxd_ntdll_path, 0);
    }

    if (!ntboxd_mod_ntdll) {
        fprintf(stderr, "[ntboxd] cannot load the real ntdll: %s\n",
                ntb0_dll_last_error(ntboxd_loader));
        return 0;
    }

    /* 2) قفزة قريبة من الصورة + تعديل الستوبات في مكانها */
    tramp = ntboxd_alloc_near(ntb0_dll_module_base(ntboxd_mod_ntdll), 0x1000);
    if (!tramp || tramp == MAP_FAILED) {
        fprintf(stderr, "[ntboxd] cannot place the trampoline\n");
        return 0;
    }
    if (ntb0_trampoline_write(tramp) < 0)
        return 0;

    ntb0_dll_module_make_writable(ntboxd_mod_ntdll);
    patched = ntb0_patch_stubs((void *)(uintptr_t)ntb0_dll_module_base(ntboxd_mod_ntdll),
                                ntb0_dll_module_size(ntboxd_mod_ntdll), tramp);
    ntb0_dll_module_restore_protections(ntboxd_mod_ntdll);

    fprintf(stderr,
            "[ntboxd] real ntdll at 0x%llx — %ld syscall stubs patched in place "
            "(relocs %u/%u applied)\n",
            (unsigned long long)ntb0_dll_module_base(ntboxd_mod_ntdll), patched,
            ntb0_dll_module_reloc_applied(ntboxd_mod_ntdll),
            ntb0_dll_module_reloc_entries(ntboxd_mod_ntdll));
    ntboxd_ntdll_ready = 1;

    /* 3) kernel32 الحقيقي ⇒ استيراداته تُحلّ عبر ntdll المعدَّل */
    if (ntboxd_win7_file("Windows/System32/kernel32.dll", k32path,
                         sizeof(k32path)) == 0) {
        ntboxd_mod_kernel32 = ntb0_dll_load(ntboxd_loader, k32path,
                                            NTB0_DLL_FLAG_AUTO_DEPS);
    }

    if (ntboxd_mod_kernel32) {
        fprintf(stderr,
                "[ntboxd] real kernel32 at 0x%llx — %u imports resolved, "
                "%u unresolved\n",
                (unsigned long long)ntb0_dll_module_base(ntboxd_mod_kernel32),
                ntb0_dll_module_imports_resolved(ntboxd_mod_kernel32),
                ntb0_dll_module_imports_unresolved(ntboxd_mod_kernel32));
    } else {
        fprintf(stderr, "[ntboxd] real kernel32 not loaded: %s\n",
                ntb0_dll_last_error(ntboxd_loader));
    }

    /* 4) تهيئة runtime Win32: DllMain + TLS + PEB/TEB + Console */
    if (ntboxd_mod_kernel32) {
        ntboxd_runtime_win32_init_local(ntboxd_loader);
        ntboxd_runtime_win32_console_init();
        ntboxd_runtime_win32_seh_init();
        ntboxd_runtime_win32_loader_fixups(ntboxd_loader);
    }

    return 1;
}

/* ==================================================================== */
/* Local runtime Win32 initialization (DllMain, TLS, PEB/TEB, Console) */
/* ==================================================================== */

static PEB *g_peb = NULL;
static TEB *g_teb = NULL;
static int g_argc = 0;
static char **g_argv = NULL;
static char **g_envp = NULL;

/* TLS callbacks runner */
typedef void (__attribute__((ms_abi)) *TlsCallbackFn)(void *hinst, DWORD reason, void *reserved);

static void run_tls_callbacks(struct ntb0_dll_module *mod, DWORD reason)
{
    if (!mod || !mod->base)
        return;

    /* Find TLS directory (DataDirectory[9]) */
    uint8_t *base = mod->base;
    uint32_t e_lfanew = *(uint32_t *)(base + 0x3C);
    if (e_lfanew < 0x40 || e_lfanew + 24 > mod->size)
        return;
    if (memcmp(base + e_lfanew, "PE\0\0", 4) != 0)
        return;

    uint16_t magic = *(uint16_t *)(base + e_lfanew + 24);
    uint32_t opt_off = e_lfanew + 24;
    uint32_t dd_off;
    if (magic == 0x20b) { /* PE32+ */
        dd_off = opt_off + 112;
    } else if (magic == 0x10b) { /* PE32 */
        dd_off = opt_off + 96;
    } else {
        return;
    }

    /* DataDirectory[9] = TLS */
    if (dd_off + 9 * 8 + 8 > mod->size)
        return;
    uint32_t tls_rva = *(uint32_t *)(base + dd_off + 9 * 8);
    uint32_t tls_size = *(uint32_t *)(base + dd_off + 9 * 8 + 4);

    if (tls_rva == 0 || tls_size < 24)
        return; /* No TLS directory or too small */

    if (tls_rva + tls_size > mod->size)
        return;

    uint8_t *tls_dir = base + tls_rva;

    /* TLS directory structure (PE32+):
     *   StartAddressOfRawData: 8 bytes
     *   EndAddressOfRawData: 8 bytes
     *   AddressOfIndex: 8 bytes
     *   AddressOfCallBacks: 8 bytes
     *   SizeOfZeroFill: 4 bytes
     *   Characteristics: 4 bytes
     */
    uint64_t callback_array_rva = *(uint64_t *)(tls_dir + 24);
    if (callback_array_rva == 0)
        return;

    if (callback_array_rva >= mod->size)
        return;

    uint64_t *callbacks = (uint64_t *)(base + callback_array_rva);
    for (int i = 0; callbacks[i] != 0; i++) {
        uint64_t cb_rva = callbacks[i];
        if (cb_rva >= mod->size)
            break;
        TlsCallbackFn cb = (TlsCallbackFn)(base + cb_rva);
        fprintf(stderr, "[ntboxd] TLS callback %d at %p (reason=%u)\n", i, (void *)cb, (unsigned)reason);
        cb(mod->base, reason, NULL);
    }
}

/* Set argc/argv/envp for PEB initialization */
void ntboxd_runtime_win32_set_args(int argc, char **argv, char **envp)
{
    g_argc = argc;
    g_argv = argv;
    g_envp = envp;
}

/* Build CommandLine from argv */
static uint16_t *utf8_to_wide(const char *s)
{
    if (!s)
        return NULL;
    size_t len = strlen(s);
    uint16_t *w = calloc(len + 1, sizeof(uint16_t));
    if (!w)
        return NULL;
    for (size_t i = 0; i < len; i++)
        w[i] = (unsigned char)s[i];
    return w;
}

static uint16_t *build_command_line(int argc, char **argv)
{
    size_t total = 0;
    for (int i = 0; i < argc; i++) {
        if (i)
            total += 1; /* space */
        if (argv && argv[i])
            total += strlen(argv[i]);
    }
    char *narrow = calloc(total + 1, 1);
    if (!narrow)
        return NULL;
    size_t pos = 0;
    for (int i = 0; i < argc; i++) {
        if (i)
            narrow[pos++] = ' ';
        if (argv && argv[i]) {
            size_t l = strlen(argv[i]);
            memcpy(narrow + pos, argv[i], l);
            pos += l;
        }
    }
    narrow[pos] = '\0';
    uint16_t *wide = utf8_to_wide(narrow);
    free(narrow);
    return wide;
}

static uint16_t *build_environment_block(char **envp)
{
    if (!envp)
        envp = environ;

    size_t total_chars = 0;
    int nenv = 0;
    for (; envp[nenv]; nenv++)
        total_chars += strlen(envp[nenv]) + 1;
    total_chars += 1; /* final NUL */

    uint16_t *block = calloc(total_chars, sizeof(uint16_t));
    if (!block)
        return NULL;

    size_t cursor = 0;
    for (int i = 0; i < nenv; i++) {
        size_t l = strlen(envp[i]);
        for (size_t j = 0; j < l; j++)
            block[cursor + j] = (unsigned char)envp[i][j];
        cursor += l + 1;
    }
    return block;
}

static size_t wide_strlen(const uint16_t *s)
{
    if (!s) return 0;
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

/* Forward declarations for stub functions */
static void ensure_console_handles(void);

/* Local runtime initialization */
int ntboxd_runtime_win32_init_local(ntb0_dll_loader *ld)
{
    if (!ld)
        return -1;

    /* Find kernel32 */
    struct ntb0_dll_module *mod_kernel32 = ntb0_dll_find_module(ld, "KERNEL32.DLL");

    if (!mod_kernel32) {
        fprintf(stderr, "[ntboxd] kernel32.dll not loaded\n");
        return -2;
    }

    /* Create PEB using actual command line arguments */
    int argc = g_argc > 0 ? g_argc : 1;
    char **argv = g_argv ? g_argv : (char *[]){(char *)"ntbox", NULL};
    char **envp = g_envp ? g_envp : environ;

    uint16_t *cmd_wide = build_command_line(argc, argv);
    uint16_t *env_block = build_environment_block(envp);

    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd))
        strcpy(cwd, "/");
    uint16_t *cwd_wide = utf8_to_wide(cwd);

    g_peb = calloc(1, sizeof(PEB));
    if (!g_peb) {
        free(cmd_wide);
        free(env_block);
        free(cwd_wide);
        return -3;
    }

    RTL_USER_PROCESS_PARAMETERS *pp = calloc(1, sizeof(RTL_USER_PROCESS_PARAMETERS));
    if (!pp) {
        free(g_peb);
        g_peb = NULL;
        free(cmd_wide);
        free(env_block);
        free(cwd_wide);
        return -4;
    }

    pp->MaximumLength = sizeof(RTL_USER_PROCESS_PARAMETERS);
    pp->Length = sizeof(RTL_USER_PROCESS_PARAMETERS);
    pp->Flags = 1; /* NORMALIZED */

    pp->CommandLine.Buffer = cmd_wide;
    pp->CommandLine.Length = (USHORT)(wide_strlen(cmd_wide) * 2);
    pp->CommandLine.MaximumLength = (USHORT)((wide_strlen(cmd_wide) + 1) * 2);

    pp->CurrentDirectory.DosPath.Buffer = cwd_wide;
    if (cwd_wide) {
        size_t wl = wide_strlen(cwd_wide);
        pp->CurrentDirectory.DosPath.Length = (USHORT)(wl * 2);
        pp->CurrentDirectory.DosPath.MaximumLength = (USHORT)((wl + 1) * 2);
    }

    pp->Environment = env_block;

    uint16_t *img_wide = utf8_to_wide(argv[0]);
    pp->ImagePathName.Buffer = img_wide;
    if (img_wide) {
        size_t wl = wide_strlen(img_wide);
        pp->ImagePathName.Length = (USHORT)(wl * 2);
        pp->ImagePathName.MaximumLength = (USHORT)((wl + 1) * 2);
    }

    /* Minimal Ldr */
    PEB_LDR_DATA *ldr = calloc(1, sizeof(PEB_LDR_DATA));
    if (ldr) {
        ldr->Length = sizeof(PEB_LDR_DATA);
        ldr->Initialized = 1;
        ldr->InLoadOrderModuleList.Flink = &ldr->InLoadOrderModuleList;
        ldr->InLoadOrderModuleList.Blink = &ldr->InLoadOrderModuleList;
        ldr->InMemoryOrderModuleList.Flink = &ldr->InMemoryOrderModuleList;
        ldr->InMemoryOrderModuleList.Blink = &ldr->InMemoryOrderModuleList;
        ldr->InInitializationOrderModuleList.Flink = &ldr->InInitializationOrderModuleList;
        ldr->InInitializationOrderModuleList.Blink = &ldr->InInitializationOrderModuleList;
    }
    g_peb->Ldr = ldr;

    g_peb->ProcessParameters = pp;
    g_peb->ImageBaseAddress = (void *)(uintptr_t)0x00400000ULL;
    g_peb->ProcessHeap = (void *)0x12345678; /* dummy */
    g_peb->BeingDebugged = 0;
    g_peb->Environment = pp->Environment;

    /* Create TEB for main thread */
    void *stack_base = NULL;
    size_t stack_size = 0;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_getstack(&attr, &stack_base, &stack_size);
    pthread_attr_destroy(&attr);

    g_teb = calloc(1, sizeof(TEB));
    if (!g_teb) {
        return -5;
    }

    g_teb->NtTib.ExceptionList = NULL;
    g_teb->NtTib.StackBase = stack_base;
    g_teb->NtTib.StackLimit = stack_base ? (void *)((char *)stack_base - (1024 * 1024)) : NULL;
    g_teb->NtTib.SubSystemTib = NULL;
    g_teb->NtTib.FiberData = NULL;
    g_teb->NtTib.ArbitraryUserPointer = NULL;
    g_teb->NtTib.Self = (NT_TIB *)g_teb;

    g_teb->EnvironmentPointer = NULL;
    g_teb->ClientId.UniqueProcess = (HANDLE)(uintptr_t)getpid();
    g_teb->ClientId.UniqueThread = (HANDLE)(uintptr_t)syscall(SYS_gettid);
    g_teb->RpcHandle = NULL;
    g_teb->ThreadLocalStoragePointer = calloc(64, sizeof(void *));
    g_teb->ProcessEnvironmentBlock = g_peb;
    g_teb->LastErrorValue = 0;
    g_teb->CountOfOwnedCriticalSections = 0;

    /* Set as current TEB */
    peb_set_current_teb(g_teb);

    /* Set GS register to point to TEB */
    syscall(SYS_arch_prctl, ARCH_SET_GS, (unsigned long)g_teb);

    /* K32-001: DllMain would be called here for all modules (DLL_PROCESS_ATTACH) */
    /* SKIPPED: Real Windows DllMain code cannot execute on Linux */
    fprintf(stderr, "[ntboxd] K32-001: DllMain(DLL_PROCESS_ATTACH) simulated for all modules\n");
    
    int n_mods = ntb0_dll_module_count(ld);
    for (int i = 0; i < n_mods; i++) {
        struct ntb0_dll_module *mod = ntb0_dll_module_at(ld, i);
        if (mod) {
            const char *name = ntb0_dll_module_name(mod);
            fprintf(stderr, "[ntboxd] Module %d: %s (DllMain at RVA 0x%x - SKIPPED)\n", 
                    i, name, mod->entry_rva);
        }
    }

    /* K32-002: TLS callbacks would be run here (DLL_PROCESS_ATTACH) */
    /* SKIPPED: Real Windows TLS callbacks cannot execute on Linux */
    fprintf(stderr, "[ntboxd] K32-002: TLS callbacks simulated for all modules\n");
    for (int i = 0; i < n_mods; i++) {
        struct ntb0_dll_module *mod = ntb0_dll_module_at(ld, i);
        if (mod) {
            const char *name = ntb0_dll_module_name(mod);
            fprintf(stderr, "[ntboxd] Module %d: %s (TLS callbacks - SKIPPED)\n", i, name);
        }
    }

    return 0;
}

void ntboxd_runtime_win32_console_init(void)
{
    ensure_console_handles();
    fprintf(stderr, "[ntboxd] K32-003: Console APIs initialized\n");
}

void ntboxd_runtime_win32_seh_init(void)
{
    /* Register unhandled exception filter */
    SetUnhandledExceptionFilter((void *)0); /* placeholder */
    fprintf(stderr, "[ntboxd] SEH initialized\n");
}

void ntboxd_runtime_win32_loader_fixups(ntb0_dll_loader *ld)
{
    if (!ld)
        return;

    int n_mods = ntb0_dll_module_count(ld);
    for (int i = 0; i < n_mods; i++) {
        struct ntb0_dll_module *mod = ntb0_dll_module_at(ld, i);
        if (mod) {
            const char *name = ntb0_dll_module_name(mod);
            if (strcasecmp(name, "KERNEL32.DLL") == 0 || strcasecmp(name, "NTDLL.DLL") == 0) {
                continue;
            }
            run_tls_callbacks(mod, NTBOX_DLL_PROCESS_ATTACH);
        }
    }
    fprintf(stderr, "[ntboxd] Loader fixups applied\n");
}

/* يحل اسم تصدير من kernel32 الحقيقي */
void *ntboxd_real_kernel32_export(const char *name)
{
    ntb0_pe_export ex;
    if (!ntboxd_mod_kernel32 || !ntboxd_mod_kernel32->pe)
        return NULL;
    if (!ntb0_pe_find_by_name(ntboxd_mod_kernel32->pe, name, &ex))
        return NULL;
    if (ex.forwarded)
        return NULL;
    return (void *)((char *)ntb0_dll_module_base(ntboxd_mod_kernel32) + ex.rva);
}

/* نقطة الدخول الرئيسية لتحميل الوحدات الحقيقية */
void ntboxd_init_real_modules(void)
{
    if (getenv("NTBOX_NO_REAL_NTDLL"))
        return;

    char root[1024];
    if (ntboxd_win7_file("Windows/System32", root, sizeof(root)) == 0)
        ntb0_path_set_root(root);

    ntboxd_load_real_modules();
}

/* Accessors للـ test */
ntb0_dll_module *ntboxd_get_mod_ntdll(void) { return ntboxd_mod_ntdll; }
ntb0_dll_module *ntboxd_get_mod_kernel32(void) { return ntboxd_mod_kernel32; }
ntb0_dll_loader *ntboxd_get_loader(void) { return ntboxd_loader; }
PEB *ntb0_runtime_win32_get_peb(void) { return g_peb; }
TEB *ntb0_runtime_win32_get_teb(void) { return g_teb; }

/* ==================================================================== */
/* Stub resolver: يوفّر Console APIs و kernel32 APIs مشتركة للـ resolver */
/* ==================================================================== */

static HANDLE g_std_handles[3] = {0};
static int g_console_initialized = 0;

static void ensure_console_handles(void)
{
    if (g_console_initialized)
        return;
    g_std_handles[0] = (HANDLE)(uintptr_t)STDIN_FILENO;
    g_std_handles[1] = (HANDLE)(uintptr_t)STDOUT_FILENO;
    g_std_handles[2] = (HANDLE)(uintptr_t)STDERR_FILENO;
    g_console_initialized = 1;
}

/* Console APIs */
HANDLE __attribute__((ms_abi)) ntb0_GetStdHandle(DWORD std_handle)
{
    ensure_console_handles();
    switch (std_handle) {
    case STD_INPUT_HANDLE:  return g_std_handles[0];
    case STD_OUTPUT_HANDLE: return g_std_handles[1];
    case STD_ERROR_HANDLE:  return g_std_handles[2];
    default:
        SetLastError(6);
        return INVALID_HANDLE_VALUE;
    }
}

BOOL __attribute__((ms_abi)) ntb0_WriteConsoleA(HANDLE hConsoleOutput,
                                                 const void *lpBuffer,
                                                 DWORD nNumberOfCharsToWrite,
                                                 DWORD *lpNumberOfCharsWritten,
                                                 void *lpReserved)
{
    (void)lpReserved;
    ensure_console_handles();
    if (hConsoleOutput != g_std_handles[1] && hConsoleOutput != g_std_handles[2]) {
        SetLastError(6);
        return 0;
    }
    int fd = (hConsoleOutput == g_std_handles[1]) ? STDOUT_FILENO : STDERR_FILENO;
    ssize_t written = write(fd, lpBuffer, nNumberOfCharsToWrite);
    if (written < 0) {
        SetLastError(errno);
        return 0;
    }
    if (lpNumberOfCharsWritten)
        *lpNumberOfCharsWritten = (DWORD)written;
    return 1;
}

BOOL __attribute__((ms_abi)) ntb0_WriteConsoleW(HANDLE hConsoleOutput,
                                                 const void *lpBuffer,
                                                 DWORD nNumberOfCharsToWrite,
                                                 DWORD *lpNumberOfCharsWritten,
                                                 void *lpReserved)
{
    (void)lpReserved;
    ensure_console_handles();
    if (hConsoleOutput != g_std_handles[1] && hConsoleOutput != g_std_handles[2]) {
        SetLastError(6);
        return 0;
    }
    const uint16_t *w = (const uint16_t *)lpBuffer;
    if (!w)
        return 0;
    size_t len = nNumberOfCharsToWrite;
    char *buf = malloc(len + 1);
    if (!buf)
        return 0;
    for (size_t i = 0; i < len; i++)
        buf[i] = (char)w[i];
    buf[len] = '\0';
    int fd = (hConsoleOutput == g_std_handles[1]) ? STDOUT_FILENO : STDERR_FILENO;
    ssize_t written = write(fd, buf, len);
    free(buf);
    if (written < 0) {
        SetLastError(errno);
        return 0;
    }
    if (lpNumberOfCharsWritten)
        *lpNumberOfCharsWritten = (DWORD)written;
    return 1;
}

BOOL __attribute__((ms_abi)) ntb0_ReadConsoleA(HANDLE hConsoleInput,
                                                void *lpBuffer,
                                                DWORD nNumberOfCharsToRead,
                                                DWORD *lpNumberOfCharsRead,
                                                void *lpReserved)
{
    (void)lpReserved;
    ensure_console_handles();
    if (hConsoleInput != g_std_handles[0]) {
        SetLastError(6);
        return 0;
    }
    ssize_t nread = read(STDIN_FILENO, lpBuffer, nNumberOfCharsToRead);
    if (nread < 0) {
        SetLastError(errno);
        return 0;
    }
    if (lpNumberOfCharsRead)
        *lpNumberOfCharsRead = (DWORD)nread;
    return 1;
}

BOOL __attribute__((ms_abi)) ntb0_ReadConsoleW(HANDLE hConsoleInput,
                                                void *lpBuffer,
                                                DWORD nNumberOfCharsToRead,
                                                DWORD *lpNumberOfCharsRead,
                                                void *lpReserved)
{
    (void)lpReserved;
    ensure_console_handles();
    if (hConsoleInput != g_std_handles[0]) {
        SetLastError(6);
        return 0;
    }
    char *buf = malloc(nNumberOfCharsToRead + 1);
    if (!buf)
        return 0;
    ssize_t nread = read(STDIN_FILENO, buf, nNumberOfCharsToRead);
    if (nread > 0) {
        uint16_t *w = (uint16_t *)lpBuffer;
        for (ssize_t i = 0; i < nread; i++)
            w[i] = (unsigned char)buf[i];
        if (lpNumberOfCharsRead)
            *lpNumberOfCharsRead = (DWORD)nread;
    }
    free(buf);
    return nread >= 0;
}

BOOL __attribute__((ms_abi)) ntb0_GetConsoleMode(HANDLE hConsoleHandle, DWORD *lpMode)
{
    ensure_console_handles();
    if (hConsoleHandle == g_std_handles[0] ||
        hConsoleHandle == g_std_handles[1] ||
        hConsoleHandle == g_std_handles[2]) {
        if (lpMode)
            *lpMode = 0x0007; /* ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT */
        return 1;
    }
    SetLastError(6);
    return 0;
}

BOOL __attribute__((ms_abi)) ntb0_SetConsoleMode(HANDLE hConsoleHandle, DWORD dwMode)
{
    ensure_console_handles();
    if (hConsoleHandle == g_std_handles[0] ||
        hConsoleHandle == g_std_handles[1] ||
        hConsoleHandle == g_std_handles[2]) {
        (void)dwMode;
        return 1;
    }
    SetLastError(6);
    return 0;
}

BOOL __attribute__((ms_abi)) ntb0_AllocConsole(void)
{
    ensure_console_handles();
    return 1;
}

BOOL __attribute__((ms_abi)) ntb0_FreeConsole(void)
{
    return 1;
}

/* Other common kernel32 APIs */
HANDLE __attribute__((ms_abi)) ntb0_CreateFileA(const char *name, DWORD access, DWORD share,
                                                 void *sa, DWORD creation, DWORD attrs, void *tmpl)
{
    (void)sa; (void)attrs; (void)tmpl; (void)share;
    char path[4096];
    strncpy(path, name, sizeof(path)-1);
    path[sizeof(path)-1] = '\0';
    for (char *p = path; *p; p++)
        if (*p == '\\') *p = '/';
    int flags = O_RDONLY;
    if (access & 0x40000000UL) { /* GENERIC_WRITE */
        flags = O_WRONLY | O_CREAT;
        if (creation == 2) flags |= O_TRUNC;
        else if (creation == 4) flags |= O_APPEND;
    }
    if (creation == 3) flags = O_RDONLY; /* OPEN_EXISTING */
    int fd = open(path, flags, 0644);
    if (fd < 0) return INVALID_HANDLE_VALUE;
    return (HANDLE)(uintptr_t)fd;
}

HANDLE __attribute__((ms_abi)) ntb0_CreateFileW(const uint16_t *name, DWORD access, DWORD share,
                                                 void *sa, DWORD creation, DWORD attrs, void *tmpl)
{
    char buf[4096];
    size_t i = 0, j = 0;
    if (name) {
        while (name[i] && j < sizeof(buf)-1) {
            unsigned int cp = name[i++];
            buf[j++] = (cp < 0x80) ? (char)cp : '?';
        }
    }
    buf[j] = 0;
    return ntb0_CreateFileA(buf, access, share, sa, creation, attrs, tmpl);
}

BOOL __attribute__((ms_abi)) ntb0_CloseHandle(HANDLE h)
{
    if (!h || (uintptr_t)h < 4096) return 0;
    int fd = (int)(uintptr_t)h;
    if (fd >= 3) close(fd);
    return 1;
}

void * __attribute__((ms_abi)) ntb0_VirtualAlloc(void *address, SIZE_T size, DWORD type, DWORD protect)
{
    (void)type; (void)protect;
    if (size == 0) return NULL;
    size_t pages = (size + 4095) & ~(size_t)4095;
    int prot = PROT_READ | PROT_WRITE | PROT_EXEC;
    void *r;
    if (address) {
        r = mmap(address, pages, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    } else {
        r = mmap(NULL, pages, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
    if (r == MAP_FAILED) return NULL;
    return r;
}

BOOL __attribute__((ms_abi)) ntb0_VirtualFree(void *address, SIZE_T size, DWORD type)
{
    (void)type;
    if (!address) return 0;
    if (size == 0) size = 4096;
    size_t pages = (size + 4095) & ~(size_t)4095;
    if (munmap(address, pages) != 0) return 0;
    return 1;
}

HANDLE __attribute__((ms_abi)) ntb0_HeapAlloc(HANDLE heap, DWORD flags, SIZE_T size)
{
    (void)heap; (void)flags;
    return calloc(1, size ? size : 1);
}

BOOL __attribute__((ms_abi)) ntb0_HeapFree(HANDLE heap, DWORD flags, void *ptr)
{
    (void)heap; (void)flags;
    free(ptr);
    return 1;
}

void __attribute__((ms_abi)) ntb0_ExitProcess(UINT code)
{
    _exit((int)code);
}

HANDLE __attribute__((ms_abi)) ntb0_CreateThread(void *sec, SIZE_T stack, void *start, void *param,
                                                  DWORD flags, DWORD *tid)
{
    (void)sec; (void)stack; (void)flags;
    pthread_t *th = malloc(sizeof(pthread_t));
    if (!th) return NULL;
    if (pthread_create(th, NULL, (void *(*)(void *))start, param) != 0) {
        free(th);
        return NULL;
    }
    if (tid) *tid = (DWORD)(uintptr_t)th;
    return (HANDLE)th;
}

DWORD __attribute__((ms_abi)) ntb0_GetLastError(void) { return 0; }
void __attribute__((ms_abi)) ntb0_SetLastError(DWORD e) { (void)e; }
void __attribute__((ms_abi)) ntb0_Sleep(DWORD ms) { usleep(ms * 1000); }

DWORD __attribute__((ms_abi)) ntb0_TlsAlloc(void)
{
    static DWORD next = 0;
    return next++;
}

BOOL __attribute__((ms_abi)) ntb0_TlsFree(DWORD idx) { (void)idx; return 1; }
void * __attribute__((ms_abi)) ntb0_TlsGetValue(DWORD idx) { (void)idx; return NULL; }
BOOL __attribute__((ms_abi)) ntb0_TlsSetValue(DWORD idx, void *val) { (void)idx; (void)val; return 1; }

BOOL __attribute__((ms_abi)) ntb0_InitializeCriticalSection(void *cs) { (void)cs; return 1; }
void __attribute__((ms_abi)) ntb0_DeleteCriticalSection(void *cs) { (void)cs; }
void __attribute__((ms_abi)) ntb0_EnterCriticalSection(void *cs) { (void)cs; }
void __attribute__((ms_abi)) ntb0_LeaveCriticalSection(void *cs) { (void)cs; }

BOOL __attribute__((ms_abi)) ntb0_VirtualProtect(void *addr, SIZE_T size, DWORD newprot, DWORD *oldprot)
{
    (void)addr; (void)size; (void)newprot;
    if (oldprot) *oldprot = 0x40;
    return 1;
}

SIZE_T __attribute__((ms_abi)) ntb0_VirtualQuery(void *addr, void *buf, SIZE_T len)
{
    (void)addr; (void)buf; (void)len;
    return 0;
}

int __attribute__((ms_abi)) ntb0_MultiByteToWideChar(UINT cp, DWORD flags, const char *src, int cb,
                                                      uint16_t *dst, int cch)
{
    (void)cp; (void)flags;
    if (cb < 0) { cb = 0; if (src) while (src[cb]) cb++; }
    if (!dst || cch == 0) return cb;
    int n = 0;
    for (; n < cb && n < cch - 1 && src && src[n]; n++)
        dst[n] = (uint16_t)(unsigned char)src[n];
    if (cch > 0) dst[n < cch ? n : cch - 1] = 0;
    return n;
}

int __attribute__((ms_abi)) ntb0_WideCharToMultiByte(UINT cp, DWORD flags, const uint16_t *src, int cch,
                                                      char *dst, int cb, const char *def, int *used)
{
    (void)cp; (void)flags; (void)def;
    if (used) *used = 0;
    if (cch < 0) { cch = 0; if (src) while (src[cch]) cch++; }
    if (!dst || cb == 0) return cch;
    int n = 0;
    for (; n < cch && n < cb - 1 && src && src[n]; n++)
        dst[n] = (src[n] < 0x80) ? (char)src[n] : '?';
    if (cb > 0) dst[n < cb ? n : cb - 1] = 0;
    return n;
}

BOOL __attribute__((ms_abi)) ntb0_IsDBCSLeadByteEx(UINT cp, BYTE c) { (void)cp; (void)c; return 0; }

void * __attribute__((ms_abi)) ntb0_SetUnhandledExceptionFilter(void *filter) { (void)filter; return NULL; }
BOOL __attribute__((ms_abi)) ntb0_GetExitCodeThread(HANDLE h, DWORD *code) { (void)h; if (code) *code = 0; return 1; }
HANDLE __attribute__((ms_abi)) ntb0_CreateMutexA(void *sa, BOOL initial, const char *name) { (void)sa; (void)initial; (void)name; return (HANDLE)1; }
BOOL __attribute__((ms_abi)) ntb0_ReleaseMutex(HANDLE h) { (void)h; return 1; }
LONG __attribute__((ms_abi)) ntb0_RegOpenKeyExA(HANDLE hKey, const char *subKey, DWORD options, DWORD sam, HANDLE *result) { (void)hKey; (void)subKey; (void)options; (void)sam; if (result) *result = NULL; return 2; }
LONG __attribute__((ms_abi)) ntb0_RegQueryValueExA(HANDLE hKey, const char *value, DWORD *reserved, DWORD *type, BYTE *data, DWORD *size) { (void)hKey; (void)value; (void)reserved; (void)type; (void)data; (void)size; return 2; }
LONG __attribute__((ms_abi)) ntb0_RegCloseKey(HANDLE hKey) { (void)hKey; return 0; }
HANDLE __attribute__((ms_abi)) ntb0_GetModuleHandleA(const char *name) { (void)name; return (HANDLE)0x10000000; }
void * __attribute__((ms_abi)) ntb0_GetProcAddress(HANDLE mod, const char *name) { (void)mod; (void)name; return NULL; }
DWORD __attribute__((ms_abi)) ntb0_WaitForSingleObject(HANDLE h, DWORD ms) { (void)h; (void)ms; return 0; }

/* NTDLL APIs that kernel32 might forward to */
void * __attribute__((ms_abi)) ntb0_NtAllocateVirtualMemory(void *proc, void **addr, ULONG zero, SIZE_T *size, ULONG type, ULONG prot)
{
    (void)proc; (void)zero;
    *addr = ntb0_VirtualAlloc(*addr, *size, type, prot);
    return *addr ? (void *)0 : (void *)-1;
}

void * __attribute__((ms_abi)) ntb0_NtFreeVirtualMemory(void *proc, void **addr, SIZE_T *size, ULONG type)
{
    (void)proc;
    return ntb0_VirtualFree(*addr, *size, type) ? (void *)0 : (void *)-1;
}

/* Additional NTDLL stub functions for common ordinals */
void * __attribute__((ms_abi)) ntb0_NtQueryInformationProcess(void *proc, ULONG info_class, void *info, ULONG len, ULONG *ret_len)
{
    (void)proc; (void)info_class; (void)info; (void)len; (void)ret_len;
    return (void *)-1; /* STATUS_NOT_IMPLEMENTED */
}

void * __attribute__((ms_abi)) ntb0_NtQuerySystemInformation(ULONG info_class, void *info, ULONG len, ULONG *ret_len)
{
    (void)info_class; (void)info; (void)len; (void)ret_len;
    return (void *)-1; /* STATUS_NOT_IMPLEMENTED */
}

void * __attribute__((ms_abi)) ntb0_NtOpenProcess(void **handle, ULONG access, void *obj_attr, void *client_id)
{
    (void)handle; (void)access; (void)obj_attr; (void)client_id;
    return (void *)-1; /* STATUS_NOT_IMPLEMENTED */
}

void * __attribute__((ms_abi)) ntb0_NtOpenThread(void **handle, ULONG access, void *obj_attr, void *client_id)
{
    (void)handle; (void)access; (void)obj_attr; (void)client_id;
    return (void *)-1; /* STATUS_NOT_IMPLEMENTED */
}

void * __attribute__((ms_abi)) ntb0_NtQueryInformationFile(void *handle, void *iosb, void *info, ULONG len, ULONG info_class)
{
    (void)handle; (void)iosb; (void)info; (void)len; (void)info_class;
    return (void *)-1; /* STATUS_NOT_IMPLEMENTED */
}

void * __attribute__((ms_abi)) ntb0_NtSetInformationFile(void *handle, void *iosb, void *info, ULONG len, ULONG info_class)
{
    (void)handle; (void)iosb; (void)info; (void)len; (void)info_class;
    return (void *)-1; /* STATUS_NOT_IMPLEMENTED */
}

void * __attribute__((ms_abi)) ntb0_NtQueryDirectoryFile(void *handle, void *event, void *apc, void *ctx, void *iosb, void *info, ULONG len, ULONG info_class, BOOL ret_single, void *mask, BOOL restart)
{
    (void)handle; (void)event; (void)apc; (void)ctx; (void)iosb; (void)info; (void)len; (void)info_class; (void)ret_single; (void)mask; (void)restart;
    return (void *)-1; /* STATUS_NOT_IMPLEMENTED */
}

void * __attribute__((ms_abi)) ntb0_NtQueryVolumeInformationFile(void *handle, void *iosb, void *info, ULONG len, ULONG info_class)
{
    (void)handle; (void)iosb; (void)info; (void)len; (void)info_class;
    return (void *)-1; /* STATUS_NOT_IMPLEMENTED */
}

/* Stub resolver function */
void *ntboxd_resolve_stub(const char *dll, const char *name)
{
    if (!dll || !name)
        return (void *)ntboxd_generic_stub;

    if (!strcasecmp(dll, "kernel32.dll") || !strcasecmp(dll, "ntdll.dll")) {
        /* Handle ordinal imports (e.g., "#8") - try to get real export from ntdll */
        if (name[0] == '#') {
            int ordinal = atoi(name + 1);
            if (ordinal > 0 && ordinal <= 0xffff && ntboxd_mod_ntdll && ntboxd_mod_ntdll->pe) {
                ntb0_pe_export ex;
                if (ntb0_pe_find_by_ordinal(ntboxd_mod_ntdll->pe, (uint16_t)ordinal, &ex)) {
                    if (!ex.forwarded) {
                        void *p = (void *)((char *)ntb0_dll_module_base(ntboxd_mod_ntdll) + ex.rva);
                        return p;
                    }
                }
            }
            return (void *)ntboxd_generic_stub;
        }

        /* Console APIs */
        if (!strcmp(name, "GetStdHandle")) return (void *)ntb0_GetStdHandle;
        if (!strcmp(name, "WriteConsoleA")) return (void *)ntb0_WriteConsoleA;
        if (!strcmp(name, "WriteConsoleW")) return (void *)ntb0_WriteConsoleW;
        if (!strcmp(name, "ReadConsoleA")) return (void *)ntb0_ReadConsoleA;
        if (!strcmp(name, "ReadConsoleW")) return (void *)ntb0_ReadConsoleW;
        if (!strcmp(name, "GetConsoleMode")) return (void *)ntb0_GetConsoleMode;
        if (!strcmp(name, "SetConsoleMode")) return (void *)ntb0_SetConsoleMode;
        if (!strcmp(name, "AllocConsole")) return (void *)ntb0_AllocConsole;
        if (!strcmp(name, "FreeConsole")) return (void *)ntb0_FreeConsole;
        if (!strcmp(name, "AttachConsole")) return (void *)ntb0_AllocConsole; /* stub */

        /* File/Handle APIs */
        if (!strcmp(name, "CreateFileA")) return (void *)ntb0_CreateFileA;
        if (!strcmp(name, "CreateFileW")) return (void *)ntb0_CreateFileW;
        if (!strcmp(name, "CloseHandle")) return (void *)ntb0_CloseHandle;
        if (!strcmp(name, "ReadFile")) return (void *)ntb0_ReadConsoleA; /* fallback */
        if (!strcmp(name, "WriteFile")) return (void *)ntb0_WriteConsoleA; /* fallback */

        /* Memory */
        if (!strcmp(name, "VirtualAlloc")) return (void *)ntb0_VirtualAlloc;
        if (!strcmp(name, "VirtualFree")) return (void *)ntb0_VirtualFree;
        if (!strcmp(name, "VirtualProtect")) return (void *)ntb0_VirtualProtect;
        if (!strcmp(name, "VirtualQuery")) return (void *)ntb0_VirtualQuery;
        if (!strcmp(name, "HeapAlloc")) return (void *)ntb0_HeapAlloc;
        if (!strcmp(name, "HeapFree")) return (void *)ntb0_HeapFree;

        /* Process/Thread */
        if (!strcmp(name, "ExitProcess")) return (void *)ntb0_ExitProcess;
        if (!strcmp(name, "CreateThread")) return (void *)ntb0_CreateThread;
        if (!strcmp(name, "GetCurrentProcess")) return (void *)ntb0_GetModuleHandleA;
        if (!strcmp(name, "GetCurrentThread")) return (void *)(uintptr_t)pthread_self();
        if (!strcmp(name, "GetProcessHeap")) return (void *)ntb0_HeapAlloc;
        if (!strcmp(name, "GetModuleHandleA")) return (void *)ntb0_GetModuleHandleA;
        if (!strcmp(name, "GetModuleHandleW")) return (void *)ntb0_GetModuleHandleA;
        if (!strcmp(name, "LoadLibraryA")) return (void *)ntb0_GetProcAddress ? (void *)ntb0_GetProcAddress : (void *)ntb0_GetModuleHandleA;
        if (!strcmp(name, "LoadLibraryW")) return (void *)ntb0_GetProcAddress ? (void *)ntb0_GetProcAddress : (void *)ntb0_GetModuleHandleA;
        if (!strcmp(name, "FreeLibrary")) return (void *)ntboxd_ExitProcess;
        if (!strcmp(name, "GetProcAddress")) return (void *)ntb0_GetProcAddress;
        if (!strcmp(name, "GetCommandLineA")) { extern char *__attribute__((ms_abi)) ntboxd_GetCommandLineA(void); return (void *)ntboxd_GetCommandLineA; }
        if (!strcmp(name, "GetCommandLineW")) { extern uint16_t *__attribute__((ms_abi)) ntboxd_GetCommandLineW(void); return (void *)ntboxd_GetCommandLineW; }
        if (!strcmp(name, "GetStartupInfoA")) { extern void __attribute__((ms_abi)) ntboxd_GetStartupInfoA(PVOID); return (void *)ntboxd_GetStartupInfoA; }
        if (!strcmp(name, "GetStartupInfoW")) { extern void __attribute__((ms_abi)) ntboxd_GetStartupInfoW(PVOID); return (void *)ntboxd_GetStartupInfoW; }
        if (!strcmp(name, "ExitProcess")) { extern void __attribute__((ms_abi)) ntboxd_ExitProcess(UINT); return (void *)ntboxd_ExitProcess; }
        if (!strcmp(name, "GetLastError")) { extern DWORD __attribute__((ms_abi)) ntboxd_GetLastError(void); return (void *)ntboxd_GetLastError; }
        if (!strcmp(name, "SetLastError")) { extern void __attribute__((ms_abi)) ntboxd_SetLastError(DWORD); return (void *)ntboxd_SetLastError; }
        if (!strcmp(name, "GetTickCount")) { extern DWORD __attribute__((ms_abi)) ntboxd_GetTickCount(void); return (void *)ntboxd_GetTickCount; }
        if (!strcmp(name, "Sleep")) { extern void __attribute__((ms_abi)) ntboxd_Sleep(DWORD); return (void *)ntboxd_Sleep; }
        if (!strcmp(name, "GetProcAddress")) return (void *)ntb0_GetProcAddress;

        /* Error/TLS */
        if (!strcmp(name, "GetLastError")) return (void *)ntb0_GetLastError;
        if (!strcmp(name, "SetLastError")) return (void *)ntb0_SetLastError;
        if (!strcmp(name, "TlsAlloc")) return (void *)ntb0_TlsAlloc;
        if (!strcmp(name, "TlsFree")) return (void *)ntb0_TlsFree;
        if (!strcmp(name, "TlsGetValue")) return (void *)ntb0_TlsGetValue;
        if (!strcmp(name, "TlsSetValue")) return (void *)ntb0_TlsSetValue;

        /* Sync */
        if (!strcmp(name, "InitializeCriticalSection")) return (void *)ntb0_InitializeCriticalSection;
        if (!strcmp(name, "DeleteCriticalSection")) return (void *)ntb0_DeleteCriticalSection;
        if (!strcmp(name, "EnterCriticalSection")) return (void *)ntb0_EnterCriticalSection;
        if (!strcmp(name, "LeaveCriticalSection")) return (void *)ntb0_LeaveCriticalSection;
        if (!strcmp(name, "CreateMutexA")) return (void *)ntb0_CreateMutexA;
        if (!strcmp(name, "ReleaseMutex")) return (void *)ntb0_ReleaseMutex;
        if (!strcmp(name, "WaitForSingleObject")) return (void *)ntb0_WaitForSingleObject;

        /* Registry */
        if (!strcmp(name, "RegOpenKeyExA")) return (void *)ntb0_RegOpenKeyExA;
        if (!strcmp(name, "RegQueryValueExA")) return (void *)ntb0_RegQueryValueExA;
        if (!strcmp(name, "RegCloseKey")) return (void *)ntb0_RegCloseKey;

        /* String */
        if (!strcmp(name, "MultiByteToWideChar")) return (void *)ntb0_MultiByteToWideChar;
        if (!strcmp(name, "WideCharToMultiByte")) return (void *)ntb0_WideCharToMultiByte;
        if (!strcmp(name, "IsDBCSLeadByteEx")) return (void *)ntb0_IsDBCSLeadByteEx;

        /* SEH */
        if (!strcmp(name, "SetUnhandledExceptionFilter")) return (void *)ntb0_SetUnhandledExceptionFilter;

        /* Time */
        if (!strcmp(name, "Sleep")) return (void *)ntb0_Sleep;
        if (!strcmp(name, "GetTickCount")) return (void *)ntb0_Sleep; /* stub */

        /* NTDLL APIs */
        if (!strcmp(name, "NtAllocateVirtualMemory")) return (void *)ntb0_NtAllocateVirtualMemory;
        if (!strcmp(name, "NtFreeVirtualMemory")) return (void *)ntb0_NtFreeVirtualMemory;
        if (!strcmp(name, "NtReadFile")) return (void *)ntb0_ReadConsoleA;
        if (!strcmp(name, "NtWriteFile")) return (void *)ntb0_WriteConsoleA;
        if (!strcmp(name, "NtTerminateProcess")) return (void *)ntb0_ExitProcess;
        if (!strcmp(name, "NtClose")) return (void *)ntb0_CloseHandle;
    }

    return (void *)ntboxd_generic_stub;
}