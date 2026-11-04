#define _GNU_SOURCE
#include "input.h"
#include "ntb0_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* internal state                                                      */
/* ------------------------------------------------------------------ */

#define INP_Q_CAP 1024

typedef struct inp_qslot {
    win32k_msg_t msg;
    int used;
} inp_qslot_t;

typedef struct inp_window {
    HWND hwnd;
    inp_qslot_t *slots;
    int cap;
    int n;
    int head;
} inp_window_t;

static inp_window_t **g_wins = NULL;
static int g_nwin = 0, g_capwin = 0;
static HWND g_focus = NULL;
static HWND g_pointer_win = NULL;
static int g_px = 0, g_py = 0;
static unsigned int g_mods = 0;    /* mouse-button state (MK_LBUTTON etc) */
static unsigned int g_keymods = 0; /* keyboard mods: MK_SHIFT|MK_CONTROL|INP_MOD_* */
static nt_input_serials_t g_serials;

static inp_window_t *find_win(HWND hwnd) {
    int i;
    for (i = 0; i < g_nwin; i++) {
        if (g_wins[i] && g_wins[i]->hwnd == hwnd) return g_wins[i];
    }
    return NULL;
}

static inp_window_t *ensure_win(HWND hwnd) {
    inp_window_t *w = find_win(hwnd);
    if (w) return w;
    if (g_nwin == g_capwin) {
        int nc = g_capwin ? g_capwin * 2 : 16;
        inp_window_t **np = realloc(g_wins, (size_t)nc * sizeof(*np));
        if (!np) return NULL;
        g_wins = np;
        g_capwin = nc;
    }
    w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->hwnd = hwnd;
    w->cap = 64;
    w->slots = calloc((size_t)w->cap, sizeof(*w->slots));
    if (!w->slots) { free(w); return NULL; }
    g_wins[g_nwin++] = w;
    return w;
}

static void enqueue(inp_window_t *w, const win32k_msg_t *m) {
    if (!w || !m) return;
    if (w->n >= w->cap) {
        /* grow */
        int nc = w->cap * 2;
        inp_qslot_t *ns = realloc(w->slots, (size_t)nc * sizeof(*ns));
        if (!ns) return;
        memset(ns + w->cap, 0, (size_t)(nc - w->cap) * sizeof(*ns));
        w->slots = ns;
        w->cap = nc;
    }
    w->slots[w->n].msg = *m;
    w->slots[w->n].used = 1;
    w->n++;
}

/* ------------------------------------------------------------------ */
/* tables: evdev keycode -> VK / scancode / char (T-183)                */
/* ------------------------------------------------------------------ */

/* strict evdev -> VK.  Returns the VK, sets *mapped=0 for unknown keys. */
static unsigned int evdev_vk_table(unsigned int key, int *mapped) {
    *mapped = 1;
    /* keys that collide with the letter range must be resolved FIRST */
    if (key == 42 || key == 54) return VK_SHIFT;   /* LSHIFT / RSHIFT */
    /* letters: KEY_A..KEY_Z = 30..55 */
    if (key >= 30 && key <= 55) return (key - 30) + 'A';
    /* digits top row: KEY_1..KEY_0 = 2..11 */
    if (key >= 2 && key <= 11) {
        static const char d[] = "1234567890";
        return (unsigned char)d[key - 2];
    }
    switch (key) {
        case 1:   return VK_ESCAPE;      /* KEY_ESC        */
        case 14:  return VK_BACK;        /* KEY_BACKSPACE  */
        case 15:  return VK_TAB;         /* KEY_TAB        */
        case 28:  return VK_RETURN;      /* KEY_ENTER      */
        case 29: case 97:  return VK_CONTROL;  /* LCTRL/RCTRL    */
        case 42: case 54:  return VK_SHIFT;    /* LSHIFT/RSHIFT  */
        case 56: case 100: return VK_MENU;     /* LALT/RALT      */
        case 57:  return VK_SPACE;       /* KEY_SPACE      */
        case 58:  return VK_CAPITAL;     /* KEY_CAPSLOCK   */
        case 69:  return VK_NUMLOCK;     /* KEY_NUMLOCK    */
        case 70:  return VK_SCROLL;      /* KEY_SCROLLLOCK */
        case 119: return VK_PAUSE;       /* KEY_PAUSE      */
        case 99:  return VK_SNAPSHOT;    /* KEY_SYSRQ      */
        case 127: return VK_APPS;        /* KEY_COMPOSE    */
        case 125: return VK_LWIN;        /* KEY_LEFTMETA   */
        case 126: return VK_RWIN;        /* KEY_RIGHTMETA  */
        case 103: return VK_UP;
        case 105: return VK_LEFT;
        case 106: return VK_RIGHT;
        case 108: return VK_DOWN;
        case 102: return VK_HOME;
        case 104: return VK_PRIOR;
        case 107: return VK_END;
        case 109: return VK_NEXT;
        case 110: return VK_INSERT;
        case 111: return VK_DELETE;
        /* function row */
        case 59: case 60: case 61: case 62: case 63: case 64:
        case 65: case 66: case 67: case 68:
            return VK_F1 + (key - 59);
        case 87: return VK_F11;
        case 88: return VK_F12;
        /* punctuation — US layout VKs */
        case 12: return VK_OEM_MINUS;    /* - _   */
        case 13: return VK_OEM_PLUS;     /* = +   */
        case 26: return VK_OEM_4;        /* [ {   */
        case 27: return VK_OEM_6;        /* ] }   */
        case 39: return VK_OEM_1;        /* ; :   */
        case 40: return VK_OEM_7;        /* ' "   */
        case 41: return VK_OEM_3;        /* ` ~   */
        case 43: return VK_OEM_5;        /* \ |   */
        case 51: return VK_OEM_COMMA;    /* , <   */
        case 52: return VK_OEM_PERIOD;   /* . >   */
        case 53: return VK_OEM_2;        /* / ?   */
        /* keypad */
        case 96: return VK_RETURN;       /* KEY_KPENTER   */
        case 98: return VK_DIVIDE;       /* KEY_KPSLASH   */
        case 74: return VK_SUBTRACT;     /* KEY_KPMINUS   */
        case 78: return VK_ADD;          /* KEY_KPPLUS    */
        /* numlock-dependent keypad keys are resolved by the caller */
        case 71: case 72: case 73:       /* KP7 KP8 KP9  */
        case 75: case 76: case 77:       /* KP4 KP5 KP6  */
        case 79: case 80: case 81:       /* KP1 KP2 KP3  */
        case 82:                         /* KP0          */
        case 83:                         /* KEY_KPDOT    */
        {
            unsigned int nl = (g_keymods & INP_MOD_NUMLOCK) ? 1 : 0;
            switch (key) {
                case 71: return nl ? VK_NUMPAD7 : VK_HOME;
                case 72: return nl ? VK_NUMPAD8 : VK_UP;
                case 73: return nl ? VK_NUMPAD9 : VK_PRIOR;
                case 75: return nl ? VK_NUMPAD4 : VK_LEFT;
                case 76: return nl ? VK_NUMPAD5 : VK_CLEAR;
                case 77: return nl ? VK_NUMPAD6 : VK_RIGHT;
                case 79: return nl ? VK_NUMPAD1 : VK_END;
                case 80: return nl ? VK_NUMPAD2 : VK_DOWN;
                case 81: return nl ? VK_NUMPAD3 : VK_NEXT;
                case 82: return nl ? VK_NUMPAD0 : VK_INSERT;
                case 83: return nl ? VK_DECIMAL : VK_DELETE;
            }
            break;
        }
        default: break;
    }
    *mapped = 0;
    return 0;
}

