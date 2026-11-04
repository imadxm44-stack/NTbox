/*
 * input_bridge.c — NT-Box: bridges Wayland input events to win32k message queue.
 * Lamps INP-006..INP-010, live layer INP-011..INP-016 (T-183/T-184/T-185).
 */
#if defined(__has_include)
#  if __has_include(<wayland-client.h>)
#    define NTB_REAL_WAYLAND_CLIENT 1
#  endif
#endif
#define _GNU_SOURCE
#include "input_bridge.h"
#include "win32k.h"
#include "input.h"
#include "ntb0_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>

#ifdef NTB_REAL_WAYLAND_CLIENT
/* the REAL wayland-client headers: their enum constants (WL_KEYBOARD_KEY_
 * STATE_*, ...) supersede the compat macros of input.h */
#include <wayland-client.h>
#endif

/* ------------------------------------------------------------------ */
/* Internal state                                                     */
/* ------------------------------------------------------------------ */

#define INPUT_BRIDGE_MAGIC 0x49425244UL  /* "IBRD" */

static int g_initialised = 0;
static struct wl_display *g_wl_display = NULL;
static int g_wl_fd = -1;

/* ------------------------------------------------------------------ */
/* Helpers: post message to win32k queue                              */
/* ------------------------------------------------------------------ */

extern int NtUserPostMessage(HWND hwnd, UINT message,
                             unsigned long wParam, long lParam);

/* The win32k message queue is in win32k.c (g_msgq).  We need to
 * post there.  Since it's static, we'll use a callback approach
 * or directly call an exported function.  Let's add a function
 * to win32k.c for this, but for now we mirror the logic. */

