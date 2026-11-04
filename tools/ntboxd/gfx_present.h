/*
 * gfx_present.h — NT-Box: the PRESENTATION half of the zero-copy path.
 *
 * Master plan W9a ("الوايلاند ياخذريمات من فيرام" — Wayland takes the frames
 * from the GPU).  The renderer inside the box (tools/ntboxd/ntboxd.c) used to
 * draw into a wl_shm CPU framebuffer, which meant one full-frame memcpy per
 * displayed frame.  This module replaces that half of the path:
 *
 *     /dev/dri/renderD128 -> gbm device -> gbm surface (ARGB8888)
 *        -> EGL 1.5 + GLES2   (the box's own renderer draws here, on the GPU)
 *        -> eglSwapBuffers
 *        -> gbm_surface_lock_front_buffer + gbm_bo_get_handle
 *        -> DRM_IOCTL_PRIME_HANDLE_TO_FD   (the dma-buf fd)
 *        -> drmModeAddFB2                   (scanout framebuffer, optional)
 *        -> zwp_linux_dmabuf_v1 (linux-dmabuf v1) -> wl_buffer
 *        -> wl_surface.attach + wl_surface.commit
 *
 * The pixels never pass through the CPU: the compositor imports the very same
 * GPU allocation through a dma-buf file descriptor.  The module counts the
 * CPU copies of frame-sized buffers that happen inside its own frame path
 * (memcpy/memmove of >= 64 KiB) so that the "zero copy" claim stays a
 * MEASUREMENT and not an assertion (plan criterion GFX-007).
 *
 * The module is written like the sibling probe tools/ntboxgfx: every
 * external library (libgbm, libEGL, libwayland-client) is dlopen()ed at run
 * time and every protocol table is embedded, so the file always compiles on a
 * machine without gbm.h / EGL/ / xf86drm.h / wayland-client.h.  The only link
 * dependencies are -ldl -lm.
 *
 * Honesty contract
 *   * gfx_present_create() returns NULL when the environment cannot provide a
 *     GPU; the reason is available through gfx_present_error().  The caller
 *     then falls back to the CPU (wl_shm) path — no crash, no fake pass.
 *   * gfx_present_copies() is what the interposed memcpy()/memmove() counter
 *     actually saw.  It is never a hard coded zero.
 *
 * The "lamp" (المصابيح) pattern of tools/ntboxd/syscalls/ntb0_trace.h is
 * duplicated in the test file (tools/ntboxd/test_present.c); ntb0_trace.h
 * itself is NOT modified.
 *
 * ---------------------------------------------------------------------------
 * HOW TO WIRE IT INTO ntboxd.c  (the box's Wayland thread)
 * ---------------------------------------------------------------------------
 *
 *   #include "gfx_present.h"
 *
 *   static gfx_present_t *gpu;          // the module handle
 *
 *   // once, after the wl_display / wl_surface of the box exist:
 *   gpu = gfx_present_create();
 *   if (!gpu) {
 *           fprintf(stderr, "[wayland] %s — using the CPU framebuffer\n",
 *                   gfx_present_error());
 *           return;                     // fall back to wl_shm
 *   }
 *   gfx_present_wl_use(gpu, wl_display, wl_surface);   // the box's objects
 *   gfx_present_make_current(gpu);                      // the GPU context
 *
 *   // every frame, instead of memcpy(shm_data, ntboxd_wl_fb, ...):
 *   draw_into_the_gpu();                  // the box's renderer
 *   gfx_present_frame(gpu);               // swap -> dma-buf -> Wayland
 *
 *   // at the end:
 *   gfx_present_destroy(gpu);
 *
 *   The module never touches the pixels: gfx_present_frame() hands the GPU
 *   allocation itself to the compositor.  gfx_present_copies(gpu) reports how
 *   many frame-sized CPU copies the frame path really performed (0).
 */
#ifndef GFX_PRESENT_H
#define GFX_PRESENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque module handle. */
typedef struct gfx_present gfx_present_t;

/* ------------------------------------------------------------------ */
/* lifecycle                                                          */
/* ------------------------------------------------------------------ */

