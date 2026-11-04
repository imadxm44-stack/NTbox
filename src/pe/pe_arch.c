/*
 * DublKernel — common PE architecture loader (x86 PE32 + x64 PE32+).
 * Shared by pe_run.c (.sys / kernel-mode service) and pe_user.c (.exe / user-mode app).
 * All Windows PE files are loaded as-is; resource access goes through linux_bridge.
 * Focus: service loading + app execution. No games, no bloat, no textures, no wallpapers.
 */

#include "ntbox.h"
#include "ntbox_types.h"
#include "pe_arch.h"
#include "pe_loader.h"
#include "ntb_trace.h"
#include "linux_bridge.h"

#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/string.h>
#include <asm/pgtable.h>
#include <asm/tlbflush.h>

/* Pattern: movabs rax, 0xfffff78000000320 (KUSER_SHARED_DATA on Windows) */
static const unsigned char kuser_pat[10] = {
    0x48, 0xb8, 0x20, 0x03, 0x00, 0x00, 0x80, 0xf7, 0xff, 0xff
};

/* ------------------------------------------------------------------ */
/* قائمة модуلي الخدمة (ليست تاسك مانجر للمستخدم)                     */
/* ------------------------------------------------------------------ */

struct list_head ntbox_module_list = LIST_HEAD_INIT(ntbox_module_list);
DEFINE_SPINLOCK(ntbox_module_lock);

int ntbox_module_add(const char *name, void *base, u32 size, int is_dll)
{
    struct ntbox_module *m;

    m = kmalloc(sizeof(*m), GFP_KERNEL);
    if (!m)
        return -ENOMEM;
    strncpy(m->name, name, sizeof(m->name) - 1);
    m->name[sizeof(m->name) - 1] = '\0';
    m->base = base;
    m->size = size;
    m->is_dll = is_dll;
    INIT_LIST_HEAD(&m->node);

    spin_lock(&ntbox_module_lock);
    /*
     * Defensive: a corrupted list head (e.g. a wild write from a previous
     * failed PE load) makes list_add_tail() write through a NULL pointer
     * and Oops the kernel. Detect and rebuild instead of dying.
     */
    if (ntbox_module_list.next != &ntbox_module_list ||
        ntbox_module_list.prev != &ntbox_module_list) {
        NTBOX_ERR("pe_arch: module list head corrupted (next=%p prev=%p) — reinitializing\n",
                  ntbox_module_list.next, ntbox_module_list.prev);
        INIT_LIST_HEAD(&ntbox_module_list);
    }
    list_add_tail(&m->node, &ntbox_module_list);
    spin_unlock(&ntbox_module_lock);

    NTBOX_LOG("module loaded: %s base=%p size=%u is_dll=%d\n",
              m->name, m->base, m->size, m->is_dll);
    return 0;
}

void ntbox_module_remove(void *base)
{
    struct ntbox_module *m, *tmp;

    spin_lock(&ntbox_module_lock);
    if (ntbox_module_list.next != &ntbox_module_list ||
        ntbox_module_list.prev != &ntbox_module_list) {
        NTBOX_ERR("pe_arch: module list head corrupted — skipping removal\n");
        spin_unlock(&ntbox_module_lock);
        return;
    }
    list_for_each_entry_safe(m, tmp, &ntbox_module_list, node) {
        if (m->base == base) {
            list_del(&m->node);
            kfree(m);
            break;
        }
    }
    spin_unlock(&ntbox_module_lock);
}

/* ------------------------------------------------------------------ */
/* Detection                                                          */
/* ------------------------------------------------------------------ */

int ntbox_pe_detect_arch(const void *data, size_t len,
                         struct ntbox_pe_info *info)
{
    const u8 *b = data;
    u32 e_lfanew;
    const u8 *nt, *opt;
    u16 machine, magic;
    u16 optsize;

    if (!data || !info || len < 64)
        return -EINVAL;
    memset(info, 0, sizeof(*info));

    if (b[0] != 'M' || b[1] != 'Z') {
        NTBOX_ERR("pe_arch: bad MZ magic\n");
        return -EINVAL;
    }
    e_lfanew = *(const u32 *)(b + 0x3C);
    if (e_lfanew == 0 || e_lfanew + 4 + 20 + 2 > len) {
        NTBOX_ERR("pe_arch: bad e_lfanew\n");
        return -EINVAL;
    }
    nt = b + e_lfanew;
    if (nt[0] != 'P' || nt[1] != 'E' || nt[2] != 0 || nt[3] != 0) {
        NTBOX_ERR("pe_arch: bad PE signature\n");
        return -EINVAL;
    }