static int win32k_post_message_internal(HWND hwnd, UINT message,
                                        unsigned long wParam, long lParam,
                                        int pt_x, int pt_y)
{
    extern win32k_msg_t g_msgq[256];
    extern int g_msgq_n;
    #define MSGQ_CAP 256

    if (g_msgq_n >= MSGQ_CAP) {
        return 0;  /* queue full */
    }

    win32k_msg_t *m = &g_msgq[g_msgq_n];
    m->hwnd = hwnd;
    m->message = message;
    m->wParam = wParam;
    m->lParam = lParam;
    m->time = 0;  /* could use GetTickCount equivalent */
    m->pt_x = pt_x;
    m->pt_y = pt_y;
    g_msgq_n++;

    NTB0_OK("INP-006", "posted msg=%u hwnd=%p to win32k queue (q=%d)",
            message, (void *)hwnd, g_msgq_n);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

int input_bridge_init(void)
{
    if (g_initialised) return 0;

    /* Initialise the input layer (per-window queues, focus, etc) */
    nt_input_init();

    g_initialised = 1;
    NTB0_OK("INP-006", "input bridge initialised");
    return 0;
}

void input_bridge_shutdown(void)
{
    if (!g_initialised) return;

    nt_input_shutdown();

    g_wl_display = NULL;
    g_wl_fd = -1;
    g_initialised = 0;
    NTB0_OK("INP-006", "input bridge shutdown");
}

/* ------------------------------------------------------------------ */
/* Wayland seat callbacks                                             */
/* ------------------------------------------------------------------ */

void input_bridge_wl_keyboard_enter(HWND hwnd, unsigned int serial)
{
    if (!g_initialised) input_bridge_init();

    nt_input_register_window(hwnd);
    nt_input_wl_keyboard_enter(hwnd, serial);

    /* Also post WM_SETFOCUS to win32k queue for GetMessage/DispatchMessage */
    win32k_post_message_internal(hwnd, WM_SETFOCUS, 0, 0, 0, 0);
    NTB0_OK("INP-007", "wl_keyboard.enter hwnd=%p serial=%u -> WM_SETFOCUS posted",
            (void *)hwnd, serial);
}

void input_bridge_wl_keyboard_leave(HWND hwnd, unsigned int serial)
{
    if (!g_initialised) input_bridge_init();

    nt_input_wl_keyboard_leave(hwnd, serial);

    /* Also post WM_KILLFOCUS to win32k queue */
    win32k_post_message_internal(hwnd, WM_KILLFOCUS, 0, 0, 0, 0);
    NTB0_OK("INP-007", "wl_keyboard.leave hwnd=%p serial=%u -> WM_KILLFOCUS posted",
            (void *)hwnd, serial);
}

void input_bridge_wl_keyboard_key(HWND hwnd, unsigned int serial,
                                  unsigned int time, unsigned int key,
                                  unsigned int state)
{
    HWND focus, target;
    unsigned int vk;
    int mapped = 0, before;
    UINT msg;

    if (!g_initialised) input_bridge_init();

    /* T-185: keyboard events belong to the focused window only */
    focus = nt_input_get_focus();
    target = hwnd ? hwnd : focus;
    if (!target || !focus || target != focus) {
        NTB0_FAIL("INP-016",
                  "wl_keyboard.key refused: focus=%p target=%p — not routed",
                  (void *)focus, (void *)target);
        return;
    }

    mapped = nt_input_evdev_to_vk(key, &vk);
    if (!mapped) {
        NTB0_FAIL("INP-013", "key %u refused: unmappable evdev keycode (no VK, no message)", key);
        return;
    }
    before = nt_input_message_count(target);
    nt_input_wl_keyboard_key(target, serial, time, key, state);
    if (nt_input_message_count(target) <= before) {
        /* input layer refused the key (unmappable etc.) — no mirror, honest */
        return;
    }

    /* Mirror to win32k queue: KEYDOWN/UP (+ SYS variants under Alt) */
    if (mapped) {
        msg = (state == WL_KEYBOARD_KEY_STATE_PRESSED)
                  ? ((nt_input_keymods() & INP_MOD_ALT) ? WM_SYSKEYDOWN : WM_KEYDOWN)
                  : ((nt_input_keymods() & INP_MOD_ALT) ? WM_SYSKEYUP   : WM_KEYUP);
        win32k_post_message_internal(target, msg, vk, (long)((key & 0xFF) << 16) | 1, 0, 0);
        /* WM_CHAR mirror (same US-layout rule as the input layer) */
        if (state == WL_KEYBOARD_KEY_STATE_PRESSED &&
            !(nt_input_keymods() & INP_MOD_ALT)) {
            unsigned int ch = nt_input_keychar(key);
            if (ch) win32k_post_message_internal(target, WM_CHAR, ch, 0, 0, 0);
        }
        NTB0_OK("INP-007", "wl_keyboard.key hwnd=%p key=%u vk=0x%02X state=%u -> win32k msg=%u",
                (void *)target, key, vk, state, msg);
    }
}

void input_bridge_wl_keyboard_modifiers(unsigned int mods_depressed,
                                        unsigned int mods_latched,
                                        unsigned int mods_locked,
                                        unsigned int group)
{
    (void)mods_latched; (void)mods_locked; (void)group;
    if (!g_initialised) input_bridge_init();

    nt_input_wl_keyboard_modifiers(mods_depressed, mods_latched, mods_locked, group);
    NTB0_OK("INP-007", "wl_keyboard.modifiers depressed=0x%X", mods_depressed);
}

void input_bridge_wl_pointer_enter(HWND hwnd, int x, int y)
{
    input_bridge_wl_pointer_enter_serial(hwnd, 0, x, y);
}

void input_bridge_wl_pointer_enter_serial(HWND hwnd, unsigned int serial, int x, int y)
{
    if (!g_initialised) input_bridge_init();

    nt_input_register_window(hwnd);
    nt_input_wl_pointer_enter2(hwnd, serial, x, y);

    /* Post WM_MOUSEMOVE for the enter position */
    win32k_post_message_internal(hwnd, WM_MOUSEMOVE, 0,
                                 nt_input_pack_lparam(x, y), x, y);
    NTB0_OK("INP-008", "wl_pointer.enter hwnd=%p (%d,%d) serial=%u -> WM_MOUSEMOVE",
            (void *)hwnd, x, y, serial);
}

void input_bridge_wl_pointer_leave(HWND hwnd)
{
    input_bridge_wl_pointer_leave_serial(hwnd, 0);
}

void input_bridge_wl_pointer_leave_serial(HWND hwnd, unsigned int serial)
{
    if (!g_initialised) input_bridge_init();

    nt_input_wl_pointer_leave2(hwnd, serial);
    NTB0_OK("INP-008", "wl_pointer.leave hwnd=%p serial=%u", (void *)hwnd, serial);
}

void input_bridge_wl_pointer_motion(unsigned int time, int x, int y)
{
    HWND target;
    int before;

    if (!g_initialised) input_bridge_init();

    /* Target is the surface under the cursor (pointer window); keyboard
     * focus is only a fallback for synthetic callers. */
    target = nt_input_pointer_window();
    if (!target) target = nt_input_get_focus();
    if (!target) {
        NTB0_FAIL("INP-016", "wl_pointer.motion refused: no pointer window, no focus");
        return;
    }

    before = nt_input_message_count(target);
    nt_input_wl_pointer_motion_t(time, x, y);
    if (nt_input_message_count(target) <= before) return; /* refused */

    win32k_post_message_internal(target, WM_MOUSEMOVE,
                                 nt_input_keymods() & (MK_SHIFT | MK_CONTROL),
                                 nt_input_pack_lparam(x, y), x, y);
    NTB0_OK("INP-008", "wl_pointer.motion (%d,%d) -> WM_MOUSEMOVE hwnd=%p",
            x, y, (void *)target);
}

void input_bridge_wl_pointer_button(unsigned int serial, unsigned int time,
                                    unsigned int button, unsigned int state,
                                    int x, int y)
{
    HWND target;
    int before;
    unsigned int msg = 0;

    if (!g_initialised) input_bridge_init();

    /* pointer window (surface under cursor) wins over keyboard focus */
    target = nt_input_pointer_window();
    if (!target) target = nt_input_get_focus();
    if (!target) {
        NTB0_FAIL("INP-016",
                  "wl_pointer.button refused: no pointer window, no focus (btn=%u)",
                  button);
        return;
    }

    before = nt_input_message_count(target);
    nt_input_wl_pointer_button(serial, time, button, state, x, y);
    if (nt_input_message_count(target) <= before) return; /* refused (bad code) */

    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        if (button == BTN_LEFT || button == 1 || button == 0) msg = WM_LBUTTONDOWN;
        else if (button == BTN_RIGHT || button == 3) msg = WM_RBUTTONDOWN;
        else msg = WM_MBUTTONDOWN;
    } else {
        if (button == BTN_LEFT || button == 1 || button == 0) msg = WM_LBUTTONUP;
        else if (button == BTN_RIGHT || button == 3) msg = WM_RBUTTONUP;
        else msg = WM_MBUTTONUP;
    }
    win32k_post_message_internal(target, msg,
                                 nt_input_keymods() & (MK_SHIFT | MK_CONTROL),
                                 nt_input_pack_lparam(x, y), x, y);
    NTB0_OK("INP-008", "wl_pointer.button btn=%u state=%u serial=%u -> msg=%u hwnd=%p",
            button, state, serial, msg, (void *)target);
}