unsigned int nt_input_key_to_vk(unsigned int key) {
    int mapped = 0;
    /* If it looks like ASCII/VK already, keep it (normalise letter case). */
    if (key >= 'a' && key <= 'z') return key - 'a' + 'A';
    if (key >= 'A' && key <= 'Z') return key;
    if (key >= VK_F1 && key <= VK_F12) return key;   /* already a VK */
    if (key >= 0xA0) return key;                    /* extended VK range */
    /* evdev range: use the strict table */
    {
        unsigned int vk = evdev_vk_table(key, &mapped);
        if (mapped) return vk;
    }
    if (key >= '0' && key <= '9') return key;
    /* legacy fallback for raw ASCII punctuation passed as "key" */
    if (key >= 32 && key < 127) return key;
    return key; /* honest passthrough for unknown raw codes */
}

long nt_input_pack_lparam(int x, int y) {
    return (long)(((unsigned long)(y & 0xFFFF) << 16) | (unsigned long)(x & 0xFFFF));
}

unsigned int nt_input_button_to_mk(unsigned int button) {
    switch (button) {
        case BTN_LEFT:   case 1: case 0: return MK_LBUTTON;
        case BTN_RIGHT:  case 3:         return MK_RBUTTON;
        case BTN_MIDDLE: case 2:         return MK_MBUTTON;
        default:                         return 0;
    }
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                          */
/* ------------------------------------------------------------------ */
int nt_input_init(void) {
    g_focus = NULL;
    g_pointer_win = NULL;
    g_px = g_py = 0;
    g_mods = 0;
    g_keymods = 0;
    memset(&g_serials, 0, sizeof(g_serials));
    NTB0_OK("INP-001", "input system initialised");
    return 0;
}

void nt_input_shutdown(void) {
    int i;
    for (i = 0; i < g_nwin; i++) {
        if (g_wins[i]) {
            free(g_wins[i]->slots);
            free(g_wins[i]);
        }
    }
    free(g_wins);
    g_wins = NULL;
    g_nwin = g_capwin = 0;
    g_focus = NULL;
    g_pointer_win = NULL;
    g_keymods = 0;
    g_mods = 0;
}

/* ------------------------------------------------------------------ */
/* registration                                                       */
/* ------------------------------------------------------------------ */
int nt_input_register_window(HWND hwnd) {
    if (!hwnd) return -1;
    if (find_win(hwnd)) {
        NTB0_OK("INP-001", "window already registered hwnd=%p", (void *)hwnd);
        return 0;
    }
    if (!ensure_win(hwnd)) return -1;
    NTB0_OK("INP-001", "registered window hwnd=%p (total=%d)", (void *)hwnd, g_nwin);
    return 0;
}

int nt_input_unregister_window(HWND hwnd) {
    int i;
    for (i = 0; i < g_nwin; i++) {
        if (g_wins[i] && g_wins[i]->hwnd == hwnd) {
            free(g_wins[i]->slots);
            free(g_wins[i]);
            memmove(&g_wins[i], &g_wins[i + 1], (size_t)(g_nwin - i - 1) * sizeof(*g_wins));
            g_nwin--;
            if (g_focus == hwnd) g_focus = NULL;
            if (g_pointer_win == hwnd) g_pointer_win = NULL;
            return 0;
        }
    }
    return -1;
}

int nt_input_window_count(void) { return g_nwin; }

int nt_input_is_registered(HWND hwnd) {
    return find_win(hwnd) != NULL;
}

/* ------------------------------------------------------------------ */
/* focus                                                              */
/* ------------------------------------------------------------------ */
HWND nt_input_get_focus(void) { return g_focus; }

int nt_input_set_focus(HWND hwnd) {
    if (hwnd != g_focus) {
        HWND prev = g_focus;
        if (prev) {
            inp_window_t *pw = find_win(prev);
            if (pw) {
                win32k_msg_t m;
                memset(&m, 0, sizeof(m));
                m.hwnd = prev;
                m.message = WM_KILLFOCUS;
                m.wParam = (unsigned long)(uintptr_t)hwnd;
                m.lParam = 0;
                m.pt_x = 0; m.pt_y = 0;
                enqueue(pw, &m);
            }
            NTB0_OK("INP-004", "WM_KILLFOCUS hwnd=%p (new=%p)", (void *)prev, (void *)hwnd);
        }
        g_focus = hwnd;
        if (hwnd) {
            inp_window_t *nw = ensure_win(hwnd);
            if (nw) {
                win32k_msg_t m;
                memset(&m, 0, sizeof(m));
                m.hwnd = hwnd;
                m.message = WM_SETFOCUS;
                m.wParam = (unsigned long)(uintptr_t)prev;
                m.lParam = 0;
                enqueue(nw, &m);
            }
            NTB0_OK("INP-004", "WM_SETFOCUS hwnd=%p (prev=%p)", (void *)hwnd, (void *)prev);
        }
    }
    return 0;
}

void nt_input_wl_keyboard_enter(HWND hwnd, unsigned int serial) {
    g_serials.kb_enter = serial;
    nt_input_register_window(hwnd);
    nt_input_set_focus(hwnd);
    NTB0_OK("INP-004", "wl_keyboard.enter -> focus hwnd=%p serial=%u", (void *)hwnd, serial);
}

void nt_input_wl_keyboard_leave(HWND hwnd, unsigned int serial) {
    g_serials.kb_leave = serial;
    if (g_focus == hwnd) {
        inp_window_t *w = find_win(hwnd);
        if (w) {
            win32k_msg_t m;
            memset(&m, 0, sizeof(m));
            m.hwnd = hwnd;
            m.message = WM_KILLFOCUS;
            m.wParam = 0;
            m.lParam = 0;
            enqueue(w, &m);
        }
        g_focus = NULL;
        NTB0_OK("INP-004", "wl_keyboard.leave -> WM_KILLFOCUS hwnd=%p", (void *)hwnd);
    } else {
        /* still notify */
        inp_window_t *w = ensure_win(hwnd);
        if (w) {
            win32k_msg_t m;
            memset(&m, 0, sizeof(m));
            m.hwnd = hwnd;
            m.message = WM_KILLFOCUS;
            m.wParam = (unsigned long)(uintptr_t)g_focus;
            enqueue(w, &m);
        }
    }
}

/* ------------------------------------------------------------------ */
/* keyboard events                                                    */
/* ------------------------------------------------------------------ */
/* set-1 scancode for a Windows VK, found by reversing the strict evdev
 * table.  Returns 0 when no row knows the VK (caller falls back to
 * vk & 0xFF for legacy synthetic keys).  When several evdev rows share a
 * VK (numpad vs the extended navigation cluster) the extended row wins —
 * its scancode is the canonical Win32 one with KF_EXTENDED set. */
static unsigned int vk_to_scan(unsigned int vk, unsigned int *ext) {
    unsigned int k, row_vk, first = 0, first_ext = 0, best = 0, best_ext = 0;
    int mapped;
    for (k = 1; k <= 120; k++) {
        row_vk = evdev_vk_table(k, &mapped);
        if (!mapped || row_vk != vk) continue;
        {
            unsigned int e = 0;
            unsigned int sc = nt_input_wl_key_to_scan(k, &e);
            if (!first) { first = sc; first_ext = e; }
            if (e && !best) { best = sc; best_ext = 1; }
        }
    }
    if (best) {
        if (ext) *ext = best_ext;
        return best;
    }
    if (ext) *ext = first_ext;
    return first;
}

/* legacy entry: takes a Windows virtual key (synthetic tests feed
 * Win32-style VK codes).  The live evdev path enters through
 * nt_input_wl_keyboard_key, which translates the keycode first and
 * refuses unmappable codes honestly. */
void nt_input_key_event(HWND hwnd, unsigned int vk, unsigned int state,
                        unsigned int modifiers) {
    HWND target = hwnd ? hwnd : g_focus;   /* T-185: focus-only routing */
    inp_window_t *w;
    unsigned int scan, ext;
    long lp;
    win32k_msg_t m;

    if (!target) {
        NTB0_FAIL("INP-016", "key event dropped: no focused window (vk=0x%02X)", vk);
        return;
    }
    if (hwnd && g_focus && hwnd != g_focus) {
        NTB0_FAIL("INP-016",
                  "keyboard event dropped: target %p is not the focused window %p",
                  (void *)target, (void *)g_focus);
        return;
    }
    nt_input_register_window(target);
    w = ensure_win(target);
    if (!w) { NTB0_FAIL("INP-002", "cannot queue window %p", (void *)target); return; }

    /* keyboard modifier maintenance (keyboard-side state) */
    if (vk == VK_SHIFT   || vk == VK_LSHIFT || vk == VK_RSHIFT) {
        if (state) g_keymods |= MK_SHIFT; else g_keymods &= ~MK_SHIFT;
    }
    else if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) {
        if (state) g_keymods |= MK_CONTROL; else g_keymods &= ~MK_CONTROL;
    }
    else if (vk == VK_MENU   || vk == VK_LMENU  || vk == VK_RMENU) {
        if (state) g_keymods |= INP_MOD_ALT; else g_keymods &= ~INP_MOD_ALT;
    }
    else if (vk == VK_LWIN   || vk == VK_RWIN) {
        if (state) g_keymods |= INP_MOD_META; else g_keymods &= ~INP_MOD_META;
    }
    else if (vk == VK_CAPITAL && state) {
        /* legacy soft tracking of the latch; the live path supersedes it with
         * the authoritative xkb lock bit from the modifiers event */
        g_keymods ^= INP_MOD_CAPSLOCK;
    }
    /* caller-provided modifiers are authoritative when given */
    if (modifiers) g_keymods = modifiers;

    memset(&m, 0, sizeof(m));
    m.hwnd = target;
    /* Win32: keys with ALT held arrive as WM_SYSKEYDOWN/UP */
    m.message = (g_keymods & INP_MOD_ALT) ?
        (state ? WM_SYSKEYDOWN : WM_SYSKEYUP) :
        (state ? WM_KEYDOWN    : WM_KEYUP);
    m.wParam = vk;

    scan = vk_to_scan(vk, &ext);
    if (!scan) scan = vk & 0xFF;        /* legacy: VK doubles as pseudo scan */
    lp = 1L | ((long)(scan & 0xFF) << 16) | (ext ? 0x01000000L : 0L);
    if (state == 0) lp |= 0xC0000000L;  /* prev-state=1, transition=1 (keyup) */
    m.lParam = lp;
    m.pt_x = 0; m.pt_y = 0;
    enqueue(w, &m);
    NTB0_OK("INP-002", "vk 0x%02X state=%u msg=0x%03X hwnd=%p mods=0x%X",
            vk, state, m.message, (void *)target, g_keymods);

    /* WM_CHAR translation (US layout): keydown only, no Alt (Win32 produces
     * WM_SYSCHAR for those — honestly not synthesised here). */
    if (state == 1 && !(g_keymods & INP_MOD_ALT)) {
        unsigned int ch = nt_input_vkchar(vk);
        if (ch) {
            memset(&m, 0, sizeof(m));
            m.hwnd = target;
            m.message = WM_CHAR;
            m.wParam = ch;
            m.lParam = lp;
            m.pt_x = 0; m.pt_y = 0;
            enqueue(w, &m);
            NTB0_OK("INP-002", "WM_CHAR 0x%02X -> hwnd=%p (vk=0x%02X)", ch, (void *)target, vk);
        }
    }
}

