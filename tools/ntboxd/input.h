/*
 * input.h — NT-Box input layer (W8, T-181..T-185).
 *
 * Translates Wayland input events (wl_keyboard / wl_pointer on a wl_seat)
 * into Win32 messages and delivers them into per-window message queues.
 *
 * The layer keeps its OWN message queues (one per registered window).  The
 * win32k module keeps a separate global queue; both coexist — the input
 * layer is the producer, the application reads through nt_input_* (or, if
 * it prefers, through the win32k PeekMessage/GetMessage helpers on the
 * messages that were also mirrored there).
 */
#ifndef NTBOX_INPUT_H
#define NTBOX_INPUT_H

#include "win32k.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Win32 message ids that win32k.h does not (yet) declare              */
/* ------------------------------------------------------------------ */
#ifndef WM_KEYDOWN
#define WM_KEYDOWN        0x0100u
#endif
#ifndef WM_KEYUP
#define WM_KEYUP          0x0101u
#endif
#ifndef WM_CHAR
#define WM_CHAR           0x0102u
#endif
#ifndef WM_SYSKEYDOWN
#define WM_SYSKEYDOWN     0x0104u
#endif
#ifndef WM_SYSKEYUP
#define WM_SYSKEYUP       0x0105u
#endif
#ifndef WM_MOUSEMOVE
#define WM_MOUSEMOVE      0x0200u
#endif
#ifndef WM_LBUTTONDOWN
#define WM_LBUTTONDOWN    0x0201u
#endif
#ifndef WM_LBUTTONUP
#define WM_LBUTTONUP      0x0202u
#endif
#ifndef WM_LBUTTONDBLCLK
#define WM_LBUTTONDBLCLK  0x0203u
#endif
#ifndef WM_RBUTTONDOWN
#define WM_RBUTTONDOWN    0x0204u
#endif
#ifndef WM_RBUTTONUP
#define WM_RBUTTONUP      0x0205u
#endif
#ifndef WM_RBUTTONDBLCLK
#define WM_RBUTTONDBLCLK  0x0206u
#endif
#ifndef WM_MBUTTONDOWN
#define WM_MBUTTONDOWN    0x0207u
#endif
#ifndef WM_MBUTTONUP
#define WM_MBUTTONUP      0x0208u
#endif
#ifndef WM_MOUSEWHEEL
#define WM_MOUSEWHEEL     0x020Au
#endif
#ifndef WM_MOUSEHWHEEL
#define WM_MOUSEHWHEEL    0x020Eu
#endif
#ifndef WM_SETFOCUS
#define WM_SETFOCUS       0x0007u
#endif
#ifndef WM_KILLFOCUS
#define WM_KILLFOCUS      0x0008u
#endif
#ifndef WM_ACTIVATE
#define WM_ACTIVATE       0x0006u
#endif

/* mouse button state flags (wParam of mouse messages) */
#define MK_LBUTTON   0x0001u
#define MK_RBUTTON   0x0002u
#define MK_SHIFT     0x0004u
#define MK_CONTROL   0x0008u
#define MK_MBUTTON   0x0010u
#define MK_XBUTTON1  0x0020u
#define MK_XBUTTON2  0x0040u

/* Wayland button codes (evdev) */
#define BTN_LEFT     272u
#define BTN_RIGHT    273u
#define BTN_MIDDLE   274u
#define BTN_SIDE     275u
#define BTN_EXTRA    276u

/* Wayland key state (only when real wayland-client.h is not in scope —
 * it provides the same values as enum constants) */
#ifndef NTB_REAL_WAYLAND_CLIENT
#define WL_KEYBOARD_KEY_STATE_RELEASED 0u
#define WL_KEYBOARD_KEY_STATE_PRESSED  1u

/* Wayland pointer button state */
#define WL_POINTER_BUTTON_STATE_RELEASED 0u
#define WL_POINTER_BUTTON_STATE_PRESSED  1u

/* Wayland pointer axis */
#define WL_POINTER_AXIS_VERTICAL_SCROLL   0u
#define WL_POINTER_AXIS_HORIZONTAL_SCROLL 1u
#endif