    machine = *(const u16 *)(nt + 4);
    /*
     * SizeOfOptionalHeader lives at COFF+16 == nt+20.
     * (nt+16 is NumberOfSymbols — using it shifts the optional header and
     *  silently mis-parses every PE that ships a symbol table.)
     */
    optsize = *(const u16 *)(nt + 20);
    if (optsize < PE_OFF_SIZEOFHEADERS + 4) {
        NTBOX_ERR("pe_arch: bad SizeOfOptionalHeader %u\n", optsize);
        return -EINVAL;
    }
    if ((size_t)(nt - b) + 24 + optsize > len) {
        NTBOX_ERR("pe_arch: optional header out of file\n");
        return -EINVAL;
    }
    opt = nt + 24;
    magic = *(const u16 *)opt;

    info->machine = machine;
    info->magic = magic;

    if (machine == IMAGE_FILE_MACHINE_I386 && magic == PE_NT_OPTIONAL_HDR32_MAGIC) {
        info->arch = NTBOX_PE_ARCH_I386;
        info->is_pe32 = 1;
    } else if (machine == IMAGE_FILE_MACHINE_AMD64 && magic == PE_NT_OPTIONAL_HDR64_MAGIC) {
        info->arch = NTBOX_PE_ARCH_AMD64;
        info->is_pe32 = 0;
    } else {
        NTBOX_ERR("pe_arch: unsupported machine=0x%x magic=0x%x\n", machine, magic);
        return -ENOTSUPP;
    }

    info->entry_rva = *(const u32 *)(opt + PE_OFF_ENTRYPOINT);
    info->size_of_image = *(const u32 *)(opt + PE_OFF_SIZEOFIMAGE);
    info->size_of_headers = *(const u32 *)(opt + PE_OFF_SIZEOFHEADERS);

    if (info->is_pe32)
        info->image_base_file = *(const u32 *)(opt + PE32_OFF_IMAGEBASE);
    else
        info->image_base_file = *(const u64 *)(opt + PE64_OFF_IMAGEBASE);

    /* Import / BaseReloc directories live at different offsets per arch. */
    if (info->is_pe32) {
        if (optsize >= PE32_OFF_RELOC_DIR + 8) {
            info->import_rva = *(const u32 *)(opt + PE32_OFF_IMPORT_DIR);
            info->reloc_rva  = *(const u32 *)(opt + PE32_OFF_RELOC_DIR);
            info->reloc_size = *(const u32 *)(opt + PE32_OFF_RELOC_DIR + 4);
        }
    } else {
        if (optsize >= PE64_OFF_RELOC_DIR + 8) {
            info->import_rva = *(const u32 *)(opt + PE64_OFF_IMPORT_DIR);
            info->reloc_rva  = *(const u32 *)(opt + PE64_OFF_RELOC_DIR);
            info->reloc_size = *(const u32 *)(opt + PE64_OFF_RELOC_DIR + 4);
        }
    }

    NTBOX_LOG("pe_arch: arch=%s machine=0x%x magic=0x%x entry=0x%x base=0x%llx "
              "image=0x%x import_rva=0x%x reloc_rva=0x%x\n",
              info->is_pe32 ? "PE32(x86)" : "PE32+(x64)",
              machine, magic, info->entry_rva,
              (unsigned long long)info->image_base_file,
              info->size_of_image, info->import_rva, info->reloc_rva);

    return 0;
}

/* ------------------------------------------------------------------ */
/* Loading                                                            */
/* ------------------------------------------------------------------ */

static int copy_sections(u8 *image, const u8 *b, size_t filesize,
                         const u8 *sections, u16 nsec, u32 size_of_image)
{
    u32 i;

    for (i = 0; i < nsec; i++) {
        const u8 *sh = sections + i * 40;
        u32 vsize = *(const u32 *)(sh + 8);
        u32 vaddr = *(const u32 *)(sh + 12);
        u32 rawsize = *(const u32 *)(sh + 16);
        u32 rawptr = *(const u32 *)(sh + 20);
        u32 tocopy = rawsize;

        if (rawptr == 0 || tocopy == 0)
            continue;
        if (vsize != 0 && tocopy > vsize)
            tocopy = vsize;
        if ((u64)rawptr + (u64)tocopy > (u64)filesize)
            continue;
        /* 64-bit compare: u32 vaddr + tocopy can overflow and wrap past the
         * image bound -> wild write into kernel memory. */
        if ((u64)vaddr + (u64)tocopy > (u64)size_of_image)
            continue;
        memcpy(image + vaddr, b + rawptr, tocopy);
    }
    return 0;
}