void nt_input_wl_keyboard_key(HWND hwnd, unsigned int serial, unsigned int time,
                              unsigned int key, unsigned int state) {
    HWND target = hwnd ? hwnd : g_focus;
    int n_before;
    inp_window_t *w;

    if (!target) {
        NTB0_FAIL("INP-016", "wl_keyboard.key dropped: no keyboard focus (key=%u)", key);
        return;
    }
    if (g_focus && target != g_focus) {
        NTB0_FAIL("INP-016",
                  "wl_keyboard.key dropped: window %p holds no keyboard focus (%p)",
                  (void *)target, (void *)g_focus);
        return;
    }
    g_serials.kb_key = serial;

    /* live path: strict evdev→VK.  Unmappable evdev codes are refused
     * honestly — no VK, no message, no silent pass-through. */
    {
        unsigned int vk;
        int mapped = 0;
        vk = evdev_vk_table(key, &mapped);
        if (!mapped) {
            NTB0_FAIL("INP-015",
                      "wl_keyboard.key %u refused: unmappable evdev keycode (no VK, no message)",
                      key);
            return;
        }
        w = find_win(target);
        n_before = w ? w->n : 0;
        nt_input_key_event(target, vk, state, 0);
    }
    /* stamp the Wayland u32 time onto the produced messages */
    w = find_win(target);
    if (w) {
        for (; n_before < w->n; n_before++) w->slots[n_before].msg.time = time;
    }
}

