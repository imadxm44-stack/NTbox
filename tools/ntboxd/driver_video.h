/*
 * driver_video.h — Video driver skeleton for NT-Box (uses gfx_present).
 *
 * Implements video mode setting, frame presentation via GPU/dma-buf path.
 * Integrates with gfx_present for zero-copy Wayland presentation.
 *
 * Lamps: VID-001..VID-004
 *   VID-001 OK — ntb0_video_init() initializes GPU context
 *   VID-002 OK — ntb0_video_set_mode() configures display mode
 *   VID-003 OK — ntb0_video_present() presents frame via gfx_present
 *   VID-004 OK — ntb0_video_get_caps() reports GPU capabilities
 */
#ifndef NTB0_DRIVER_VIDEO_H
#define NTB0_DRIVER_VIDEO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* ------------------------------------------------------------------ */
/* Video mode / configuration                                           */
/* ------------------------------------------------------------------ */

struct ntb0_video_mode {
    uint32_t width;
    uint32_t height;
    uint32_t refresh_rate;  /* Hz, 0 = default */
    uint32_t bpp;           /* bits per pixel (24, 32) */
    uint32_t flags;         /* NTB0_VIDEO_FLAG_* */
};

#define NTB0_VIDEO_FLAG_FULLSCREEN    (1u << 0)
#define NTB0_VIDEO_FLAG_VSYNC         (1u << 1)
#define NTB0_VIDEO_FLAG_HDR           (1u << 2)
#define NTB0_VIDEO_FLAG_INTERLACED    (1u << 3)

struct ntb0_video_caps {
    uint32_t max_width;
    uint32_t max_height;
    uint32_t min_width;
    uint32_t min_height;
    uint32_t supported_bpp_mask;  /* bit i = bpp (i+1) supported */
    uint32_t max_refresh_rate;
    char adapter_name[128];
    char driver_name[64];
    uint32_t video_memory_mb;
    uint32_t caps_flags;
};

#define NTB0_VIDEO_CAP_3D          (1u << 0)
#define NTB0_VIDEO_CAP_VIDEO       (1u << 1)
#define NTB0_VIDEO_CAP_OVERLAY     (1u << 2)
#define NTB0_VIDEO_CAP_GAMMA       (1u << 3)
#define NTB0_VIDEO_CAP_DMA_BUF     (1u << 4)
#define NTB0_VIDEO_CAP_MULTI_MON   (1u << 5)

/* ------------------------------------------------------------------ */
/* Video device handle                                                  */
/* ------------------------------------------------------------------ */

typedef struct ntb0_video_device ntb0_video_device_t;

/* ------------------------------------------------------------------ */
/* Public API                                                            */
/* ------------------------------------------------------------------ */

/*
 * Initializes the video subsystem.
 * Creates GPU context via gfx_present, connects to Wayland compositor.
 * Returns device handle on success, NULL on failure.
 *
 * mode: initial video mode (NULL for default 640x480@60Hz 32bpp).
 *       On return, filled with actual mode.
 */
ntb0_video_device_t *ntb0_video_init(struct ntb0_video_mode *mode);

/*
 * Deinitializes the video subsystem.
 * Releases GPU context, Wayland connection, all buffers.
 */
void ntb0_video_deinit(ntb0_video_device_t *dev);

/*
 * Sets the video mode (resolution, refresh rate, format).
 * Recreates GPU surfaces if resolution changes.
 * Returns 0 on success, negative error code on failure.
 */
int ntb0_video_set_mode(ntb0_video_device_t *dev, const struct ntb0_video_mode *mode);

/*
 * Gets the current video mode.
 */
int ntb0_video_get_mode(ntb0_video_device_t *dev, struct ntb0_video_mode *mode);

/*
 * Gets GPU/driver capabilities.
 */
int ntb0_video_get_caps(ntb0_video_device_t *dev, struct ntb0_video_caps *caps);

/*
 * Presents a frame to the display.
 * frame: pointer to frame data (CPU accessible) OR NULL if using GPU buffer.
 * frame_size: size of frame data in bytes.
 * If frame is NULL, assumes the GPU context already has the rendered frame
 * and just needs to swap/present (gfx_present path).
 * Returns 0 on success, negative error code on failure.
 */
int ntb0_video_present(ntb0_video_device_t *dev, const void *frame, size_t frame_size);

/*
 * Presents a frame using a GPU buffer handle (dma-buf / GEM handle).
 * For zero-copy path where Windows driver renders directly to GPU buffer.
 * gpu_handle: driver-specific GPU buffer handle (GEM handle, dma-buf fd, etc.)
 * handle_type: type of handle (NTB0_VIDEO_HANDLE_*)
 * Returns 0 on success, negative error code on failure.
 */