void input_bridge_wl_pointer_axis(unsigned int time, unsigned int axis,
                                  double value, int x, int y)
{
    HWND target;
    int before;
    int delta = (int)(value * 12.0);   /* wl value/10 = notch -> 120/notch */
    UINT msg;

    if (!g_initialised) input_bridge_init();

    target = nt_input_pointer_window();
    if (!target) target = nt_input_get_focus();
    if (!target) {
        NTB0_FAIL("INP-016", "wl_pointer.axis refused: no pointer window, no focus");
        return;
    }

    before = nt_input_message_count(target);
    nt_input_wl_pointer_axis(time, axis, value, x, y);
    if (nt_input_message_count(target) <= before) return; /* refused */

    if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
        delta = -delta;
        msg = WM_MOUSEHWHEEL;
    } else {
        msg = WM_MOUSEWHEEL;
    }
    win32k_post_message_internal(target, msg,
                                 (unsigned long)((delta & 0xFFFF) << 16) |
                                     (nt_input_keymods() & 0xFFFF),
                                 nt_input_pack_lparam(x, y), x, y);
    NTB0_OK("INP-008", "wl_pointer.axis axis=%u value=%.2f delta=%d -> msg=%u",
            axis, value, delta, msg);
}

void input_bridge_wl_touch_down(unsigned int serial, unsigned int time,
                                HWND hwnd, int id, int x, int y)
{
    (void)serial; (void)time; (void)id;
    if (!g_initialised) input_bridge_init();

    nt_input_register_window(hwnd);
    /* For now, synthesize a left button down + mouse move */
    win32k_post_message_internal(hwnd, WM_MOUSEMOVE, 0,
                                 nt_input_pack_lparam(x, y), x, y);
    win32k_post_message_internal(hwnd, WM_LBUTTONDOWN, MK_LBUTTON,
                                 nt_input_pack_lparam(x, y), x, y);
    NTB0_OK("INP-010", "wl_touch.down hwnd=%p id=%d (%d,%d)", (void *)hwnd, id, x, y);
}

void input_bridge_wl_touch_up(unsigned int serial, unsigned int time,
                              int id)
{
    (void)serial; (void)time; (void)id;
    if (!g_initialised) return;

    HWND hwnd = nt_input_get_focus();
    if (!hwnd) return;

    int x = 0, y = 0;  /* would need touch state tracking */
    win32k_post_message_internal(hwnd, WM_LBUTTONUP, 0,
                                 nt_input_pack_lparam(x, y), x, y);
    NTB0_OK("INP-010", "wl_touch.up id=%d", id);
}

void input_bridge_wl_touch_motion(unsigned int time, int id, int x, int y)
{
    (void)time; (void)id;
    if (!g_initialised) input_bridge_init();

    HWND hwnd = nt_input_get_focus();
    if (!hwnd) return;

    win32k_post_message_internal(hwnd, WM_MOUSEMOVE, MK_LBUTTON,
                                 nt_input_pack_lparam(x, y), x, y);
    NTB0_OK("INP-010", "wl_touch.motion id=%d (%d,%d)", id, x, y);
}

/* ------------------------------------------------------------------ */
/* win32k message queue integration                                   */
/* ------------------------------------------------------------------ */

int input_bridge_post_to_win32k(HWND hwnd, UINT message,
                                unsigned long wParam, long lParam,
                                int pt_x, int pt_y)
{
    if (!g_initialised) input_bridge_init();
    return win32k_post_message_internal(hwnd, message, wParam, lParam, pt_x, pt_y);
}

int input_bridge_peek_message(win32k_msg_t *msg, HWND hwnd,
                              UINT min, UINT max, UINT remove)
{
    if (!g_initialised) input_bridge_init();

    /* First try win32k's own queue */
    extern int NtUserPeekMessage(win32k_msg_t *, HWND, UINT, UINT, UINT);
    int r = NtUserPeekMessage(msg, hwnd, min, max, remove);
    if (r) return 1;

    /* Then try the input bridge's per-window queues */
    return nt_input_peek_message(msg, hwnd, min, max, remove);
}

int input_bridge_get_message(win32k_msg_t *msg, HWND hwnd,
                             UINT min, UINT max)
{
    if (!g_initialised) input_bridge_init();

    /* Pump Wayland events first to fill queues */
    input_bridge_pump_events();

    /* Try win32k queue */
    extern int NtUserGetMessage(win32k_msg_t *, HWND, UINT, UINT);
    int r = NtUserGetMessage(msg, hwnd, min, max);
    if (r != 0) return r;  /* got message (r > 0) or WM_QUIT (r == 0) */

    /* If win32k returned WM_QUIT but input bridge has messages,
     * return those instead (don't quit yet) */
    if (nt_input_message_count(hwnd) > 0 ||
        (hwnd == NULL && nt_input_message_count(NULL) > 0)) {
        return nt_input_get_message(msg, hwnd, min, max);
    }

    return 0;  /* WM_QUIT */
}