/* virtual key codes (subset) */
#define VK_BACK      0x08u
#define VK_TAB       0x09u
#define VK_RETURN    0x0Du
#define VK_SHIFT     0x10u
#define VK_CONTROL   0x11u
#define VK_MENU      0x12u
#define VK_CAPITAL   0x14u
#define VK_ESCAPE    0x1Bu
#define VK_SPACE     0x20u
#define VK_LEFT      0x25u
#define VK_UP        0x26u
#define VK_RIGHT     0x27u
#define VK_DOWN      0x28u
#define VK_DELETE    0x2Eu
#define VK_LSHIFT    0xA0u
#define VK_RSHIFT    0xA1u
#define VK_LCONTROL  0xA2u
#define VK_RCONTROL  0xA3u
#define VK_LMENU     0xA4u
#define VK_RMENU     0xA5u
#define VK_PRIOR     0x21u
#define VK_NEXT      0x22u
#define VK_END       0x23u
#define VK_HOME      0x24u
#define VK_INSERT    0x2Du
#define VK_SNAPSHOT  0x2Cu
#define VK_PAUSE     0x13u
#define VK_LWIN      0x5Bu
#define VK_RWIN      0x5Cu
#define VK_APPS      0x5Du
#define VK_NUMLOCK   0x90u
#define VK_SCROLL    0x91u
#define VK_NUMPAD0   0x60u
#define VK_NUMPAD1   0x61u
#define VK_NUMPAD2   0x62u
#define VK_NUMPAD3   0x63u
#define VK_NUMPAD4   0x64u
#define VK_NUMPAD5   0x65u
#define VK_NUMPAD6   0x66u
#define VK_NUMPAD7   0x67u
#define VK_NUMPAD8   0x68u
#define VK_NUMPAD9   0x69u
#define VK_MULTIPLY  0x6Au
#define VK_ADD       0x6Bu
#define VK_SEPARATOR 0x6Cu
#define VK_SUBTRACT  0x6Du
#define VK_DECIMAL   0x6Eu
#define VK_DIVIDE    0x6Fu
#define VK_F1        0x70u
#define VK_F2        0x71u
#define VK_F3        0x72u
#define VK_F4        0x73u
#define VK_F5        0x74u
#define VK_F6        0x75u
#define VK_F7        0x76u
#define VK_F8        0x77u
#define VK_F9        0x78u
#define VK_F10       0x79u
#define VK_F11       0x7Au
#define VK_F12       0x7Bu
#define VK_OEM_1     0xBAu
#define VK_OEM_PLUS  0xBBu
#define VK_OEM_COMMA 0xBCu
#define VK_OEM_MINUS 0xBDu
#define VK_OEM_PERIOD 0xBEu
#define VK_OEM_2     0xBFu
#define VK_OEM_3     0xC0u
#define VK_OEM_4     0xDBu
#define VK_OEM_5     0xDCu
#define VK_OEM_6     0xDDu
#define VK_OEM_7     0xDEu
#define VK_CLEAR     0x0Cu

#ifndef WM_SYSCHAR
#define WM_SYSCHAR        0x0106u
#endif

/* keyboard modifier bits (input layer state; MK_SHIFT/MK_CONTROL are reused).
 * Alt/Win are NOT Win32 MK_ flags (MK_ is for mouse messages), so they live
 * in their own bits here. */
#define INP_MOD_ALT      0x0020u  /* Alt (VK_MENU)                    */
#define INP_MOD_META     0x0080u  /* Super / Windows key              */
#define INP_MOD_CAPSLOCK 0x0100u  /* CapsLock latched                 */
#define INP_MOD_NUMLOCK  0x0200u  /* NumLock latched                  */

/* ------------------------------------------------------------------ */
/* lifecycle                                                          */
/* ------------------------------------------------------------------ */
int  nt_input_init(void);
void nt_input_shutdown(void);

/* ------------------------------------------------------------------ */
/* window registration                                                */
/* ------------------------------------------------------------------ */
int   nt_input_register_window(HWND hwnd);
int   nt_input_unregister_window(HWND hwnd);
int   nt_input_window_count(void);
int   nt_input_is_registered(HWND hwnd);

/* ------------------------------------------------------------------ */
/* focus                                                              */
/* ------------------------------------------------------------------ */
HWND nt_input_get_focus(void);
int  nt_input_set_focus(HWND hwnd);
void nt_input_wl_keyboard_enter(HWND hwnd, unsigned int serial);
void nt_input_wl_keyboard_leave(HWND hwnd, unsigned int serial);

