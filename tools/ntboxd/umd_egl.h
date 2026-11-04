/*
 * umd_egl.h — NT-Box: the GBM/EGL front-buffer hand-off used by the
 * swapchain UMD bridge (master plan W9a, T-215/T-216/T-217).
 *
 * WHY THIS FILE EXISTS
 *   tools/ntboxd/gfx_present.c implements the PRESENTATION half (the Wayland
 *   side).  But the D3D11 swapchain (this UMD) must still be able to
 *   *prove* that its buffers are scannout-capable GPU allocations when NO
 *   compositor is running (verify.sh treats a missing WAYLAND_DISPLAY as
 *   SKIP, and the test must work in a DRY mode):
 *
 *     eglSwapBuffers (same EGL surface that is also used for real framing)
 *       -> gbm_surface_lock_front_buffer   (the buffer the GPU rendered)
 *         -> gbm_bo_get_fd                 (the dma-buf fd, straight from libgbm)
 *           -> PRIME_FD_TO_HANDLE on the KMS node
 *             -> drmModeAddFB2             (scannout-capable proof)
 *
 *   When a compositor IS running, the real framing goes through
 *   gfx_present_frame() (linux-dmabuf -> wl_surface.attach -> commit) and
 *   this module is not in the way at all.
 *
 * Everything is dlopen()ed (no SDK headers on this machine), exactly like
 * gfx_present.c and ntboxgfx.c.  The DRM uniAPI structs are local copies of
 * the upstream definitions.  The only link dependency is -ldl.
 *
 * Note on the copy counter: this file performs NO memcpy of its own; frame
 * bytes NEVER pass through the CPU.  The memcpy()/memmove() interposition
 * that MEASURES the CPU copies lives in gfx_present.c of the test binary —
 * umd_egl is deliberately dumb so the counted number covers exactly the
 * Present() path of the swapchain when the test opens the measurement
 * window.
 */
#ifndef NTBOX_UMD_EGL_H
#define NTBOX_UMD_EGL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The metadata of one exported present buffer. */
struct umd_frame {
    int dmabuf_fd;        /* the dma-buf fd (-1 when nothing is exported)  */
    uint32_t width;       /* buffer geometry as GBM reported it            */
    uint32_t height;
    uint32_t stride;      /* bytes per row                                 */
    uint32_t format;      /* DRM fourcc, ARGB8888                          */
    uint64_t modifier;    /* 0 == DRM_FORMAT_MOD_LINEAR, scannout-capable  */
    uint32_t usage;       /* the GBM usage flags of the surface (from the
                             creator; LB = LINEAR|SCANOUT)                 */
    uint32_t fb_id;       /* KMS framebuffer id of the frame (0 = none)    */
    char fb_method[128];  /* how the framebuffer was registered            */
};

#define UMD_GBM_USE_LINEAR   (1u << 0)
#define UMD_GBM_USE_SCANOUT  (1u << 2)
#define UMD_DRM_FORMAT_ARGB8888 0x34325241u  /* 'A','R','2','4' */
#define UMD_DRM_FORMAT_MOD_LINEAR 0u

/*
 * Bind libgbm + libEGL (dlopen/dlsym).  Returns 0 on success, -1 with a
 * reason in umd_egl_error().  Idempotent.
 */
int umd_egl_backend(void);

/* eglSwapBuffers on the surface + display of the module's own GBM path. */
int umd_egl_swap(void *egl_dpy, void *egl_surface);

/*
 * Take the front buffer of a gbm_surface, export it as a dma-buf fd and
 * register it as a KMS framebuffer on the first usable /dev/dri/cardN
 * (scannout-capable PROOF).  Fills *out.  A previous fd in *out is closed.
 * Returns 0 on success.
 */
int umd_egl_lock_export(void *gbm_surface, struct umd_frame *out);

/* Close everything the struct holds (dma-buf fd + KMS framebuffer). */
void umd_frame_release(struct umd_frame *f);

/* The last error ("" when nothing failed). */
const char *umd_egl_error(void);

#ifdef __cplusplus
}
#endif
#endif /* NTBOX_UMD_EGL_H */
