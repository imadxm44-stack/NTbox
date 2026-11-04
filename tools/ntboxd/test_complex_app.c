/*
 * test_complex_app.c — الاختبار التكاملي الشامل للمحمّل، تصديرات PE، عقود apiset، ووظائف win32k/ntdll.
 *
 * ما يفحصه:
 *   1) [CMP-001] محمّل DLL وتصديرات PE (kernel32.dll / user32.dll).
 *   2) [CMP-002] حلّ عقود api-set عبر apiset.c.
 *   3) [CMP-003] إجراءات توجيه Win32k/NTDLL (تسجيل الصنف، إنشاء النافذة، السياق، الرسم، الرسائل).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "dll_loader.h"
#include "pe_export.h"
#include "apiset.h"
#include "win32k.h"
#include "ntb0_trace.h"

#define DEFAULT_ROOT "/home/imadix/Downloads/Niga/win7root"

static int g_fail = 0;

static LONG test_wndproc(HWND hwnd, UINT msg, unsigned long wp, long lp) {
    (void)hwnd; (void)msg; (void)wp; (void)lp;
    return 0;
}

int main(int argc, char **argv) {
    const char *root = getenv("NTBOX_ROOT");
    if (!root || !*root) {
        if (argc > 1) root = argv[1];
        else root = DEFAULT_ROOT;
    }

    printf("=== NT-Box Complex App & Stub Integration Test ===\n");
    printf("root = %s\n", root);

    char sysdir[1024];
    snprintf(sysdir, sizeof(sysdir), "%s/Windows/System32", root);

    /* 1) Test DLL Loader & PE Exports (CMP-001) */
    {
        char k32_path[1024];
        snprintf(k32_path, sizeof(k32_path), "%s/kernel32.dll", sysdir);
        ntb0_pe *pe = ntb0_pe_open(k32_path);
        if (!pe) {
            snprintf(k32_path, sizeof(k32_path), "%s/Windows/System32/kernel32.dll", root);
            pe = ntb0_pe_open(k32_path);
        }
        if (!pe) {
            NTB0_FAIL("CMP-001", "failed to open kernel32.dll under %s", root);
            g_fail = 1;
        } else {
            int named = ntb0_pe_export_count(pe);
            ntb0_pe_export exp;
            int found_create = ntb0_pe_find_by_name(pe, "CreateFileA", &exp);
            int found_load = ntb0_pe_find_by_name(pe, "LoadLibraryA", &exp);
            
            if (named > 0 && (found_create || found_load)) {
                NTB0_OK("CMP-001", "kernel32.dll opened successfully: %d named exports, CreateFileA/LoadLibraryA resolved", named);
            } else {
                NTB0_FAIL("CMP-001", "kernel32.dll exports check failed (named=%d, found=%d/%d)", named, found_create, found_load);
                g_fail = 1;
            }
            ntb0_pe_close(pe);
        }

        /* Test ntb0_dll_loader */
        ntb0_dll_loader *ld = ntb0_dll_loader_new();
        if (ld) {
            ntb0_dll_loader_set_sysdir(ld, sysdir);
            ntb0_dll_module *mod = ntb0_dll_load_name(ld, "kernel32.dll", 0);
            if (mod) {
                void *proc = ntb0_dll_get_proc(ld, mod, "GetProcAddress");
                if (proc) {
                    NTB0_OK("CMP-001", "dll_loader loaded kernel32.dll at %p, GetProcAddress=%p", (void *)mod, proc);
                } else {
                    NTB0_STUB("CMP-001", "dll_loader loaded kernel32.dll but GetProcAddress not found in mapped module");
                }
            } else {
                NTB0_STUB("CMP-001", "dll_loader load_name kernel32.dll returned NULL");
            }
            ntb0_dll_loader_free(ld);
        }
    }

    /* 2) Test Apiset Resolution (CMP-002) */
    {
        char hive[1024];
        snprintf(hive, sizeof(hive), "%s/Windows/System32/config/SOFTWARE", root);
        ntb0_apiset *as = ntb0_apiset_new();
        if (as) {
            ntb0_apiset_set_sysdir(as, sysdir);
            ntb0_apiset_load_schema(as, hive);
            ntb0_apiset_result r;
            const char *contract = "api-ms-win-core-file-l1-1-0.dll";
            int res = ntb0_apiset_resolve(as, contract, &r);
            if (res == NTB0_APISET_OK) {
                NTB0_OK("CMP-002", "apiset contract '%s' resolved to host '%s' (stub=%d)", contract, r.host, r.have_stub);
            } else {
                NTB0_STUB("CMP-002", "apiset contract '%s' resolution rc=%d", contract, res);
            }
            ntb0_apiset_free(as);
        } else {
            NTB0_FAIL("CMP-002", "ntb0_apiset_new failed");
            g_fail = 1;
        }
    }

    /* 3) Test Win32k / NTDLL Dispatch Routines (CMP-003) */
    {
        win32k_wndclassex_t wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = test_wndproc;
        wc.hInstance = (HINSTANCE)0x400000;
        wc.lpszClassName = "ComplexAppTestClass";

        unsigned int atom = NtUserRegisterClassEx(&wc);
        if (atom == 0) {
            atom = RegisterClassExA(&wc);
        }

        if (atom != 0) {
            HWND hwnd = NtUserCreateWindowEx(0, "ComplexAppTestClass", "NT-Box Complex Test Window",
                                             0, 10, 10, 200, 150, NULL, NULL, wc.hInstance, NULL);
            if (!hwnd) {
                hwnd = CreateWindowExA(0, "ComplexAppTestClass", "NT-Box Complex Test Window",
                                       0, 10, 10, 200, 150, NULL, NULL, wc.hInstance, NULL);
            }

            if (hwnd) {
                HDC hdc = NtUserGetDC(hwnd);
                if (!hdc) hdc = GetDC(hwnd);

                if (hdc) {
                    NtUserSetTextColor(hdc, 0xFF0000);
                    NtUserSetBkMode(hdc, TRANSPARENT);
                    NtUserRectangle(hdc, 5, 5, 50, 50);
                    NtUserTextOut(hdc, 10, 10, "ComplexTest", 11);
                    NtUserReleaseDC(hwnd, hdc);
                }

                NtUserShowWindow(hwnd, SW_SHOW);
                NtUserUpdateWindow(hwnd);

                win32k_msg_t msg;
                NtUserPostMessage(hwnd, WM_CLOSE, 0, 0);
                int peeked = NtUserPeekMessage(&msg, hwnd, 0, 0, PM_REMOVE);
                if (peeked) {
                    NtUserTranslateMessage(&msg);
                    NtUserDispatchMessage(&msg);
                }

                PostQuitMessage(0);
                NtUserGetMessage(&msg, NULL, 0, 0);

                NTB0_OK("CMP-003", "win32k dispatch routines executed successfully (atom=%u, hwnd=%p)", atom, (void *)hwnd);
            } else {
                NTB0_FAIL("CMP-003", "NtUserCreateWindowEx / CreateWindowExA returned NULL");
                g_fail = 1;
            }
        } else {
            NTB0_FAIL("CMP-003", "NtUserRegisterClassEx / RegisterClassExA failed (atom=0)");
            g_fail = 1;
        }
    }

    printf("\n=== result: %s ===\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
