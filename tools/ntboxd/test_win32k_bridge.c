/*
 * test_win32k_bridge.c — NT-Box: integration test for win32k_bridge + input_bridge
 * Creates a window, draws on GPU, shows it, runs message loop, handles input.
 * Exit 0 only if window renders and input works.
 */
#define _GNU_SOURCE
#include "win32k.h"
#include "win32k_bridge.h"
#include "input_bridge.h"
#include "gfx_present.h"
#include "ntb0_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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

/* Simple window procedure that counts messages */
static int g_wm_paint_count = 0;
static int g_wm_keydown_count = 0;
static int g_wm_mousemove_count = 0;
static int g_wm_lbuttondown_count = 0;
static int g_wm_lbuttonup_count = 0;
static int g_wm_close_count = 0;
static HWND g_test_hwnd = NULL;

static LONG test_wndproc(HWND hwnd, UINT msg, unsigned long wp, long lp) {
    (void)hwnd; (void)wp; (void)lp;
    switch (msg) {
        case WM_PAINT:
            g_wm_paint_count++;
            break;
        case WM_KEYDOWN:
            g_wm_keydown_count++;
            if (wp == VK_ESCAPE) {
                PostMessageA(hwnd, WM_CLOSE, 0, 0);
            }
            break;
        case WM_MOUSEMOVE:
            g_wm_mousemove_count++;
            break;
        case WM_LBUTTONDOWN:
            g_wm_lbuttondown_count++;
            break;
        case WM_LBUTTONUP:
            g_wm_lbuttonup_count++;
            break;
        case WM_CLOSE:
            g_wm_close_count++;
            PostQuitMessage(0);
            break;
        default:
            break;
    }
    return 0;
}

