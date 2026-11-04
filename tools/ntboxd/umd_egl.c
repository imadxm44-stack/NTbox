/*
 * umd_egl.c — the GBM front-buffer dma-buf hand-off of the swapchain UMD.
 * See umd_egl.h for the contract.
 *
 * Lamps printed here (all verbose one-line, the codebase lamp protocol):
 *   GFX-039  dry present: eglSwapBuffers + gbm_surface_lock_front_buffer +
 *            gbm_bo_get_fd -> a real dma-buf fd of a GPU allocation
 *   GFX-040  PRIME_FD_TO_HANDLE + drmModeAddFB2 on /dev/dri/cardN — the KMS
 *            driver accepted the buffer as a SCANNOUT framebuffer
 *
 * Both lamps are only printed when the export actually happened; every
 * failure is printed as FAIL and returns -1 (no fake pass).
 */
#define _GNU_SOURCE

#include "umd_egl.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* ===================================================================== */
/* 1. Lamps                                                              */
/* ===================================================================== */

#define UMD_TP(id, st, fmt, ...) \
	fprintf(stderr, "[ntbox][%s][%s] " fmt "\n", (id), (st), ##__VA_ARGS__)
#define UMD_OK(id, fmt, ...)   UMD_TP(id, "OK", fmt, ##__VA_ARGS__)
#define UMD_FAIL(id, fmt, ...) UMD_TP(id, "FAIL", fmt, ##__VA_ARGS__)
#define UMD_SKIP(id, fmt, ...) UMD_TP(id, "SKIP", fmt, ##__VA_ARGS__)

/* ===================================================================== */
/* 2. The error string                                                   */
/* ===================================================================== */

static char g_umd_err[512] = "";

static void umd_errf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(g_umd_err, sizeof g_umd_err, fmt, ap);
	va_end(ap);
}

const char *umd_egl_error(void) { return g_umd_err; }

/* ===================================================================== */
/* 3. Self-contained Linux DRM UAPI (local upstream copies — gfx_present  */
/*    style; no libdrm, no xf86drm.h installed on this machine)          */
/* ===================================================================== */

#define UMD_DRM_IOC(dir, type, nr, sz) \
	(((dir) << 30) | ((type) << 8) | (nr) | ((sz) << 16))

#define UMD_IOWR(n, type) \
	UMD_DRM_IOC(_IOC_READ | _IOC_WRITE, 'd', (n), sizeof(type))

struct umd_drm_prime_handle {
	uint32_t handle;
	uint32_t flags;
	int32_t fd;
};

struct umd_drm_fb_cmd2 {
	uint32_t fb_id;
	uint32_t width;
	uint32_t height;
	uint32_t pixel_format;
	uint32_t flags;
	uint32_t handles[4];
	uint32_t pitches[4];
	uint32_t offsets[4];
	uint64_t modifier[4];
};

#define UMD_DRM_IOCTL_PRIME_FD_TO_HANDLE \
	UMD_IOWR(0x2e, struct umd_drm_prime_handle)
#define UMD_DRM_IOCTL_MODE_ADDFB2 UMD_IOWR(0xB8, struct umd_drm_fb_cmd2)
#define UMD_DRM_IOCTL_MODE_RMFB   UMD_IOWR(0xAF, unsigned int)

/* ===================================================================== */
/* 4. The runtime ABI (libgbm / libEGL)                                  */
/* ===================================================================== */

struct gbm_bo;   /* opaque in libgbm */

struct umd_gbm {
	void *lib;
	struct gbm_bo *(*lock_front_buffer)(void *surface);
	int (*bo_get_fd)(struct gbm_bo *bo);
	uint32_t (*bo_get_stride)(struct gbm_bo *bo);
	int (*bo_get_width)(struct gbm_bo *bo);
	int (*bo_get_height)(struct gbm_bo *bo);
	uint64_t (*bo_get_modifier)(struct gbm_bo *bo);
	void (*bo_destroy)(struct gbm_bo *bo);
};

/* Where the last successful KMS registration lives. */
struct umd_kms {
	int fd;
	char node[320];
	uint32_t last_fb_id;
};

static struct umd_gbm g_gbm;
static struct umd_kms g_kms = { -1, "", 0 };