/* ------------------------------------------------------------------ */
/* keyboard events                                                    */
/* ------------------------------------------------------------------ */
/* key: a Windows virtual key (VK, wParam convention); modifiers apply.
 * state: 0=released, 1=pressed (Wayland convention).                 */
/* modifiers: bitmask of currently depressed modifiers (optional).    */
void nt_input_key_event(HWND hwnd, unsigned int key, unsigned int state,
                        unsigned int modifiers);
/* live entry: key is a raw Linux evdev keycode, translated with the strict
 * evdev→VK table first; unmappable codes are refused honestly (no message).
 * Serial+time stamping and focused-only routing live here.            */
void nt_input_wl_keyboard_key(HWND hwnd, unsigned int serial, unsigned int time,
                             unsigned int key, unsigned int state);
void nt_input_wl_keyboard_modifiers(unsigned int mods_depressed,
                                   unsigned int mods_latched,
                                   unsigned int mods_locked,
                                   unsigned int group);
void nt_input_push_key(HWND hwnd, unsigned int key, unsigned int state,
                       unsigned int modifiers);

/* ------------------------------------------------------------------ */
/* pointer events                                                     */
/* ------------------------------------------------------------------ */
void nt_input_wl_pointer_enter(HWND hwnd, int x, int y);
void nt_input_wl_pointer_leave(HWND hwnd);
void nt_input_wl_pointer_motion(unsigned int time, int x, int y);
void nt_input_wl_pointer_motion_to(HWND hwnd, int x, int y);
void nt_input_wl_pointer_button(unsigned int serial, unsigned int time,
                                unsigned int button, unsigned int state,
                                int x, int y);
void nt_input_wl_pointer_axis(unsigned int time, unsigned int axis,
                              double value, int x, int y);
void nt_input_push_mouse_move(HWND hwnd, int x, int y);
void nt_input_push_mouse_button(HWND hwnd, unsigned int button,
                               unsigned int state, int x, int y);
void nt_input_push_mouse_wheel(HWND hwnd, int delta, int x, int y);

/* live variants carrying the real Wayland serials (T-183) */
void nt_input_wl_pointer_enter2(HWND hwnd, unsigned int serial, int x, int y);
void nt_input_wl_pointer_leave2(HWND hwnd, unsigned int serial);
void nt_input_wl_pointer_motion_t(unsigned int time, int x, int y);
unsigned int nt_input_vkchar(unsigned int vk);
void nt_input_note_kb_mods_serial(unsigned int serial);

/* ------------------------------------------------------------------ */
/* message queues                                                     */
/* ------------------------------------------------------------------ */
int nt_input_post_message(HWND hwnd, UINT message, unsigned long wParam,
                          long lParam, int pt_x, int pt_y);
int nt_input_peek_message(win32k_msg_t *msg, HWND hwnd, UINT min, UINT max,
                          UINT remove);
int nt_input_get_message(win32k_msg_t *msg, HWND hwnd, UINT min, UINT max);
int nt_input_message_count(HWND hwnd);
void nt_input_clear(HWND hwnd);
void nt_input_clear_all(void);

/* ------------------------------------------------------------------ */
/* serials (T-183: proper Wayland serial bookkeeping)                  */
/* ------------------------------------------------------------------ */
typedef struct nt_input_serials {
    unsigned int kb_enter;      /* last wl_keyboard.enter serial      */
    unsigned int kb_leave;      /* last wl_keyboard.leave serial      */
    unsigned int kb_key;        /* last wl_keyboard.key serial        */
    unsigned int kb_mods;      /* last wl_keyboard.modifiers serial   */
    unsigned int ptr_enter;    /* last wl_pointer.enter serial       */
    unsigned int ptr_leave;    /* last wl_pointer.leave serial       */
    unsigned int ptr_button;   /* last wl_pointer.button serial      */
} nt_input_serials_t;

int nt_input_get_serials(nt_input_serials_t *out);

/* keyboard modifier state (MK_SHIFT|MK_CONTROL|INP_MOD_*) */
unsigned int nt_input_keymods(void);
void         nt_input_set_keymods(unsigned int m);

/* strict evdev keycode translation (T-183):
 * returns 1 and fills *vk if the keycode is mapped, 0 otherwise. */
int nt_input_evdev_to_vk(unsigned int key, unsigned int *vk);

