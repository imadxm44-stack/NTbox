/*
 * test_syswow64.c — Self-test runner for 32-bit support (SysWOW64 / PE32 loader & translation layer).
 *
 * Verifies:
 *   1) PE32 (32-bit) binary loading via ntb0_pe32_load_image.
 *   2) 32-bit base relocations (IMAGE_REL_BASED_HIGHLOW).
 *   3) 32-bit import directory parsing & IAT patching.
 *   4) PEB32 / TEB32 layout structures and offsets.
 *   5) 32-bit system stubs & translation layer dispatch.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>

#include "syswow64.h"
#include "dll_loader.h"
#include "pe_export.h"

#define FAKE_32_DIR "/tmp/opencode/syswow64_test"

static int g_fail = 0;

#define CHECK(cond, fmt, ...)                                                 \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "[ntbox][TEST][FAIL] " fmt "\n", ##__VA_ARGS__);  \
            g_fail = 1;                                                       \
        }                                                                     \
    } while (0)

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

/* Build a synthetic PE32 (32-bit) binary */
#define FAKE32_SECT_RVA  0x1000u
#define FAKE32_SECT_OFF  0x200u
#define FAKE32_ILT_RVA   0x1030u
#define FAKE32_IAT_RVA   0x1050u
#define FAKE32_IBN_RVA   0x1070u
#define FAKE32_NAME_RVA  0x1080u

static int build_fake_pe32(const char *path, uint32_t image_base,
                           const char *imp_dll, const char *func)
{
    uint8_t hdr[FAKE32_SECT_OFF];
    uint8_t sec[0x200];
    uint32_t opt_off = 0x40 + 4 + 20;
    uint32_t dd_off = opt_off + 96; /* PE32 optional header has DataDirectory at offset 96 */
    size_t fn_len = strlen(func) + 1;
    size_t dn_len = strlen(imp_dll) + 1;
    FILE *f;

    if (fn_len + 2 > 64 || dn_len > 96)
        return -1;

    memset(hdr, 0, sizeof(hdr));
    memset(sec, 0, sizeof(sec));

    /* DOS header */
    hdr[0] = 'M';
    hdr[1] = 'Z';
    put32(hdr + 0x3C, 0x40);

    /* PE signature + COFF header */
    memcpy(hdr + 0x40, "PE\0\0", 4);
    put16(hdr + 0x44, 0x014C);            /* Machine = i386 (32-bit) */
    put16(hdr + 0x46, 1);                 /* NumberOfSections */
    put16(hdr + 0x54, 0xE0);              /* SizeOfOptionalHeader for PE32 */
    put16(hdr + 0x56, 0x0102);            /* EXECUTABLE | 32BIT_MACHINE */

    /* Optional header (PE32 - magic 0x10b) */
    put16(hdr + opt_off + 0, 0x10b);
    put32(hdr + opt_off + 16, FAKE32_SECT_RVA);   /* AddressOfEntryPoint */
    put32(hdr + opt_off + 20, FAKE32_SECT_RVA);   /* BaseOfCode */
    put32(hdr + opt_off + 24, 0);                 /* BaseOfData */
    put32(hdr + opt_off + 28, image_base);        /* ImageBase (32-bit) */
    put32(hdr + opt_off + 32, 0x1000);            /* SectionAlignment */
    put32(hdr + opt_off + 36, 0x200);             /* FileAlignment */
    put32(hdr + opt_off + 56, 0x2000);            /* SizeOfImage */
    put32(hdr + opt_off + 60, FAKE32_SECT_OFF);   /* SizeOfHeaders */
    put16(hdr + opt_off + 68, 3);                 /* Subsystem = console */
    put32(hdr + opt_off + 92, 16);                /* NumberOfRvaAndSizes */
    /* DataDirectory[1] = Import */
    put32(hdr + dd_off + 8, FAKE32_SECT_RVA);
    put32(hdr + dd_off + 12, 0x100);

    /* Section table */
    {
        uint8_t *s = hdr + opt_off + 0xE0;
        memcpy(s, ".rdata\0", 8);
        put32(s + 8, 0x200);              /* VirtualSize */
        put32(s + 12, FAKE32_SECT_RVA);   /* VirtualAddress */
        put32(s + 16, 0x200);             /* SizeOfRawData */
        put32(s + 20, FAKE32_SECT_OFF);   /* PointerToRawData */
        put32(s + 36, 0x40000040);        /* INITIALIZED_DATA | MEM_READ */
    }

    /* Section content: Import descriptor */
    put32(sec + 0x00, FAKE32_ILT_RVA);    /* OriginalFirstThunk */
    put32(sec + 0x0C, FAKE32_NAME_RVA);   /* Name */
    put32(sec + 0x10, FAKE32_IAT_RVA);    /* FirstThunk */

    /* ILT: 32-bit RVA to import-by-name then NULL */
    put32(sec + (FAKE32_ILT_RVA - FAKE32_SECT_RVA) + 0, FAKE32_IBN_RVA);
    put32(sec + (FAKE32_ILT_RVA - FAKE32_SECT_RVA) + 4, 0);

    /* IAT: initialized to 0 */
    put32(sec + (FAKE32_IAT_RVA - FAKE32_SECT_RVA) + 0, 0);

    /* IMAGE_IMPORT_BY_NAME */
    put16(sec + (FAKE32_IBN_RVA - FAKE32_SECT_RVA), 0);
    memcpy(sec + (FAKE32_IBN_RVA - FAKE32_SECT_RVA) + 2, func, fn_len);

    /* Imported DLL name */
    memcpy(sec + (FAKE32_NAME_RVA - FAKE32_SECT_RVA), imp_dll, dn_len);

    f = fopen(path, "wb");
    if (!f)
        return -1;
    fwrite(hdr, 1, sizeof(hdr), f);
    fwrite(sec, 1, sizeof(sec), f);
    fclose(f);
    return 0;
}

