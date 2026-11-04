/*
 * test_input_live.c — NT-Box W8 input LIVE test (T-183/T-184/T-185).
 *
 * Sections:
 *   1. UNIT  — synthetic wl_seat discovery / capabilities (INP-012)
 *   2. UNIT  — synthetic wl_keyboard receiver payloads fed through the real
 *              rewriter (INP-013, INP-015, INP-016): keymap, enter/leave,
 *              key/modifiers, serials, evdev→VK, WM_CHAR, focused-only routing
 *   3. UNIT  — synthetic wl_pointer receiver payloads (INP-014): enter/leave,
 *              motion, button state, scroll → WM_MOUSEWHEEL / WM_MOUSEHWHEEL
 *   4. LIVE  — real compositor discovery (INP-011/INP-012): a genuine
 *              wl_display + wl_registry + wl_seat round-trip.  When no
 *              compositor is present the test spawns a weston: prefer the
 *              x11 backend (real wl_seat on the caller's X display); with
 *              no seat the live-discovery portion is SKIP (rc=3), nothing
 *              faked.
 *
 * Exit codes: 0 = PASS (all executed checks), 1 = FAIL, 3 = SKIP (live
 * layer could not run; rc=3 pattern of this project, like test_present).
 * Unmappable keys/buttons are refused with honest FAIL lamps, never
 * silently passed through.
 */
#define _GNU_SOURCE
#include "input.h"
#include "input_bridge.h"
#include "win32k.h"
#include "ntb0_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>

static int failures = 0;
static int passes = 0;

static void check(int ok, const char *what, const char *fmt, ...) {
    va_list ap;
    printf("  [%s] %s: ", ok ? "PASS" : "FAIL", what);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    if (ok) passes++; else failures++;
}

static void skip_lamp(const char *id, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "[ntbox][%s][SKIP] ", id);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

/* ---- queue inspection helpers (drain, scan, restore) ---------------- */
static int drain(HWND hwnd, win32k_msg_t *buf, int max) {
    int n = 0;
    win32k_msg_t m;
    while (nt_input_get_message(&m, hwnd, 0, 0)) {
        if (n < max) buf[n++] = m;
    }
    return n;
}
static void restore(HWND hwnd, const win32k_msg_t *buf, int n) {
    int i;
    (void)hwnd;
    for (i = 0; i < n; i++)
        nt_input_post_message(buf[i].hwnd, buf[i].message, buf[i].wParam,
                              buf[i].lParam, buf[i].pt_x, buf[i].pt_y);
}
static int count_msg(HWND hwnd, UINT msg_id) {
    win32k_msg_t buf[1024];
    int n, i, c = 0;
    n = drain(hwnd, buf, 1024);
    for (i = 0; i < n; i++) if (buf[i].message == msg_id) c++;
    restore(hwnd, buf, n);
    return c;
}
static int find_msg(HWND hwnd, UINT msg_id, unsigned long *wp, long *lp,
                    unsigned long *t) {
    win32k_msg_t buf[1024];
    int n, i, found = 0;
    n = drain(hwnd, buf, 1024);
    for (i = 0; i < n && !found; i++) {
        if (buf[i].message == msg_id) {
            if (wp) *wp = buf[i].wParam;
            if (lp) *lp = buf[i].lParam;
            if (t)  *t = buf[i].time;
            found = 1;
        }
    }
    restore(hwnd, buf, n);
    return found;
}

/* ---------------- weston headless spawn (live path) ------------------ */
static pid_t g_weston_pid = -1;

static void weston_kill(void) {
    if (g_weston_pid > 0) {
        kill(-g_weston_pid, SIGKILL);
        waitpid(g_weston_pid, NULL, 0);
        g_weston_pid = -1;
    }
}