enum ntb0_video_handle_type {
    NTB0_VIDEO_HANDLE_GEM = 0,      /* GEM handle (drmPrime) */
    NTB0_VIDEO_HANDLE_DMA_BUF = 1,  /* dma-buf file descriptor */
    NTB0_VIDEO_HANDLE_GL_TEX = 2,   /* GL texture ID */
};

int ntb0_video_present_gpu(ntb0_video_device_t *dev,
                           uint64_t gpu_handle,
                           enum ntb0_video_handle_type handle_type,
                           uint32_t width, uint32_t height, uint32_t stride);

/*
 * Waits for vertical blank / frame completion.
 * timeout_ms: -1 = wait forever, 0 = non-blocking poll.
 * Returns 1 if vblank occurred, 0 on timeout, -1 on error.
 */
int ntb0_video_wait_vblank(ntb0_video_device_t *dev, int timeout_ms);

/*
 * Gets the GPU render context for direct rendering.
 * Returns EGL context / GBM device / etc. depending on backend.
 * The returned pointers are owned by the video device.
 */
void *ntb0_video_get_egl_display(ntb0_video_device_t *dev);
void *ntb0_video_get_egl_context(ntb0_video_device_t *dev);
void *ntb0_video_get_gbm_device(ntb0_video_device_t *dev);
void *ntb0_video_get_gbm_surface(ntb0_video_device_t *dev);

/*
 * Creates a GPU buffer for rendering (GEM buffer).
 * Returns handle on success, 0 on failure.
 * Use ntb0_video_present_gpu() to present it.
 */
uint64_t ntb0_video_create_gpu_buffer(ntb0_video_device_t *dev,
                                      uint32_t width, uint32_t height,
                                      uint32_t format, uint32_t flags);

/*
 * Destroys a GPU buffer created with ntb0_video_create_gpu_buffer().
 */
void ntb0_video_destroy_gpu_buffer(ntb0_video_device_t *dev, uint64_t handle);

/*
 * Maps a GPU buffer for CPU access (if supported).
 * Returns pointer to mapped memory, or NULL on failure.
 */
void *ntb0_video_map_gpu_buffer(ntb0_video_device_t *dev, uint64_t handle, size_t *size_out);

/*
 * Unmaps a GPU buffer.
 */
void ntb0_video_unmap_gpu_buffer(ntb0_video_device_t *dev, uint64_t handle, void *ptr);

/*
 * Returns a human-readable error string for the last operation.
 */
const char *ntb0_video_error(ntb0_video_device_t *dev);

/*
 * Returns the underlying gfx_present module for advanced usage.
 */
void *ntb0_video_get_gfx_present(ntb0_video_device_t *dev);

/* ------------------------------------------------------------------ */
/* Windows driver interface (IRP-style)                                  */
/* ------------------------------------------------------------------ */

/*
 * Handles an IRP_MJ_DEVICE_CONTROL for video.
 * Implements IOCTL_VIDEO_* codes (compatible with Windows VIDEOPORT/WMIDISP).
 * Returns NTSTATUS.
 */
long ntb0_video_device_control(ntb0_video_device_t *dev,
                               uint32_t ioctl_code,
                               const void *in_buf, uint32_t in_len,
                               void *out_buf, uint32_t out_len,
                               uint32_t *bytes_returned);

/* Common IOCTL codes (match Windows VIDEO_* IOCTLs) */
#define NTB0_IOCTL_VIDEO_QUERY_MODES        0x00090000
#define NTB0_IOCTL_VIDEO_SET_MODE           0x00090004
#define NTB0_IOCTL_VIDEO_RESET_DEVICE       0x00090008
#define NTB0_IOCTL_VIDEO_GET_CAPS           0x0009000C
#define NTB0_IOCTL_VIDEO_QUERY_CURSOR       0x00090010
#define NTB0_IOCTL_VIDEO_SET_CURSOR_POS     0x00090014
#define NTB0_IOCTL_VIDEO_SET_CURSOR_ATTR    0x00090018
#define NTB0_IOCTL_VIDEO_ENABLE_VDM         0x0009001C
#define NTB0_IOCTL_VIDEO_DISABLE_VDM        0x00090020
#define NTB0_IOCTL_VIDEO_REGISTER_VDM       0x00090024
#define NTB0_IOCTL_VIDEO_QUERY_PALETTE      0x00090028
#define NTB0_IOCTL_VIDEO_SET_PALETTE        0x0009002C
#define NTB0_IOCTL_VIDEO_MAP_VIDEO_MEMORY   0x00090030
#define NTB0_IOCTL_VIDEO_UNMAP_VIDEO_MEMORY 0x00090034
#define NTB0_IOCTL_VIDEO_QUERY_PUBLIC_ACCESS 0x00090038

#ifdef __cplusplus
}
#endif

#endif /* NTB0_DRIVER_VIDEO_H */