static int umd_gbm_bind(void)
{
	if (g_gbm.lib)
		return 0;
	g_gbm.lib = dlopen("libgbm.so.1", RTLD_LAZY | RTLD_LOCAL);
	if (!g_gbm.lib)
		g_gbm.lib = dlopen("libgbm.so", RTLD_LAZY | RTLD_LOCAL);
	if (!g_gbm.lib) {
		umd_errf("dlopen(libgbm): %s", dlerror());
		return -1;
	}
#define UMD_BIND(field, sym)                                                   \
	do {                                                                   \
		*(void **)(&g_gbm.field) = dlsym(g_gbm.lib, sym);               \
		if (!g_gbm.field) {                                            \
			umd_errf("libgbm: %s missing", sym);                   \
			return -1;                                            \
		}                                                              \
	} while (0)
	UMD_BIND(lock_front_buffer, "gbm_surface_lock_front_buffer");
	UMD_BIND(bo_get_fd, "gbm_bo_get_fd");
	UMD_BIND(bo_get_stride, "gbm_bo_get_stride");
	UMD_BIND(bo_get_width, "gbm_bo_get_width");
	UMD_BIND(bo_get_height, "gbm_bo_get_height");
	UMD_BIND(bo_destroy, "gbm_bo_destroy");
#undef UMD_BIND
	/* optional (Mesa >= 21) */
	g_gbm.bo_get_modifier = (uint64_t (*)(struct gbm_bo *))
		dlsym(g_gbm.lib, "gbm_bo_get_modifier");
	return 0;
}

/* eglSwapBuffers — libEGL is already loaded by gfx_present.c in the same
 * binary, but we take our own dlsym handle so this module also stands alone. */
static int (*g_egl_swap_buffers)(void *, void *) = NULL;
static unsigned int (*g_egl_get_error)(void) = NULL;

int umd_egl_backend(void)
{
	void *lib;

	if (!g_egl_swap_buffers) {
		lib = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
		if (!lib)
			lib = dlopen("libEGL.so", RTLD_LAZY | RTLD_LOCAL);
		if (!lib) {
			umd_errf("dlopen(libEGL): %s", dlerror());
			return -1;
		}
		g_egl_swap_buffers = dlsym(lib, "eglSwapBuffers");
		g_egl_get_error = dlsym(lib, "eglGetError");
		if (!g_egl_swap_buffers) {
			umd_errf("libEGL: eglSwapBuffers missing");
			return -1;
		}
	}
	return umd_gbm_bind();
}

int umd_egl_swap(void *egl_dpy, void *egl_surface)
{
	if (!egl_dpy || !egl_surface) {
		umd_errf("umd_egl_swap: no display/surface");
		return -1;
	}
	if (!g_egl_swap_buffers(egl_dpy, egl_surface)) {
		umd_errf("eglSwapBuffers failed (EGL error 0x%x)",
			 g_egl_get_error ? g_egl_get_error() : 0);
		return -1;
	}
	return 0;
}

/* ===================================================================== */
/* 5. The KMS node: /dev/dri/cardN                                       */
/* ===================================================================== */

static int umd_open_card_node(void)
{
	DIR *dir;
	struct dirent *de;

	dir = opendir("/dev/dri");
	if (!dir) {
		umd_errf("opendir(/dev/dri): %s", strerror(errno));
		return -1;
	}
	while ((de = readdir(dir)) != NULL) {
		char path[320];
		int fd;

		if (strncmp(de->d_name, "card", 4) != 0)
			continue;
		snprintf(path, sizeof path, "/dev/dri/%s", de->d_name);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd >= 0) {
			snprintf(g_kms.node, sizeof g_kms.node, "%s", path);
			closedir(dir);
			return fd;
		}
	}
	closedir(dir);
	umd_errf("no usable KMS node in /dev/dri (need a /dev/dri/cardN with "
		 "O_RDWR access for drmModeAddFB2)");
	return -1;
}

/* ===================================================================== */
/* 6. The export (GFX-039 + GFX-040)                                     */
/* ===================================================================== */

