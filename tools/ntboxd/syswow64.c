/*
 * syswow64.c — SysWOW64 / PE32 loader & translation layer implementation.
 *
 * This module provides:
 *   1) PE32 (32-bit) binary loading via ntb0_pe32_load_image()
 *   2) 32-bit base relocations (IMAGE_REL_BASED_HIGHLOW) — delegated to dll_loader.c
 *   3) 32-bit import directory parsing & resolution — delegated to dll_loader.c
 *   4) Translation layer dispatch for 32-bit system stubs
 *
 * Key design: SysWOW64 is now a full peer to System32 via:
 *   - ntb0_dll_loader_set_syswow64_sysdir() for dedicated 32-bit directory
 *   - ntb0_dll_load_name() prioritizes syswow64_sysdir for 32-bit modules
 *   - The resolver callback should check module->is64 to pick the right sysdir
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#include "syswow64.h"
#include "dll_loader.h"
#include "pe_export.h"

/* Stub registry for 32-bit translation layer */
#define MAX_STUBS 256

typedef struct {
    char                    dll[128];
    char                    func[128];
    ntb0_syswow64_stub_fn   fn;
} syswow64_stub_entry;

static syswow64_stub_entry g_stubs[MAX_STUBS];
static unsigned            g_n_stubs = 0;

int ntb0_syswow64_register_stub(const char *dll, const char *func,
                                ntb0_syswow64_stub_fn stub)
{
    if (!dll || !func || !stub || g_n_stubs >= MAX_STUBS)
        return -1;
    snprintf(g_stubs[g_n_stubs].dll, sizeof(g_stubs[0].dll), "%s", dll);
    snprintf(g_stubs[g_n_stubs].func, sizeof(g_stubs[0].func), "%s", func);
    g_stubs[g_n_stubs].fn = stub;
    g_n_stubs++;
    return 0;
}

uint32_t ntb0_syswow64_dispatch(const char *dll, const char *func,
                                uint32_t *stack_args)
{
    unsigned i;
    for (i = 0; i < g_n_stubs; i++) {
        if (strcasecmp(g_stubs[i].dll, dll) == 0 &&
            strcasecmp(g_stubs[i].func, func) == 0) {
            if (g_stubs[i].fn)
                return g_stubs[i].fn(stack_args);
        }
    }
    return 0;
}

/* Dedicated PE32 load function */
ntb0_dll_module *ntb0_pe32_load_image(ntb0_dll_loader *ld, const char *path,
                                       uint64_t base, unsigned flags)
{
    /* Delegate to standard ntb0_dll_load_at which fully supports PE32 (is64 == 0) */
    return ntb0_dll_load_at(ld, path, base, flags);
}

/*
 * ntb0_syswow64_apply_relocs — Apply 32-bit base relocations (IMAGE_REL_BASED_HIGHLOW)
 *
 * The actual relocation logic is already handled by dll_loader.c during module loading.
 * dll_loader.c processes the .reloc section and patches DWORD relocations uniformly
 * for both PE32 and PE64 images. This hook is provided for any PE32-specific
 * relocation handling if needed in the future.
 */
int ntb0_syswow64_apply_relocs(ntb0_dll_loader *ld, struct ntb0_dll_module *mod,
                                const void *im)
{
    (void)ld;
    (void)mod;
    (void)im;
    return 0;
}

/*
 * ntb0_syswow64_resolve_imports — Resolve PE32 imports (32-bit system DLLs)
 *
 * When a 32-bit application imports kernel32.dll, ntdll.dll, etc., the resolver
 * should resolve them to the 32-bit versions in SysWOW64 (not System32).
 *
 * The actual import resolution is handled by dll_loader.c, which:
 *   1) Parses the PE32 import directory (.idata)
 *   2) Calls the resolver callback for each import
 *   3) The callback uses ntb0_dll_load_name() to load dependencies
 *
 * ntb0_dll_load_name() now has priority logic:
 *   - If syswow64_sysdir is set, try it first (full peer to System32)
 *   - Then try sysdir
 *   - Then fallback to auto-substitution
 *
 * This hook allows PE32-specific import handling if needed.
 */
int ntb0_syswow64_resolve_imports(ntb0_dll_loader *ld, struct ntb0_dll_module *mod,
                                   const void *im, unsigned flags)
{
    (void)ld;
    (void)mod;
    (void)im;
    (void)flags;
    return 0;
}
