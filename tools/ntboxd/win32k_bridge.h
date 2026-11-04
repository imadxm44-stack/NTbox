/*
 * win32k_bridge.h — NT-Box: bridges win32k window manager to GPU presentation (gfx_present)
 * and input events.  Lamps WIN-011..WIN-020, INP-006..INP-010.
 */
#ifndef NTBOX_WIN32K_BRIDGE_H
#define NTBOX_WIN32K_BRIDGE_H

#include "win32k.h"
#include "gfx_present.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Bridge lifecycle                                                   */
/* ------------------------------------------------------------------ */

/* Initialise the bridge.  Creates the gfx_present module and prepares
 * the GPU render target.  Returns 0 on success, -1 on failure (the
 * reason is available via gfx_present_error()).  The bridge can run
 * without a GPU (falls back to software); call this once at startup. */
int win32k_bridge_init(void);

/* Shutdown the bridge: releases gfx_present and all per-window state. */
void win32k_bridge_shutdown(void);

/* ------------------------------------------------------------------ */
/* Per-window GPU resources                                           */
/* ------------------------------------------------------------------ */

/* Associates a gfx_present context with a window.  This is called from
 * NtUserCreateWindowEx (or the user32 alias).  The bridge takes ownership
 * of the GPU context for this window; the caller must not destroy it. */
int win32k_bridge_window_created(HWND hwnd, int width, int height,
                                 const char *title, HINSTANCE hinst);

/* Called when the window is destroyed; releases the GPU resources. */
void win32k_bridge_window_destroyed(HWND hwnd);

/* Called when the window is shown (SW_SHOW etc).  Commits the first frame
 * if needed.  Returns 1 on success, 0 on failure. */
int win32k_bridge_window_show(HWND hwnd, int nCmdShow);

/* Called when the window needs repainting (UpdateWindow / WM_PAINT).
 * Returns 1 on success, 0 on failure. */
int win32k_bridge_window_update(HWND hwnd);

/* Resize the GPU render target for a window.  Returns 1 on success. */
int win32k_bridge_window_resize(HWND hwnd, int width, int height);

/* ------------------------------------------------------------------ */
/* DC / GDI drawing bridge                                            */
/* ------------------------------------------------------------------ */

/* Gets the EGL surface for a window.  The returned surface is the
 * one gfx_present_frame() will swap.  The caller must call
 * gfx_present_make_current() before drawing and
 * gfx_present_release_current() after. */
void *win32k_bridge_get_egl_surface(HWND hwnd);

/* Called after drawing commands (Rectangle, TextOut, etc) are accumulated
 * on a window.  This flushes the GPU commands and presents the frame. */
int win32k_bridge_present_window(HWND hwnd);

/* Set text color on the GPU context associated with a window.  Returns the
 * previous color.  The actual GL state is applied on the next draw. */
COLORREF win32k_bridge_set_text_color(HWND hwnd, COLORREF color);

/* Set background mode (OPAQUE / TRANSPARENT).  Returns the previous mode. */
int win32k_bridge_set_bk_mode(HWND hwnd, int mode);

/* Draw a rectangle on the GPU.  left,top,right,bottom in window coords. */
int win32k_bridge_rectangle(HWND hwnd, int left, int top, int right, int bottom);

/* Draw text on the GPU.  For simplicity this renders to a texture and
 * blits; a real implementation would use a glyph atlas. */
int win32k_bridge_text_out(HWND hwnd, int x, int y, const char *str, int len);

/* ------------------------------------------------------------------ */
/* MessageBox bridge                                                  */
/* ------------------------------------------------------------------ */

/* Shows a message box using Wayland (xdg_popup or a dedicated surface).
 * Returns IDOK (1) on success.  Falls back to stderr if no compositor. */
int win32k_bridge_message_box(HWND hwnd, const char *text,
                              const char *caption, UINT type);

/* ------------------------------------------------------------------ */
/* Message loop integration                                           */
/* ------------------------------------------------------------------ */

/* Processes pending Wayland events and feeds them into the win32k
 * message queue via the input bridge.  Returns the number of events
 * processed.  Call this from the application's message loop. */
int win32k_bridge_pump_events(void);

/* Called when the application enters the idle loop (GetMessage blocks).
 * Allows the bridge to wait for Wayland events efficiently. */
void win32k_bridge_wait_for_events(void);

/* ------------------------------------------------------------------ */
/* Global accessors                                                   */
/* ------------------------------------------------------------------ */

/* Returns the global gfx_present handle (for advanced use). */
gfx_present_t *win32k_bridge_gfx_present(void);

/* Returns 1 if GPU presentation is active, 0 if software fallback. */
int win32k_bridge_is_gpu_active(void);

#ifdef __cplusplus
}
#endif

#endif /* NTBOX_WIN32K_BRIDGE_H */