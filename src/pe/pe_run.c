/*
 * DublKernel M3 — real PE execution (Windows 7 base)
 * Kernel-mode .sys loader: supports x86 (PE32) and x64 (PE32+).
 * Calls the real DriverEntry of the loaded driver.
 * All resource access (CPU/RAM/disk/GPU) goes through linux_bridge → Linux kernel.
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

/* Common: load any PE (x86 PE32 or x64 PE32+) and run its entry point.
 * pe_run path is used for .sys drivers (DriverEntry). */
int ntbox_run_real_pe(const void *filedata, size_t filesize, const char *path)
{
    struct ntbox_pe_info info;
    u8 *image;
    PDRIVER_OBJECT drv;
    NTSTATUS status;
    int ret = 0;

    (void)path;

    if (!filedata || filesize < 0x100)
        return -EINVAL;

    /* Same rule as the user-mode path: 64-bit kernel mode cannot execute
     * 32-bit (PE32/x86) machine code — reject instead of jumping into it. */
    ret = ntbox_pe_detect_arch(filedata, filesize, &info);
    if (ret)
        return ret;
    if (info.is_pe32) {
        NTBOX_ERR("pe_run: PE32 (x86 32-bit) drivers cannot run in 64-bit kernel mode\n");
        return -ENOTSUPP;
    }

    /* Load PE image (detects x86/x64, applies relocs + imports) */
    image = ntbox_pe_load_image(filedata, filesize, &info);
    if (!image) {
        NTBOX_ERR("pe_run: failed to load PE image\n");
        return -EINVAL;
    }

    /* Create driver object */
    drv = ntbox_create_driver_object();
    if (!drv) {
        NTBOX_ERR("pe_run: failed to create DRIVER_OBJECT\n");
        ntbox_pe_unload_image(image);
        return -ENOMEM;
    }

    /* Call real DriverEntry */
    {
        typedef NTSTATUS (NTB_MSABI *ENTRY)(PDRIVER_OBJECT, PUNICODE_STRING);
        ENTRY entry = (ENTRY)(image + info.entry_rva);
        static const u16 regbuf[] = {
            '\\','R','e','g','i','s','t','r','y','\\',
            'M','a','c','h','i','n','e','\\',
            'S','y','s','t','e','m','\\',
            'C','u','r','r','e','n','t','C','o','n','t','r','o','l','S','e','t','\\',
            'S','e','r','v','i','c','e','s','\\',
            'a','m','d','k','m','d','a','g','\0'
        };
        UNICODE_STRING regstr;
        regstr.Length = (u16)((sizeof(regbuf) / sizeof(regbuf[0]) - 1) * sizeof(u16));
        regstr.MaximumLength = (u16)sizeof(regbuf);
        regstr.Buffer = (u16 *)regbuf;

        NTBOX_LOG("pe_run: calling real DriverEntry at image+0x%x (arch=%s)\n",
                  info.entry_rva, info.is_pe32 ? "PE32(x86)" : "PE32+(x64)");
        status = entry(drv, &regstr);
        NTBOX_LOG("pe_run: DriverEntry returned 0x%08x\n", (unsigned int)status);
        if (status != STATUS_SUCCESS) {
            ntbox_destroy_driver_object(drv);
            ntbox_pe_unload_image(image);
            return -EIO;
        }
    }

    /* IRP test (uses the driver's own dispatch routines) */
    ret = ntbox_test_create_close(drv);
    if (ret != 0) {
        NTBOX_ERR("pe_run: IRP test failed %d\n", ret);
    } else {
        NTBOX_LOG("NT-Box Demo Test: PASSED (real PE, arch=%s)\n",
                  info.is_pe32 ? "PE32(x86)" : "PE32+(x64)");
    }

    ntbox_destroy_driver_object(drv);
    ntbox_pe_unload_image(image);
    return ret;
}