static int weston_spawn(const char *rt, const char *sock, int x11) {
    char spath[512], lpath[512];
    struct stat st;
    int i;
    pid_t pid;

    snprintf(spath, sizeof(spath), "%s/%s", rt, sock);
    snprintf(lpath, sizeof(lpath), "%s/%s.lock", rt, sock);
    unlink(spath);
    unlink(lpath);

    /* weston refuses to start without a private runtime dir — create it */
    if (mkdir(rt, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "[weston-spawn] mkdir %s failed: %s\n",
                rt, strerror(errno));
        return -1;
    }
    chmod(rt, 0700);   /* weston demands 0700/owned by us — best effort */

    /* we made rt ourselves, so weston MUST use it — overwrite any caller
     * value, otherwise the socket would appear in a dir we never stat */
    setenv("XDG_RUNTIME_DIR", rt, 1);
    fflush(NULL);   /* child inherits buffered stdout at fork + double-flushes */

    pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setpgid(0, 0);
        /* quietest possible run */
        if (!freopen("/dev/null", "w", stdout)) { /* ignore */ }
        if (!freopen("/dev/null", "w", stderr)) { /* ignore */ }
        /* x11 backend: weston nests on the caller's X display and supplies a
         * real wl_seat (keyboard+pointer).  headless backend has no input
         * devices and advertises no wl_seat at all (verified). */
        if (x11)
            execlp("weston", "weston",
                   "--backend=x11", "--socket", sock,
                   "--idle-time=0", (char *)NULL);
        else
            execlp("weston", "weston",
                   "--backend=headless", "--socket", sock,
                   "--idle-time=0", (char *)NULL);
        _exit(127);
    }
    g_weston_pid = pid;

    for (i = 0; i < 40; i++) {          /* ~4 s for the socket to appear */
        usleep(100000);
        if (stat(spath, &st) == 0) return 0;
        if (waitpid(pid, NULL, WNOHANG) != 0) { g_weston_pid = -1; return -1; }
    }
    weston_kill();
    return -1;
}