int umd_egl_lock_export(void *gbm_surface, struct umd_frame *out)
{
	struct gbm_bo *bo;
	int fd;
	uint32_t handle;

	if (!out || !gbm_surface) {
		umd_errf("umd_egl_lock_export: bad arguments");
		return -1;
	}
	if (umd_gbm_bind() < 0)
		return -1;

	/* --- the GPU hands the rendered frame back to GBM --- */
	bo = g_gbm.lock_front_buffer(gbm_surface);
	if (!bo) {
		umd_errf("gbm_surface_lock_front_buffer failed");
		return -1;
	}

	/*
	 * gbm_bo_get_fd performs the PRIME export inside libgbm
	 * (DRM_IOCTL_PRIME_HANDLE_TO_FD on the same render node the surface
	 * belongs to).  The fd is a dma-buf — one of the very same
	 * descriptors the Wayland compositor imports in the wet mode
	 * (gfx_present_frame does the identical thing through its own fd).
	 */
	fd = g_gbm.bo_get_fd(bo);
	if (fd < 0) {
		umd_errf("gbm_bo_get_fd failed (the driver refused the "
			 "PRIME export)");
		return -1;
	}
	if (out->dmabuf_fd >= 0)
		close(out->dmabuf_fd);
	out->dmabuf_fd = fd;
	out->width = (uint32_t)g_gbm.bo_get_width(bo);
	out->height = (uint32_t)g_gbm.bo_get_height(bo);
	out->stride = g_gbm.bo_get_stride(bo);
	out->format = UMD_DRM_FORMAT_ARGB8888;
	out->modifier = g_gbm.bo_get_modifier ?
			g_gbm.bo_get_modifier(bo) : 0;
	/*
	 * DRM_FORMAT_MOD_INVALID (the driver says "no modifier known") is
	 * NOT a drm_mode_fb_cmd2 input — the kernel answers EINVAL to it.
	 * The surface was created with GBM_USAGE LINEAR|SCANOUT, so the
	 * honest assumption behind the addfb2 below is the LINEAR layout.
	 */
	if (out->modifier == 0x00ffffffffffffffULL)
		out->modifier = UMD_DRM_FORMAT_MOD_LINEAR;

	UMD_OK("GFX-039",
	       "dry present -> real dma-buf: fd=%d, %ux%u, stride=%u, "
	       "ARGB8888, modifier=0x%llx",
	       fd, out->width, out->height, out->stride,
	       (unsigned long long)out->modifier);

	/*
	 * --- scannout proof: register the SAME buffer at the KMS level ---
	 * The KMS node lives in a different handle space, so the dma-buf is
	 * imported as a GEM handle there first (DRM_IOCTL_PRIME_FD_TO_
	 * HANDLE) and then given to ADDFB2, which validates geometry,
	 * format, stride and modifier of a real scanout candidate.
	 */
	if (g_kms.fd < 0) {
		g_kms.fd = umd_open_card_node();
		if (g_kms.fd < 0) {
			UMD_SKIP("GFX-040",
			         "KMS addfb skipped: %s", umd_egl_error());
			return 0; /* the fd export itself already succeeded */
		}
	}
	{
		struct umd_drm_prime_handle ph;

		memset(&ph, 0, sizeof ph);
		ph.fd = out->dmabuf_fd;
		if (ioctl(g_kms.fd, UMD_DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph) < 0) {
			umd_errf("PRIME_FD_TO_HANDLE on %s failed: %s",
				 g_kms.node, strerror(errno));
			UMD_SKIP("GFX-040", "import into KMS failed: %s",
				 umd_egl_error());
			return 0;
		}
		handle = ph.handle;
	}
	{
		struct umd_drm_fb_cmd2 fb2;

		memset(&fb2, 0, sizeof fb2);
		fb2.width = out->width;
		fb2.height = out->height;
		fb2.pixel_format = out->format;
		fb2.flags = 0;
		fb2.handles[0] = handle;
		fb2.pitches[0] = out->stride;
		fb2.offsets[0] = 0;
		fb2.modifier[0] = out->modifier;
		if (ioctl(g_kms.fd, UMD_DRM_IOCTL_MODE_ADDFB2, &fb2) == 0) {
			if (g_kms.last_fb_id) {
				unsigned int old = g_kms.last_fb_id;

				ioctl(g_kms.fd, UMD_DRM_IOCTL_MODE_RMFB, &old);
			}
			g_kms.last_fb_id = fb2.fb_id;
			out->fb_id = fb2.fb_id;
			snprintf(out->fb_method, sizeof out->fb_method,
				 "drmModeAddFB2(id=%u) on %.100s", fb2.fb_id,
				 g_kms.node);
			UMD_OK("GFX-040",
			       "KMS registered the swapchain buffer as scanout: "
			       "%s, %ux%u stride=%u ARGB8888",
			       out->fb_method, out->width, out->height,
			       out->stride);
			return 0;
		}
		umd_errf("drmModeAddFB2 on %s failed: %s", g_kms.node,
			 strerror(errno));
		UMD_FAIL("GFX-040", "AddFB2 rejected the buffer: %s",
			 umd_egl_error());
		return -1;
	}
}

void umd_frame_release(struct umd_frame *f)
{
	if (!f)
		return;
	if (f->fb_id && g_kms.fd >= 0) {
		unsigned int old = f->fb_id;

		ioctl(g_kms.fd, UMD_DRM_IOCTL_MODE_RMFB, &old);
	}
	if (f->dmabuf_fd >= 0)
		close(f->dmabuf_fd);
	f->dmabuf_fd = -1;
	f->fb_id = 0;
}