void nt_input_wl_keyboard_modifiers(unsigned int mods_depressed,
                                   unsigned int mods_latched,
                                   unsigned int mods_locked,
                                   unsigned int group) {
    /* xkb depressed mask: Shift=bit0, Lock(Caps)=bit1, Control=bit2,
     * Mod1(Alt)=bit3, Mod2(NumLock)=bit4, Mod4(Super)=bit6 */
    (void)mods_latched; (void)mods_locked; (void)group;
    g_keymods &= ~((unsigned int)(MK_SHIFT | MK_CONTROL | INP_MOD_ALT |
                                  INP_MOD_META | INP_MOD_CAPSLOCK |
                                  INP_MOD_NUMLOCK));
    if (mods_depressed & 0x01) g_keymods |= MK_SHIFT;         /* Shift */
    if (mods_depressed & 0x02) g_keymods |= INP_MOD_CAPSLOCK;  /* CapsLock */
    if (mods_depressed & 0x04) g_keymods |= MK_CONTROL;        /* Control */
    if (mods_depressed & 0x08) g_keymods |= INP_MOD_ALT;       /* Mod1/Alt */
    if (mods_depressed & 0x10) g_keymods |= INP_MOD_NUMLOCK;   /* Mod2/NumLock */
    if (mods_depressed & 0x40) g_keymods |= INP_MOD_META;      /* Mod4/Super */
    NTB0_OK("INP-002", "modifiers depressed=0x%X latched=0x%X locked=0x%X -> keymods=0x%X",
            mods_depressed, mods_latched, mods_locked, g_keymods);
}

void nt_input_push_key(HWND hwnd, unsigned int key, unsigned int state,
                       unsigned int modifiers) {
    nt_input_key_event(hwnd, key, state, modifiers);
}

/* ------------------------------------------------------------------ */
/* pointer events                                                     */
/* ------------------------------------------------------------------ */
/* bitmask of MK_ flags meaningful in the wParam of mouse messages */
static unsigned int mouse_mk(void) {
    return (g_mods & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON |
                      MK_XBUTTON1 | MK_XBUTTON2)) |
           (g_keymods & (MK_SHIFT | MK_CONTROL));
}

void nt_input_wl_pointer_enter(HWND hwnd, int x, int y) {
    nt_input_wl_pointer_enter2(hwnd, 0, x, y);
}

void nt_input_wl_pointer_enter2(HWND hwnd, unsigned int serial, int x, int y) {
    if (!hwnd) {
        NTB0_FAIL("INP-016", "wl_pointer.enter dropped: unknown surface (serial=%u)", serial);
        return;
    }
    g_serials.ptr_enter = serial;
    nt_input_register_window(hwnd);
    g_pointer_win = hwnd;
    g_px = x; g_py = y;
    /* no message on enter in our model; focus is separate */
    NTB0_OK("INP-003", "wl_pointer.enter hwnd=%p (%d,%d) serial=%u",
            (void *)hwnd, x, y, serial);
}