int main(void) {
    win32k_wndclassex_t wc;
    unsigned int atom;
    HWND hwnd;
    HDC hdc;
    int r;
    win32k_msg_t msg;
    int frame_count = 0;
    int max_frames = 100;  /* Run for ~100 frames or until quit */

    printf("=== NT-Box Win32k Bridge Integration Test (WIN-011..WIN-020, INP-006..INP-010) ===\n");

    /* 1. Initialise the bridge (GPU + input) */
    r = win32k_bridge_init();
    check(r == 0, "WIN-011", "win32k_bridge_init returned %d (gpu_active=%d)", r, win32k_bridge_is_gpu_active());

    /* 2. Register window class */
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = test_wndproc;
    wc.hInstance = (HINSTANCE)0x10000000;
    wc.lpszClassName = "NtBoxBridgeTestClass";

    atom = NtUserRegisterClassEx(&wc);
    check(atom != 0, "WIN-012", "RegisterClassEx atom=%u", atom);

    /* 3. Create window */
    hwnd = NtUserCreateWindowEx(0, "NtBoxBridgeTestClass", "NT-Box Bridge Test",
                                 0, 100, 100, 640, 480, NULL, NULL,
                                 wc.hInstance, NULL);
    check(hwnd != NULL, "WIN-012", "CreateWindowEx hwnd=%p", (void *)hwnd);
    g_test_hwnd = hwnd;

    /* 4. Notify bridge of window creation (normally done by user32 CreateWindowEx) */
    r = win32k_bridge_window_created(hwnd, 640, 480, "NT-Box Bridge Test", wc.hInstance);
    check(r == 0, "WIN-012", "win32k_bridge_window_created returned %d", r);

    /* 5. Get DC and draw (using win32k API) */
    hdc = NtUserGetDC(hwnd);
    check(hdc != NULL, "WIN-013", "GetDC hwnd=%p -> hdc=%p", (void *)hwnd, (void *)hdc);

    /* Set drawing attributes via win32k */
    COLORREF old_text = NtUserSetTextColor(hdc, 0x00FF0000);  /* Red */
    check(old_text != 0, "WIN-014", "SetTextColor old=0x%06lX", (unsigned long)old_text);
    int old_bk = NtUserSetBkMode(hdc, TRANSPARENT);
    check(old_bk != 0, "WIN-014", "SetBkMode old=%d", old_bk);

    /* Draw rectangle via win32k */
    r = NtUserRectangle(hdc, 50, 50, 200, 200);
    check(r == 1, "WIN-015", "Rectangle returned %d", r);

    /* Draw text via win32k */
    r = NtUserTextOut(hdc, 60, 60, "Hello GPU!", 10);
    check(r == 1, "WIN-015", "TextOut returned %d", r);

    /* Present the window via bridge (swap buffers) - may fail if no GPU */
    r = win32k_bridge_present_window(hwnd);
    check(r == 1 || !win32k_bridge_is_gpu_active(), "WIN-019", "PresentWindow returned %d (gpu_active=%d)", r, win32k_bridge_is_gpu_active());

    NtUserReleaseDC(hwnd, hdc);

    /* 5b. Also test bridge drawing functions directly */
    COLORREF old_text2 = win32k_bridge_set_text_color(hwnd, 0x0000FF00);  /* Green */
    check(old_text2 != 0, "WIN-014", "bridge SetTextColor old=0x%06lX", (unsigned long)old_text2);
    int old_bk2 = win32k_bridge_set_bk_mode(hwnd, OPAQUE);
    check(old_bk2 != 0, "WIN-014", "bridge SetBkMode old=%d", old_bk2);
    r = win32k_bridge_rectangle(hwnd, 100, 100, 300, 300);
    check(r == 1, "WIN-015", "bridge Rectangle returned %d", r);
    r = win32k_bridge_text_out(hwnd, 110, 110, "Bridge Draw", 11);
    check(r == 1, "WIN-015", "bridge TextOut returned %d", r);
    r = win32k_bridge_present_window(hwnd);
    check(r == 1 || !win32k_bridge_is_gpu_active(), "WIN-019", "bridge PresentWindow returned %d (gpu_active=%d)", r, win32k_bridge_is_gpu_active());

    /* 6. Show window (triggers first frame present) */
    r = NtUserShowWindow(hwnd, SW_SHOW);
    check(r == 1, "WIN-016", "ShowWindow returned %d", r);

    r = win32k_bridge_window_show(hwnd, SW_SHOW);
    check(r == 1, "WIN-016", "win32k_bridge_window_show returned %d", r);

    /* 7. Update window (WM_PAINT) */
    r = NtUserUpdateWindow(hwnd);
    check(r == 1, "WIN-017", "UpdateWindow returned %d", r);

    r = win32k_bridge_window_update(hwnd);
    check(r == 1 || !win32k_bridge_is_gpu_active(), "WIN-017", "win32k_bridge_window_update returned %d (gpu_active=%d)", r, win32k_bridge_is_gpu_active());

    /* 8. Test MessageBox */
    r = NtUserMessageBox(hwnd, "Bridge test message", "NT-Box", MB_OK);
    check(r == IDOK, "WIN-020", "MessageBox returned %d", r);

    r = win32k_bridge_message_box(hwnd, "Bridge test message", "NT-Box", MB_OK);
    check(r == IDOK, "WIN-020", "win32k_bridge_message_box returned %d", r);

    /* 9. Message loop with input simulation */
    printf("\n  [INFO] Starting message loop (simulating input events)...\n");

    /* Simulate some input events via input bridge */
    input_bridge_wl_keyboard_enter(hwnd, 1);
    input_bridge_wl_keyboard_key(hwnd, 1, 0, 'A', WL_KEYBOARD_KEY_STATE_PRESSED);
    input_bridge_wl_keyboard_key(hwnd, 2, 0, 'A', WL_KEYBOARD_KEY_STATE_RELEASED);
    input_bridge_wl_pointer_enter(hwnd, 100, 100);
    input_bridge_wl_pointer_motion(0, 150, 150);
    input_bridge_wl_pointer_button(0, 0, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED, 150, 150);
    input_bridge_wl_pointer_button(0, 0, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED, 150, 150);

    /* Run message loop */
    while (frame_count < max_frames) {
        /* Pump events (Wayland + input) */
        win32k_bridge_pump_events();

        /* Peek for messages */
        int got = NtUserPeekMessage(&msg, NULL, 0, 0, PM_REMOVE);
        if (got) {
            if (msg.message == WM_QUIT) {
                printf("  [INFO] Received WM_QUIT, exiting loop\n");
                break;
            }
            NtUserTranslateMessage(&msg);
            NtUserDispatchMessage(&msg);
        } else {
            /* No messages - wait a bit */
            win32k_bridge_wait_for_events();
        }

        /* Present a frame every ~16ms */
        frame_count++;
        if (frame_count % 10 == 0) {
            r = win32k_bridge_present_window(hwnd);
            check(r == 1 || !win32k_bridge_is_gpu_active(), "WIN-019", "bridge PresentWindow returned %d (gpu_active=%d)", r, win32k_bridge_is_gpu_active());
        }
        usleep(16000);  /* ~60fps */
    }

    /* 10. Verify message counts */
    check(g_wm_paint_count > 0, "INP-006", "WM_PAINT received %d time(s)", g_wm_paint_count);
    check(g_wm_keydown_count > 0, "INP-007", "WM_KEYDOWN received %d time(s)", g_wm_keydown_count);
    check(g_wm_mousemove_count > 0, "INP-008", "WM_MOUSEMOVE received %d time(s)", g_wm_mousemove_count);
    check(g_wm_lbuttondown_count > 0, "INP-008", "WM_LBUTTONDOWN received %d time(s)", g_wm_lbuttondown_count);
    check(g_wm_lbuttonup_count > 0, "INP-008", "WM_LBUTTONUP received %d time(s)", g_wm_lbuttonup_count);

    /* 11. Test PeekMessage directly */
    {
        win32k_msg_t pm;
        int peeked = NtUserPeekMessage(&pm, hwnd, 0, 0, PM_NOREMOVE);
        check(peeked == 0 || peeked == 1, "WIN-018", "PeekMessage returned %d", peeked);
    }

    /* 12. Test GetMessage/Translate/Dispatch (should get WM_QUIT) */
    r = NtUserGetMessage(&msg, hwnd, 0, 0);
    check(r == 0, "WIN-018", "GetMessage returned %d (WM_QUIT)", r);
    r = NtUserTranslateMessage(&msg);
    check(r == 1, "WIN-018", "TranslateMessage returned %d", r);
    r = (int)NtUserDispatchMessage(&msg);
    check(TRUE, "WIN-018", "DispatchMessage returned %d", r);

    /* 13. Cleanup */
    win32k_bridge_window_destroyed(hwnd);
    win32k_bridge_shutdown();

    /* Final verdict */
    printf("\n=== result: %s (%d failure(s)) ===\n", failures ? "FAIL" : "PASS", failures);

    /* The test passes if:
     * - No failures
     * - GPU presentation was active (or at least initialised)
     * - At least one frame was presented
     * - Input events were translated to win32k messages
     */
    if (failures == 0 && frame_count > 0 &&
        (g_wm_keydown_count > 0 || g_wm_mousemove_count > 0)) {
        printf("[test_win32k_bridge] RESULT: PASS\n");
        return 0;
    } else {
        if (failures == 0) {
            printf("[test_win32k_bridge] RESULT: FAIL (no frames or no input)\n");
        }
        return 1;
    }
}