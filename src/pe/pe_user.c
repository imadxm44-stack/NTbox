/*
 * DublKernel M6 — user-mode PE execution (.exe).
 * Supports x86 (PE32) and x64 (PE32+).
 * Loads the PE image, resolves imports via linux_bridge → Linux kernel,
 * then calls the entry point. All resource access goes through the bridge.
 * Focus: run any Windows .exe as if on real Windows. No games, no bloat.
 */

#include "ntbox.h"
#include "ntbox_types.h"
#include "pe_arch.h"
#include "pe_loader.h"
#include "ntb_trace.h"
#include "nt_object.h"
#include "irp_core.h"
#include "ntapi_stubs.h"
#include "linux_bridge.h"

#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/string.h>

/*
 * Run a user-mode .exe (x86 PE32 or x64 PE32+).
 * Auto-detects architecture from the PE file.
 * Registers the module in the service module list (not a user task manager).
 */
int ntbox_run_user_pe(const void *filedata, size_t filesize, const char *path)
{
    struct ntbox_pe_info info;
    u8 *image;
    int ret = 0;

    (void)path;

    if (!filedata || filesize < 0x100)
        return -EINVAL;

    /*
     * A 64-bit kernel cannot execute 32-bit (PE32/x86) machine code:
     * there is no IA-32 execution state in kernel mode, and a PE32 image
     * cannot be relocated to a <4GiB address from vmalloc. Reject cleanly
     * instead of jumping into x86 code (that used to Oops the kernel).
     * 32-bit Windows binaries are executed by the user-space translator
     * (tools/ntboxd) inside a 32-bit helper process.
     */
    ret = ntbox_pe_detect_arch(filedata, filesize, &info);
    if (ret)
        return ret;
    if (info.is_pe32) {
        NTBOX_ERR("pe_user: PE32 (x86 32-bit) images cannot run in 64-bit kernel mode\n");
        return -ENOTSUPP;
    }

    /* Load PE image (detects x86/x64, applies relocs + imports) */
    image = ntbox_pe_load_image(filedata, filesize, &info);
    if (!image) {
        NTBOX_ERR("pe_user: failed to load PE image\n");
        return -EINVAL;
    }

    /* Register in service module list (for service tracking, not user task manager) */
    ntbox_module_add("userapp", image, info.size_of_image, 0);

    /* Call user-mode entry point */
    ret = ntbox_pe_call_entry(image, &info, 0); /* is_dll=0 for EXE */
    NTBOX_LOG("pe_user: entry returned %d\n", ret);

    /* Unregister and free */
    ntbox_module_remove(image);
    ntbox_pe_unload_image(image);

    NTBOX_LOG("NT-Box User PE: PASSED (arch=%s)\n",
              info.is_pe32 ? "PE32(x86)" : "PE32+(x64)");
    return ret;
}