void nt_input_wl_pointer_leave(HWND hwnd) {
    nt_input_wl_pointer_leave2(hwnd, 0);
}

void nt_input_wl_pointer_leave2(HWND hwnd, unsigned int serial) {
    g_serials.ptr_leave = serial;
    if (!hwnd || g_pointer_win == hwnd) g_pointer_win = NULL;
    NTB0_OK("INP-003", "wl_pointer.leave hwnd=%p serial=%u", (void *)hwnd, serial);
}

static HWND pointer_target(HWND hwnd) {
    if (hwnd) return hwnd;
    if (g_pointer_win) return g_pointer_win;
    return g_focus;
}

void nt_input_wl_pointer_motion(unsigned int time, int x, int y) {
    HWND target = g_pointer_win ? g_pointer_win : g_focus;
    if (!target) {
        NTB0_FAIL("INP-016", "wl_pointer.motion dropped: no pointer target (%d,%d)", x, y);
        return;
    }
    nt_input_wl_pointer_motion_t(time, x, y);
}

/* live variant: motion has no wl serial, but carries the u32 time */
void nt_input_wl_pointer_motion_t(unsigned int time, int x, int y) {
    HWND target = g_pointer_win ? g_pointer_win : g_focus;
    if (!target) {
        NTB0_FAIL("INP-016", "wl_pointer.motion dropped: no pointer target (%d,%d)", x, y);
        return;
    }
    nt_input_register_window(target);
    inp_window_t *w = ensure_win(target);
    if (!w) return;
    g_px = x; g_py = y;

    win32k_msg_t m;
    memset(&m, 0, sizeof(m));
    m.hwnd = target;
    m.message = WM_MOUSEMOVE;
    m.wParam = mouse_mk();              /* MK_ flags + shift/ctrl state */
    m.lParam = nt_input_pack_lparam(x, y);
    m.time = time;
    m.pt_x = x; m.pt_y = y;
    enqueue(w, &m);
    NTB0_OK("INP-003", "WM_MOUSEMOVE hwnd=%p (%d,%d) t=%u", (void *)target, x, y, time);
}

void nt_input_wl_pointer_motion_to(HWND hwnd, int x, int y) {
    HWND target = pointer_target(hwnd);
    if (!target) {
        NTB0_FAIL("INP-016", "WM_MOUSEMOVE dropped: no pointer target");
        return;
    }
    nt_input_register_window(target);
    inp_window_t *w = ensure_win(target);
    if (!w) return;
    g_px = x; g_py = y;
    if (!g_pointer_win) g_pointer_win = target;

    win32k_msg_t m;
    memset(&m, 0, sizeof(m));
    m.hwnd = target;
    m.message = WM_MOUSEMOVE;
    m.wParam = mouse_mk();              /* MK_ flags + shift/ctrl state */
    m.lParam = nt_input_pack_lparam(x, y);
    m.pt_x = x; m.pt_y = y;
    enqueue(w, &m);
    NTB0_OK("INP-003", "WM_MOUSEMOVE hwnd=%p (%d,%d)", (void *)target, x, y);
}

void nt_input_wl_pointer_button(unsigned int serial, unsigned int time,
                                unsigned int button, unsigned int state,
                                int x, int y) {
    HWND target = pointer_target(NULL);
    /* pointer window (surface under cursor) wins over focus */
    if (g_pointer_win) target = g_pointer_win;
    if (!target) {
        NTB0_FAIL("INP-016",
                  "button %u dropped: no pointer window and no focus (serial=%u)",
                  button, serial);
        return;
    }
    if (button != BTN_LEFT && button != BTN_RIGHT && button != BTN_MIDDLE &&
        button != 1 && button != 2 && button != 3 && button != 0) {
        NTB0_FAIL("INP-014", "button %u refused: unmappable Wayland button code", button);
        return;
    }
    nt_input_register_window(target);
    inp_window_t *w = ensure_win(target);
    if (!w) return;
    if (x == 0 && y == 0) { x = g_px; y = g_py; }  /* wl button events carry no coords */
    else { g_px = x; g_py = y; }

    unsigned int msg;
    unsigned int mk = nt_input_button_to_mk(button);
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        if (button == BTN_LEFT || button == 1 || button == 0) msg = WM_LBUTTONDOWN;
        else if (button == BTN_RIGHT || button == 3) msg = WM_RBUTTONDOWN;
        else msg = WM_MBUTTONDOWN;
    } else {
        if (button == BTN_LEFT || button == 1 || button == 0) msg = WM_LBUTTONUP;
        else if (button == BTN_RIGHT || button == 3) msg = WM_RBUTTONUP;
        else msg = WM_MBUTTONUP;
    }

    g_serials.ptr_button = serial;

    /* update modifier/button state */
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) g_mods |= mk;
    else g_mods &= ~mk;

    win32k_msg_t m;
    memset(&m, 0, sizeof(m));
    m.hwnd = target;
    m.message = msg;
    m.wParam = mouse_mk();
    m.lParam = nt_input_pack_lparam(x, y);
    m.time = time;
    m.pt_x = x; m.pt_y = y;
    enqueue(w, &m);
    NTB0_OK("INP-003", "button %u state=%u -> msg=%u hwnd=%p (%d,%d) serial=%u",
            button, state, msg, (void *)target, x, y, serial);
}

