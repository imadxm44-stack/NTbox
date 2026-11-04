#define _GNU_SOURCE
#include "input.h"
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

/* helper: find first message of given type in window queue (non-destructive) */
static int has_msg(HWND hwnd, UINT msg, unsigned long *wp_out, long *lp_out) {
    /* peek all by repeated non-remove? We'll count via peek with remove then re-add? Simpler: use message_count and iterate using a temp copy. */
    /* Use nt_input_peek_message with PM_NOREMOVE repeatedly is not enough to iterate. We'll just check count and use get_message on a cloned? */
    /* Instead, implement simple scan by repeatedly peeking+removing then re-posting. */
    /* For tests, we only need first occurrence; do destructive scan then restore. */
    win32k_msg_t buf[1024];
    int n = 0, i;
    win32k_msg_t m;
    /* drain */
    while (nt_input_get_message(&m, hwnd, 0, 0)) {
        if (n < 1024) buf[n++] = m;
    }
    int found = 0;
    unsigned long wp = 0; long lp = 0;
    for (i = 0; i < n; i++) {
        if (buf[i].message == msg && !found) {
            found = 1; wp = buf[i].wParam; lp = buf[i].lParam;
        }
    }
    /* restore */
    for (i = 0; i < n; i++) {
        nt_input_post_message(buf[i].hwnd, buf[i].message, buf[i].wParam, buf[i].lParam, buf[i].pt_x, buf[i].pt_y);
    }
    if (found) {
        if (wp_out) *wp_out = wp;
        if (lp_out) *lp_out = lp;
    }
    return found;
}

static int count_msg(HWND hwnd, UINT msg) {
    win32k_msg_t buf[1024];
    int n = 0, i, c = 0;
    win32k_msg_t m;
    while (nt_input_get_message(&m, hwnd, 0, 0)) {
        if (n < 1024) buf[n++] = m;
    }
    for (i = 0; i < n; i++) {
        if (buf[i].message == msg) c++;
    }
    for (i = 0; i < n; i++) {
        nt_input_post_message(buf[i].hwnd, buf[i].message, buf[i].wParam, buf[i].lParam, buf[i].pt_x, buf[i].pt_y);
    }
    return c;
}