static void patch_kuser_shared_data(u8 *image, u32 size_of_image)
{
    static u64 ntb_kuser_cookie = 0x00002b992ddfa232ULL;
    size_t off;

    for (off = 0; off + 10 <= size_of_image; off++) {
        if (memcmp(image + off, kuser_pat, 10) == 0) {
            *(u64 *)(image + off + 2) = (u64)&ntb_kuser_cookie;
            NTBOX_LOG("pe_arch: patched KUSER_SHARED_DATA ref at +0x%zx\n", off);
        }
    }
}

static void patch_io_create_device(u8 *image, u32 size_of_image)
{
    const unsigned char p1[5] = { 0x4c, 0x89, 0x5c, 0x24, 0x30 };
    const unsigned char r1[5] = { 0x4c, 0x89, 0x5c, 0x24, 0x38 };
    const unsigned char p2[5] = { 0xc6, 0x44, 0x24, 0x28, 0x00 };
    const unsigned char r2[5] = { 0xc6, 0x44, 0x24, 0x30, 0x00 };
    const unsigned char p3[8] = { 0xc7, 0x44, 0x24, 0x20, 0x00, 0x1, 0x00, 0x00 };
    const unsigned char r3[8] = { 0xc7, 0x44, 0x24, 0x28, 0x00, 0x1, 0x00, 0x00 };
    size_t off;

    for (off = 0; off + 8 <= size_of_image; off++) {
        if (memcmp(image + off, p3, 8) == 0) {
            memcpy(image + off, r3, 8);
            NTBOX_LOG("pe_arch: patched IoCreateDevice arg3 at +0x%zx\n", off);
        }
        if (off + 5 <= size_of_image) {
            if (memcmp(image + off, p1, 5) == 0) {
                memcpy(image + off, r1, 5);
                NTBOX_LOG("pe_arch: patched IoCreateDevice arg1 at +0x%zx\n", off);
            }
            if (memcmp(image + off, p2, 5) == 0) {
                memcpy(image + off, r2, 5);
                NTBOX_LOG("pe_arch: patched IoCreateDevice arg2 at +0x%zx\n", off);
            }
        }
    }
}

static void apply_relocations(u8 *image, struct ntbox_pe_info *info,
                              u64 new_base, u64 delta)
{
    u8 *rel;
    u32 remaining;
    u32 nblk = 0;
    u32 img_size = info->size_of_image;

    if (delta == 0 || !info->reloc_rva || !info->reloc_size)
        return;

    /* The relocation table itself must live inside the image. */
    if ((u64)info->reloc_rva + (u64)info->reloc_size > (u64)img_size) {
        NTBOX_ERR("pe_arch: relocation table out of image (rva=0x%x size=%u img=%u) — skipped\n",
                  info->reloc_rva, info->reloc_size, img_size);
        return;
    }

    rel = image + info->reloc_rva;
    remaining = info->reloc_size;

    while (remaining >= 8) {
        u32 page_rva = *(const u32 *)rel;
        u32 block_size = *(const u32 *)(rel + 4);
        u32 nent;
        u16 *ents;
        u32 e;

        if (block_size < 8 || block_size > remaining)
            break;
        nent = (block_size - 8) / 2;
        ents = (u16 *)(rel + 8);

        for (e = 0; e < nent; e++) {
            u16 entry = ents[e];
            u8 type = entry >> 12;
            u32 off = entry & 0xFFF;
            u64 target = (u64)page_rva + (u64)off;

            if (type == 0) /* IMAGE_REL_BASED_ABSOLUTE */
                continue;
            if (type == 3 && info->is_pe32) { /* HIGHLOW — 32-bit fixup */
                if (target + 4 > (u64)img_size) {
                    NTBOX_ERR("pe_arch: reloc target 0x%llx out of image — skipped\n",
                              (unsigned long long)target);
                    continue;
                }
                u32 *t = (u32 *)(image + target);
                *t += (u32)delta;
            } else if (type == 10 && !info->is_pe32) { /* DIR64 — 64-bit fixup */
                if (target + 8 > (u64)img_size) {
                    NTBOX_ERR("pe_arch: reloc target 0x%llx out of image — skipped\n",
                              (unsigned long long)target);
                    continue;
                }
                u64 *t = (u64 *)(image + target);
                *t += delta;
            } else {
                NTBOX_LOG("pe_arch: reloc type %u skipped (pe32=%d)\n",
                          type, info->is_pe32);
            }
        }
        rel += block_size;
        remaining -= block_size;
        nblk++;
    }
    NTBOX_LOG("pe_arch: applied %d reloc blocks (delta=0x%llx, pe32=%d)\n",
              nblk, (unsigned long long)delta, info->is_pe32);
}