void nt_input_wl_pointer_axis(unsigned int time, unsigned int axis,
                              double value, int x, int y) {
    HWND target = g_pointer_win ? g_pointer_win : g_focus;
    if (!target) {
        NTB0_FAIL("INP-016", "axis event dropped: no pointer target");
        return;
    }
    nt_input_register_window(target);
    inp_window_t *w = ensure_win(target);
    if (!w) return;
    int ix = x ? x : g_px;
    int iy = y ? y : g_py;
    /* Wayland axis value for wheel scroll is a multiple of 10 per wheel
     * notch (libinput convention); WM_MOUSEWHEEL wants 120 per notch. */
    int delta = (int)(value * 12.0);
    if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL)
        delta = -delta;   /* wl "away"=left, Win32 HWHEEL positive=right */

    win32k_msg_t m;
    memset(&m, 0, sizeof(m));
    m.hwnd = target;
    if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL)
        m.message = WM_MOUSEHWHEEL;
    else
        m.message = WM_MOUSEWHEEL;
    m.wParam = (unsigned long)((delta & 0xFFFF) << 16) | (mouse_mk() & 0xFFFF);
    m.lParam = nt_input_pack_lparam(ix, iy);
    m.time = time;
    m.pt_x = ix; m.pt_y = iy;
    enqueue(w, &m);
    NTB0_OK("INP-003", "msg=0x%03X hwnd=%p delta=%d t=%u (axis=%u value=%.2f)",
            m.message, (void *)target, delta, time, axis, value);
}

void nt_input_push_mouse_move(HWND hwnd, int x, int y) {
    nt_input_wl_pointer_motion_to(hwnd, x, y);
}

void nt_input_push_mouse_button(HWND hwnd, unsigned int button,
                               unsigned int state, int x, int y) {
    /* set pointer window temporarily */
    HWND prev = g_pointer_win;
    g_pointer_win = hwnd;
    nt_input_wl_pointer_button(0, 0, button, state, x, y);
    g_pointer_win = prev;
}

void nt_input_push_mouse_wheel(HWND hwnd, int delta, int x, int y) {
    HWND prev = g_pointer_win;
    g_pointer_win = hwnd;
    /* convert delta directly */
    inp_window_t *w = ensure_win(hwnd);
    if (w) {
        win32k_msg_t m;
        memset(&m, 0, sizeof(m));
        m.hwnd = hwnd;
        m.message = WM_MOUSEWHEEL;
        m.wParam = (unsigned long)((delta & 0xFFFF) << 16) | (g_mods & 0xFFFF);
        m.lParam = nt_input_pack_lparam(x, y);
        m.pt_x = x; m.pt_y = y;
        enqueue(w, &m);
    }
    g_pointer_win = prev;
    NTB0_OK("INP-003", "WM_MOUSEWHEEL hwnd=%p delta=%d", (void *)hwnd, delta);
}

/* ------------------------------------------------------------------ */
/* message queues                                                     */
/* ------------------------------------------------------------------ */
int nt_input_post_message(HWND hwnd, UINT message, unsigned long wParam,
                          long lParam, int pt_x, int pt_y) {
    if (!hwnd) return -1;
    nt_input_register_window(hwnd);
    inp_window_t *w = ensure_win(hwnd);
    if (!w) return -1;
    win32k_msg_t m;
    memset(&m, 0, sizeof(m));
    m.hwnd = hwnd;
    m.message = message;
    m.wParam = wParam;
    m.lParam = lParam;
    m.pt_x = pt_x;
    m.pt_y = pt_y;
    enqueue(w, &m);
    NTB0_OK("INP-005", "delivered msg=%u to hwnd=%p (q=%d)", message, (void *)hwnd, w->n);
    return 0;
}

static int msg_matches(const win32k_msg_t *m, HWND hwnd_filter, UINT min, UINT max) {
    if (hwnd_filter && m->hwnd != hwnd_filter) return 0;
    if (min == 0 && max == 0) return 1;
    if (min && m->message < min) return 0;
    if (max && m->message > max) return 0;
    return 1;
}

int nt_input_peek_message(win32k_msg_t *msg, HWND hwnd, UINT min, UINT max,
                          UINT remove) {
    inp_window_t *w = hwnd ? find_win(hwnd) : NULL;
    /* if hwnd specified, search that window; else search all */
    if (hwnd) {
        if (!w) return 0;
        /* find first matching in this window's queue */
        int i;
        for (i = 0; i < w->n; i++) {
            if (!w->slots[i].used) continue;
            if (!msg_matches(&w->slots[i].msg, hwnd, min, max)) continue;
            if (msg) *msg = w->slots[i].msg;
            if (remove == PM_REMOVE) {
                memmove(&w->slots[i], &w->slots[i + 1], (size_t)(w->n - i - 1) * sizeof(*w->slots));
                w->n--;
            }
            return 1;
        }
        return 0;
    }
    /* no hwnd filter: search all windows */
    int wi;
    for (wi = 0; wi < g_nwin; wi++) {
        w = g_wins[wi];
        if (!w) continue;
        int i;
        for (i = 0; i < w->n; i++) {
            if (!w->slots[i].used) continue;
            if (!msg_matches(&w->slots[i].msg, NULL, min, max)) continue;
            if (msg) *msg = w->slots[i].msg;
            if (remove == PM_REMOVE) {
                memmove(&w->slots[i], &w->slots[i + 1], (size_t)(w->n - i - 1) * sizeof(*w->slots));
                w->n--;
            }
            return 1;
        }
    }
    return 0;
}

int nt_input_get_message(win32k_msg_t *msg, HWND hwnd, UINT min, UINT max) {
    /* non-blocking: returns 1 if got, 0 if empty */
    if (nt_input_peek_message(msg, hwnd, min, max, PM_REMOVE)) return 1;
    return 0;
}

int nt_input_message_count(HWND hwnd) {
    if (hwnd) {
        inp_window_t *w = find_win(hwnd);
        return w ? w->n : 0;
    }
    /* total across all */
    int total = 0, i;
    for (i = 0; i < g_nwin; i++) if (g_wins[i]) total += g_wins[i]->n;
    return total;
}

void nt_input_clear(HWND hwnd) {
    inp_window_t *w = find_win(hwnd);
    if (w) w->n = 0;
}

void nt_input_clear_all(void) {
    int i;
    for (i = 0; i < g_nwin; i++) if (g_wins[i]) g_wins[i]->n = 0;
}

/* ------------------------------------------------------------------ */
/* T-183/T-185 new API: serials, modifiers, strict translation         */
/* ------------------------------------------------------------------ */
int nt_input_get_serials(nt_input_serials_t *out) {
    if (!out) return -1;
    *out = g_serials;
    return 0;
}

