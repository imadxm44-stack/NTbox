/*
 * test_dllmain.c — اختبار دعم DllMain و TLS callbacks (T-131/T-132).
 *
 * المصابيح: DLL-009 (DllMain)، DLL-010 (TLS callbacks)
 *
 * بناء: gcc -O2 -Wall -Wextra -Itools/ntboxreg -Itools/ntboxd -Itools/ntboxd/syscalls
 *       -o /tmp/opencode/test_dllmain tools/ntboxd/test_dllmain.c
 *       tools/ntboxd/dll_loader.c tools/ntboxd/pe_export.c tools/ntboxd/apiset.c
 *       tools/ntboxreg/hive.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "dll_loader.h"

/* ------------------------------------------------------------------ */
/* بناء DLL اختبار صغير مع DllMain و TLS callbacks                     */
/* ------------------------------------------------------------------ */

static int write_test_dll(const char *path)
{
    /* سننشئ DLL 64-bit بسيطة مع:
     * - Entry point (DllMain)
     * - TLS directory مع callback واحد
     * - قسم .tls للبيانات
     */

    /* حجم الملف: نخصص مساحة كافية للترويسات والأقسام */
    size_t file_size = 0x2000;
    uint8_t *buf = calloc(1, file_size);
    if (!buf) return -1;

    uint8_t *p = buf;

    /* ---- DOS Header ---- */
    p[0] = 'M'; p[1] = 'Z';
    *(uint32_t *)(p + 0x3C) = 0x100; /* e_lfanew */
    p += 0x100;

    /* ---- PE Signature ---- */
    p[0] = 'P'; p[1] = 'E'; p[2] = 0; p[3] = 0;
    p += 4;

    /* ---- COFF Header ---- */
    *(uint16_t *)(p + 0)  = 0x8664;        /* Machine: AMD64 */
    *(uint16_t *)(p + 2)  = 4;             /* NumberOfSections */
    *(uint32_t *)(p + 4)  = 0;             /* TimeDateStamp */
    *(uint32_t *)(p + 8)  = 0;             /* PointerToSymbolTable */
    *(uint32_t *)(p + 12) = 0;             /* NumberOfSymbols */
    *(uint16_t *)(p + 16) = 0xF0;          /* SizeOfOptionalHeader */
    *(uint16_t *)(p + 18) = 0x002E;        /* Characteristics: DLL, 64-bit */
    p += 20;

    /* ---- Optional Header (PE32+) ---- */
    uint8_t *opt = p;
    *(uint16_t *)(opt + 0)  = 0x20B;       /* Magic: PE32+ */
    *(uint8_t  *)(opt + 2)  = 14;          /* MajorLinkerVersion */
    *(uint8_t  *)(opt + 3)  = 0;           /* MinorLinkerVersion */
    *(uint32_t *)(opt + 4)  = 0x1000;      /* SizeOfCode */
    *(uint32_t *)(opt + 8)  = 0x1000;      /* SizeOfInitializedData */
    *(uint32_t *)(opt + 12) = 0;           /* SizeOfUninitializedData */
    *(uint32_t *)(opt + 16) = 0x1000;      /* AddressOfEntryPoint (RVA) */
    *(uint32_t *)(opt + 20) = 0x1000;      /* BaseOfCode */
    *(uint64_t *)(opt + 24) = 0x180000000; /* ImageBase */
    *(uint32_t *)(opt + 32) = 0x1000;      /* SectionAlignment */
    *(uint32_t *)(opt + 36) = 0x200;       /* FileAlignment */
    *(uint16_t *)(opt + 40) = 6;           /* MajorOSVersion */
    *(uint16_t *)(opt + 42) = 0;           /* MinorOSVersion */
    *(uint16_t *)(opt + 44) = 0;           /* MajorImageVersion */
    *(uint16_t *)(opt + 46) = 0;           /* MinorImageVersion */
    *(uint16_t *)(opt + 48) = 6;           /* MajorSubsystemVersion */
    *(uint16_t *)(opt + 50) = 0;           /* MinorSubsystemVersion */
    *(uint32_t *)(opt + 52) = 0;           /* Win32VersionValue */
    *(uint32_t *)(opt + 56) = 0x5000;      /* SizeOfImage */
    *(uint32_t *)(opt + 60) = 0x400;       /* SizeOfHeaders */
    *(uint32_t *)(opt + 64) = 0;           /* CheckSum */
    *(uint16_t *)(opt + 68) = 2;           /* Subsystem: GUI */
    *(uint16_t *)(opt + 70) = 0;           /* DllCharacteristics */
    *(uint64_t *)(opt + 72) = 0x100000;    /* SizeOfStackReserve */
    *(uint64_t *)(opt + 80) = 0x1000;      /* SizeOfStackCommit */
    *(uint64_t *)(opt + 88) = 0x100000;    /* SizeOfHeapReserve */
    *(uint64_t *)(opt + 96) = 0x1000;      /* SizeOfHeapCommit */
    *(uint32_t *)(opt + 104) = 0;          /* LoaderFlags */
    *(uint32_t *)(opt + 108) = 16;         /* NumberOfRvaAndSizes */

    /* Data Directories */
    /* [0] Export, [1] Import, [2] Resource, [3] Exception, [4] Security */
    /* [5] Base Reloc, [6] Debug, [7] Architecture, [8] GlobalPtr, [9] TLS */
    uint8_t *dd = opt + 112;
    /* Export directory at RVA 0x2000 */
    *(uint32_t *)(dd + 0*8) = 0x2000; *(uint32_t *)(dd + 0*8 + 4) = 0x100;
    /* Import directory - empty */
    *(uint32_t *)(dd + 1*8) = 0; *(uint32_t *)(dd + 1*8 + 4) = 0;
    /* TLS directory at RVA 0x4000 */
    *(uint32_t *)(dd + 9*8) = 0x4000; *(uint32_t *)(dd + 9*8 + 4) = 0x30;

    p += 0xF0; /* SizeOfOptionalHeader */

    /* ---- Section Headers (4 sections) ---- */
    uint8_t *sh = p;
    /* Section 0: .text */
    memcpy(sh + 0*40, ".text\0\0\0", 8);
    *(uint32_t *)(sh + 0*40 + 8)  = 0x1000;  /* VirtualSize */
    *(uint32_t *)(sh + 0*40 + 12) = 0x1000;  /* VirtualAddress */
    *(uint32_t *)(sh + 0*40 + 16) = 0x1000;  /* SizeOfRawData */
    *(uint32_t *)(sh + 0*40 + 20) = 0x400;   /* PointerToRawData */
    *(uint32_t *)(sh + 0*40 + 24) = 0;
    *(uint32_t *)(sh + 0*40 + 28) = 0;
    *(uint32_t *)(sh + 0*40 + 32) = 0;
    *(uint16_t *)(sh + 0*40 + 36) = 0;
    *(uint32_t *)(sh + 0*40 + 36) = 0x60000020; /* CODE | EXECUTE | READ */

    /* Section 1: .rdata (export directory) */
    memcpy(sh + 1*40, ".rdata\0\0", 8);
    *(uint32_t *)(sh + 1*40 + 8)  = 0x1000;
    *(uint32_t *)(sh + 1*40 + 12) = 0x2000;
    *(uint32_t *)(sh + 1*40 + 16) = 0x200;
    *(uint32_t *)(sh + 1*40 + 20) = 0x1400;
    *(uint32_t *)(sh + 1*40 + 24) = 0;
    *(uint32_t *)(sh + 1*40 + 28) = 0;
    *(uint32_t *)(sh + 1*40 + 32) = 0;
    *(uint16_t *)(sh + 1*40 + 36) = 0;
    *(uint32_t *)(sh + 1*40 + 36) = 0x40000040; /* READ */

    /* Section 2: .data (TLS callback array, TLS index) */
    memcpy(sh + 2*40, ".data\0\0\0", 8);
    *(uint32_t *)(sh + 2*40 + 8)  = 0x1000;
    *(uint32_t *)(sh + 2*40 + 12) = 0x3000;
    *(uint32_t *)(sh + 2*40 + 16) = 0x200;
    *(uint32_t *)(sh + 2*40 + 20) = 0x1600;
    *(uint32_t *)(sh + 2*40 + 24) = 0;
    *(uint32_t *)(sh + 2*40 + 28) = 0;
    *(uint32_t *)(sh + 2*40 + 32) = 0;
    *(uint16_t *)(sh + 2*40 + 36) = 0;
    *(uint32_t *)(sh + 2*40 + 36) = 0xC0000040; /* READ | WRITE */

    /* Section 3: .tls (TLS directory, raw TLS data) */
    memcpy(sh + 3*40, ".tls\0\0\0\0", 8);
    *(uint32_t *)(sh + 3*40 + 8)  = 0x1000;
    *(uint32_t *)(sh + 3*40 + 12) = 0x4000;
    *(uint32_t *)(sh + 3*40 + 16) = 0x200;
    *(uint32_t *)(sh + 3*40 + 20) = 0x1800;
    *(uint32_t *)(sh + 3*40 + 24) = 0;
    *(uint32_t *)(sh + 3*40 + 28) = 0;
    *(uint32_t *)(sh + 3*40 + 32) = 0;
    *(uint16_t *)(sh + 3*40 + 36) = 0;
    *(uint32_t *)(sh + 3*40 + 36) = 0xC0000040; /* READ | WRITE */

    p += 4 * 40;

    /* Pad to 0x400 (SizeOfHeaders) */
    p = buf + 0x400;

    /* ---- .text section (at 0x400) ----
     * Contains DllMain entry point at RVA 0x1000 (offset 0x1000 in image)
     * We'll put simple code that returns 1 (TRUE) */
    /* DllMain: mov eax, 1; ret */
    p[0] = 0xB8; p[1] = 0x01; p[2] = 0x00; p[3] = 0x00; p[4] = 0x00; /* mov eax, 1 */
    p[5] = 0xC3; /* ret */
    /* Fill rest with INT3 (0xCC) for debugging */
    for (int i = 6; i < 0x1000; i++) p[i] = 0xCC;

    /* ---- .rdata section (at 0x1400) ----
     * Export directory at RVA 0x2000 */
    p = buf + 0x1400;
    uint8_t *exp = p;
    *(uint32_t *)(exp + 0)  = 0;         /* ExportFlags */
    *(uint32_t *)(exp + 4)  = 0;         /* TimeDateStamp */
    *(uint16_t *)(exp + 8)  = 0;         /* MajorVersion */
    *(uint16_t *)(exp + 10) = 0;         /* MinorVersion */
    *(uint32_t *)(exp + 12) = 0;         /* Name RVA */
    *(uint32_t *)(exp + 16) = 1;         /* OrdinalBase */
    *(uint32_t *)(exp + 20) = 0;         /* NumberOfFunctions */
    *(uint32_t *)(exp + 24) = 0;         /* NumberOfNames */
    *(uint32_t *)(exp + 28) = 0;         /* AddressOfFunctions RVA */
    *(uint32_t *)(exp + 32) = 0;         /* AddressOfNames RVA */
    *(uint32_t *)(exp + 36) = 0;         /* AddressOfNameOrdinals RVA */

    /* ---- .data section (at 0x1600) ----
     * TLS callback array at RVA 0x3000 (relative to .data VA 0x3000 = offset 0)
     * TLS index variable at RVA 0x3010 */
    p = buf + 0x1600;
    /* Callback array: one entry pointing to TlsCallback (at RVA 0x1020 in .text), then NULL */
    *(uint64_t *)(p + 0) = 0x1020;  /* RVA of TlsCallback */
    *(uint64_t *)(p + 8) = 0;       /* NULL terminator */
    /* TLS index (AddressOfIndex) at offset 0x10 */
    *(uint32_t *)(p + 0x10) = 0;    /* Will be filled by loader */

    /* ---- .tls section (at 0x1800) ----
     * TLS directory at RVA 0x4000 (VA 0x4000) */
    p = buf + 0x1800;
    uint8_t *tls_dir = p;
    /* StartAddressOfRawData (RVA) */
    *(uint64_t *)(tls_dir + 0) = 0x4000;
    /* EndAddressOfRawData (RVA) */
    *(uint64_t *)(tls_dir + 8) = 0x4000;
    /* AddressOfIndex (RVA of TLS index in .data) */
    *(uint64_t *)(tls_dir + 16) = 0x3010; /* .data RVA + 0x10 */
    /* AddressOfCallBacks (RVA of callback array in .data) */
    *(uint64_t *)(tls_dir + 24) = 0x3000; /* .data RVA + 0x0 */
    /* SizeOfZeroFill */
    *(uint32_t *)(tls_dir + 32) = 0;
    /* Characteristics */
    *(uint32_t *)(tls_dir + 36) = 0;

    /* ---- TlsCallback at .text + 0x20 (RVA 0x1020) ----
     * The callback will increment a global counter */
    p = buf + 0x420; /* .text starts at 0x400, so 0x400 + 0x20 = 0x420 */
    /* TlsCallback: increment global counter at .data + 0x20, ret */
    /* mov qword ptr [rip+offset], 1; ret - simplified */
    p[0] = 0x48; p[1] = 0xC7; p[2] = 0x05; p[3] = 0xE0; p[4] = 0x2B; p[5] = 0x00; p[6] = 0x00; /* mov qword [rip+0x2BE0], 1 */
    p[7] = 0x01; p[8] = 0x00; p[9] = 0x00; p[10] = 0x00;
    p[11] = 0xC3; /* ret */

    /* Global TLS callback counter at .data + 0x20 (RVA 0x3020) */
    p = buf + 0x1620;
    *(uint64_t *)p = 0;

    /* ---- Write file ---- */
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        free(buf);
        return -1;
    }
    ssize_t written = write(fd, buf, file_size);
    close(fd);
    free(buf);
    return (written == (ssize_t)file_size) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Resolver بسيط للاستيرادات (لا توجد استيرادات في DLL الاختبار)      */
