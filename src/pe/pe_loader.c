#include "ntbox.h"
#include "pe_loader.h"
#include "ntbox_types.h"
#include "linux_bridge.h"

/* PE (Portable Executable) minimal validation for Demo. */
int ntbox_pe_check(const void *data, size_t len)
{
    const unsigned char *b = data;
    u32 e_lfanew;

    if (!data || len < 64)
        return -EINVAL;
    if (b[0] != 'M' || b[1] != 'Z') {
        NTBOX_ERR("pe_loader: bad MZ magic\n");
        return -EINVAL;
    }
    e_lfanew = *(const u32 *)(b + 0x3C);
    if (e_lfanew == 0 || e_lfanew + 4 + 20 + 2 > len) {
        NTBOX_ERR("pe_loader: bad e_lfanew\n");
        return -EINVAL;
    }
    if (b[e_lfanew] != 'P' || b[e_lfanew + 1] != 'E' ||
        b[e_lfanew + 2] != 0 || b[e_lfanew + 3] != 0) {
        NTBOX_ERR("pe_loader: bad PE signature\n");
        return -EINVAL;
    }
    /* Optional header magic: 0x20B = PE32+ (x86_64) */
    {
        const unsigned char *opt = b + e_lfanew + 4 + 20;
        u16 magic;
        if (e_lfanew + 4 + 20 + 2 > len)
            return -EINVAL;
        magic = *(const u16 *)opt;
        if (magic != 0x20B && magic != 0x10B) {
            NTBOX_ERR("pe_loader: bad optional header magic 0x%x\n", magic);
            return -EINVAL;
        }
    }
    NTBOX_LOG("pe_loader: MZ/PE OK\n");
    return 0;
}

unsigned long ntbox_pe_get_entry(const void *data)
{
    const unsigned char *b = data;
    u32 e_lfanew;
    const unsigned char *opt;
    u32 rva;

    if (!b)
        return 0;
    e_lfanew = *(const u32 *)(b + 0x3C);
    opt = b + e_lfanew + 4 + 20; /* skip PE sig + COFF */
    rva = *(const u32 *)(opt + 16); /* AddressOfEntryPoint */
    NTBOX_LOG("pe_loader: DriverEntry found at 0x%lx\n", (unsigned long)rva);
    return rva;
}