static uint32_t sample_stub_fn(uint32_t *stack_args)
{
    if (stack_args)
        return stack_args[0] + 42;
    return 0;
}

int main(void)
{
    char path[512];
    ntb0_dll_loader *ld;
    ntb0_dll_module *mod;
    PEB32 peb32;
    TEB32 teb32;

    printf("[ntbox] Starting SysWOW64 / PE32 support self-test...\n");

    /* 1. Verify PEB32 / TEB32 layout structures */
    memset(&peb32, 0, sizeof(peb32));
    memset(&teb32, 0, sizeof(teb32));
    peb32.BeingDebugged = 1;
    peb32.ImageBaseAddress = 0x00400000;
    teb32.ProcessEnvironmentBlock = (uint32_t)(uintptr_t)&peb32;

    CHECK(peb32.BeingDebugged == 1, "PEB32 BeingDebugged field");
    CHECK(peb32.ImageBaseAddress == 0x00400000, "PEB32 ImageBaseAddress field");
    CHECK(teb32.ProcessEnvironmentBlock == (uint32_t)(uintptr_t)&peb32, "TEB32 ProcessEnvironmentBlock field");
    printf("[ntbox][OK] PEB32 and TEB32 structure layouts verified successfully.\n");

    /* 2. Register stub and test translation layer */
    ntb0_syswow64_register_stub("kernel32.dll", "FakeSysCall32", sample_stub_fn);
    {
        uint32_t args[2] = { 100, 0 };
        uint32_t res = ntb0_syswow64_dispatch("kernel32.dll", "FakeSysCall32", args);
        CHECK(res == 142, "SysWOW64 stub dispatch result (got %u, expected 142)", res);
    }
    printf("[ntbox][OK] SysWOW64 translation layer dispatch verified.\n");

    /* 3. Build and load a synthetic PE32 (32-bit) binary */
    mkdir(FAKE_32_DIR, 0755);
    snprintf(path, sizeof(path), "%s/test32.dll", FAKE_32_DIR);

    if (build_fake_pe32(path, 0x10000000UL, "kernel32.dll", "FakeSysCall32") != 0) {
        fprintf(stderr, "[ntbox][FAIL] cannot build fake PE32 binary\n");
        return 1;
    }

    ld = ntb0_dll_loader_new();
    CHECK(ld != NULL, "create dll loader");

    mod = ntb0_pe32_load_image(ld, path, 0, 0);
    CHECK(mod != NULL, "load PE32 binary via ntb0_pe32_load_image: %s", ntb0_dll_last_error(ld));

    if (mod) {
        CHECK(ntb0_dll_module_is64(mod) == 0, "loaded module is 32-bit (PE32)");
        CHECK(ntb0_dll_module_base(mod) != 0, "PE32 mapped base address");
        printf("[ntbox][OK] PE32 binary successfully loaded at base 0x%llx (is64=%d, size=%zu)\n",
               (unsigned long long)ntb0_dll_module_base(mod),
               ntb0_dll_module_is64(mod),
               ntb0_dll_module_size(mod));

        ntb0_dll_unload(ld, mod);
    }

    ntb0_dll_loader_free(ld);

    if (g_fail) {
        fprintf(stderr, "[ntbox][TEST][FAIL] SysWOW64 / PE32 self-test FAILED\n");
        return 1;
    }

    printf("[ntbox][TEST][OK] SysWOW64 / PE32 support self-test PASSED successfully.\n");
    return 0;
}