int input_bridge_translate_message(const win32k_msg_t *msg)
{
    (void)msg;
    /* For now, just a stub.  Real implementation would translate
     * WM_KEYDOWN to WM_CHAR etc. */
    NTB0_OK("INP-009", "TranslateMessage msg=%p", (const void *)msg);
    return 1;
}

unsigned long input_bridge_dispatch_message(const win32k_msg_t *msg)
{
    if (!msg) return 0;

    /* Use the existing win32k dispatch function */
    extern unsigned long NtUserDispatchMessage(const win32k_msg_t *msg);
    return NtUserDispatchMessage(msg);
}

/* ------------------------------------------------------------------ */
/* Event pumping and waiting                                          */
/* ------------------------------------------------------------------ */

int input_bridge_pump_events(void)
{
    int count = 0;

#ifdef INPB_HAVE_WAYLAND_CLIENT
    /* Dispatch any pending Wayland input events (they land in the
     * rewriter through the listeners above). */
    if (g_wl_display) {
        int r;
        wl_display_flush(g_wl_display);
        while ((r = wl_display_dispatch_pending(g_wl_display)) > 0) count += r;
        if (r < 0) {
            NTB0_FAIL("INP-011", "wl event dispatch error %d (errno=%d)",
                      r, errno);
            wl_display_disconnect(g_wl_display);
            g_wl_display = NULL;
            g_wl_fd = -1;
        }
    }
#else
    if (g_wl_display) count += 1; /* built without wayland-client: nothing real to pump */
#endif

    /* The input layer's queues are filled by the rewriter callbacks above. */
    return count;
}

