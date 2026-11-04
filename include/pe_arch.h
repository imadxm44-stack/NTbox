#ifndef PE_ARCH_H
#define PE_ARCH_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/spinlock.h>

/* PE machine types */
#define IMAGE_FILE_MACHINE_I386   0x14c   /* x86 32-bit */
#define IMAGE_FILE_MACHINE_AMD64  0x8664  /* x64 64-bit */

/* PE optional header magic */
#define PE_NT_OPTIONAL_HDR32_MAGIC 0x10b   /* PE32 (32-bit) */
#define PE_NT_OPTIONAL_HDR64_MAGIC 0x20b   /* PE32+ (64-bit) */

/* Optional header field offsets */
#define PE_OFF_ENTRYPOINT   16   /* AddressOfEntryPoint (u32) */
#define PE_OFF_SIZEOFIMAGE  56   /* SizeOfImage (u32) */
#define PE_OFF_SIZEOFHEADERS 60  /* SizeOfHeaders (u32) */

/*
 * DataDirectory offsets DIFFER between PE32 and PE32+:
 *   PE32  : DataDirectory starts at opt+96
 *   PE32+ : DataDirectory starts at opt+112
 * Using the PE32+ offsets on a PE32 header reads the wrong field
 * (Exception/Debug instead of Import/BaseReloc) -> imports silently skipped.
 */
#define PE32_OFF_IMPORT_DIR (96 + 1 * 8)   /* DataDirectory[1] = Import Table */
#define PE32_OFF_RELOC_DIR  (96 + 5 * 8)   /* DataDirectory[5] = Base Relocation Table */
#define PE64_OFF_IMPORT_DIR (112 + 1 * 8)  /* DataDirectory[1] = Import Table */
#define PE64_OFF_RELOC_DIR  (112 + 5 * 8)  /* DataDirectory[5] = Base Relocation Table */

/* Keep old names pointing at the PE32+ layout (PE32+ is the only arch the
 * 64-bit kernel can execute). */
#define PE_OFF_IMPORT_DIR   PE64_OFF_IMPORT_DIR
#define PE_OFF_RELOC_DIR    PE64_OFF_RELOC_DIR

/* PE32-specific ImageBase offset (u32 at opt+28; PE32+ uses u64 at opt+24) */
#define PE32_OFF_IMAGEBASE  28
#define PE64_OFF_IMAGEBASE  24

/* PE architecture detection result */
enum ntbox_pe_arch {
    NTBOX_PE_ARCH_UNKNOWN = 0,
    NTBOX_PE_ARCH_I386,   /* x86 32-bit PE32 */
    NTBOX_PE_ARCH_AMD64,  /* x64 64-bit PE32+ */
};

/* Collected PE info shared by kernel-mode (.sys) and user-mode (.exe) loaders. */
struct ntbox_pe_info {
    enum ntbox_pe_arch arch;
    int is_pe32;
    u16 machine;
    u16 magic;
    u32 entry_rva;
    u64 image_base_file;   /* preferred base from the PE file */
    u32 size_of_image;
    u32 size_of_headers;
    u32 import_rva;
    u32 reloc_rva;
    u32 reloc_size;
    int unresolved_imports;
};

/*
 * Detect PE architecture from raw file data.
 * Returns 0 on success, negative errno on failure.
 */
int ntbox_pe_detect_arch(const void *data, size_t len,
                         struct ntbox_pe_info *info);

/*
 * Load a PE image (.exe or .sys) into executable memory.
 * Returns allocated image buffer (caller must vfree) or NULL on error.
 */
u8 *ntbox_pe_load_image(const void *filedata, size_t filesize,
                        struct ntbox_pe_info *info);

/*
 * Load a DLL into memory (same as image load, but entry point not called here).
 * Returns allocated image buffer or NULL on error.
 */
u8 *ntbox_pe_load_dll(const void *filedata, size_t filesize,
                      struct ntbox_pe_info *info);

/*
 * Call the entry point of a loaded PE.
 *   is_dll = 0: EXE entry (mainCRTStartup / WinMain)
 *   is_dll = 1: DLL entry (DllEntryPoint with DLL_PROCESS_ATTACH)
 * Returns the entry return value, or negative errno on failure.
 */
int ntbox_pe_call_entry(u8 *image, struct ntbox_pe_info *info, int is_dll);

/* Free a loaded PE image. */
void ntbox_pe_unload_image(u8 *image);

/* ---------- خدمة التشغيل: قائمة модуلي الخدمة فقط ---------- */
/* (ليست تاسك مانجر للمستخدم، بل قائمة الخدمات المحملة داخل NT-Box) */

struct ntbox_module {
    char name[64];
    void *base;
    u32 size;
    int is_dll;
    struct list_head node;
};

extern struct list_head ntbox_module_list;
extern spinlock_t ntbox_module_lock;

int ntbox_module_add(const char *name, void *base, u32 size, int is_dll);
void ntbox_module_remove(void *base);

#endif /* PE_ARCH_H */