/* Return 1 if rva points to a NUL-terminated string inside the image. */
static int pe_str_valid(const u8 *image, u32 img_size, u32 rva)
{
    u32 i;

    if (rva == 0 || rva >= img_size)
        return 0;
    for (i = rva; i < img_size; i++)
        if (image[i] == '\0')
            return 1;
    return 0;
}

static int resolve_imports(u8 *image, struct ntbox_pe_info *info)
{
    u8 *imp;
    int nimp = 0, nunresolved = 0;
    u32 img_size = info->size_of_image;

    if (!info->import_rva)
        return 0;

    /* Every RVA used below is relative to the image: reject anything that
     * would read or write outside the mapped image (kernel memory corruption). */
    if ((u64)info->import_rva + 20 > (u64)img_size) {
        NTBOX_ERR("pe_arch: import dir out of image (rva=0x%x img=%u) — skipped\n",
                  info->import_rva, img_size);
        return 0;
    }

    imp = image + info->import_rva;

    while ((u64)(imp - image) + 20 <= (u64)img_size) {
        u32 orig_thunk = *(const u32 *)imp;
        u32 name_rva = *(const u32 *)(imp + 12);
        u32 first_thunk = *(const u32 *)(imp + 16);
        u32 thunk_rva;
        u32 orig_rva;
        u32 *thunk;
        int orig_ok;

        if (name_rva == 0 && first_thunk == 0 && orig_thunk == 0)
            break;

        if (name_rva && !pe_str_valid(image, img_size, name_rva)) {
            NTBOX_ERR("pe_arch: import dll name rva 0x%x out of image — skipped\n",
                      name_rva);
            imp += 20;
            nimp++;
            continue;
        }

        thunk_rva = first_thunk ? first_thunk : orig_thunk;
        orig_rva = orig_thunk ? orig_thunk : first_thunk;

        if (thunk_rva == 0 || thunk_rva >= img_size) {
            NTBOX_ERR("pe_arch: import thunk rva 0x%x out of image — skipped\n",
                      thunk_rva);
            imp += 20;
            nimp++;
            continue;
        }

        thunk = (u32 *)(image + thunk_rva);
        orig_ok = (orig_rva && (u64)orig_rva + 4 <= (u64)img_size);

        {
            u32 max_idx = (img_size - thunk_rva) / 4;
            u32 orig_max = orig_ok ? (img_size - orig_rva) / 4 : 0;
            u32 idx = 0;

            while (idx < max_idx) {
                u32 t;
                void *resolved = NULL;

                if (orig_ok) {
                    if (idx >= orig_max)
                        break;
                    t = *(const u32 *)(image + orig_rva + idx * 4);
                } else {
                    t = thunk[idx];
                }
                if (t == 0)
                    break;

                if (t & 0x80000000ULL) {
                    NTBOX_LOG("pe_arch: ordinal import %u from %s (unsupported)\n",
                              (unsigned int)(t & 0xFFFF),
                              name_rva ? (const char *)(image + name_rva) : "?");
                } else {
                    const char *name;
                    if (!pe_str_valid(image, img_size, t + 2)) {
                        NTBOX_ERR("pe_arch: import name rva 0x%x out of image — skipped\n",
                                  t + 2);
                        idx++;
                        continue;
                    }
                    name = (const char *)(image + t + 2);
                    resolved = ntbox_resolve_import(
                        name_rva ? (const char *)(image + name_rva) : "?", name);
                    if (!resolved) {
                        NTBOX_LOG("pe_arch: UNRESOLVED import %s!%s\n",
                                  name_rva ? (const char *)(image + name_rva) : "?", name);
                        nunresolved++;
                    } else {
                        NTBOX_LOG("pe_arch: resolved %s!%s -> %p\n",
                                  name_rva ? (const char *)(image + name_rva) : "?", name,
                                  resolved);
                    }
                }
                thunk[idx] = (u32)(unsigned long)resolved;
                idx++;
            }
        }
        imp += 20;
        nimp++;
    }

    info->unresolved_imports = nunresolved;
    NTBOX_LOG("pe_arch: processed %d import descriptors (%d unresolved)\n",
              nimp, nunresolved);
    return nunresolved;
}

u8 *ntbox_pe_load_image(const void *filedata, size_t filesize,
                        struct ntbox_pe_info *info)
{
    const u8 *b = filedata;
    u32 e_lfanew;
    const u8 *nt, *opt, *sections;
    u16 nsec;
    u32 optsize;
    u8 *image;
    u64 new_base, delta;
    size_t hc;
    int ret;