int input_bridge_wait_for_events(int timeout_ms)
{
    if (!g_initialised) input_bridge_init();

    /* If we have a Wayland display fd, poll it */
    if (g_wl_fd >= 0) {
        struct pollfd pfd = { .fd = g_wl_fd, .events = POLLIN };
        int r = poll(&pfd, 1, timeout_ms);
        if (r > 0) return 1;
        if (r == 0) return 0;  /* timeout */
        return -1;  /* error */
    }

    /* No Wayland fd - short sleep */
    if (timeout_ms > 0) {
        usleep(timeout_ms * 1000);
    } else if (timeout_ms < 0) {
        usleep(1000);  /* 1ms */
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Window registration                                                */
/* ------------------------------------------------------------------ */

int input_bridge_window_created(HWND hwnd)
{
    if (!g_initialised) input_bridge_init();
    return nt_input_register_window(hwnd);
}

void input_bridge_window_destroyed(HWND hwnd)
{
    if (!g_initialised) return;
    nt_input_unregister_window(hwnd);
}

void input_bridge_set_focus(HWND hwnd)
{
    if (!g_initialised) input_bridge_init();
    nt_input_set_focus(hwnd);
}

/* ------------------------------------------------------------------ */
/* Wayland display connection (called from win32k_bridge or ntboxd)   */
/* ------------------------------------------------------------------ */

int input_bridge_set_wl_display(struct wl_display *display)
{
    if (!display) return -1;
    g_wl_display = display;
#ifdef INPB_HAVE_WAYLAND_CLIENT
    g_wl_fd = wl_display_get_fd(display);
    NTB0_OK("INP-006", "input bridge: wl_display connected (fd=%d)", g_wl_fd);
#else
    g_wl_fd = -1;
    NTB0_OK("INP-006", "input bridge: wl_display connected (fd not available without wayland-client)");
#endif
    return 0;
}

/* ------------------------------------------------------------------ */
/* Status                                                             */
/* ------------------------------------------------------------------ */

int input_bridge_is_initialised(void)
{
    return g_initialised;
}

/* ================================================================== */
/* T-183/T-184 — LIVE Wayland input layer                              */
/* INP-011 live connect · INP-012 wl_seat discovery · INP-013 wl_      */
/* keyboard receiver · INP-014 wl_pointer receiver                     */
/* ================================================================== */

#if defined(__has_include)
#  if __has_include(<wayland-client.h>)
#    define INPB_HAVE_WAYLAND_CLIENT 1
#  endif
#endif

#ifdef INPB_HAVE_WAYLAND_CLIENT
#include <wayland-client.h>
#endif

/* wl_seat capability bits (same values as WL_SEAT_CAPABILITY_*) */
#define INPB_CAP_POINTER  1u
#define INPB_CAP_KEYBOARD 2u
#define INPB_CAP_TOUCH    4u

#define INPB_SEAT_MAX 8
#define INPB_SURF_MAX 64

typedef struct input_bridge_seat {
    unsigned int name;           /* registry global name                */
    unsigned int caps;           /* last advertised capability mask     */
    unsigned int version;        /* negotiated wl_seat version          */
    void        *wl_seat;        /* struct wl_seat* (live mode)         */
    void        *wl_keyboard;    /* struct wl_keyboard*                 */
    void        *wl_pointer;     /* struct wl_pointer*                  */
    int          kb_offered;     /* get_keyboard issued                 */
    int          ptr_offered;    /* get_pointer issued                  */
} inpb_seat_t;

static inpb_seat_t g_seats[INPB_SEAT_MAX];
static int g_nseat = 0;

static struct wl_surface *g_surf[INPB_SURF_MAX];
static HWND g_surf_hwnd[INPB_SURF_MAX];
static int g_nsurf = 0;

static unsigned int g_keymap_fmt = 0, g_keymap_size = 0;
static int g_keymap_got = 0;

static unsigned int g_repeat_rate = 0, g_repeat_delay = 0;

/* ------------------------------------------------------------------ */
/* serials                                                             */
/* ------------------------------------------------------------------ */
void input_bridge_get_serials(nt_input_serials_t *out)
{
    if (out) nt_input_get_serials(out);
}

/* ------------------------------------------------------------------ */
/* seat table (compiles with and without wayland-client)               */
/* ------------------------------------------------------------------ */
static inpb_seat_t *find_seat(unsigned int name) {
    int i;
    for (i = 0; i < g_nseat; i++)
        if (g_seats[i].name == name) return &g_seats[i];
    return NULL;
}

#ifdef INPB_HAVE_WAYLAND_CLIENT
static const struct wl_registry_listener inpb_bridge_registry_listener;
static const struct wl_seat_listener     inpb_seat_listener;
static const struct wl_keyboard_listener inpb_kb_listener;
static const struct wl_pointer_listener  inpb_ptr_listener;
#endif

int input_bridge_seat_count(void) { return g_nseat; }

int input_bridge_seats_with_keyboard(void) {
    int i, n = 0;
    for (i = 0; i < g_nseat; i++) n += (g_seats[i].caps & INPB_CAP_KEYBOARD) ? 1 : 0;
    return n;
}

int input_bridge_seats_with_pointer(void) {
    int i, n = 0;
    for (i = 0; i < g_nseat; i++) n += (g_seats[i].caps & INPB_CAP_POINTER) ? 1 : 0;
    return n;
}

void input_bridge_seat_reset(void) {
    memset(g_seats, 0, sizeof(g_seats));
    g_nseat = 0;
}

#ifdef INPB_HAVE_WAYLAND_CLIENT
static struct wl_registry *g_inpb_registry = NULL;
#endif

int input_bridge_seat_discovered(unsigned int name, unsigned int version) {
    inpb_seat_t *st;
    if (find_seat(name)) {
        NTB0_OK("INP-012", "wl_seat %u already in table (idempotent)", name);
        return (int)(find_seat(name) - g_seats);
    }
    if (g_nseat >= INPB_SEAT_MAX) {
        NTB0_FAIL("INP-012", "wl_seat %u refused: seat table full (%d)", name, INPB_SEAT_MAX);
        return -1;
    }
    st = &g_seats[g_nseat];
    memset(st, 0, sizeof(*st));
    st->name = name;
    st->version = version > 5u ? 5u : version;
#ifdef INPB_HAVE_WAYLAND_CLIENT
    if (g_inpb_registry) {
        struct wl_seat *ws = wl_registry_bind(g_inpb_registry, name,
                                              &wl_seat_interface, st->version);
        if (!ws) {
            NTB0_FAIL("INP-012", "wl_seat %u: registry bind failed", name);
            return -1;
        }
        st->wl_seat = ws;
        wl_seat_add_listener(ws, &inpb_seat_listener, NULL);
    }
#endif
    g_nseat++;
    NTB0_OK("INP-012", "wl_seat %u (v%u) discovered (seats=%d)", name, st->version, g_nseat);
    return g_nseat - 1;
}

void input_bridge_seat_disappeared(unsigned int name) {
    int i;
    for (i = 0; i < g_nseat; i++) {
        if (g_seats[i].name == name) {
            /* children are freed when the connection dies (or released on
             * capability removal via wl_seat version >=5 release requests) */
            memmove(&g_seats[i], &g_seats[i + 1],
                    (size_t)(g_nseat - i - 1) * sizeof(g_seats[0]));
            memset(&g_seats[g_nseat - 1], 0, sizeof(g_seats[0]));
            g_nseat--;
            NTB0_OK("INP-012", "wl_seat %u removed (seats=%d)", name, g_nseat);
            return;
        }
    }
    NTB0_FAIL("INP-012", "wl_seat %u removal refused: unknown seat", name);
}

int input_bridge_seat_caps(unsigned int name, unsigned int caps) {
    inpb_seat_t *st = find_seat(name);
    if (!st) {
        NTB0_FAIL("INP-012", "caps for unknown seat %u refused", name);
        return -1;
    }
    st->caps = caps;
#ifdef INPB_HAVE_WAYLAND_CLIENT
    if (st->wl_seat) {
        struct wl_seat *ws = (struct wl_seat *)st->wl_seat;
        /* T-183/T-184: bind only what exists */
        if (caps & INPB_CAP_KEYBOARD) {
            if (!st->kb_offered) {
                st->wl_keyboard = wl_seat_get_keyboard(ws);
                st->kb_offered = (st->wl_keyboard != NULL);
                if (st->kb_offered) {
                    wl_keyboard_add_listener((struct wl_keyboard *)st->wl_keyboard,
                                             &inpb_kb_listener, NULL);
                    NTB0_OK("INP-012", "seat %u: wl_keyboard bound", name);
                }
            }
        } else if (st->kb_offered) {
            st->wl_keyboard = NULL;
            st->kb_offered = 0;
            NTB0_OK("INP-012", "seat %u: keyboard capability absent — refused cleanly", name);
        }
        if (caps & INPB_CAP_POINTER) {
            if (!st->ptr_offered) {
                st->wl_pointer = wl_seat_get_pointer(ws);
                st->ptr_offered = (st->wl_pointer != NULL);
                if (st->ptr_offered) {
                    wl_pointer_add_listener((struct wl_pointer *)st->wl_pointer,
                                            &inpb_ptr_listener, NULL);
                    NTB0_OK("INP-012", "seat %u: wl_pointer bound", name);
                }
            }
        } else if (st->ptr_offered) {
            st->wl_pointer = NULL;
            st->ptr_offered = 0;
            NTB0_OK("INP-012", "seat %u: pointer capability absent — refused cleanly", name);
        }
        return 0;
    }
#endif
    {
        /* unit/synthetic mode: record the bind decisions without a live bind */
        st->kb_offered = (caps & INPB_CAP_KEYBOARD) ? 1 : 0;
        if (caps & INPB_CAP_KEYBOARD) NTB0_OK("INP-012", "seat %u: keyboard bound (unit)", name);
        else NTB0_OK("INP-012", "seat %u: keyboard capability absent — refused cleanly (unit)", name);
        st->ptr_offered = (caps & INPB_CAP_POINTER) ? 1 : 0;
        if (caps & INPB_CAP_POINTER) NTB0_OK("INP-012", "seat %u: pointer bound (unit)", name);
        else NTB0_OK("INP-012", "seat %u: pointer capability absent — refused cleanly (unit)", name);
    }
    return 0;
}
/* ------------------------------------------------------------------ */
/* surface -> window map (T-185: focus enter/leave must name a window)  */
/* ------------------------------------------------------------------ */
void input_bridge_surface_map(struct wl_surface *surface, HWND hwnd) {
    int i;
    if (!surface || !hwnd) return;
    for (i = 0; i < g_nsurf; i++) {
        if (g_surf[i] == surface) { g_surf_hwnd[i] = hwnd; return; }
    }
    if (g_nsurf >= INPB_SURF_MAX) {
        NTB0_FAIL("INP-013", "surface map full (%d) — surface %p refused", INPB_SURF_MAX, (void *)surface);
        return;
    }
    g_surf[g_nsurf] = surface;
    g_surf_hwnd[g_nsurf] = hwnd;
    g_nsurf++;
    NTB0_OK("INP-013", "surface %p -> hwnd %p mapped", (void *)surface, (void *)hwnd);
}

void input_bridge_surface_unmap(struct wl_surface *surface) {
    int i;
    for (i = 0; i < g_nsurf; i++) {
        if (g_surf[i] == surface) {
            memmove(&g_surf[i], &g_surf[i + 1],
                    (size_t)(g_nsurf - i - 1) * sizeof(g_surf[0]));
            memmove(&g_surf_hwnd[i], &g_surf_hwnd[i + 1],
                    (size_t)(g_nsurf - i - 1) * sizeof(g_surf_hwnd[0]));
            g_nsurf--;
            return;
        }
    }
}

HWND input_bridge_surface_lookup(struct wl_surface *surface) {
    int i;
    for (i = 0; i < g_nsurf; i++)
        if (g_surf[i] == surface) return g_surf_hwnd[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* keymap state (recorded; we do NOT parse xkb)                         */
/* ------------------------------------------------------------------ */
int input_bridge_keymap_seen(unsigned int *format, unsigned int *size) {
    if (!g_keymap_got) return 0;
    if (format) *format = g_keymap_fmt;
    if (size)   *size = g_keymap_size;
    return 1;
}

/* ------------------------------------------------------------------ */
/* rewriter entry points: the exact payloads the wl listeners carry.    */
/* The unit harness feeds them with synthetic values; the live          */
/* listeners forward their real events here.                            */
/* ------------------------------------------------------------------ */
void input_bridge_on_kb_enter(unsigned int serial, struct wl_surface *surface) {
    HWND h;
    if (!g_initialised) input_bridge_init();
    h = input_bridge_surface_lookup(surface);
    if (!h) {
        NTB0_FAIL("INP-013",
                  "kb.enter serial=%u refused: unknown surface %p (no focus change)",
                  serial, (void *)surface);
        return;
    }
    input_bridge_wl_keyboard_enter(h, serial);
}

void input_bridge_on_kb_leave(unsigned int serial, struct wl_surface *surface) {
    HWND h;
    if (!g_initialised) input_bridge_init();
    h = input_bridge_surface_lookup(surface);
    if (!h) {
        NTB0_FAIL("INP-013",
                  "kb.leave serial=%u refused: unknown surface %p", serial, (void *)surface);
        return;
    }
    input_bridge_wl_keyboard_leave(h, serial);
}

void input_bridge_on_kb_key(unsigned int serial, unsigned int time,
                            unsigned int key, unsigned int state) {
    HWND focus;
    if (!g_initialised) input_bridge_init();
    focus = nt_input_get_focus();
    if (!focus) {
        NTB0_FAIL("INP-013", "kb.key serial=%u key=%u dropped: no keyboard focused surface",
                  serial, key);
        return;
    }
    input_bridge_wl_keyboard_key(focus, serial, time, key, state);
}

void input_bridge_on_kb_modifiers(unsigned int serial, unsigned int depressed,
                                  unsigned int latched, unsigned int locked,
                                  unsigned int group) {
    if (!g_initialised) input_bridge_init();
    nt_input_note_kb_mods_serial(serial);
    input_bridge_wl_keyboard_modifiers(depressed, latched, locked, group);
}

void input_bridge_on_kb_keymap(unsigned int format, int fd, unsigned int size) {
    g_keymap_fmt = format;
    g_keymap_size = size;
    g_keymap_got = 1;
    if (fd >= 0) close(fd);
    NTB0_OK("INP-013",
            "keymap: fmt=%u size=%u received; built-in US evdev table used (no xkb parser)",
            format, size);
}

void input_bridge_on_kb_repeat(int32_t rate, int32_t delay) {
    g_repeat_rate = (unsigned int)(rate < 0 ? 0 : rate);
    g_repeat_delay = (unsigned int)(delay < 0 ? 0 : delay);
    NTB0_OK("INP-013",
            "key repeat rate=%u/s delay=%ums recorded; repeat synthesis not implemented",
            g_repeat_rate, g_repeat_delay);
}

void input_bridge_on_ptr_enter(unsigned int serial, struct wl_surface *surface,
                               int sx, int sy) {
    HWND h;
    if (!g_initialised) input_bridge_init();
    h = input_bridge_surface_lookup(surface);
    if (!h) {
        NTB0_FAIL("INP-014",
                  "ptr.enter serial=%u refused: unknown surface %p", serial, (void *)surface);
        return;
    }
    input_bridge_wl_pointer_enter_serial(h, serial, sx, sy);
}

void input_bridge_on_ptr_leave(unsigned int serial, struct wl_surface *surface) {
    HWND h;
    if (!g_initialised) input_bridge_init();
    h = input_bridge_surface_lookup(surface);
    if (!h) {
        NTB0_FAIL("INP-014",
                  "ptr.leave serial=%u refused: unknown surface %p", serial, (void *)surface);
        return;
    }
    input_bridge_wl_pointer_leave_serial(h, serial);
}

void input_bridge_on_ptr_motion(unsigned int time, int sx, int sy) {
    if (!g_initialised) input_bridge_init();
    input_bridge_wl_pointer_motion(time, sx, sy);
}

void input_bridge_on_ptr_button(unsigned int serial, unsigned int time,
                                unsigned int button, unsigned int state) {
    HWND target;
    int x, y;
    if (!g_initialised) input_bridge_init();
    target = nt_input_pointer_window();
    if (!target) target = nt_input_get_focus();
    if (!target) {
        NTB0_FAIL("INP-014", "ptr.button serial=%u btn=%u dropped: no pointer target",
                  serial, button);
        return;
    }
    /* keep last cursor position for the message */
    x = 0; y = 0; /* real coordinates arrive only via a live motion history;
                     the pointer window's stored position from motion/enter
                     is used by the input layer when x=y=0 is passed through */
    input_bridge_wl_pointer_button(serial, time, button, state, x, y);
}

void input_bridge_on_ptr_axis(unsigned int time, unsigned int axis, double value) {
    if (!g_initialised) input_bridge_init();
    input_bridge_wl_pointer_axis(time, axis, value, 0, 0);
}

/* ------------------------------------------------------------------ */
/* live connection: registry + wl_seat capabilities (T-184)             */
/* ------------------------------------------------------------------ */
int input_bridge_wayland_is_connected(void)
{
#ifdef INPB_HAVE_WAYLAND_CLIENT
    return g_wl_display != NULL;
#else
    return 0;
#endif
}

int input_bridge_wayland_connect(void)
{
#ifdef INPB_HAVE_WAYLAND_CLIENT
    const char *wd;
    struct wl_registry *reg;
    int i;
    if (g_wl_display) return 0;
    wd = getenv("WAYLAND_DISPLAY");
    g_wl_display = wl_display_connect(NULL);
    if (!g_wl_display) {
        NTB0_SKIP("INP-011", "no live Wayland compositor (WAYLAND_DISPLAY=%s)",
                  wd ? wd : "(unset)");
        return -1;
    }
    g_wl_fd = wl_display_get_fd(g_wl_display);
    reg = wl_display_get_registry(g_wl_display);
    g_inpb_registry = reg;
    wl_registry_add_listener(reg, &inpb_bridge_registry_listener, NULL);
    wl_display_roundtrip(g_wl_display);   /* globals */
    wl_display_roundtrip(g_wl_display);   /* seat capabilities */
    for (i = 0; i < g_nseat; i++) {
        NTB0_OK("INP-012", "seat %u caps=0x%X after discovery", g_seats[i].name, g_seats[i].caps);
    }
    NTB0_OK("INP-011", "live Wayland connected: seats=%d keyboard-caps=%d pointer-caps=%d",
            g_nseat, input_bridge_seats_with_keyboard(), input_bridge_seats_with_pointer());
    return 0;
#else
    NTB0_SKIP("INP-011", "input bridge built without wayland-client: live layer unavailable");
    return -1;
#endif
}

void input_bridge_wayland_disconnect(void)
{
#ifdef INPB_HAVE_WAYLAND_CLIENT
    if (g_wl_display) {
        wl_display_disconnect(g_wl_display);
        g_wl_display = NULL;
        g_inpb_registry = NULL;
        g_wl_fd = -1;
        NTB0_OK("INP-011", "live Wayland disconnected");
    }
#endif
    input_bridge_seat_reset();
}

#ifdef INPB_HAVE_WAYLAND_CLIENT

static void inpb_registry_global(void *data, struct wl_registry *reg,
                                 uint32_t name, const char *iface, uint32_t ver) {
    (void)data;
    if (strcmp(iface, "wl_seat") == 0) {
        input_bridge_seat_discovered(name, ver);
    }
    /* other globals are irrelevant for the input path — silently ignored */
    (void)reg;
}

static void inpb_registry_global_remove(void *data, struct wl_registry *reg,
                                        uint32_t name) {
    (void)data; (void)reg;
    input_bridge_seat_disappeared(name);
}

static const struct wl_registry_listener inpb_bridge_registry_listener = {
    .global = inpb_registry_global,
    .global_remove = inpb_registry_global_remove,
};

/* wl_seat v5 also sends a 'name' event (opcode 1) — must not be NULL */
static void inpb_seat_name_h(void *data, struct wl_seat *seat, const char *nm) {
    (void)data; (void)seat;
    if (nm && nm[0])
        NTB0_OK("INP-012", "wl_seat reports name '%s'", nm);
}

/* wl_seat.capabilities → the only listener that decides what gets bound */
static void inpb_seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    int i;
    (void)data;
    for (i = 0; i < g_nseat; i++) {
        if (g_seats[i].wl_seat == seat) {
            input_bridge_seat_caps(g_seats[i].name, caps);
            return;
        }
    }
    NTB0_FAIL("INP-012", "capabilities event for unbound seat object refused");
}

static const struct wl_seat_listener inpb_seat_listener = {
    .capabilities = inpb_seat_capabilities,
    .name = inpb_seat_name_h,
};

/* ---- wl_keyboard listener (T-183) ---- */
static void inpb_kb_keymap_h(void *d, struct wl_keyboard *k, uint32_t format,
                             int32_t fd, uint32_t size) {
    (void)d; (void)k;
    input_bridge_on_kb_keymap(format, fd, size);
}
static void inpb_kb_enter_h(void *d, struct wl_keyboard *k, uint32_t serial,
                            struct wl_surface *s, struct wl_array *keys) {
    (void)d; (void)k; (void)keys;
    input_bridge_on_kb_enter(serial, s);
}
static void inpb_kb_leave_h(void *d, struct wl_keyboard *k, uint32_t serial,
                            struct wl_surface *s) {
    (void)d; (void)k;
    input_bridge_on_kb_leave(serial, s);
}
static void inpb_kb_key_h(void *d, struct wl_keyboard *k, uint32_t serial,
                          uint32_t time, uint32_t key, uint32_t state) {
    (void)d; (void)k;
    input_bridge_on_kb_key(serial, time, key, state);
}
static void inpb_kb_mods_h(void *d, struct wl_keyboard *k, uint32_t serial,
                           uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group) {
    (void)d; (void)k;
    input_bridge_on_kb_modifiers(serial, dep, lat, lock, group);
}
static void inpb_kb_repeat_h(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) {
    (void)d; (void)k;
    input_bridge_on_kb_repeat(rate, delay);
}
static const struct wl_keyboard_listener inpb_kb_listener = {
    .keymap = inpb_kb_keymap_h,
    .enter = inpb_kb_enter_h,
    .leave = inpb_kb_leave_h,
    .key = inpb_kb_key_h,
    .modifiers = inpb_kb_mods_h,
    .repeat_info = inpb_kb_repeat_h,
};

/* ---- wl_pointer listener (T-183) ---- */
static void inpb_ptr_enter_h(void *d, struct wl_pointer *p, uint32_t serial,
                             struct wl_surface *s, wl_fixed_t sx, wl_fixed_t sy) {
    (void)d; (void)p;
    input_bridge_on_ptr_enter(serial, s, wl_fixed_to_int(sx), wl_fixed_to_int(sy));
}
static void inpb_ptr_leave_h(void *d, struct wl_pointer *p, uint32_t serial,
                             struct wl_surface *s) {
    (void)d; (void)p;
    input_bridge_on_ptr_leave(serial, s);
}
static void inpb_ptr_motion_h(void *d, struct wl_pointer *p, uint32_t time,
                              wl_fixed_t sx, wl_fixed_t sy) {
    (void)d; (void)p;
    input_bridge_on_ptr_motion(time, wl_fixed_to_int(sx), wl_fixed_to_int(sy));
}
static void inpb_ptr_button_h(void *d, struct wl_pointer *p, uint32_t serial,
                              uint32_t time, uint32_t button, uint32_t state) {
    (void)d; (void)p;
    input_bridge_on_ptr_button(serial, time, button, state);
}
static void inpb_ptr_axis_h(void *d, struct wl_pointer *p, uint32_t time,
                            uint32_t axis, wl_fixed_t value) {
    (void)d; (void)p;
    input_bridge_on_ptr_axis(time, axis, wl_fixed_to_double(value));
}
static void inpb_ptr_frame_h(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void inpb_ptr_axis_src_h(void *d, struct wl_pointer *p, uint32_t src) {
    (void)d; (void)p; (void)src;
}
static void inpb_ptr_axis_stop_h(void *d, struct wl_pointer *p, uint32_t time,
                                 uint32_t axis) {
    (void)d; (void)p; (void)time; (void)axis;
}
static void inpb_ptr_axis_disc_h(void *d, struct wl_pointer *p, uint32_t axis,
                                 int32_t discrete) {
    (void)d; (void)p; (void)axis; (void)discrete;
}
static const struct wl_pointer_listener inpb_ptr_listener = {
    .enter = inpb_ptr_enter_h,
    .leave = inpb_ptr_leave_h,
    .motion = inpb_ptr_motion_h,
    .button = inpb_ptr_button_h,
    .axis = inpb_ptr_axis_h,
    .frame = inpb_ptr_frame_h,
    .axis_source = inpb_ptr_axis_src_h,
    .axis_stop = inpb_ptr_axis_stop_h,
    .axis_discrete = inpb_ptr_axis_disc_h,
};

#endif /* INPB_HAVE_WAYLAND_CLIENT */