/*
 * Create the module: opens the DRM render node, creates the GBM device and
 * the GBM surface (ARGB8888, usage LINEAR|SCANOUT) and brings up the EGL
 * display + GLES2 context.
 *
 * Returns NULL when the environment cannot provide a GPU (no /dev/dri, no
 * libgbm, no libEGL, ...).  gfx_present_error() then explains why; the
 * caller is expected to fall back to the CPU framebuffer path.
 */
gfx_present_t *gfx_present_create(void);

/* Release everything the module owns.  NULL is accepted and ignored. */
void gfx_present_destroy(gfx_present_t *p);

/* The reason of the last failure (never NULL; "" when nothing failed). */
const char *gfx_present_error(void);

/* The DRM node the module renders on ("/dev/dri/renderD128", ...). */
const char *gfx_present_drm_node(const gfx_present_t *p);

/* The name of the DRM driver behind that node ("amdgpu", ...). */
const char *gfx_present_drm_driver(const gfx_present_t *p);

/* ------------------------------------------------------------------ */
/* the GPU render target                                              */
/* ------------------------------------------------------------------ */

/* Size of the render target in pixels (the box's framebuffer size). */
void gfx_present_target_size(const gfx_present_t *p, int *w, int *h);

/*
 * Change the size of the render target.  The GBM surface and the EGL
 * window surface are recreated; call it before the first frame.
 */
int gfx_present_resize(gfx_present_t *p, int w, int h);

/* ------------------------------------------------------------------ */
/* EGL / GLES access for the box's renderer                           */
/* ------------------------------------------------------------------ */

/*
 * The EGL display of the module.  The box's renderer may use it to create
 * its own EGL surface on the GBM surface (gfx_present_egl_window()) or to
 * create its own context; the ready-made ones are exposed as well.
 */
void *gfx_present_egl(const gfx_present_t *p);
void *gfx_present_egl_config(const gfx_present_t *p);
void *gfx_present_egl_context(const gfx_present_t *p);
void *gfx_present_egl_surface(const gfx_present_t *p);
void *gfx_present_egl_window(const gfx_present_t *p);   /* struct gbm_surface * */

/*
 * Make the module's context + surface current, so that the renderer can
 * draw into the GPU buffer.  gfx_present_frame() swaps THAT surface: the
 * renderer must draw into gfx_present_egl_surface() (or into a surface it
 * created on gfx_present_egl_window() and made current itself — in that
 * case the box has to hand that surface to the module, see
 * gfx_present_set_egl_surface()).
 */
int gfx_present_make_current(gfx_present_t *p);
int gfx_present_release_current(gfx_present_t *p);

/*
 * Use another EGLSurface (created by the caller on the module's GBM window)
 * as the surface that gfx_present_frame() swaps.  Pass NULL to go back to
 * the module's own surface.
 */
int gfx_present_set_egl_surface(gfx_present_t *p, void *egl_surface);

/* The GL_RENDERER string of the GPU ("" when it is not known yet). */
const char *gfx_present_renderer(const gfx_present_t *p);

/* ------------------------------------------------------------------ */
/* the Wayland hand-off                                               */
/* ------------------------------------------------------------------ */

/*
 * Connect to the compositor on $WAYLAND_DISPLAY, bind wl_compositor and
 * zwp_linux_dmabuf_v1 and create the surface the frames are attached to
 * (with an xdg-shell role, so that the compositor really renders it and
 * releases the buffer afterwards).
 */
int gfx_present_wl_connect(gfx_present_t *p);

/*
 * Adopt the Wayland objects of the box itself: an existing wl_display and
 * an existing wl_surface.  This is what ntboxd.c uses: the box already owns
 * the surface and the event loop, the module only has to attach the dma-buf
 * buffer to it.
 */
int gfx_present_wl_use(gfx_present_t *p, void *wl_display, void *wl_surface);

/* 1 when the module is ready to present. */
int gfx_present_wl_ready(const gfx_present_t *p);

/*
 * The Wayland objects the module works with.  gfx_present_wl_use() needs
 * them to hand the module the display and the surface of the box; they are
 * also handy for diagnostics.
 */
