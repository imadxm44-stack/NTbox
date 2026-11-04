/*
 * input_bridge.h — NT-Box: bridges Wayland input events to win32k message queue.
 * Lamps INP-006..INP-010.
 */
#ifndef NTBOX_INPUT_BRIDGE_H
#define NTBOX_INPUT_BRIDGE_H

#include "win32k.h"
#include "input.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Bridge lifecycle                                                   */
/* ------------------------------------------------------------------ */

/* Initialise the input bridge.  This sets up the connection between
 * Wayland input (wl_keyboard, wl_pointer, wl_touch) and the win32k
 * message queues.  Returns 0 on success. */
int input_bridge_init(void);

/* Shutdown the input bridge. */
void input_bridge_shutdown(void);

/* ------------------------------------------------------------------ */
/* Wayland seat callbacks (called from the Wayland event loop)        */
/* ------------------------------------------------------------------ */

/* Keyboard focus enters a surface.  hwnd is the win32k window handle
 * associated with the surface.  serial is the Wayland serial. */
void input_bridge_wl_keyboard_enter(HWND hwnd, unsigned int serial);

/* Keyboard focus leaves a surface. */
void input_bridge_wl_keyboard_leave(HWND hwnd, unsigned int serial);

/* Keyboard key event.  key is the Linux evdev keycode or a VK code.
 * state: 1 = pressed, 0 = released. */
void input_bridge_wl_keyboard_key(HWND hwnd, unsigned int serial,
                                  unsigned int time, unsigned int key,
                                  unsigned int state);

/* Keyboard modifiers changed. */
void input_bridge_wl_keyboard_modifiers(unsigned int mods_depressed,
                                        unsigned int mods_latched,
                                        unsigned int mods_locked,
                                        unsigned int group);

/* Pointer enters a surface. */
void input_bridge_wl_pointer_enter(HWND hwnd, int x, int y);

/* Pointer enters a surface, carrying the real wl serial (T-183). */
void input_bridge_wl_pointer_enter_serial(HWND hwnd, unsigned int serial, int x, int y);

/* Pointer leaves a surface. */
void input_bridge_wl_pointer_leave(HWND hwnd);
void input_bridge_wl_pointer_leave_serial(HWND hwnd, unsigned int serial);

/* Pointer motion (absolute coordinates in surface). */
void input_bridge_wl_pointer_motion(unsigned int time, int x, int y);

/* Pointer button event.  button is a Linux evdev button code (BTN_LEFT etc).
 * state: 1 = pressed, 0 = released. */
void input_bridge_wl_pointer_button(unsigned int serial, unsigned int time,
                                    unsigned int button, unsigned int state,
                                    int x, int y);

/* Pointer axis (scroll). */
void input_bridge_wl_pointer_axis(unsigned int time, unsigned int axis,
                                  double value, int x, int y);

/* Touch down/up/motion - optional, for future. */
void input_bridge_wl_touch_down(unsigned int serial, unsigned int time,
                                HWND hwnd, int id, int x, int y);
void input_bridge_wl_touch_up(unsigned int serial, unsigned int time,
                              int id);
void input_bridge_wl_touch_motion(unsigned int time, int id, int x, int y);

/* ------------------------------------------------------------------ */
/* win32k message queue integration                                   */
/* ------------------------------------------------------------------ */

/* Post a message directly to the win32k global message queue.
 * This is used by the input bridge to inject translated messages. */
int input_bridge_post_to_win32k(HWND hwnd, UINT message,
                                unsigned long wParam, long lParam,
                                int pt_x, int pt_y);

/* Peek a message from the win32k queue, also checking the input bridge
 * queue.  Mirrors NtUserPeekMessage semantics. */
int input_bridge_peek_message(win32k_msg_t *msg, HWND hwnd,
                              UINT min, UINT max, UINT remove);

/* Get a message from the win32k queue, blocking if empty.
 * Mirrors NtUserGetMessage semantics. */
int input_bridge_get_message(win32k_msg_t *msg, HWND hwnd,
                             UINT min, UINT max);

/* Translate virtual-key messages to character messages.
 * Mirrors NtUserTranslateMessage semantics. */
int input_bridge_translate_message(const win32k_msg_t *msg);

/* Dispatch a message to the window procedure.
 * Mirrors NtUserDispatchMessage semantics. */
unsigned long input_bridge_dispatch_message(const win32k_msg_t *msg);

/* Process pending Wayland events and translate them to win32k messages.
 * Call this from the application's message loop (e.g. in GetMessage
 * when the queue is empty).  Returns number of events processed. */
int input_bridge_pump_events(void);