/* evdev keycode -> Win32 set-1 scancode (decimal, bits 16..23 of lParam).
 * Returns the scancode or 0 if unknown; *ext receives the extended-key flag
 * (KF_EXTENDED, bit 24 of lParam). */
unsigned int nt_input_wl_key_to_scan(unsigned int key, unsigned int *ext);

/* character translation (US layout, honouring the current modifier state):
 * returns the WM_CHAR code or 0 when the key produces no character. */
unsigned int nt_input_keychar(unsigned int evdev_key);

/* TranslateMessage-equivalent on the input layer queues: for a WM_KEYDOWN
 * message, posts the matching WM_CHAR to the same window.  Returns 1 if a
 * char message was posted, 0 otherwise. */
int nt_input_translate_message(const win32k_msg_t *msg);

/* window the pointer is currently over (0 if none) */
HWND nt_input_pointer_window(void);

/* helpers */
unsigned int nt_input_key_to_vk(unsigned int key);
long nt_input_pack_lparam(int x, int y);
unsigned int nt_input_button_to_mk(unsigned int button);

#ifdef __cplusplus
}
#endif

/* ------------------------------------------------------------------ */
/* Convenience aliases (many plausible spellings)                      */
/* ------------------------------------------------------------------ */
/* init */
#define input_init                    nt_input_init
#define input_shutdown                nt_input_shutdown
#define ntbox_input_init              nt_input_init
#define ntbox_input_shutdown          nt_input_shutdown

/* registration */
#define input_register_window          nt_input_register_window
#define input_unregister_window       nt_input_unregister_window
#define input_window_count            nt_input_window_count
#define input_is_registered           nt_input_is_registered
#define register_input_window         nt_input_register_window
#define unregister_input_window       nt_input_unregister_window
#define nt_input_add_window           nt_input_register_window
#define nt_input_remove_window        nt_input_unregister_window
#define NtInputRegisterWindow         nt_input_register_window
#define NtInputUnregisterWindow       nt_input_unregister_window

/* focus */
#define input_get_focus               nt_input_get_focus
#define input_set_focus               nt_input_set_focus
#define nt_input_focus                nt_input_set_focus
#define NtInputSetFocus               nt_input_set_focus
#define NtInputGetFocus               nt_input_get_focus

/* keyboard */
#define input_key_event               nt_input_key_event
#define input_push_key                nt_input_push_key
#define input_wl_keyboard_enter       nt_input_wl_keyboard_enter
#define input_wl_keyboard_leave       nt_input_wl_keyboard_leave
#define input_wl_keyboard_key         nt_input_wl_keyboard_key
#define wl_keyboard_enter             nt_input_wl_keyboard_enter
#define wl_keyboard_leave             nt_input_wl_keyboard_leave
#define wl_keyboard_key               nt_input_wl_keyboard_key
#define wl_keyboard_modifiers         nt_input_wl_keyboard_modifiers
#define NtInputKeyEvent               nt_input_key_event

/* pointer */
#define input_wl_pointer_enter        nt_input_wl_pointer_enter
#define input_wl_pointer_leave        nt_input_wl_pointer_leave
#define input_wl_pointer_motion       nt_input_wl_pointer_motion
#define input_wl_pointer_button       nt_input_wl_pointer_button
#define input_wl_pointer_axis         nt_input_wl_pointer_axis
#define wl_pointer_enter              nt_input_wl_pointer_enter
#define wl_pointer_leave              nt_input_wl_pointer_leave
#define wl_pointer_motion             nt_input_wl_pointer_motion
#define wl_pointer_button             nt_input_wl_pointer_button
#define wl_pointer_axis               nt_input_wl_pointer_axis
#define input_push_mouse_move         nt_input_push_mouse_move
#define input_push_mouse_button       nt_input_push_mouse_button
#define input_push_mouse_wheel        nt_input_push_mouse_wheel

/* queues */
#define input_peek_message            nt_input_peek_message
#define input_get_message             nt_input_get_message
#define input_message_count           nt_input_message_count
#define input_clear                   nt_input_clear
#define input_clear_all               nt_input_clear_all
#define input_post_message            nt_input_post_message
#define PeekMessageInput              nt_input_peek_message
#define GetMessageInput               nt_input_get_message

/* (the old wl_seat_get_pointer/get_keyboard aliases were removed: they
 * collided with real Wayland-client function names and were misleading —
 * seat binding is done by the input bridge live layer, T-184) */

#endif /* NTBOX_INPUT_H */