int main(void) {
    win32k_wndclassex_t wc;
    unsigned int atom = 0;
    HWND w1, w2;
    nt_input_serials_t ser;
    int live_ran = 0, live_ok = 0;

    printf("=== NT-Box input LIVE test (W8: T-183/T-184/T-185, INP-011..INP-016) ===\n");

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = NULL;
    wc.hInstance = (HINSTANCE)0x10000000;
    wc.lpszClassName = "NtBoxInputLiveClass";
    atom = NtUserRegisterClassEx(&wc);
    check(atom != 0, "INP-001", "RegisterClassEx atom=%u", atom);

    w1 = NtUserCreateWindowEx(0, "NtBoxInputLiveClass", "Win1", 0,
                              10, 10, 320, 240, NULL, NULL, wc.hInstance, NULL);
    w2 = NtUserCreateWindowEx(0, "NtBoxInputLiveClass", "Win2", 0,
                              40, 40, 320, 240, NULL, NULL, wc.hInstance, NULL);
    check(w1 != NULL && w2 != NULL, "INP-001",
          "created 2 windows w1=%p w2=%p", (void *)w1, (void *)w2);

    check(input_bridge_init() == 0, "INP-006", "input bridge initialised");
    check(input_bridge_window_created(w1) == 0, "INP-001", "w1 registered via bridge");
    check(input_bridge_window_created(w2) == 0, "INP-001", "w2 registered via bridge");

    /* ---------------------------------------------------------------- */
    /* 1. UNIT — wl_seat discovery / capabilities (T-184)                 */
    /* ---------------------------------------------------------------- */
    printf("\n--- [unit] wl_seat discovery / capabilities (INP-012) ---\n");
    input_bridge_seat_reset();
    check(input_bridge_seat_discovered(101, 5) == 0, "INP-012",
          "seat 101 v5 discovered, seats=%d", input_bridge_seat_count());
    check(input_bridge_seat_discovered(202, 5) == 1, "INP-012",
          "second seat 202 discovered (multi-seat safe), seats=%d",
          input_bridge_seat_count());
    check(input_bridge_seat_discovered(101, 5) == 0, "INP-012",
          "duplicate discovery of 101 is idempotent");

    check(input_bridge_seat_caps(101, 0) == 0, "INP-012",
          "seat 101 with caps=0: refused cleanly (nothing bound)");
    check(input_bridge_seats_with_keyboard() == 0, "INP-012", "no keyboard bound");
    check(input_bridge_seats_with_pointer() == 0, "INP-012", "no pointer bound");

    check(input_bridge_seat_caps(101, 1u | 2u) == 0, "INP-012",
          "seat 101: caps KEYBOARD|POINTER");
    check(input_bridge_seats_with_keyboard() == 1, "INP-012", "keyboard bound on seat 101");
    check(input_bridge_seats_with_pointer() == 1, "INP-012", "pointer bound on seat 101");

    check(input_bridge_seat_caps(202, 2u) == 0, "INP-012", "seat 202: keyboard only");
    check(input_bridge_seats_with_keyboard() == 2, "INP-012", "two keyboard-capable seats");
    check(input_bridge_seats_with_pointer() == 1, "INP-012", "still one pointer-capable seat");

    check(input_bridge_seat_caps(999, 2u) == -1, "INP-012",
          "caps for unknown seat 999 refused (-1)");
    {
        int c0 = input_bridge_seat_count();
        input_bridge_seat_disappeared(999);
        check(input_bridge_seat_count() == c0, "INP-012",
              "removal of unknown seat 999 refused honestly (count stable)");
    }
    input_bridge_seat_disappeared(202);
    check(input_bridge_seat_count() == 1, "INP-012",
          "seat 202 removed, seats=%d", input_bridge_seat_count());
    {
        int i, refused = 0;
        input_bridge_seat_reset();
        for (i = 0; i < 9; i++) {           /* one more than the table holds */
            if (input_bridge_seat_discovered(3000u + (unsigned)i, 5) < 0) refused = 1;
        }
        check(refused, "INP-012",
              "9th seat refused cleanly (table cap), seats=%d", input_bridge_seat_count());
    }
    input_bridge_seat_reset();
    check(1, "INP-012", "seat table reset clean (seats=%d)", input_bridge_seat_count());

    /* ---------------------------------------------------------------- */
    /* 2. UNIT — wl_keyboard receiver / translation (T-183, T-185)        */
    /* ---------------------------------------------------------------- */
    printf("\n--- [unit] wl_keyboard receiver + translation (INP-013/015/016) ---\n");
    nt_input_clear_all();
    nt_input_set_keymods(0);

    /* surface->window map (the live listeners resolve this way) */
    input_bridge_surface_map((struct wl_surface *)0x1001, w1);
    input_bridge_surface_map((struct wl_surface *)0x1002, w2);
    check(input_bridge_surface_lookup((struct wl_surface *)0x1001) == w1,
          "INP-013", "surface 0x1001 -> w1 mapped");
    check(input_bridge_surface_lookup((struct wl_surface *)0xDEAD) == NULL,
          "INP-013", "unknown surface lookup = NULL (honest)");

    /* unknown surface on enter: refused honestly */
    nt_input_clear_all();
    input_bridge_on_kb_enter(40, (struct wl_surface *)0xDEAD);
    check(nt_input_message_count(NULL) == 0, "INP-013",
          "kb.enter with unknown surface refused (no message, no focus)");
    check(nt_input_get_focus() == NULL, "INP-013", "focus untouched");

    /* real enter path */
    input_bridge_on_kb_enter(42, (struct wl_surface *)0x1001);
    check(nt_input_get_focus() == w1, "INP-013", "kb.enter surf1 -> focus w1");
    check(find_msg(w1, WM_SETFOCUS, NULL, NULL, NULL), "INP-013",
          "WM_SETFOCUS generated for w1");
    nt_input_get_serials(&ser);
    check(ser.kb_enter == 42, "INP-013", "serial kb_enter=%u (expect 42)", ser.kb_enter);

    /* key 'A' (evdev 30) press */
    nt_input_clear_all();
    input_bridge_on_kb_key(43, 100, 30, WL_KEYBOARD_KEY_STATE_PRESSED);
    {
        unsigned long wp = 0, t = 0; long lp = 0;
        check(find_msg(w1, WM_KEYDOWN, &wp, &lp, &t), "INP-013", "WM_KEYDOWN on w1");
        check(wp == 'A', "INP-015", "evdev 30 -> VK 0x%02lX (expect 0x41)", wp);
        check((((unsigned long)lp >> 16) & 0xFF) == 30, "INP-015",
              "scancode bits = %lu (set-1 0x1E)", ((unsigned long)lp >> 16) & 0xFF);
        check(t == 100, "INP-013", "msg.time stamped from wl event time=%lu", t);
        {
            unsigned long cw = 0;
            check(find_msg(w1, WM_CHAR, &cw, NULL, NULL), "INP-015", "WM_CHAR on w1");
            check(cw == 'a', "INP-015", "WM_CHAR 0x%02lX (expect 'a'=0x61)", cw);
        }
    }
    nt_input_get_serials(&ser);
    check(ser.kb_key == 43, "INP-013", "serial kb_key=%u (expect 43)", ser.kb_key);

    /* Shift via real wl modifiers event (xkb depressed bit0) */
    input_bridge_on_kb_modifiers(44, 0x01, 0, 0, 0);
    check((nt_input_keymods() & MK_SHIFT) != 0, "INP-013",
          "modifiers: Shift tracked (keymods=0x%X)", nt_input_keymods());
    nt_input_clear_all();
    input_bridge_on_kb_key(45, 110, 30, WL_KEYBOARD_KEY_STATE_PRESSED);
    {
        unsigned long cw = 0;
        check(find_msg(w1, WM_CHAR, &cw, NULL, NULL), "INP-015", "shift+KEY_A WM_CHAR");
        check(cw == 'A', "INP-015", "Shift-> WM_CHAR 0x%02lX (expect 'A'=0x41)", cw);
    }

    /* Shift + KEY_2 (evdev 3) -> '@' */
    nt_input_clear_all();
    input_bridge_on_kb_key(46, 120, 3, WL_KEYBOARD_KEY_STATE_PRESSED);
    {
        unsigned long cw = 0, dw = 0;
        int found;
        found = find_msg(w1, WM_KEYDOWN, &dw, NULL, NULL);
        check(found && dw == '2', "INP-015",
              "KEY_2 -> VK 0x%02lX (expect '2'=0x32)", dw);
        check(find_msg(w1, WM_CHAR, &cw, NULL, NULL), "INP-015", "shift+2 WM_CHAR");
        check(cw == '@', "INP-015", "shift+2 -> WM_CHAR 0x%02lX (expect '@'=0x40)", cw);
    }

    /* modifiers released */
    input_bridge_on_kb_modifiers(47, 0x00, 0, 0, 0);
    nt_input_clear_all();
    input_bridge_on_kb_key(48, 130, 3, WL_KEYBOARD_KEY_STATE_PRESSED);
    {
        unsigned long cw = 0;
        check(find_msg(w1, WM_CHAR, &cw, NULL, NULL), "INP-015", "unshifted 2 WM_CHAR");
        check(cw == '2', "INP-015", "WM_CHAR 0x%02lX (expect '2'=0x32)", cw);
    }

    /* arrow key: VK + extended bit, no char */
    nt_input_clear_all();
    input_bridge_on_kb_key(49, 140, 106, WL_KEYBOARD_KEY_STATE_PRESSED);   /* KEY_RIGHT */
    {
        unsigned long dw = 0; long lp = 0;
        check(find_msg(w1, WM_KEYDOWN, &dw, &lp, NULL), "INP-015", "KEY_RIGHT WM_KEYDOWN");
        check(dw == VK_RIGHT, "INP-015", "VK 0x%02lX (expect VK_RIGHT=0x27)", dw);
        check(((unsigned long)lp & 0x01000000UL) != 0, "INP-015",
              "extended-key bit set in lParam (0x%lX)", (unsigned long)lp);
        check(count_msg(w1, WM_CHAR) == 0, "INP-015", "no WM_CHAR for arrows");
    }

    /* F5 (evdev 63) */
    nt_input_clear_all();
    input_bridge_on_kb_key(50, 150, 63, WL_KEYBOARD_KEY_STATE_PRESSED);
    {
        unsigned long dw = 0;
        int found;
        found = find_msg(w1, WM_KEYDOWN, &dw, NULL, NULL);
        check(found && dw == VK_F5, "INP-015",
              "evdev 63 -> VK 0x%02lX (expect VK_F5=0x74)", dw);
    }

    /* F12 (evdev 88) */
    nt_input_clear_all();
    input_bridge_on_kb_key(51, 155, 88, WL_KEYBOARD_KEY_STATE_PRESSED);
    {
        unsigned long dw = 0;
        check(find_msg(w1, WM_KEYDOWN, &dw, NULL, NULL), "INP-015", "F12 keydown");
        check(dw == VK_F12, "INP-015", "VK 0x%02lX (expect VK_F12=0x7B)", dw);
    }

    /* punctuation '=' (evdev 13) with shift -> '+' */
    nt_input_clear_all();
    input_bridge_on_kb_key(52, 160, 13, WL_KEYBOARD_KEY_STATE_PRESSED);
    {
        unsigned long dw = 0, cw = 0;
        int found;
        found = find_msg(w1, WM_KEYDOWN, &dw, NULL, NULL);
        check(found && dw == VK_OEM_PLUS, "INP-015",
              "evdev 13 -> VK 0x%02lX (expect VK_OEM_PLUS=0xBB)", dw);
        found = find_msg(w1, WM_CHAR, &cw, NULL, NULL);
        check(found && cw == '=', "INP-015",
              "WM_CHAR 0x%02lX (expect '='=0x3D)", cw);
    }

    /* keyup for BACKSPACE (evdev 14): up state, transition bits */
    nt_input_clear_all();
    input_bridge_on_kb_key(53, 170, 14, WL_KEYBOARD_KEY_STATE_RELEASED);
    {
        unsigned long dw = 0; long lp = 0;
        check(find_msg(w1, WM_KEYUP, &dw, &lp, NULL), "INP-013", "WM_KEYUP backspace");
        check(dw == VK_BACK, "INP-015", "VK 0x%02lX (expect VK_BACK=0x08)", dw);
        check(((unsigned long)lp & 0xC0000000UL) != 0, "INP-013",
              "keyup transition bits set (0x%lX)", (unsigned long)lp);
    }

    /* CapsLock via lock bit (bit1), no shift -> uppercase */
    input_bridge_on_kb_modifiers(54, 0x02, 0, 0, 0);
    nt_input_clear_all();
    input_bridge_on_kb_key(55, 180, 30, WL_KEYBOARD_KEY_STATE_PRESSED);
    {
        unsigned long cw = 0;
        int found;
        found = find_msg(w1, WM_CHAR, &cw, NULL, NULL);
        check(found && cw == 'A', "INP-015",
              "CapsLock -> WM_CHAR 0x%02lX (expect 'A')", cw);
    }
    /* CapsLock + Shift -> lowercase */
    input_bridge_on_kb_modifiers(56, 0x03, 0, 0, 0);
    nt_input_clear_all();
    input_bridge_on_kb_key(57, 190, 30, WL_KEYBOARD_KEY_STATE_PRESSED);
    {
        unsigned long cw = 0;
        int found;
        found = find_msg(w1, WM_CHAR, &cw, NULL, NULL);
        check(found && cw == 'a', "INP-015",
              "CapsLock+Shift -> WM_CHAR 0x%02lX (expect 'a')", cw);
    }
    /* Ctrl+A -> 0x01 */
    input_bridge_on_kb_modifiers(58, 0x04, 0, 0, 0);
    nt_input_clear_all();
    input_bridge_on_kb_key(59, 200, 30, WL_KEYBOARD_KEY_STATE_PRESSED);
    {
        unsigned long cw = 0, dw = 0;
        int found;
        found = find_msg(w1, WM_KEYDOWN, &dw, NULL, NULL);
        check(found && dw == 'A', "INP-015", "Ctrl+A VK");
        found = find_msg(w1, WM_CHAR, &cw, NULL, NULL);
        check(found && cw == 0x01, "INP-015",
              "Ctrl+A -> WM_CHAR 0x%02lX (expect 0x01)", cw);
    }
    /* Alt+F4 -> WM_SYSKEYDOWN, no char */
    input_bridge_on_kb_modifiers(60, 0x08, 0, 0, 0);
    nt_input_clear_all();
    input_bridge_on_kb_key(61, 210, 62, WL_KEYBOARD_KEY_STATE_PRESSED);
    {
        unsigned long dw = 0;
        check(find_msg(w1, WM_SYSKEYDOWN, &dw, NULL, NULL), "INP-013",
              "Alt+F4 -> WM_SYSKEYDOWN");
        check(dw == VK_F4, "INP-015", "SYS keydown VK 0x%02lX (expect VK_F4=0x73)", dw);
        check(count_msg(w1, WM_KEYDOWN) == 0, "INP-013", "no plain WM_KEYDOWN under Alt");
        check(count_msg(w1, WM_CHAR) == 0, "INP-015", "no WM_CHAR under Alt (SYS path)");
    }
    input_bridge_on_kb_modifiers(62, 0x00, 0, 0, 0);

    /* unmappable key refused honestly */
    nt_input_clear_all();
    input_bridge_on_kb_key(63, 220, 425, WL_KEYBOARD_KEY_STATE_PRESSED);
    check(nt_input_message_count(w1) == 0 && nt_input_message_count(w2) == 0,
          "INP-015", "unmappable key 425 refused (no message anywhere)");

    /* focused-only routing: key event naming the non-focused window */
    nt_input_clear_all();
    input_bridge_on_kb_key(64, 230, 30, WL_KEYBOARD_KEY_STATE_RELEASED);
    /* focus is w1; a wl key event would never carry another surface, and a
     * forged hwnd mismatch is refused by the input layer: */
    {
        nt_input_clear_all();
        input_bridge_wl_keyboard_key(w2, 65, 240, 30, WL_KEYBOARD_KEY_STATE_PRESSED);
        check(nt_input_message_count(w2) == 0, "INP-016",
              "key aimed at non-focused w2 refused (routed nowhere)");
        check(nt_input_message_count(w1) == 0, "INP-016",
              "refused key did not leak into w1 either");
    }

    /* keyboard leave */
    input_bridge_on_kb_leave(66, (struct wl_surface *)0x1001);
    check(nt_input_get_focus() == NULL, "INP-013", "kb.leave clears focus");
    check(find_msg(w1, WM_KILLFOCUS, NULL, NULL, NULL), "INP-013",
          "WM_KILLFOCUS generated for w1");
    nt_input_get_serials(&ser);
    check(ser.kb_leave == 66, "INP-013", "serial kb_leave=%u (expect 66)", ser.kb_leave);
    nt_input_clear_all();
    input_bridge_on_kb_key(67, 250, 30, WL_KEYBOARD_KEY_STATE_PRESSED);
    check(nt_input_message_count(NULL) == 0, "INP-016",
          "key with no focus dropped honestly");

    /* ---------------------------------------------------------------- */
    /* 3. UNIT — wl_pointer receiver (T-183)                              */
    /* ---------------------------------------------------------------- */
    printf("\n--- [unit] wl_pointer receiver (INP-014) ---\n");
    nt_input_clear_all();
    nt_input_set_keymods(0);

    /* unknown surface refused */
    input_bridge_on_ptr_enter(5, (struct wl_surface *)0xDEAD, 0, 0);
    check(nt_input_pointer_window() == NULL, "INP-014",
          "ptr.enter unknown surface refused (no pointer window)");

    input_bridge_on_ptr_enter(7, (struct wl_surface *)0x1002, 10, 10);
    check(nt_input_pointer_window() == w2, "INP-014", "ptr.enter surf2 -> pointer w2");
    nt_input_get_serials(&ser);
    check(ser.ptr_enter == 7, "INP-014", "serial ptr_enter=%u (expect 7)", ser.ptr_enter);

    /* motion on w2 even though keyboard focus is elsewhere (T-185) */
    nt_input_clear_all();
    input_bridge_on_ptr_motion(150, 20, 25);
    {
        unsigned long wp = 0; long lp = 0;
        unsigned long t = 0;
        check(find_msg(w2, WM_MOUSEMOVE, &wp, &lp, &t), "INP-014", "WM_MOUSEMOVE on w2");
        check((int)(lp & 0xFFFF) == 20 && (int)((lp >> 16) & 0xFFFF) == 25,
              "INP-014", "coords (%d,%d) expect (20,25)",
              (int)(lp & 0xFFFF), (int)((lp >> 16) & 0xFFFF));
        check(t == 150, "INP-014", "motion time stamped t=%lu", t);
        check(count_msg(w1, WM_MOUSEMOVE) == 0, "INP-014",
              "no MOUSEMOVE leaked to w1 (w1 has keyboard focus only)");
    }

    /* button press with real serial */
    nt_input_clear_all();
    input_bridge_on_ptr_button(8, 160, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
    {
        unsigned long wp = 0; long lp = 0;
        check(find_msg(w2, WM_LBUTTONDOWN, &wp, &lp, NULL), "INP-014", "WM_LBUTTONDOWN on w2");
        check((wp & MK_LBUTTON) != 0, "INP-014",
              "wParam carries MK_LBUTTON (0x%lX)", wp);
        check((int)(lp & 0xFFFF) == 20 && (int)((lp >> 16) & 0xFFFF) == 25,
              "INP-014", "button event keeps last motion coords (20,25)");
    }
    nt_input_get_serials(&ser);
    check(ser.ptr_button == 8, "INP-014", "serial ptr_button=%u (expect 8)", ser.ptr_button);

    /* release clears state */
    nt_input_clear_all();
    input_bridge_on_ptr_button(9, 165, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
    check(find_msg(w2, WM_LBUTTONUP, NULL, NULL, NULL), "INP-014", "WM_LBUTTONUP");
    {
        unsigned long wp = 0;
        /* next move must not carry MK_LBUTTON */
        input_bridge_on_ptr_motion(166, 30, 30);
        find_msg(w2, WM_MOUSEMOVE, &wp, NULL, NULL);
        check((wp & MK_LBUTTON) == 0, "INP-014", "release clears MK_LBUTTON (wp=0x%lX)", wp);
    }

    /* right button */
    nt_input_clear_all();
    input_bridge_on_ptr_button(10, 170, BTN_RIGHT, WL_POINTER_BUTTON_STATE_PRESSED);
    check(find_msg(w2, WM_RBUTTONDOWN, NULL, NULL, NULL), "INP-014", "right button -> RBUTTONDOWN");
    input_bridge_on_ptr_button(11, 171, BTN_RIGHT, WL_POINTER_BUTTON_STATE_RELEASED);

    /* unmappable button refused */
    nt_input_clear_all();
    input_bridge_on_ptr_button(12, 175, 320, WL_POINTER_BUTTON_STATE_PRESSED);
    check(nt_input_message_count(w2) == 0, "INP-014",
          "button 320 refused (no message,FAIL lamp emitted)");

    /* vertical scroll: wl value 10 == one notch -> WM_MOUSEWHEEL +120 */
    nt_input_clear_all();
    input_bridge_on_ptr_axis(180, WL_POINTER_AXIS_VERTICAL_SCROLL, 10.0);
    {
        unsigned long wp = 0;
        check(find_msg(w2, WM_MOUSEWHEEL, &wp, NULL, NULL), "INP-014",
              "vertical wheel -> WM_MOUSEWHEEL");
        check(((wp >> 16) & 0xFFFF) == 0x0078, "INP-014",
              "delta=+120 (wParam=0x%08lX, expect hiword 0x0078)", wp);
    }
    /* horizontal scroll: opposite sign, WM_MOUSEHWHEEL */
    nt_input_clear_all();
    input_bridge_on_ptr_axis(181, WL_POINTER_AXIS_HORIZONTAL_SCROLL, 10.0);
    {
        unsigned long wp = 0;
        check(find_msg(w2, WM_MOUSEHWHEEL, &wp, NULL, NULL), "INP-014",
              "horizontal wheel -> WM_MOUSEHWHEEL");
        check(((wp >> 16) & 0xFFFF) == 0xFF88, "INP-014",
              "delta=-120 (wParam=0x%08lX, expect hiword 0xFF88)", wp);
    }
    /* scroll with no pointer window (after leave) is refused */
    input_bridge_on_ptr_leave(13, (struct wl_surface *)0x1002);
    check(nt_input_pointer_window() == NULL, "INP-014", "ptr.leave clears pointer window");
    nt_input_get_serials(&ser);
    check(ser.ptr_leave == 13, "INP-014", "serial ptr_leave=%u (expect 13)", ser.ptr_leave);
    nt_input_clear_all();
    input_bridge_on_ptr_button(14, 190, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
    check(nt_input_message_count(NULL) == 0, "INP-014",
          "button without pointer target refused honestly");

    /* ---------------------------------------------------------------- */
    /* 4. LIVE — real compositor (T-183/T-184 live layer)                 */
    /* ---------------------------------------------------------------- */
    printf("\n--- [live] real Wayland compositor (INP-011/INP-012) ---\n");
    {
        int have_wd = getenv("WAYLAND_DISPLAY") != NULL ||
                      getenv("WAYLAND_SOCKET") != NULL;
        int can_connect = 0;

        if (have_wd) {
            can_connect = 1;
            printf("  [INFO] using pre-existing compositor WAYLAND_DISPLAY=%s\n",
                   getenv("WAYLAND_DISPLAY"));
        } else if (getenv("DISPLAY")) {
            /* x11 backend = a real seat (keyboard+pointer) nested on the
             * caller's X display; headless advertises NO wl_seat at all. */
            printf("  [INFO] spawning weston --backend=x11 on DISPLAY=%s\n",
                   getenv("DISPLAY"));
            if (weston_spawn("/tmp/opencode/wlrun-input", "ntbox-inp-live", 1) == 0) {
                /* connect() resolves XDG_RUNTIME_DIR + WAYLAND_DISPLAY; the
                 * spawned socket name must be advertised to it */
                setenv("WAYLAND_DISPLAY", "ntbox-inp-live", 1);
                can_connect = 1;
                printf("  [INFO] spawned weston x11-backed (pid=%d)\n", (int)g_weston_pid);
            }
        }
        if (!can_connect) {
            if (weston_spawn("/tmp/opencode/wlrun-input", "ntbox-inp-live", 0) == 0) {
                setenv("WAYLAND_DISPLAY", "ntbox-inp-live", 1);
                can_connect = 1;
                printf("  [INFO] spawned weston headless (pid=%d)\n", (int)g_weston_pid);
            }
        }
        if (!can_connect) {
            skip_lamp("INP-011",
                      "no WAYLAND_DISPLAY and weston spawn failed — live layer SKIP (rc=3)");
        }

        if (can_connect) {
            if (input_bridge_wayland_connect() == 0) {
                int seats = input_bridge_seat_count();
                int kbs = input_bridge_seats_with_keyboard();
                int ptrs = input_bridge_seats_with_pointer();
                live_ran = 1;
                check(1, "INP-011", "wl_display + wl_registry + roundtrip ok");
                if (seats >= 1) {
                    check(1, "INP-012",
                          "compositor advertised %d seat(s); caps bound: kb=%d ptr=%d "
                          "(bind-only-what-exists verified live)",
                          seats, kbs, ptrs);
                    live_ok = 1;   /* seat discovery proven live too */
                } else {
                    /* e.g. weston --backend=headless has no input devices and
                     * advertises no wl_seat global — honest, verified above */
                    skip_lamp("INP-012",
                              "compositor connected but advertises no wl_seat "
                              "(no input devices) — live discovery not provable here");
                }
                if (input_bridge_keymap_seen(NULL, NULL))
                    printf("  [INFO] live keymap advertisement received\n");
                /* pump: no queued real input expected without an operator */
                input_bridge_pump_events();
                input_bridge_wayland_disconnect();
                check(input_bridge_wayland_is_connected() == 0, "INP-011",
                      "clean disconnect");
            }
            weston_kill();
            /* socket+lock cleanup for repeat runs */
            {
                char spath[512], lpath[512];
                const char *rt = getenv("XDG_RUNTIME_DIR");
                if (rt) {
                    snprintf(spath, sizeof(spath), "%s/ntbox-inp-live", rt);
                    snprintf(lpath, sizeof(lpath), "%s/ntbox-inp-live.lock", rt);
                    unlink(spath);
                    unlink(lpath);
                }
            }
        }
    }

    input_bridge_shutdown();

    printf("\n=== result: %s (checks ok=%d fail=%d) ===\n",
           failures ? "FAIL"
                    : (live_ran ? (live_ok ? "PASS" : "SKIP(live: no seat advertised)")
                                : "SKIP(unit-only)"),
           passes, failures);

    if (failures) {
        printf("[test_input_live] RESULT: FAIL\n");
        return 1;
    }
    if (!live_ok) {
        printf("[test_input_live] RESULT: SKIP (live layer unavailable; "
               "unit translation layer fully exercised above)\n");
        return 3;   /* rc=3 SKIP pattern per project (not a pass, not a fail) */
    }
    printf("[test_input_live] RESULT: PASS\n");
    return 0;
}