unsigned int nt_input_keymods(void) { return g_keymods; }

void nt_input_set_keymods(unsigned int m) { g_keymods = m; }

void nt_input_note_kb_mods_serial(unsigned int serial) { g_serials.kb_mods = serial; }

HWND nt_input_pointer_window(void) { return g_pointer_win; }

int nt_input_evdev_to_vk(unsigned int key, unsigned int *vk) {
    int mapped = 0;
    unsigned int v = evdev_vk_table(key, &mapped);
    if (vk) *vk = v;
    return mapped;
}

/* evdev -> Win32 set-1 scancode (decimal).  For most of the main block the
 * Linux evdev code IS the decimal set-1 scancode; only the extended cluster
 * (arrows / navigation / right modifiers / keypad operators) differs. */
unsigned int nt_input_wl_key_to_scan(unsigned int key, unsigned int *ext) {
    if (ext) *ext = 0;
    if (key == 42 || key == 54) return 0x2A;        /* shift */
    if (key >= 30 && key <= 55) return key;         /* letters */
    if (key >= 2 && key <= 13)  return key;         /* digits row + - = */
    switch (key) {
        case 1:  return 0x01;                       /* Esc   */
        case 14: return 0x0E;                       /* Bksp  */
        case 15: return 0x0F;                       /* Tab   */
        case 16: return 0x10; case 17: return 0x11; /* Q W   */
        case 18: return 0x12; case 19: return 0x13; case 20: return 0x14;
        case 21: return 0x15; case 22: return 0x16; case 23: return 0x17;
        case 24: return 0x18; case 25: return 0x19; case 26: return 0x1A;
        case 27: return 0x1B;
        case 28: return 0x1C;                       /* Enter */
        case 29: return 0x1D;                       /* LCtrl */
        case 39: return 0x27; case 40: return 0x28; case 41: return 0x29;
        case 43: return 0x2B;
        case 44: return 0x2C; case 45: return 0x2D; case 46: return 0x2E;
        case 47: return 0x2F; case 48: return 0x30; case 49: return 0x31;
        case 50: return 0x32; case 51: return 0x33; case 52: return 0x34;
        case 53: return 0x35;
        case 56: return 0x38;                       /* LAlt  */
        case 57: return 0x39;                       /* Space */
        case 58: return 0x3A;                       /* Caps  */
        case 59: return 0x3B; case 60: return 0x3C; case 61: return 0x3D;
        case 62: return 0x3E; case 63: return 0x3F; case 64: return 0x40;
        case 65: return 0x41; case 66: return 0x42; case 67: return 0x43;
        case 68: return 0x44;                       /* F1..F10 */
        case 69: return 0x45;                       /* NumLock */
        case 70: return 0x46;                       /* ScrollLock */
        case 71: return 0x47; case 72: return 0x48; case 73: return 0x49; /* KP7..9 (numlock scan) */
        case 74: return 0x4A;                        /* KP-  */
        case 75: return 0x4B; case 76: return 0x4C; case 77: return 0x4D; /* KP4..6 */
        case 78: return 0x4E;                        /* KP+  */
        case 79: return 0x4F; case 80: return 0x50; case 81: return 0x51; /* KP1..3 */
        case 82: return 0x52;                        /* KP0  */
        case 83: return 0x53;                        /* KP.  */
        case 87: return 0x57;                        /* F11  */
        case 88: return 0x58;                        /* F12  */
        /* extended cluster: same base scancodes, KF_EXTENDED set */
        case 102: if (ext) *ext = 1; return 0x47;    /* Home   */
        case 103: if (ext) *ext = 1; return 0x48;    /* Up     */
        case 104: if (ext) *ext = 1; return 0x49;    /* PgUp   */
        case 105: if (ext) *ext = 1; return 0x4B;    /* Left   */
        case 106: if (ext) *ext = 1; return 0x4D;    /* Right  */
        case 107: if (ext) *ext = 1; return 0x4F;    /* End    */
        case 108: if (ext) *ext = 1; return 0x50;    /* Down   */
        case 109: if (ext) *ext = 1; return 0x51;    /* PgDn   */
        case 110: if (ext) *ext = 1; return 0x52;    /* Insert */
        case 111: if (ext) *ext = 1; return 0x53;    /* Delete */
        case 96:  if (ext) *ext = 1; return 0x9C;    /* KPENTER */
        case 97:  if (ext) *ext = 1; return 0x1D;    /* RCtrl  */
        case 98:  if (ext) *ext = 1; return 0xB5;    /* KP/    */
        case 100: if (ext) *ext = 1; return 0x38;    /* RAlt   */
        default: break;
    }
    return 0; /* unknown: honest refusal upstream */
}

/* ---- character translation (US layout) ------------------------------ */
static unsigned int evdev_char_pair(unsigned int key, unsigned int *shft) {
    *shft = 0;
    switch (key) {
        case 12: *shft = '_'; return '-';
        case 13: *shft = '+'; return '=';
        case 26: *shft = '{'; return '[';
        case 27: *shft = '}'; return ']';
        case 39: *shft = ':'; return ';';
        case 40: *shft = '"'; return '\'';
        case 41: *shft = '~'; return '`';
        case 43: *shft = '|'; return '\\';
        case 51: *shft = '<'; return ',';
        case 52: *shft = '>'; return '.';
        case 53: *shft = '?'; return '/';
        default:  return 0;
    }
}