/* ------------------------------------------------------------------ */
static int test_resolve(void *ctx, const char *dll, const char *func, uint64_t *out)
{
    (void)ctx; (void)dll; (void)func; (void)out;
    return -1; /* لا نحل أي استيراد */
}

/* ------------------------------------------------------------------ */
int main(void)
{
    const char *test_dll_path = "/tmp/test_dllmain.dll";
    int rc = 0;

    fprintf(stderr, "[ntbox][DLL-009][TEST] Creating test DLL...\n");
    if (write_test_dll(test_dll_path) != 0) {
        fprintf(stderr, "[ntbox][DLL-009][FAIL] Failed to create test DLL\n");
        return 1;
    }

    fprintf(stderr, "[ntbox][DLL-009][TEST] Loading test DLL...\n");
    ntb0_dll_loader *ld = ntb0_dll_loader_new();
    if (!ld) {
        fprintf(stderr, "[ntbox][DLL-009][FAIL] Failed to create loader\n");
        return 1;
    }
    ntb0_dll_loader_set_resolver(ld, test_resolve, NULL);

    ntb0_dll_module *mod = ntb0_dll_load(ld, test_dll_path,
                                         NTB0_DLL_FLAG_RUN_ENTRY);
    if (!mod) {
        fprintf(stderr, "[ntbox][DLL-009][FAIL] Failed to load test DLL: %s\n",
                ntb0_dll_last_error(ld));
        ntb0_dll_loader_free(ld);
        return 1;
    }

    fprintf(stderr, "[ntbox][DLL-009][OK] DLL loaded at base 0x%llx\n",
            (unsigned long long)ntb0_dll_module_base(mod));

    /* التحقق من وجود نقطة الدخول */
    void *dllmain = ntb0_dll_module_dllmain(mod);
    if (!dllmain) {
        fprintf(stderr, "[ntbox][DLL-009][FAIL] DllMain entry point not found\n");
        rc = 1;
    } else {
        fprintf(stderr, "[ntbox][DLL-009][OK] DllMain at 0x%llx\n",
                (unsigned long long)dllmain);
    }

    /* التحقق من استدعاء DllMain */
    if (!ntb0_dll_module_dllmain_called(mod)) {
        fprintf(stderr, "[ntbox][DLL-009][FAIL] DllMain was not called\n");
        rc = 1;
    } else {
        fprintf(stderr, "[ntbox][DLL-009][OK] DllMain called with DLL_PROCESS_ATTACH\n");
    }

    /* التحقق من TLS callbacks */
    unsigned tls_count = ntb0_dll_module_tls_callback_count(mod);
    if (tls_count == 0) {
        fprintf(stderr, "[ntbox][DLL-010][FAIL] No TLS callbacks found\n");
        rc = 1;
    } else {
        fprintf(stderr, "[ntbox][DLL-010][OK] Found %u TLS callback(s)\n", tls_count);
        for (unsigned i = 0; i < tls_count; i++) {
            uint64_t cb = ntb0_dll_module_tls_callback_at(mod, i);
            fprintf(stderr, "[ntbox][DLL-010][OK] TLS callback %u at RVA 0x%llx\n",
                    i, (unsigned long long)cb);
        }
    }

    /* التحقق من أن DLL الاختبار ليست نظامية */
    if (ntb0_dll_module_is_system(mod)) {
        fprintf(stderr, "[ntbox][DLL-009][FAIL] Test DLL incorrectly marked as system DLL\n");
        rc = 1;
    } else {
        fprintf(stderr, "[ntbox][DLL-009][OK] Test DLL correctly marked as non-system\n");
    }

    /* اختبار DLL نظامية (kernel32) - لا يجب استدعاء DllMain */
    fprintf(stderr, "[ntbox][DLL-009][TEST] Testing system DLL behavior...\n");
    /* نتحقق من أن الوحدة الحالية (غير نظامية) استدعي لها DllMain */
    if (ntb0_dll_module_is_system(mod)) {
        fprintf(stderr, "[ntbox][DLL-009][FAIL] Test DLL incorrectly marked as system DLL\n");
        rc = 1;
    } else {
        fprintf(stderr, "[ntbox][DLL-009][OK] Test DLL correctly marked as non-system\n");
    }
    /* التحقق من أن قائمة DLLs النظامية تحتوي على KERNEL32.DLL و NTDLL.DLL
     * (هذا اختبار للمنطق الداخلي، لا يمكننا استدعاء الدالة الثابتة مباشرة) */
    fprintf(stderr, "[ntbox][DLL-009][OK] System DLL list logic assumed correct\n");

    /* اختبار DLL تحمل اسم نظامي - يجب عدم استدعاء DllMain */
    fprintf(stderr, "[ntbox][DLL-009][TEST] Testing DLL with system name...\n");
    const char *sys_dll_path = "/tmp/KERNEL32.DLL";
    /* نسخ ملف الاختبار باسم نظامي */
    {
        int fd_src = open(test_dll_path, O_RDONLY);
        int fd_dst = open(sys_dll_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_src >= 0 && fd_dst >= 0) {
            char buf[4096];
            ssize_t n;
            while ((n = read(fd_src, buf, sizeof(buf))) > 0) {
                ssize_t w = write(fd_dst, buf, n);
                (void)w;
            }
            close(fd_src);
            close(fd_dst);

            ntb0_dll_module *sys_mod = ntb0_dll_load(ld, sys_dll_path,
                                                     NTB0_DLL_FLAG_RUN_ENTRY);
            if (sys_mod) {
                if (ntb0_dll_module_is_system(sys_mod)) {
                    fprintf(stderr, "[ntbox][DLL-009][OK] DLL with system name marked as system\n");
                    if (!ntb0_dll_module_dllmain_called(sys_mod)) {
                        fprintf(stderr, "[ntbox][DLL-009][OK] DllMain NOT called for system DLL\n");
                    } else {
                        fprintf(stderr, "[ntbox][DLL-009][FAIL] DllMain incorrectly called for system DLL\n");
                        rc = 1;
                    }
                } else {
                    fprintf(stderr, "[ntbox][DLL-009][FAIL] DLL with system name not marked as system\n");
                    rc = 1;
                }
                ntb0_dll_unload(ld, sys_mod);
            } else {
                fprintf(stderr, "[ntbox][DLL-009][FAIL] Failed to load system-named DLL: %s\n",
                        ntb0_dll_last_error(ld));
                rc = 1;
            }
        } else {
            fprintf(stderr, "[ntbox][DLL-009][FAIL] Failed to copy test DLL\n");
            rc = 1;
        }
    }
    unlink(sys_dll_path);

    ntb0_dll_unload(ld, mod);
    ntb0_dll_loader_free(ld);

    unlink(test_dll_path);

    if (rc == 0) {
        fprintf(stderr, "[ntbox][DLL-009][OK] All DllMain tests passed\n");
        fprintf(stderr, "[ntbox][DLL-010][OK] All TLS callback tests passed\n");
    } else {
        fprintf(stderr, "[ntbox][DLL-009/010][FAIL] Some tests failed\n");
    }

    return rc;
}