void *gfx_present_wl_display(const gfx_present_t *p);
void *gfx_present_wl_surface(const gfx_present_t *p);

/* Human readable description of the compositor ("weston headless", ...). */
const char *gfx_present_compositor(const gfx_present_t *p);

/* ------------------------------------------------------------------ */
/* the frame                                                          */
/* ------------------------------------------------------------------ */

/*
 * Present one frame:
 *   eglSwapBuffers -> gbm lock front buffer -> dma-buf fd -> drmModeAddFB
 *   -> zwp_linux_dmabuf_v1 (wl_buffer) -> wl_surface.attach -> commit.
 *
 * Returns 0 on success, -1 on failure (gfx_present_error() explains).
 */
int gfx_present_frame(gfx_present_t *p);

/*
 * Wait until the compositor releases the buffer (the wl_buffer "release"
 * event — the proof that the compositor imported the dma-buf and finished
 * rendering it).  timeout_ms < 0 waits forever.
 * Returns 1 when the release arrived, 0 on timeout, -1 on error.
 */
int gfx_present_wait_release(gfx_present_t *p, int timeout_ms);

/* 1 when the compositor released the last buffer. */
int gfx_present_released(const gfx_present_t *p);

/* The dma-buf file descriptor of the last frame (-1 when there is none). */
int gfx_present_dmabuf_fd(const gfx_present_t *p);

/* The KMS framebuffer id of the last frame (0 when there is none). */
int gfx_present_fb_id(const gfx_present_t *p);

/* How that framebuffer was registered ("drmModeAddFB2(id=..) on .."). */
const char *gfx_present_fb_method(const gfx_present_t *p);

/* Size / stride of the last exported buffer. */
uint32_t gfx_present_buffer_width(const gfx_present_t *p);
uint32_t gfx_present_buffer_height(const gfx_present_t *p);
uint32_t gfx_present_buffer_stride(const gfx_present_t *p);

/* ------------------------------------------------------------------ */
/* CPU copy accounting (the measurement behind the zero-copy claim)   */
/* ------------------------------------------------------------------ */

/*
 * Number of CPU copies of frame-sized buffers (memcpy/memmove of >= 64 KiB)
 * that the counter SAW in the module's frame path.  0 means the frame
 * really stayed in GPU memory.
 */
unsigned long gfx_present_copies(const gfx_present_t *p);

/* The same counter for the last frame only. */
unsigned long gfx_present_copies_last(const gfx_present_t *p);

/* Bytes moved by the counted copies (total / last frame). */
unsigned long gfx_present_copy_bytes(const gfx_present_t *p);
unsigned long gfx_present_copy_bytes_last(const gfx_present_t *p);

/* Bytes moved by the FRAME-SIZED copies only (>= 64 KiB). */
unsigned long gfx_present_copy_big_bytes(const gfx_present_t *p);
unsigned long gfx_present_copy_big_bytes_last(const gfx_present_t *p);

/* Number of memcpy/memmove calls of any size (total / last frame). */
unsigned long gfx_present_copy_calls(const gfx_present_t *p);
unsigned long gfx_present_copy_calls_last(const gfx_present_t *p);

/*
 * Open / close the measurement window by hand.  The box can wrap its own
 * drawing in it so that the reported number covers draw + swap + export +
 * present and not only the hand-off.  gfx_present_frame() opens and closes
 * the window itself when nobody else did.
 */
void gfx_present_copy_window_begin(gfx_present_t *p);
void gfx_present_copy_window_end(gfx_present_t *p);

/* ===================================================================== */
/* GDI-dmabuf integration                                                */
/* ===================================================================== */

/*
 * Link win32k GDI operations to the GPU rendering backend.
 * Allows Rectangle, TextOut, FillRect to draw directly to the dmabuf
 * surface, eliminating CPU copies. Call after gfx_present_make_current().
 */
int gfx_present_link_gdi(gfx_present_t *p);

#ifdef __cplusplus
}
#endif

#endif /* GFX_PRESENT_H */