unsigned int nt_input_keychar(unsigned int evdev_key) {
    unsigned int sh, upper;

    /* control combinations: Ctrl+letter -> 0x01..0x1A.  Ctrl on digits /
     * punctuation produces no char (honest: not invented). */
    if (g_keymods & INP_MOD_ALT) return 0;      /* no WM_CHAR under Alt */
    if (g_keymods & MK_CONTROL) {
        if (evdev_key >= 30 && evdev_key <= 55)
            return ('a' + (evdev_key - 30)) & 0x1F;
        return 0;
    }

    switch (evdev_key) {
        case 14: return 0x08;   /* Backspace */
        case 15: return 0x09;   /* Tab       */
        case 28: return 0x0D;   /* Enter     */
        case 96: return 0x0D;   /* KPENTER   */
        case 1:  return 0x1B;   /* Esc       */
        case 57: return ' ';    /* Space     */
        default: break;
    }

    /* digits row */
    if (evdev_key >= 2 && evdev_key <= 11) {
        static const char nd[]  = "1234567890";
        static const char nsh[] = "!@#$%^&*()";
        return (unsigned char)(g_keymods & MK_SHIFT ? nsh[evdev_key - 2]
                                                    : nd[evdev_key - 2]);
    }

    /* letters: CapsLock XOR Shift gives the uppercase */
    if (evdev_key >= 30 && evdev_key <= 55) {
        upper = ((g_keymods & MK_SHIFT) != 0) ^ ((g_keymods & INP_MOD_CAPSLOCK) != 0);
        unsigned int base = 'a' + (evdev_key - 30);
        return upper ? (base - 'a' + 'A') : base;
    }

    /* keypad digits (with NumLock; without it they are navigation keys
     * and produce no character) */
    if ((g_keymods & INP_MOD_NUMLOCK) &&
        (evdev_key == 82 || evdev_key == 83 ||
         (evdev_key >= 71 && evdev_key <= 73) ||
         (evdev_key >= 75 && evdev_key <= 77) ||
         (evdev_key >= 79 && evdev_key <= 81))) {
        if (evdev_key >= 79 && evdev_key <= 81) return '1' + (evdev_key - 79);
        if (evdev_key >= 71 && evdev_key <= 73) return '7' + (evdev_key - 71);
        if (evdev_key >= 75 && evdev_key <= 77) return '4' + (evdev_key - 75);
        if (evdev_key == 82) return '0';
        if (evdev_key == 83) return '.';
    }

    sh = 0;
    {
        unsigned int base = evdev_char_pair(evdev_key, &sh);
        if (base) return (g_keymods & MK_SHIFT) ? sh : base;
    }
    return 0; /* no character for this key: honest */
}

unsigned int nt_input_vkchar(unsigned int vk) {
    static const char oem1[2]  = { ';', ':' };
    static const char oem2[2]  = { '=', '+' };
    static const char oem3[2]  = { ',', '<' };
    static const char oem4[2]  = { '-', '_' };
    static const char oem5[2]  = { '.', '>' };
    static const char oem6[2]  = { '/', '?' };
    static const char oem7[2]  = { '`', '~' };
    static const char oem11[2] = { '[', '{' };
    static const char oem22[2] = { '\\', '|' };
    static const char oem33[2] = { ']', '}' };
    static const char oem44[2] = { '\'', '"' };
    const unsigned int sh = (g_keymods & MK_SHIFT) ? 1 : 0;

    if (g_keymods & INP_MOD_ALT) return 0;
    if (g_keymods & MK_CONTROL) {
        if (vk >= 'A' && vk <= 'Z') return vk & 0x1F;
        return 0;
    }
    switch (vk) {
        case VK_BACK:   return 0x08;
        case VK_TAB:    return 0x09;
        case VK_RETURN: return 0x0D;
        case VK_ESCAPE: return 0x1B;
        case VK_SPACE:  return ' ';
        case VK_MULTIPLY: return '*';
        case VK_ADD:      return '+';
        case VK_SUBTRACT: return '-';
        case VK_DECIMAL:  return '.';
        case VK_DIVIDE:   return '/';
        case VK_NUMPAD0: case VK_NUMPAD1: case VK_NUMPAD2: case VK_NUMPAD3:
        case VK_NUMPAD4: case VK_NUMPAD5: case VK_NUMPAD6: case VK_NUMPAD7:
        case VK_NUMPAD8: case VK_NUMPAD9:
            if (g_keymods & INP_MOD_NUMLOCK) return '0' + (vk - VK_NUMPAD0);
            return 0;
        case VK_OEM_1:     return (unsigned char)oem1[sh];
        case VK_OEM_PLUS:  return (unsigned char)oem2[sh];
        case VK_OEM_COMMA: return (unsigned char)oem3[sh];
        case VK_OEM_MINUS: return (unsigned char)oem4[sh];
        case VK_OEM_PERIOD:return (unsigned char)oem5[sh];
        case VK_OEM_2:     return (unsigned char)oem6[sh];
        case VK_OEM_3:     return (unsigned char)oem7[sh];
        case VK_OEM_4:     return (unsigned char)oem11[sh];
        case VK_OEM_5:     return (unsigned char)oem22[sh];
        case VK_OEM_6:     return (unsigned char)oem33[sh];
        case VK_OEM_7:     return (unsigned char)oem44[sh];
        default: break;
    }
    if (vk >= '1' && vk <= '9') {
        static const char dsh[] = "!@#$%^&*(";   /* shift+1..shift+9 */
        return (unsigned char)(sh ? dsh[vk - '0' - 1] : (char)vk);
    }
    if (vk == '0') return (unsigned char)(sh ? ')' : '0');
    if (vk >= 'A' && vk <= 'Z') {
        unsigned int upper = sh ^ ((g_keymods & INP_MOD_CAPSLOCK) != 0);
        return upper ? vk : (vk - 'A' + 'a');
    }
    return 0;
}

int nt_input_translate_message(const win32k_msg_t *msg) {
    unsigned int ch;
    if (!msg || msg->message != WM_KEYDOWN) return 0;
    ch = nt_input_vkchar((unsigned int)msg->wParam);
    if (!ch) return 0;
    return nt_input_post_message(msg->hwnd, WM_CHAR, ch, msg->lParam,
                                 msg->pt_x, msg->pt_y) == 0 ? 1 : 0;
}
