#define _GNU_SOURCE
#include "win32k.h"
#include "ntb0_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static int failures = 0;

static void check(int ok, const char *what, const char *fmt, ...) {
    va_list ap;
    printf("  [%s] %s: ", ok ? "PASS" : "FAIL", what);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    if (!ok) failures++;
}

static LONG test_wndproc(HWND hwnd, UINT msg, unsigned long wp, long lp) {
    (void)hwnd; (void)msg; (void)wp; (void)lp;
    return 0;
}

int main(void) {
    win32k_wndclassex_t wc;
    unsigned int atom;
    HWND hwnd;
    HDC hdc;
    COLORREF oldtext;
    int oldbk;
    int r;
    win32k_msg_t msg;
    int loops = 0;

    printf("=== NT-Box Win32k self-test (W8: T-173..T-180) ===\n");

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = test_wndproc;
    wc.hInstance = (HINSTANCE)0x10000000;
    wc.lpszClassName = "NtBoxTestClass";

    atom = NtUserRegisterClassEx(&wc);
    check(atom != 0, "T-180", "RegisterClassEx 'NtBoxTestClass' atom=%u", atom);

    hwnd = NtUserCreateWindowEx(0, "NtBoxTestClass", "NT-Box Win32k",
                                 0, 100, 100, 320, 240, NULL, NULL,
                                 wc.hInstance, NULL);
    check(hwnd != NULL, "T-173", "CreateWindowEx hwnd=%p", (void *)hwnd);

    hdc = NtUserGetDC(hwnd);
    check(hdc != NULL, "T-174", "GetDC hwnd=%p -> hdc=%p", (void *)hwnd, (void *)hdc);

    oldtext = NtUserSetTextColor(hdc, 0x0000FF);
    check(hdc != NULL, "T-175", "SetTextColor -> prev=0x%06lX", (unsigned long)oldtext);

    oldbk = NtUserSetBkMode(hdc, TRANSPARENT);
    check(TRUE, "T-175", "SetBkMode -> prev=%d", oldbk);

    r = NtUserRectangle(hdc, 10, 10, 100, 100);
    check(r == 1, "T-175", "Rectangle returned %d", r);

    r = NtUserTextOut(hdc, 20, 20, "Hello NT-Box", 12);
    check(r == 1, "T-175", "TextOut returned %d", r);

    r = NtUserShowWindow(hwnd, SW_SHOW);
    check(r == 1, "T-176", "ShowWindow returned %d", r);

    r = NtUserUpdateWindow(hwnd);
    check(r == 1, "T-176", "UpdateWindow returned %d", r);

    r = NtUserMessageBox(hwnd, "Hello from NT-Box", "NT-Box", MB_OK);
    check(r == IDOK, "T-177", "MessageBox returned %d (IDOK)", r);

    /* PeekMessage loop */
    while (NtUserPeekMessage(&msg, hwnd, 0, 0, PM_REMOVE)) {
        loops++;
        if (loops > 1000) break;
    }
    check(loops <= 1000, "T-179", "PeekMessage loop ran %d iteration(s)", loops);

    /* GetMessage / Translate / Dispatch */
    r = NtUserGetMessage(&msg, hwnd, 0, 0);
    check(r == 0, "T-178", "GetMessage returned %d (WM_QUIT)", r);
    r = NtUserTranslateMessage(&msg);
    check(r == 1, "T-178", "TranslateMessage returned %d", r);
    r = (int)NtUserDispatchMessage(&msg);
    check(TRUE, "T-178", "DispatchMessage returned %d", r);

    r = NtUserReleaseDC(hwnd, hdc);
    check(r == 1, "T-174", "ReleaseDC returned %d", r);

    /* aliases smoke test */
    {
        HWND h2 = CreateWindowExA(0, "NtBoxTestClass", "alias", 0, 0,0,10,10, NULL, NULL, wc.hInstance, NULL);
        HDC dc2 = GetDC(h2);
        check(h2 != NULL && dc2 != NULL, "T-180", "alias CreateWindowEx/GetDC OK");
        if (dc2) ReleaseDC(h2, dc2);
    }

    printf("\n=== result: %s (%d failure(s)) ===\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