    ret = ntbox_pe_detect_arch(b, filesize, info);
    if (ret)
        return NULL;

    e_lfanew = *(const u32 *)(b + 0x3C);
    nt = b + e_lfanew;
    nsec = *(const u16 *)(nt + 6);
    optsize = *(const u16 *)(nt + 20);
    opt = nt + 24;
    sections = opt + optsize;

    if ((size_t)(sections - b) + (size_t)nsec * 40 > filesize) {
        NTBOX_ERR("pe_arch: section table out of file\n");
        return NULL;
    }

    if (info->size_of_image == 0 || info->size_of_image > (128 * 1024 * 1024)) {
        NTBOX_ERR("pe_arch: bad SizeOfImage %u\n", info->size_of_image);
        return NULL;
    }
    if (nsec == 0 || nsec > 96) {
        NTBOX_ERR("pe_arch: bad nsec %u\n", nsec);
        return NULL;
    }

    image = vmalloc(info->size_of_image);
    if (!image) {
        NTBOX_ERR("pe_arch: vmalloc(%u) failed\n", info->size_of_image);
        return NULL;
    }
    memset(image, 0, info->size_of_image);

    /* Clear NX so loaded PE code can execute. */
    {
        unsigned long a;
        for (a = (unsigned long)image;
             a < (unsigned long)image + info->size_of_image;
             a += PAGE_SIZE) {
            int level;
            pte_t *pte = lookup_address(a, &level);
            if (pte) {
                set_pte(pte, __pte(pte_val(*pte) & ~_PAGE_NX));
            }
        }
        __flush_tlb_all();
    }

    /* Copy headers */
    hc = info->size_of_headers;
    if (hc > filesize) hc = filesize;
    if (hc > info->size_of_image) hc = info->size_of_image;
    memcpy(image, b, hc);

    /* Copy sections */
    copy_sections(image, b, filesize, sections, nsec, info->size_of_image);

    /* Patch Windows KUSER_SHARED_DATA references */
    patch_kuser_shared_data(image, info->size_of_image);

    /* Patch IoCreateDevice argument placement (MSVC quirk) */
    patch_io_create_device(image, info->size_of_image);

    /* Base relocations */
    new_base = (u64)image;
    delta = new_base - info->image_base_file;
    if (info->is_pe32) {
        /* A 32-bit image needs a 32-bit delta and a <4GiB load address;
         * a 64-bit kernel vmalloc base can never satisfy that. */
        NTBOX_ERR("pe_arch: PE32 relocation skipped (64-bit kernel base 0x%llx)\n",
                  (unsigned long long)new_base);
        delta = 0;
    }
    apply_relocations(image, info, new_base, delta);

    /* Resolve imports */
    ret = resolve_imports(image, info);
    if (ret > 0) {
        NTB_FAIL(NTB_TID_PE_001, -ENOTSUPP,
                 "%d unresolved imports — aborting (error isolation)",
                 info->unresolved_imports);
        vfree(image);
        return NULL;
    }

    NTBOX_LOG("pe_arch: image loaded at %p (base=0x%llx, delta=0x%llx, entry=0x%x)\n",
              image, (unsigned long long)new_base,
              (unsigned long long)delta, info->entry_rva);
    return image;
}

u8 *ntbox_pe_load_dll(const void *filedata, size_t filesize,
                      struct ntbox_pe_info *info)
{
    /* DLL loading uses the same image loader; entry point is called separately
     * via ntbox_pe_call_entry() with is_dll=1. */
    return ntbox_pe_load_image(filedata, filesize, info);
}

int ntbox_pe_call_entry(u8 *image, struct ntbox_pe_info *info, int is_dll)
{
    if (is_dll) {
        NTBOX_LOG("pe_arch: calling DllEntryPoint at image+0x%x (DLL_PROCESS_ATTACH)\n",
                  info->entry_rva);
        int (*dll_entry)(PVOID, ULONG, PVOID) =
            (int (*)(PVOID, ULONG, PVOID))(image + info->entry_rva);
        return dll_entry(NULL, 1, NULL); /* DLL_PROCESS_ATTACH = 1 */
    } else {
        NTBOX_LOG("pe_arch: calling EXE entry at image+0x%x\n", info->entry_rva);
        int (*exe_entry)(void) = (int (*)(void))(image + info->entry_rva);
        return exe_entry();
    }
}

void ntbox_pe_unload_image(u8 *image)
{
    if (image)
        vfree(image);
}