/* Wait for input events (Wayland fd) efficiently.  Call this when
 * GetMessage would block.  Returns when events are available or
 * timeout_ms expires (timeout_ms < 0 = wait forever). */
int input_bridge_wait_for_events(int timeout_ms);

/* ------------------------------------------------------------------ */
/* Window registration (sync with win32k)                             */
/* ------------------------------------------------------------------ */

/* Called when a win32k window is created.  Registers it with the
 * input system so it can receive messages. */
int input_bridge_window_created(HWND hwnd);

/* Called when a win32k window is destroyed. */
void input_bridge_window_destroyed(HWND hwnd);

/* Called when a window gains/loses focus via win32k (SetFocus etc). */
void input_bridge_set_focus(HWND hwnd);

/* ------------------------------------------------------------------ */
/* Wayland display connection (called from win32k_bridge or ntboxd)   */
/* ------------------------------------------------------------------ */

/* Set the Wayland display for the input bridge to use for event polling.
 * Forward declaration of wl_display to avoid including wayland-client.h. */
struct wl_display;
int input_bridge_set_wl_display(struct wl_display *display);

/* ------------------------------------------------------------------ */
/* LIVE Wayland input (T-183/T-184) — INP-011..INP-016                 */
/* ------------------------------------------------------------------ */

/* Serial bookkeeping (mirrors nt_input_serials_t from input.h). */
void input_bridge_get_serials(nt_input_serials_t *out);

/* Own Wayland connection + wl_seat discovery via the registry.
 * Returns 0 when connected (possibly zero seats — they are "bound only
 * what exists", never invented).  Returns -1 when there is no compositor. */
int  input_bridge_wayland_connect(void);
void input_bridge_wayland_disconnect(void);
int  input_bridge_wayland_is_connected(void);

/* wl_seat table (usable both for the live registry and for the unit
 * harness that feeds synthetic seats).  "apply caps" binds get_keyboard /
 * get_pointer per capability bit and refuses cleanly when a capability is
 * absent — nothing is faked. */
int  input_bridge_seat_discovered(unsigned int name, unsigned int version);
void input_bridge_seat_disappeared(unsigned int name);
int  input_bridge_seat_caps(unsigned int name, unsigned int caps);
int  input_bridge_seat_count(void);
int  input_bridge_seats_with_keyboard(void);
int  input_bridge_seats_with_pointer(void);
void input_bridge_seat_reset(void);

/* Surface -> window mapping.  The live listeners resolve surfaces to
 * win32k windows through this map; unknown surfaces are refused honestly. */
struct wl_surface;
void input_bridge_surface_map(struct wl_surface *surface, HWND hwnd);
void input_bridge_surface_unmap(struct wl_surface *surface);
HWND input_bridge_surface_lookup(struct wl_surface *surface);

/* Raw rewriter entry points — the exact payloads the wl_keyboard /
 * wl_pointer listeners carry.  The unit harness feeds these with synthetic
 * values; the live listeners forward their real events here. */
void input_bridge_on_kb_enter(unsigned int serial, struct wl_surface *surface);
void input_bridge_on_kb_leave(unsigned int serial, struct wl_surface *surface);
void input_bridge_on_kb_key(unsigned int serial, unsigned int time,
                            unsigned int key, unsigned int state);
void input_bridge_on_kb_modifiers(unsigned int serial, unsigned int depressed,
                                  unsigned int latched, unsigned int locked,
                                  unsigned int group);
/* wl_keyboard.keymap(format, fd, size): recorded, fd closed.  We do NOT
 * parse xkb — the built-in US evdev table is used and stated honestly. */
void input_bridge_on_kb_keymap(unsigned int format, int fd, unsigned int size);
void input_bridge_on_kb_repeat(int32_t rate, int32_t delay);

void input_bridge_on_ptr_enter(unsigned int serial, struct wl_surface *surface,
                               int sx, int sy);
void input_bridge_on_ptr_leave(unsigned int serial, struct wl_surface *surface);
void input_bridge_on_ptr_motion(unsigned int time, int sx, int sy);
void input_bridge_on_ptr_button(unsigned int serial, unsigned int time,
                                unsigned int button, unsigned int state);
void input_bridge_on_ptr_axis(unsigned int time, unsigned int axis,
                              double value);

/* keymap advertisement state (set by wl_keyboard.keymap) */
int  input_bridge_keymap_seen(unsigned int *format, unsigned int *size);

/* ------------------------------------------------------------------ */
/* Status                                                             */
/* ------------------------------------------------------------------ */

int input_bridge_is_initialised(void);

#ifdef __cplusplus
}
#endif

#endif /* NTBOX_INPUT_BRIDGE_H */