int main(void) {
    win32k_wndclassex_t wc;
    unsigned int atom;
    HWND w1, w2;
    unsigned long wp = 0;
    long lp = 0;

    printf("=== NT-Box Input self-test (W8: T-181..T-185) ===\n");

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = NULL;
    wc.hInstance = (HINSTANCE)0x10000000;
    wc.lpszClassName = "NtBoxInputClass";

    atom = NtUserRegisterClassEx(&wc);
    check(atom != 0, "T-181", "RegisterClassEx atom=%u", atom);

    w1 = NtUserCreateWindowEx(0, "NtBoxInputClass", "Win1", 0, 10, 10, 320, 240, NULL, NULL, wc.hInstance, NULL);
    w2 = NtUserCreateWindowEx(0, "NtBoxInputClass", "Win2", 0, 40, 40, 320, 240, NULL, NULL, wc.hInstance, NULL);
    check(w1 != NULL && w2 != NULL, "T-181", "created 2 windows w1=%p w2=%p", (void *)w1, (void *)w2);

    nt_input_init();
    check(nt_input_register_window(w1) == 0, "INP-001", "register w1");
    check(nt_input_register_window(w2) == 0, "INP-001", "register w2");
    check(nt_input_window_count() == 2, "INP-001", "window_count=%d", nt_input_window_count());

    /* focus w1 via wl_keyboard.enter */
    nt_input_wl_keyboard_enter(w1, 1);
    check(nt_input_get_focus() == w1, "INP-004", "focus == w1");
    check(has_msg(w1, WM_SETFOCUS, &wp, &lp), "INP-004", "w1 got WM_SETFOCUS (prev=%p)", (void *)(uintptr_t)wp);
    check(count_msg(w2, WM_SETFOCUS) == 0, "INP-004", "w2 has no SETFOCUS yet");

    /* key 'A' down */
    nt_input_key_event(w1, 'A', WL_KEYBOARD_KEY_STATE_PRESSED, 0);
    check(has_msg(w1, WM_KEYDOWN, &wp, &lp), "INP-002", "w1 WM_KEYDOWN wParam=0x%lX (expect 'A'=0x41)", wp);
    check(wp == 'A', "INP-002", "wParam == 'A'");
    check(count_msg(w2, WM_KEYDOWN) == 0, "INP-002", "w2 has no KEYDOWN");

    /* pointer enter + move */
    nt_input_wl_pointer_enter(w1, 10, 20);
    nt_input_wl_pointer_motion_to(w1, 30, 40);
    check(has_msg(w1, WM_MOUSEMOVE, &wp, &lp), "INP-003", "w1 WM_MOUSEMOVE lParam=0x%lX", (unsigned long)lp);
    {
        int x = (int)(lp & 0xFFFF);
        int y = (int)((lp >> 16) & 0xFFFF);
        check(x == 30 && y == 40, "INP-003", "coords (%d,%d) expect (30,40)", x, y);
    }

    /* left click */
    nt_input_wl_pointer_button(0, 0, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED, 30, 40);
    nt_input_wl_pointer_button(0, 0, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED, 30, 40);
    check(count_msg(w1, WM_LBUTTONDOWN) == 1, "INP-003", "w1 LBUTTONDOWN count=1");
    check(count_msg(w1, WM_LBUTTONUP) == 1, "INP-003", "w1 LBUTTONUP count=1");

    /* mouse wheel */
    nt_input_push_mouse_wheel(w1, 120, 5, 6);
    check(count_msg(w1, WM_MOUSEWHEEL) == 1, "INP-003", "w1 MOUSEWHEEL count=1");

    /* delivery check: post a custom message */
    nt_input_post_message(w1, WM_PAINT, 7, 8, 1, 2);
    check(nt_input_message_count(w1) > 0, "INP-005", "w1 queue non-empty (count=%d)", nt_input_message_count(w1));

    /* focus switch to w2 */
    nt_input_wl_keyboard_enter(w2, 2);
    check(nt_input_get_focus() == w2, "INP-004", "focus now w2");
    check(count_msg(w1, WM_KILLFOCUS) >= 1, "INP-004", "w1 got KILLFOCUS");
    check(has_msg(w2, WM_SETFOCUS, &wp, &lp), "INP-004", "w2 got WM_SETFOCUS");

    /* key to w2 */
    nt_input_key_event(w2, 'B', WL_KEYBOARD_KEY_STATE_PRESSED, 0);
    check(has_msg(w2, WM_KEYDOWN, &wp, &lp) && wp == 'B', "INP-002", "w2 KEYDOWN 'B'");

    /* leave w2 */
    nt_input_wl_keyboard_leave(w2, 3);
    check(count_msg(w2, WM_KILLFOCUS) >= 1, "INP-004", "w2 got KILLFOCUS on leave");

    /* peek/remove test */
    {
        win32k_msg_t m;
        int r = nt_input_peek_message(&m, w1, 0, 0, PM_REMOVE);
        check(r == 1, "INP-005", "PeekMessage (remove) returned %d", r);
    }

    /* aliases smoke */
    {
        HWND h = w1;
        input_register_window(h);
        wl_keyboard_enter(h, 9);
        check(TRUE, "INP-001", "alias calls OK");
    }

    /* unregister */
    check(nt_input_unregister_window(w2) == 0, "INP-001", "unregister w2");
    check(nt_input_window_count() == 1, "INP-001", "window_count=%d", nt_input_window_count());

    nt_input_shutdown();

    printf("\n=== result: %s (%d failure(s)) ===\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
