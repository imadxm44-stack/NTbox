/*
 * gfx_present.c — NT-Box: the PRESENTATION half of the zero-copy path.
 *
 * See gfx_present.h for the contract.  In one paragraph:
 *
 *   The box draws on the GPU into a GBM buffer.  This module turns that
 *   buffer into a dma-buf file descriptor and hands the descriptor to the
 *   Wayland compositor through the linux-dmabuf v1 protocol.  The pixels
 *   are never copied by the CPU; the compositor imports the same GPU
 *   allocation.  The number of frame-sized CPU copies inside that path is
 *   counted (memcpy/memmove interposed in this binary) and exposed through
 *   gfx_present_copies(), so that "zero copy" stays a MEASUREMENT.
 *
 * BUILD (no external link dependency, everything is dlopen()ed):
 *
 *   gcc -O2 -Wall -Wextra -fno-builtin-memcpy -fno-builtin-memmove \
 *       -o /tmp/opencode/test_present tools/ntboxd/test_present.c \
 *       tools/ntboxd/gfx_present.c -ldl -lm
 *
 * Techniques reused from the sibling probe tools/ntboxgfx/ntboxgfx.c (which
 * proved the same path end to end on this machine):
 *   * The GBM surface operations are only installed once eglInitialize()
 *     ran on the GBM display.
 *   * While the EGL window surface is current it owns the GBM front buffer;
 *     eglSwapBuffers() gives it back and only then can the buffer object
 *     that carries the rendered frame be locked.
 *   * The linux-dmabuf v1 and xdg-shell protocol tables below were produced
 *     with the LOCAL wayland-scanner 1.22.0 from the XML files shipped by
 *     the distribution (/usr/share/wayland-protocols/...).  Nothing was
 *     downloaded.
 */

#define _GNU_SOURCE

#include "gfx_present.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ===================================================================== */
/* 1. CPU copy accounting — the measurement behind the zero-copy claim   */
/* ===================================================================== */

/*
 * memcpy()/memmove() are interposed inside this binary, so every CPU copy
 * performed by this module AND by the libraries it dlopen()s (libgbm,
 * libEGL, libGLESv2, libwayland-client) is seen by the counter: those
 * libraries call memcpy through their PLT, which resolves to the
 * definition below.
 *
 * The implementations use volatile byte accesses on purpose: that stops
 * the compiler from turning the loop into a call to memcpy() again (which
 * would recurse) and stops it from being replaced by a builtin.
 *
 * A "big" copy is a copy large enough to carry framebuffer data.  The
 * smallest frame this module renders is 640x480x4 = 1.2 MiB, so a 64 KiB
 * threshold cannot be reached by protocol marshalling (a Wayland request is
 * a few dozen bytes) but is trivially reached by a real framebuffer copy.
 */
#define GFX_BIG_COPY (64UL * 1024UL)

static unsigned long g_cp_calls;      /* memcpy+memmove calls in the window  */
static unsigned long g_cp_bytes;      /* bytes moved in the window           */
static unsigned long g_cp_big_calls;  /* calls >= GFX_BIG_COPY               */
static unsigned long g_cp_big_bytes;  /* bytes of the "big" calls            */
static int g_cp_window;               /* the measurement window is open      */

static void gfx_cp_account(size_t n)
{
	if (!g_cp_window)
		return;
	g_cp_calls++;
	g_cp_bytes += (unsigned long)n;
	if ((unsigned long)n >= GFX_BIG_COPY) {
		g_cp_big_calls++;
		g_cp_big_bytes += (unsigned long)n;
	}
}

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);

void *memcpy(void *dst, const void *src, size_t n)
{
	volatile unsigned char *d = (volatile unsigned char *)dst;
	const volatile unsigned char *s = (const volatile unsigned char *)src;
	size_t i;

	gfx_cp_account(n);
	for (i = 0; i < n; i++)
		d[i] = s[i];
	return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
	volatile unsigned char *d = (volatile unsigned char *)dst;
	const volatile unsigned char *s = (const volatile unsigned char *)src;
	size_t i;

	gfx_cp_account(n);
	if (d < s) {
		for (i = 0; i < n; i++)
			d[i] = s[i];
	} else {
		for (i = n; i > 0; i--)
			d[i - 1] = s[i - 1];
	}
	return dst;
}

/*
 * The fortified variants are what a compiler with _FORTIFY_SOURCE (the
 * default of most distributions) actually calls, so they have to be
 * interposed as well — otherwise the counter would miss exactly the copies
 * that matter (the big ones, where the size is known at compile time).
 */
void *__memcpy_chk(void *dst, const void *src, size_t n, size_t dstlen);
void *__memmove_chk(void *dst, const void *src, size_t n, size_t dstlen);

void *__memcpy_chk(void *dst, const void *src, size_t n, size_t dstlen)
{
	(void)dstlen;
	return memcpy(dst, src, n);
}

void *__memmove_chk(void *dst, const void *src, size_t n, size_t dstlen)
{
	(void)dstlen;
	return memmove(dst, src, n);
}

/* ===================================================================== */
/* 2. Error reporting                                                   */
/* ===================================================================== */

static char g_gfx_err[512] = "";

static void gfx_errf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(g_gfx_err, sizeof g_gfx_err, fmt, ap);
	va_end(ap);
}

const char *gfx_present_error(void)
{
	return g_gfx_err;
}

/* ===================================================================== */
/* 3. Runtime library loading (dlopen) — the module always builds        */
/* ===================================================================== */

static void *gfx_dlopen(const char *soname)
{
	void *h = dlopen(soname, RTLD_LAZY | RTLD_LOCAL);

	if (!h) {
		const char *e = dlerror();

		gfx_errf("dlopen(%s): %s", soname, e ? e : "unknown error");
		return NULL;
	}
	return h;
}

static void *gfx_dlsym_opt(void *h, const char *sym)
{
	return dlsym(h, sym);
}

/* ===================================================================== */
/* 4. Linux DRM UAPI — self contained (no libdrm needed)                */
/* ===================================================================== */

#define GFX_DRM_IOWR(nr, type) \
	_IOC(_IOC_READ | _IOC_WRITE, 'd', (nr), sizeof(type))

struct gfx_drm_version {
	int version_major;
	int version_minor;
	int version_patchlevel;
	size_t name_len;
	char *name;
	size_t date_len;
	char *date;
	size_t desc_len;
	char *desc;
};

struct gfx_drm_prime_handle {
	uint32_t handle;
	uint32_t flags;
	int32_t fd;
};

struct gfx_drm_fb_cmd {
	uint32_t fb_id;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t bpp;
	uint32_t depth;
	uint32_t handle;
};

struct gfx_drm_fb_cmd2 {
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

#define GFX_DRM_IOCTL_VERSION \
	GFX_DRM_IOWR(0x00, struct gfx_drm_version)
#define GFX_DRM_IOCTL_PRIME_HANDLE_TO_FD \
	GFX_DRM_IOWR(0x2d, struct gfx_drm_prime_handle)
#define GFX_DRM_IOCTL_PRIME_FD_TO_HANDLE \
	GFX_DRM_IOWR(0x2e, struct gfx_drm_prime_handle)
#define GFX_DRM_IOCTL_MODE_ADDFB \
	GFX_DRM_IOWR(0xAE, struct gfx_drm_fb_cmd)
#define GFX_DRM_IOCTL_MODE_ADDFB2 \
	GFX_DRM_IOWR(0xB8, struct gfx_drm_fb_cmd2)
#define GFX_DRM_IOCTL_MODE_RMFB \
	GFX_DRM_IOWR(0xAF, unsigned int)

/* DRM fourcc: 'A','R','2','4' */
#define GFX_DRM_FORMAT_ARGB8888 0x34325241u
#define GFX_DRM_FORMAT_XRGB8888 0x34325258u
/* DRM_FORMAT_MOD_INVALID = 0x00ffffffffffffff */
#define GFX_DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffULL

/* GBM usage flags (gbm.h is not installed here; these are the upstream ABI) */
#define GFX_GBM_BO_USE_LINEAR     (1u << 0)
#define GFX_GBM_BO_USE_CURSOR     (1u << 1)
#define GFX_GBM_BO_USE_SCANOUT    (1u << 2)
#define GFX_GBM_BO_USE_CURSOR_ANY (1u << 3)
#define GFX_GBM_BO_USE_RENDERING  (1u << 4)

/* ===================================================================== */
/* 5. GBM / EGL / Wayland — the runtime ABI                            */
/* ===================================================================== */

struct gbm_device;
struct gbm_surface;
struct gbm_bo;

typedef void *EGLDisplay;
typedef void *EGLContext;
typedef void *EGLSurface;
typedef void *EGLConfig;
typedef void *EGLNativeDisplayType;
typedef int32_t EGLint;
typedef uint32_t EGLenum;
typedef uint32_t EGLBoolean;
typedef intptr_t EGLAttrib;

#define EGL_FALSE              0
#define EGL_NONE               0x3038
#define EGL_NO_SURFACE         ((EGLSurface)0)
#define EGL_NO_CONTEXT         ((EGLContext)0)
#define EGL_SURFACE_TYPE       0x3033
#define EGL_WINDOW_BIT         0x0004
#define EGL_RENDERABLE_TYPE    0x3040
#define EGL_OPENGL_ES2_BIT     0x0004
#define EGL_RED_SIZE           0x3024
#define EGL_GREEN_SIZE         0x3023
#define EGL_BLUE_SIZE          0x3022
#define EGL_ALPHA_SIZE         0x3021
#define EGL_DEPTH_SIZE         0x3025
#define EGL_OPENGL_ES_API      0x30A0
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_PLATFORM_GBM_KHR   0x31D7
#define EGL_VENDOR             0x3053
#define EGL_VERSION            0x3054

struct gfx_gbm {
	void *lib;
	struct gbm_device *(*create_device)(int fd);
	void (*device_destroy)(struct gbm_device *dev);
	struct gbm_surface *(*surface_create)(struct gbm_device *dev, int w, int h,
					      uint32_t format, uint32_t usage, ...);
	void (*surface_destroy)(struct gbm_surface *s);
	struct gbm_bo *(*lock_front_buffer)(struct gbm_surface *s);
	uint32_t (*bo_get_handle)(struct gbm_bo *bo);
	uint32_t (*bo_get_stride)(struct gbm_bo *bo);
	int (*bo_get_width)(struct gbm_bo *bo);
	int (*bo_get_height)(struct gbm_bo *bo);
	uint64_t (*bo_get_modifier)(struct gbm_bo *bo);   /* Mesa >= 21 */
	void (*bo_destroy)(struct gbm_bo *bo);
};

struct gfx_egl {
	void *lib;
	EGLDisplay (*get_platform_display)(EGLenum platform, void *native,
					   const EGLAttrib *attrs);
	EGLDisplay (*get_display)(EGLNativeDisplayType native);
	EGLBoolean (*initialize)(EGLDisplay dpy, EGLint *maj, EGLint *min);
	EGLBoolean (*terminate)(EGLDisplay dpy);
	EGLBoolean (*bind_api)(EGLenum api);
	EGLBoolean (*choose_config)(EGLDisplay dpy, const EGLint *attrs,
				    EGLConfig *cfgs, EGLint n, EGLint *num);
	EGLContext (*create_context)(EGLDisplay dpy, EGLConfig cfg,
				     EGLContext share, const EGLint *attrs);
	EGLSurface (*create_window_surface)(EGLDisplay dpy, EGLConfig cfg,
					    void *native, const EGLint *attrs);
	EGLBoolean (*make_current)(EGLDisplay dpy, EGLSurface draw, EGLSurface read,
				   EGLContext ctx);
	EGLBoolean (*destroy_surface)(EGLDisplay dpy, EGLSurface surf);
	EGLBoolean (*destroy_context)(EGLDisplay dpy, EGLContext ctx);
	EGLBoolean (*swap_buffers)(EGLDisplay dpy, EGLSurface surf);
	EGLint (*get_error)(void);
	const char *(*query_string)(EGLDisplay dpy, EGLint name);
};

/* The public ABI of libwayland-client (stable, reproduced here). */
struct wl_interface;
struct wl_message {
	const char *name;
	const char *signature;
	const struct wl_interface **types;
};
struct wl_interface {
	const char *name;
	int version;
	int method_count;
	const struct wl_message *methods;
	int event_count;
	const struct wl_message *events;
};

struct wl_proxy;
struct wl_display;

/* Core protocol opcodes (wayland.xml). */
#define GFX_WL_DISPLAY_GET_REGISTRY 1
#define GFX_WL_REGISTRY_BIND         0
#define GFX_WL_COMPOSITOR_CREATE_SURFACE 0
#define GFX_WL_SURFACE_ATTACH  1
#define GFX_WL_SURFACE_DAMAGE  2
#define GFX_WL_SURFACE_COMMIT  6

/* wl_proxy_marshal_flags flags (wayland-client-core.h). */
#define WL_MARSHAL_FLAG_DESTROY 1

struct gfx_wl {
	void *lib;
	struct wl_display *(*display_connect)(const char *name);
	void (*display_disconnect)(struct wl_display *d);
	int (*display_roundtrip)(struct wl_display *d);
	int (*display_get_error)(struct wl_display *d);
	int (*display_flush)(struct wl_display *d);
	int (*display_dispatch_pending)(struct wl_display *d);
	int (*display_prepare_read)(struct wl_display *d);
	int (*display_read_events)(struct wl_display *d);
	int (*display_get_fd)(struct wl_display *d);
	struct wl_proxy *(*proxy_marshal_flags)(struct wl_proxy *p, uint32_t opcode,
						const struct wl_interface *iface,
						uint32_t version, uint32_t flags, ...);
	int (*proxy_add_listener)(struct wl_proxy *p, void (**impl)(void), void *data);
	uint32_t (*proxy_get_version)(struct wl_proxy *p);
	void (*proxy_destroy)(struct wl_proxy *p);
	/* core interface tables (data symbols of the library) */
	const struct wl_interface *iface_registry;
	const struct wl_interface *iface_compositor;
	const struct wl_interface *iface_surface;
	const struct wl_interface *iface_buffer;
};

static struct gfx_gbm g_gbm;
static struct gfx_egl g_egl;
static struct gfx_wl  g_wl;

/* ===================================================================== */
/* 6. Wayland protocol tables (linux-dmabuf v1 + xdg-shell)              */
/* ===================================================================== */

/*
 * Generated with the LOCAL wayland-scanner 1.22.0 (private-code) from
 *   /usr/share/wayland-protocols/stable/linux-dmabuf/linux-dmabuf-v1.xml
 *   /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
 * and embedded here so that the build needs neither wayland-scanner nor the
 * wayland development headers.  The tables are copied verbatim from the
 * scanner output (the argument order of every request/event is the one the
 * scanner produced).
 */

/* ---- linux-dmabuf v1 ---- */
static const struct wl_interface *g_dmabuf_types[16];
static struct wl_message g_dmabuf_requests[4];
static struct wl_message g_dmabuf_events[2];
static struct wl_interface g_dmabuf_iface;
static struct wl_message g_params_requests[4];
static struct wl_message g_params_events[2];
static struct wl_interface g_params_iface;

/* ---- xdg-shell ---- */
static const struct wl_interface *g_xdg_types[32];
static struct wl_message g_wm_base_requests[4];
static struct wl_message g_wm_base_events[1];
static struct wl_interface g_wm_base_iface;
static struct wl_message g_positioner_requests[10];
static struct wl_interface g_positioner_iface;
static struct wl_message g_xdg_surface_requests[5];
static struct wl_message g_xdg_surface_events[1];
static struct wl_interface g_xdg_surface_iface;
static struct wl_message g_toplevel_requests[14];
static struct wl_message g_toplevel_events[4];
static struct wl_interface g_toplevel_iface;
static struct wl_message g_popup_requests[3];
static struct wl_message g_popup_events[3];
static struct wl_interface g_popup_iface;

/* xdg-shell opcodes used here */
#define GFX_XDG_WM_BASE_GET_XDG_SURFACE 2
#define GFX_XDG_SURFACE_GET_TOPLEVEL    1
#define GFX_XDG_SURFACE_SET_WINDOW_GEOMETRY 3
#define GFX_XDG_SURFACE_ACK_CONFIGURE   4
#define GFX_XDG_TOPLEVEL_SET_TITLE      2

static void gfx_protocol_tables_init(void)
{
	int i;

	/* ---------------- linux-dmabuf v1 ---------------- */
	for (i = 0; i < 16; i++)
		g_dmabuf_types[i] = NULL;
	g_dmabuf_types[6] = &g_params_iface;
	g_dmabuf_types[15] = g_wl.iface_buffer;

	g_dmabuf_requests[0].name = "destroy";
	g_dmabuf_requests[0].signature = "";
	g_dmabuf_requests[0].types = g_dmabuf_types + 0;
	g_dmabuf_requests[1].name = "create_params";
	g_dmabuf_requests[1].signature = "n";
	g_dmabuf_requests[1].types = g_dmabuf_types + 6;
	g_dmabuf_requests[2].name = "get_default_feedback";
	g_dmabuf_requests[2].signature = "4n";
	g_dmabuf_requests[2].types = g_dmabuf_types + 6;
	g_dmabuf_requests[3].name = "get_surface_feedback";
	g_dmabuf_requests[3].signature = "4no";
	g_dmabuf_requests[3].types = g_dmabuf_types + 6;

	g_dmabuf_events[0].name = "format";
	g_dmabuf_events[0].signature = "u";
	g_dmabuf_events[0].types = g_dmabuf_types + 0;
	g_dmabuf_events[1].name = "modifier";
	g_dmabuf_events[1].signature = "3uuu";
	g_dmabuf_events[1].types = g_dmabuf_types + 0;

	g_dmabuf_iface.name = "zwp_linux_dmabuf_v1";
	g_dmabuf_iface.version = 5;
	g_dmabuf_iface.method_count = 4;
	g_dmabuf_iface.methods = g_dmabuf_requests;
	g_dmabuf_iface.event_count = 2;
	g_dmabuf_iface.events = g_dmabuf_events;

	g_params_requests[0].name = "destroy";
	g_params_requests[0].signature = "";
	g_params_requests[0].types = g_dmabuf_types + 0;
	g_params_requests[1].name = "add";
	g_params_requests[1].signature = "huuuuu";
	g_params_requests[1].types = g_dmabuf_types + 0;
	g_params_requests[2].name = "create";
	g_params_requests[2].signature = "iiuu";
	g_params_requests[2].types = g_dmabuf_types + 0;
	g_params_requests[3].name = "create_immed";
	g_params_requests[3].signature = "2niiuu";
	g_params_requests[3].types = g_dmabuf_types + 0;

	g_params_events[0].name = "created";
	g_params_events[0].signature = "n";
	g_params_events[0].types = g_dmabuf_types + 15;
	g_params_events[1].name = "failed";
	g_params_events[1].signature = "";
	g_params_events[1].types = g_dmabuf_types + 0;

	g_params_iface.name = "zwp_linux_buffer_params_v1";
	g_params_iface.version = 5;
	g_params_iface.method_count = 4;
	g_params_iface.methods = g_params_requests;
	g_params_iface.event_count = 2;
	g_params_iface.events = g_params_events;

	/* ---------------- xdg-shell ---------------- */
	for (i = 0; i < 32; i++)
		g_xdg_types[i] = NULL;
	g_xdg_types[4] = &g_positioner_iface;
	g_xdg_types[5] = &g_xdg_surface_iface;
	g_xdg_types[6] = g_wl.iface_surface;
	g_xdg_types[7] = &g_toplevel_iface;
	g_xdg_types[8] = &g_popup_iface;
	g_xdg_types[9] = &g_xdg_surface_iface;
	g_xdg_types[10] = &g_positioner_iface;
	g_xdg_types[11] = &g_toplevel_iface;
	g_xdg_types[24] = &g_positioner_iface;

	g_wm_base_requests[0].name = "destroy";
	g_wm_base_requests[0].signature = "";
	g_wm_base_requests[0].types = g_xdg_types + 0;
	g_wm_base_requests[1].name = "create_positioner";
	g_wm_base_requests[1].signature = "n";
	g_wm_base_requests[1].types = g_xdg_types + 4;
	g_wm_base_requests[2].name = "get_xdg_surface";
	g_wm_base_requests[2].signature = "no";
	g_wm_base_requests[2].types = g_xdg_types + 5;
	g_wm_base_requests[3].name = "pong";
	g_wm_base_requests[3].signature = "u";
	g_wm_base_requests[3].types = g_xdg_types + 0;

	g_wm_base_events[0].name = "ping";
	g_wm_base_events[0].signature = "u";
	g_wm_base_events[0].types = g_xdg_types + 0;

	g_wm_base_iface.name = "xdg_wm_base";
	g_wm_base_iface.version = 7;
	g_wm_base_iface.method_count = 4;
	g_wm_base_iface.methods = g_wm_base_requests;
	g_wm_base_iface.event_count = 1;
	g_wm_base_iface.events = g_wm_base_events;

	g_positioner_requests[0].name = "destroy";
	g_positioner_requests[0].signature = "";
	g_positioner_requests[0].types = g_xdg_types + 0;
	g_positioner_requests[1].name = "set_size";
	g_positioner_requests[1].signature = "ii";
	g_positioner_requests[1].types = g_xdg_types + 0;
	g_positioner_requests[2].name = "set_anchor_rect";
	g_positioner_requests[2].signature = "iiii";
	g_positioner_requests[2].types = g_xdg_types + 0;
	g_positioner_requests[3].name = "set_anchor";
	g_positioner_requests[3].signature = "u";
	g_positioner_requests[3].types = g_xdg_types + 0;
	g_positioner_requests[4].name = "set_gravity";
	g_positioner_requests[4].signature = "u";
	g_positioner_requests[4].types = g_xdg_types + 0;
	g_positioner_requests[5].name = "set_constraint_adjustment";
	g_positioner_requests[5].signature = "u";
	g_positioner_requests[5].types = g_xdg_types + 0;
	g_positioner_requests[6].name = "set_offset";
	g_positioner_requests[6].signature = "ii";
	g_positioner_requests[6].types = g_xdg_types + 0;
	g_positioner_requests[7].name = "set_reactive";
	g_positioner_requests[7].signature = "3";
	g_positioner_requests[7].types = g_xdg_types + 0;
	g_positioner_requests[8].name = "set_parent_size";
	g_positioner_requests[8].signature = "3ii";
	g_positioner_requests[8].types = g_xdg_types + 0;
	g_positioner_requests[9].name = "set_parent_configure";
	g_positioner_requests[9].signature = "3u";
	g_positioner_requests[9].types = g_xdg_types + 0;

	g_positioner_iface.name = "xdg_positioner";
	g_positioner_iface.version = 7;
	g_positioner_iface.method_count = 10;
	g_positioner_iface.methods = g_positioner_requests;
	g_positioner_iface.event_count = 0;
	g_positioner_iface.events = NULL;

	g_xdg_surface_requests[0].name = "destroy";
	g_xdg_surface_requests[0].signature = "";
	g_xdg_surface_requests[0].types = g_xdg_types + 0;
	g_xdg_surface_requests[1].name = "get_toplevel";
	g_xdg_surface_requests[1].signature = "n";
	g_xdg_surface_requests[1].types = g_xdg_types + 7;
	g_xdg_surface_requests[2].name = "get_popup";
	g_xdg_surface_requests[2].signature = "n?oo";
	g_xdg_surface_requests[2].types = g_xdg_types + 8;
	g_xdg_surface_requests[3].name = "set_window_geometry";
	g_xdg_surface_requests[3].signature = "iiii";
	g_xdg_surface_requests[3].types = g_xdg_types + 0;
	g_xdg_surface_requests[4].name = "ack_configure";
	g_xdg_surface_requests[4].signature = "u";
	g_xdg_surface_requests[4].types = g_xdg_types + 0;

	g_xdg_surface_events[0].name = "configure";
	g_xdg_surface_events[0].signature = "u";
	g_xdg_surface_events[0].types = g_xdg_types + 0;

	g_xdg_surface_iface.name = "xdg_surface";
	g_xdg_surface_iface.version = 7;
	g_xdg_surface_iface.method_count = 5;
	g_xdg_surface_iface.methods = g_xdg_surface_requests;
	g_xdg_surface_iface.event_count = 1;
	g_xdg_surface_iface.events = g_xdg_surface_events;

	g_toplevel_requests[0].name = "destroy";
	g_toplevel_requests[0].signature = "";
	g_toplevel_requests[0].types = g_xdg_types + 0;
	g_toplevel_requests[1].name = "set_parent";
	g_toplevel_requests[1].signature = "?o";
	g_toplevel_requests[1].types = g_xdg_types + 11;
	g_toplevel_requests[2].name = "set_title";
	g_toplevel_requests[2].signature = "s";
	g_toplevel_requests[2].types = g_xdg_types + 0;
	g_toplevel_requests[3].name = "set_app_id";
	g_toplevel_requests[3].signature = "s";
	g_toplevel_requests[3].types = g_xdg_types + 0;
	g_toplevel_requests[4].name = "show_window_menu";
	g_toplevel_requests[4].signature = "ouii";
	g_toplevel_requests[4].types = g_xdg_types + 12;
	g_toplevel_requests[5].name = "move";
	g_toplevel_requests[5].signature = "ou";
	g_toplevel_requests[5].types = g_xdg_types + 16;
	g_toplevel_requests[6].name = "resize";
	g_toplevel_requests[6].signature = "ouu";
	g_toplevel_requests[6].types = g_xdg_types + 18;
	g_toplevel_requests[7].name = "set_max_size";
	g_toplevel_requests[7].signature = "ii";
	g_toplevel_requests[7].types = g_xdg_types + 0;
	g_toplevel_requests[8].name = "set_min_size";
	g_toplevel_requests[8].signature = "ii";
	g_toplevel_requests[8].types = g_xdg_types + 0;
	g_toplevel_requests[9].name = "set_maximized";
	g_toplevel_requests[9].signature = "";
	g_toplevel_requests[9].types = g_xdg_types + 0;
	g_toplevel_requests[10].name = "unset_maximized";
	g_toplevel_requests[10].signature = "";
	g_toplevel_requests[10].types = g_xdg_types + 0;
	g_toplevel_requests[11].name = "set_fullscreen";
	g_toplevel_requests[11].signature = "?o";
	g_toplevel_requests[11].types = g_xdg_types + 21;
	g_toplevel_requests[12].name = "unset_fullscreen";
	g_toplevel_requests[12].signature = "";
	g_toplevel_requests[12].types = g_xdg_types + 0;
	g_toplevel_requests[13].name = "set_minimized";
	g_toplevel_requests[13].signature = "";
	g_toplevel_requests[13].types = g_xdg_types + 0;

	g_toplevel_events[0].name = "configure";
	g_toplevel_events[0].signature = "iia";
	g_toplevel_events[0].types = g_xdg_types + 0;
	g_toplevel_events[1].name = "close";
	g_toplevel_events[1].signature = "";
	g_toplevel_events[1].types = g_xdg_types + 0;
	g_toplevel_events[2].name = "configure_bounds";
	g_toplevel_events[2].signature = "4ii";
	g_toplevel_events[2].types = g_xdg_types + 0;
	g_toplevel_events[3].name = "wm_capabilities";
	g_toplevel_events[3].signature = "5a";
	g_toplevel_events[3].types = g_xdg_types + 0;

	g_toplevel_iface.name = "xdg_toplevel";
	g_toplevel_iface.version = 7;
	g_toplevel_iface.method_count = 14;
	g_toplevel_iface.methods = g_toplevel_requests;
	g_toplevel_iface.event_count = 4;
	g_toplevel_iface.events = g_toplevel_events;

	g_popup_requests[0].name = "destroy";
	g_popup_requests[0].signature = "";
	g_popup_requests[0].types = g_xdg_types + 0;
	g_popup_requests[1].name = "grab";
	g_popup_requests[1].signature = "ou";
	g_popup_requests[1].types = g_xdg_types + 22;
	g_popup_requests[2].name = "reposition";
	g_popup_requests[2].signature = "3ou";
	g_popup_requests[2].types = g_xdg_types + 24;

	g_popup_events[0].name = "configure";
	g_popup_events[0].signature = "iiii";
	g_popup_events[0].types = g_xdg_types + 0;
	g_popup_events[1].name = "popup_done";
	g_popup_events[1].signature = "";
	g_popup_events[1].types = g_xdg_types + 0;
	g_popup_events[2].name = "repositioned";
	g_popup_events[2].signature = "3u";
	g_popup_events[2].types = g_xdg_types + 0;

	g_popup_iface.name = "xdg_popup";
	g_popup_iface.version = 7;
	g_popup_iface.method_count = 3;
	g_popup_iface.methods = g_popup_requests;
	g_popup_iface.event_count = 3;
	g_popup_iface.events = g_popup_events;
}

/* ===================================================================== */
/* 7. The module handle                                                 */
/* ===================================================================== */

/*
 * The Wayland listener structures.  libwayland keeps the POINTER that
 * wl_proxy_add_listener() is given, so they must outlive the call that
 * registers them: they live inside the module handle (see below), never on
 * the stack.
 */
struct gfx_registry_listener {
	void (*global)(void *data, void *registry, uint32_t name,
		       const char *interface, uint32_t version);
	void (*global_remove)(void *data, void *registry, uint32_t name);
};

struct gfx_dmabuf_listener {
	void (*format)(void *data, void *dmabuf, uint32_t format);
	void (*modifier)(void *data, void *dmabuf, uint32_t format,
			 uint32_t modifier_hi, uint32_t modifier_lo);
};

struct gfx_params_listener {
	void (*created)(void *data, void *params, void *buffer);
	void (*failed)(void *data, void *params);
};

struct gfx_buffer_listener {
	void (*release)(void *data, void *buffer);
};

struct gfx_xdg_surface_listener {
	void (*configure)(void *data, void *xdg_surface, uint32_t serial);
};

struct gfx_toplevel_listener {
	void (*configure)(void *data, void *toplevel, int32_t w, int32_t h, void *array);
	void (*close)(void *data, void *toplevel);
};

#define GFX_BUF_SLOTS 4

struct gfx_buf_slot {
	struct gfx_present *owner;
	struct wl_proxy *proxy;
	int current;    /* attached to the surface right now */
	int released;   /* the compositor released it               */
};

struct gfx_present {
	/* --- DRM / GBM / EGL --- */
	int drm_fd;
	int primary_fd;
	char node[320];
	char primary[320];
	char driver[64];
	struct gbm_device *gbm;
	struct gbm_surface *surface;
	EGLDisplay dpy;
	EGLConfig cfg;
	EGLContext ctx;
	EGLSurface egl_surf;      /* the module's own window surface */
	EGLSurface egl_swap_surf; /* the surface gfx_present_frame() swaps */
	int width, height;
	uint32_t usage;
	char renderer[192];
	char egl_version[64];

	/* --- Wayland --- */
	struct wl_display *wl_display;
	int wl_owns_display;
	struct wl_proxy *wl_compositor;
	struct wl_proxy *wl_dmabuf;
	struct wl_proxy *wl_surface;
	int wl_owns_surface;
	struct wl_proxy *wl_xdg_wm_base;
	struct wl_proxy *wl_xdg_surface;
	struct wl_proxy *wl_xdg_toplevel;
	struct wl_proxy *wl_buffer_new;   /* set by the "created" event */
	/*
	 * The listener structures MUST outlive the call that registers them:
	 * libwayland keeps the POINTER that wl_proxy_add_listener() is given.
	 * They therefore live inside the module, not on the stack.
	 */
	struct gfx_registry_listener reg_listener;
	struct gfx_dmabuf_listener dm_listener;
	struct gfx_params_listener par_listener;
	struct gfx_buffer_listener buf_listener;
	struct gfx_xdg_surface_listener xs_listener;
	struct gfx_toplevel_listener tl_listener;
	uint32_t reg_comp_name, reg_comp_version;
	uint32_t reg_dmabuf_name, reg_dmabuf_version;
	uint32_t reg_wm_base_name, reg_wm_base_version;
	uint32_t compositor_version;
	uint32_t dmabuf_version;
	uint32_t formats[128];
	int nformats;
	int argb8888_supported;
	int wl_ready;
	int released;
	char compositor[320];
	struct gfx_buf_slot slots[GFX_BUF_SLOTS];
	int slot_next;

	/* --- the last frame --- */
	int dmabuf_fd;
	uint32_t fb_id;
	char fb_method[400];
	uint32_t bo_w, bo_h, bo_stride;
	uint64_t modifier;

	/* --- copy accounting --- */
	unsigned long cp_calls, cp_bytes, cp_big_calls, cp_big_bytes;
	unsigned long cp_last_calls, cp_last_bytes, cp_last_big_calls, cp_last_big_bytes;
};

/* ===================================================================== */
/* 8. Listeners                                                          */
/* ===================================================================== */

static void gfx_reg_global(void *data, void *registry, uint32_t name,
			   const char *interface, uint32_t version)
{
	struct gfx_present *p = data;

	(void)registry;
	if (!interface)
		return;
	if (!strcmp(interface, "wl_compositor")) {
		p->reg_comp_name = name;
		p->reg_comp_version = version;
	} else if (!strcmp(interface, "zwp_linux_dmabuf_v1")) {
		p->reg_dmabuf_name = name;
		p->reg_dmabuf_version = version;
	} else if (!strcmp(interface, "xdg_wm_base")) {
		p->reg_wm_base_name = name;
		p->reg_wm_base_version = version;
	}
}

static void gfx_reg_global_remove(void *data, void *registry, uint32_t name)
{
	(void)data; (void)registry; (void)name;
}

static void gfx_dmabuf_format(void *data, void *dmabuf, uint32_t format)
{
	struct gfx_present *p = data;

	(void)dmabuf;
	if (p->nformats < (int)(sizeof p->formats / sizeof p->formats[0]))
		p->formats[p->nformats++] = format;
	if (format == GFX_DRM_FORMAT_ARGB8888)
		p->argb8888_supported = 1;
}

static void gfx_dmabuf_modifier(void *data, void *dmabuf, uint32_t format,
				 uint32_t hi, uint32_t lo)
{
	(void)data; (void)dmabuf; (void)format; (void)hi; (void)lo;
}

static void gfx_params_created(void *data, void *params, void *buffer)
{
	struct gfx_present *p = data;

	(void)params;
	/* The compositor imported the dma-buf and built a wl_buffer out of it.
	 * The "release" event of that buffer is the proof that the compositor
	 * really rendered the frame. */
	p->wl_buffer_new = (struct wl_proxy *)buffer;
}

static void gfx_params_failed(void *data, void *params)
{
	struct gfx_present *p = data;

	(void)params;
	p->wl_buffer_new = NULL;
	gfx_errf("the compositor rejected the dma-buf "
		 "(zwp_linux_buffer_params_v1.failed)");
}

static void gfx_buffer_release(void *data, void *buffer)
{
	struct gfx_buf_slot *s = data;

	(void)buffer;
	s->released = 1;
	if (s->current && s->owner)
		s->owner->released = 1;
	s->current = 0;
}

static void gfx_xdg_surface_configure(void *data, void *xdg_surface, uint32_t serial)
{
	(void)data;
	/* xdg_surface.ack_configure(serial) */
	g_wl.proxy_marshal_flags((struct wl_proxy *)xdg_surface,
				 GFX_XDG_SURFACE_ACK_CONFIGURE, NULL, 1, 0, serial);
}

static void gfx_toplevel_configure(void *data, void *toplevel, int32_t w,
				   int32_t h, void *array)
{
	(void)data; (void)toplevel; (void)w; (void)h; (void)array;
}

static void gfx_toplevel_close(void *data, void *toplevel)
{
	(void)data; (void)toplevel;
}

/* ===================================================================== */
/* 9. Binding the runtime libraries                                      */
/* ===================================================================== */

static int gfx_gbm_bind(void)
{
	char e[256];

	if (g_gbm.lib)
		return 0;
	g_gbm.lib = gfx_dlopen("libgbm.so.1");
	if (!g_gbm.lib)
		g_gbm.lib = gfx_dlopen("libgbm.so");
	if (!g_gbm.lib)
		return -1;

#define BIND(field, sym)                                                       \
	do {                                                                   \
		*(void **)(&g_gbm.field) = gfx_dlsym_opt(g_gbm.lib, sym);      \
		if (!g_gbm.field) {                                            \
			snprintf(e, sizeof e, "libgbm: %s missing", sym);      \
			gfx_errf("%s", e);                                     \
			return -1;                                            \
		}                                                              \
	} while (0)

	BIND(create_device, "gbm_create_device");
	BIND(device_destroy, "gbm_device_destroy");
	BIND(surface_create, "gbm_surface_create");
	BIND(surface_destroy, "gbm_surface_destroy");
	BIND(lock_front_buffer, "gbm_surface_lock_front_buffer");
	BIND(bo_get_handle, "gbm_bo_get_handle");
	BIND(bo_get_stride, "gbm_bo_get_stride");
	BIND(bo_get_width, "gbm_bo_get_width");
	BIND(bo_get_height, "gbm_bo_get_height");
	BIND(bo_destroy, "gbm_bo_destroy");
#undef BIND
	/* optional */
	g_gbm.bo_get_modifier = (uint64_t (*)(struct gbm_bo *))
		gfx_dlsym_opt(g_gbm.lib, "gbm_bo_get_modifier");
	return 0;
}

static int gfx_egl_bind(void)
{
	char e[256];

	if (g_egl.lib)
		return 0;
	g_egl.lib = gfx_dlopen("libEGL.so.1");
	if (!g_egl.lib)
		g_egl.lib = gfx_dlopen("libEGL.so");
	if (!g_egl.lib)
		return -1;

#define BIND(field, sym)                                                       \
	do {                                                                   \
		*(void **)(&g_egl.field) = gfx_dlsym_opt(g_egl.lib, sym);      \
		if (!g_egl.field) {                                            \
			snprintf(e, sizeof e, "libEGL: %s missing", sym);      \
			gfx_errf("%s", e);                                     \
			return -1;                                            \
		}                                                              \
	} while (0)

	BIND(initialize, "eglInitialize");
	BIND(bind_api, "eglBindAPI");
	BIND(choose_config, "eglChooseConfig");
	BIND(create_context, "eglCreateContext");
	BIND(create_window_surface, "eglCreateWindowSurface");
	BIND(make_current, "eglMakeCurrent");
	BIND(destroy_surface, "eglDestroySurface");
	BIND(destroy_context, "eglDestroyContext");
	BIND(swap_buffers, "eglSwapBuffers");
	BIND(get_error, "eglGetError");
	BIND(query_string, "eglQueryString");
#undef BIND
	/* optional */
	g_egl.terminate = (EGLBoolean (*)(EGLDisplay))
		gfx_dlsym_opt(g_egl.lib, "eglTerminate");
	g_egl.get_display = (EGLDisplay (*)(EGLNativeDisplayType))
		gfx_dlsym_opt(g_egl.lib, "eglGetDisplay");
	g_egl.get_platform_display =
		(EGLDisplay (*)(EGLenum, void *, const EGLAttrib *))
		gfx_dlsym_opt(g_egl.lib, "eglGetPlatformDisplay");
	if (!g_egl.get_platform_display)
		g_egl.get_platform_display =
			(EGLDisplay (*)(EGLenum, void *, const EGLAttrib *))
			gfx_dlsym_opt(g_egl.lib, "eglGetPlatformDisplayEXT");
	return 0;
}

static int gfx_wl_bind(void)
{
	char e[256];

	if (g_wl.lib)
		return 0;
	g_wl.lib = gfx_dlopen("libwayland-client.so.0");
	if (!g_wl.lib)
		g_wl.lib = gfx_dlopen("libwayland-client.so");
	if (!g_wl.lib)
		return -1;

#define BIND(field, sym)                                                       \
	do {                                                                   \
		*(void **)(&g_wl.field) = gfx_dlsym_opt(g_wl.lib, sym);       \
		if (!g_wl.field) {                                            \
			snprintf(e, sizeof e, "libwayland-client: %s missing", sym); \
			gfx_errf("%s", e);                                     \
			return -1;                                            \
		}                                                              \
	} while (0)

	BIND(display_connect, "wl_display_connect");
	BIND(display_disconnect, "wl_display_disconnect");
	BIND(display_roundtrip, "wl_display_roundtrip");
	BIND(display_get_error, "wl_display_get_error");
	BIND(display_flush, "wl_display_flush");
	BIND(display_dispatch_pending, "wl_display_dispatch_pending");
	BIND(display_prepare_read, "wl_display_prepare_read");
	BIND(display_read_events, "wl_display_read_events");
	BIND(display_get_fd, "wl_display_get_fd");
	BIND(proxy_marshal_flags, "wl_proxy_marshal_flags");
	BIND(proxy_add_listener, "wl_proxy_add_listener");
	BIND(proxy_get_version, "wl_proxy_get_version");
#undef BIND
	g_wl.proxy_destroy = (void (*)(struct wl_proxy *))
		gfx_dlsym_opt(g_wl.lib, "wl_proxy_destroy");

	g_wl.iface_registry = (const struct wl_interface *)
		gfx_dlsym_opt(g_wl.lib, "wl_registry_interface");
	g_wl.iface_compositor = (const struct wl_interface *)
		gfx_dlsym_opt(g_wl.lib, "wl_compositor_interface");
	g_wl.iface_surface = (const struct wl_interface *)
		gfx_dlsym_opt(g_wl.lib, "wl_surface_interface");
	g_wl.iface_buffer = (const struct wl_interface *)
		gfx_dlsym_opt(g_wl.lib, "wl_buffer_interface");
	if (!g_wl.iface_registry || !g_wl.iface_compositor ||
	    !g_wl.iface_surface || !g_wl.iface_buffer) {
		gfx_errf("libwayland-client: the core interface tables are missing");
		return -1;
	}
	gfx_protocol_tables_init();
	return 0;
}

/* ===================================================================== */
/* 10. DRM helpers                                                       */
/* ===================================================================== */

static int gfx_drm_version(int fd, char *driver, size_t drivern)
{
	struct gfx_drm_version v;
	char name[256];

	memset(&v, 0, sizeof v);
	memset(name, 0, sizeof name);
	v.name_len = sizeof name - 1;
	v.date_len = 0;
	v.desc_len = 0;
	v.name = name;
	v.date = name;
	v.desc = name;

	if (ioctl(fd, GFX_DRM_IOCTL_VERSION, &v) < 0)
		return -1;
	if (driver && drivern)
		snprintf(driver, drivern, "%s", name[0] ? name : "?");
	return 0;
}

static int gfx_drm_prime_to_fd(int fd, uint32_t handle)
{
	struct gfx_drm_prime_handle ph;

	memset(&ph, 0, sizeof ph);
	ph.handle = handle;
	ph.flags = 0;
	ph.fd = -1;
	if (ioctl(fd, GFX_DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph) < 0)
		return -1;
	return ph.fd;
}

static int gfx_drm_fd_to_handle(int fd, int dmabuf_fd, uint32_t *handle)
{
	struct gfx_drm_prime_handle ph;

	memset(&ph, 0, sizeof ph);
	ph.handle = 0;
	ph.flags = 0;
	ph.fd = dmabuf_fd;
	if (ioctl(fd, GFX_DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph) < 0)
		return -1;
	*handle = ph.handle;
	return 0;
}

/* Find a DRM node: render nodes first, primary nodes as a fallback. */
static int gfx_open_drm_node(char *node, size_t noden)
{
	DIR *dir;
	struct dirent *de;
	char best[256];
	int pass, fd;

	for (pass = 0; pass < 2; pass++) {
		const char *pfx = pass == 0 ? "renderD" : "card";

		best[0] = 0;
		dir = opendir("/dev/dri");
		if (!dir)
			continue;
		while ((de = readdir(dir)) != NULL) {
			if (strncmp(de->d_name, pfx, strlen(pfx)) != 0)
				continue;
			if (de->d_name[strlen(pfx)] == 0)
				continue;
			if (!best[0] || strcmp(de->d_name, best) < 0)
				snprintf(best, sizeof best, "%s", de->d_name);
		}
		closedir(dir);
		if (!best[0])
			continue;
		{
			char path[320];

			snprintf(path, sizeof path, "/dev/dri/%s", best);
			fd = open(path, O_RDWR | O_CLOEXEC);
			if (fd >= 0) {
				snprintf(node, noden, "%s", path);
				return fd;
			}
		}
	}
	return -1;
}

/* The KMS node is only needed to register the framebuffer (AddFB). */
static int gfx_open_primary_node(char *node, size_t noden)
{
	DIR *dir;
	struct dirent *de;
	int fd = -1;

	dir = opendir("/dev/dri");
	if (!dir)
		return -1;
	while ((de = readdir(dir)) != NULL) {
		char path[320];

		if (strncmp(de->d_name, "card", 4) != 0)
			continue;
		snprintf(path, sizeof path, "/dev/dri/%s", de->d_name);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd >= 0) {
			snprintf(node, noden, "%s", path);
			break;
		}
	}
	closedir(dir);
	return fd;
}

/* ===================================================================== */
/* 11. create / destroy                                                  */
/* ===================================================================== */

static void gfx_default_size(int *w, int *h)
{
	const char *ew = getenv("NTBOXD_FB_WIDTH");
	const char *eh = getenv("NTBOXD_FB_HEIGHT");

	*w = 640;
	*h = 480;
	if (ew && *ew) {
		int v = atoi(ew);

		if (v > 0)
			*w = v;
	}
	if (eh && *eh) {
		int v = atoi(eh);

		if (v > 0)
			*h = v;
	}
}

/* (Re)create the GBM surface and the EGL window surface on it. */
static int gfx_make_target(struct gfx_present *p, int w, int h)
{
	EGLint cfg_attrs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 0,
		EGL_NONE
	};
	EGLint ncfg = 0;

	if (p->egl_surf) {
		g_egl.make_current(p->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		g_egl.destroy_surface(p->dpy, p->egl_surf);
		p->egl_surf = NULL;
		p->egl_swap_surf = NULL;
	}
	if (p->surface) {
		g_gbm.surface_destroy(p->surface);
		p->surface = NULL;
	}

	p->surface = g_gbm.surface_create(p->gbm, w, h, GFX_DRM_FORMAT_ARGB8888,
					  p->usage);
	if (!p->surface) {
		gfx_errf("gbm_surface_create(%dx%d, ARGB8888, usage 0x%x) failed",
			 w, h, p->usage);
		return -1;
	}
	if (!g_egl.choose_config(p->dpy, cfg_attrs, &p->cfg, 1, &ncfg) || ncfg < 1) {
		gfx_errf("eglChooseConfig found no EGL_WINDOW_BIT config");
		return -1;
	}
	p->egl_surf = g_egl.create_window_surface(p->dpy, p->cfg, p->surface, NULL);
	if (!p->egl_surf) {
		gfx_errf("eglCreateWindowSurface failed (EGL error 0x%x)",
			 g_egl.get_error ? g_egl.get_error() : 0);
		return -1;
	}
	p->egl_swap_surf = p->egl_surf;
	p->width = w;
	p->height = h;
	return 0;
}

gfx_present_t *gfx_present_create(void)
{
	struct gfx_present *p;
	EGLint maj = 0, min = 0;
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	int w, h;

	g_gfx_err[0] = 0;

	/* --- is there a GPU at all? --- */
	{
		struct stat st;

		if (stat("/dev/dri", &st) < 0) {
			gfx_errf("no /dev/dri on this machine (%s) — the GPU path "
				 "is unavailable, fall back to the CPU framebuffer",
				 strerror(errno));
			return NULL;
		}
	}

	p = calloc(1, sizeof *p);
	if (!p) {
		gfx_errf("out of memory");
		return NULL;
	}
	p->drm_fd = -1;
	p->primary_fd = -1;
	p->dmabuf_fd = -1;
	p->usage = GFX_GBM_BO_USE_LINEAR | GFX_GBM_BO_USE_SCANOUT;
	gfx_default_size(&w, &h);

	/* --- the DRM render node --- */
	p->drm_fd = gfx_open_drm_node(p->node, sizeof p->node);
	if (p->drm_fd < 0) {
		gfx_errf("no usable DRM node in /dev/dri — the GPU path is "
			 "unavailable, fall back to the CPU framebuffer");
		goto fail;
	}
	gfx_drm_version(p->drm_fd, p->driver, sizeof p->driver);

	/* --- GBM --- */
	if (gfx_gbm_bind() < 0)
		goto fail;
	p->gbm = g_gbm.create_device(p->drm_fd);
	if (!p->gbm) {
		gfx_errf("gbm_create_device(%s) failed", p->node);
		goto fail;
	}

	/* --- EGL --- */
	if (gfx_egl_bind() < 0)
		goto fail;
	if (g_egl.get_platform_display)
		p->dpy = g_egl.get_platform_display(EGL_PLATFORM_GBM_KHR, p->gbm, NULL);
	if (!p->dpy && g_egl.get_display)
		p->dpy = g_egl.get_display((EGLNativeDisplayType)p->gbm);
	if (!p->dpy || !g_egl.initialize(p->dpy, &maj, &min)) {
		gfx_errf("eglInitialize on the GBM display failed (EGL error 0x%x)",
			 g_egl.get_error ? g_egl.get_error() : 0);
		goto fail;
	}
	if (g_egl.query_string) {
		const char *v = g_egl.query_string(p->dpy, EGL_VERSION);

		if (v)
			snprintf(p->egl_version, sizeof p->egl_version, "%s", v);
	}
	g_egl.bind_api(EGL_OPENGL_ES_API);

	if (gfx_make_target(p, w, h) < 0)
		goto fail;

	p->ctx = g_egl.create_context(p->dpy, p->cfg, NULL, ctx_attrs);
	if (!p->ctx) {
		gfx_errf("eglCreateContext failed (EGL error 0x%x)",
			 g_egl.get_error ? g_egl.get_error() : 0);
		goto fail;
	}
	return p;

fail:
	gfx_present_destroy(p);
	return NULL;
}

void gfx_present_destroy(gfx_present_t *p)
{
	int i;

	if (!p)
		return;
	if (p->wl_display) {
		for (i = 0; i < GFX_BUF_SLOTS; i++) {
			if (p->slots[i].proxy && g_wl.proxy_destroy)
				g_wl.proxy_destroy(p->slots[i].proxy);
			p->slots[i].proxy = NULL;
		}
		if (p->wl_xdg_toplevel && g_wl.proxy_destroy)
			g_wl.proxy_destroy(p->wl_xdg_toplevel);
		if (p->wl_xdg_surface && g_wl.proxy_destroy)
			g_wl.proxy_destroy(p->wl_xdg_surface);
		if (p->wl_surface && p->wl_owns_surface && g_wl.proxy_destroy)
			g_wl.proxy_destroy(p->wl_surface);
		if (p->wl_dmabuf && g_wl.proxy_destroy)
			g_wl.proxy_destroy(p->wl_dmabuf);
		if (p->wl_compositor && g_wl.proxy_destroy)
			g_wl.proxy_destroy(p->wl_compositor);
		if (p->wl_owns_display && g_wl.display_disconnect)
			g_wl.display_disconnect(p->wl_display);
	}
	if (p->egl_surf && g_egl.destroy_surface)
		g_egl.destroy_surface(p->dpy, p->egl_surf);
	if (p->ctx && g_egl.destroy_context)
		g_egl.destroy_context(p->dpy, p->ctx);
	if (p->dpy && g_egl.terminate)
		g_egl.terminate(p->dpy);
	if (p->surface && g_gbm.surface_destroy)
		g_gbm.surface_destroy(p->surface);
	if (p->gbm && g_gbm.device_destroy)
		g_gbm.device_destroy(p->gbm);
	if (p->primary_fd >= 0)
		close(p->primary_fd);
	if (p->dmabuf_fd >= 0)
		close(p->dmabuf_fd);
	if (p->drm_fd >= 0)
		close(p->drm_fd);
	free(p);
}

/* ===================================================================== */
/* 12. The public accessors                                              */
/* ===================================================================== */

const char *gfx_present_drm_node(const gfx_present_t *p)
{
	return p ? p->node : "";
}

const char *gfx_present_drm_driver(const gfx_present_t *p)
{
	return p ? p->driver : "";
}

void gfx_present_target_size(const gfx_present_t *p, int *w, int *h)
{
	if (w)
		*w = p ? p->width : 0;
	if (h)
		*h = p ? p->height : 0;
}

int gfx_present_resize(gfx_present_t *p, int w, int h)
{
	if (!p || w <= 0 || h <= 0) {
		gfx_errf("gfx_present_resize: bad size %dx%d", w, h);
		return -1;
	}
	if (p->width == w && p->height == h)
		return 0;
	return gfx_make_target(p, w, h);
}

void *gfx_present_egl(const gfx_present_t *p) { return p ? p->dpy : NULL; }
void *gfx_present_egl_config(const gfx_present_t *p) { return p ? p->cfg : NULL; }
void *gfx_present_egl_context(const gfx_present_t *p) { return p ? p->ctx : NULL; }
void *gfx_present_egl_surface(const gfx_present_t *p) { return p ? p->egl_surf : NULL; }
void *gfx_present_egl_window(const gfx_present_t *p) { return p ? p->surface : NULL; }

int gfx_present_set_egl_surface(gfx_present_t *p, void *egl_surface)
{
	if (!p) {
		gfx_errf("gfx_present_set_egl_surface: no module");
		return -1;
	}
	p->egl_swap_surf = egl_surface ? (EGLSurface)egl_surface : p->egl_surf;
	return 0;
}

int gfx_present_make_current(gfx_present_t *p)
{
	if (!p || !p->dpy) {
		gfx_errf("gfx_present_make_current: no module");
		return -1;
	}
	if (!g_egl.make_current(p->dpy, p->egl_surf, p->egl_surf, p->ctx)) {
		gfx_errf("eglMakeCurrent failed (EGL error 0x%x)",
			 g_egl.get_error ? g_egl.get_error() : 0);
		return -1;
	}
	/* Best effort: remember which GPU we are drawing on. */
	{
		void *gl = dlopen("libGLESv2.so.2", RTLD_LAZY | RTLD_LOCAL);

		if (gl) {
			const unsigned char *(*getstring)(unsigned int) =
				(const unsigned char *(*)(unsigned int))
				dlsym(gl, "glGetString");

			if (getstring) {
				const char *r = (const char *)getstring(0x1F01);

				if (r)
					snprintf(p->renderer, sizeof p->renderer, "%s", r);
			}
			dlclose(gl);
		}
	}
	return 0;
}

int gfx_present_release_current(gfx_present_t *p)
{
	if (!p || !p->dpy)
		return -1;
	return g_egl.make_current(p->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) ? 0 : -1;
}

const char *gfx_present_renderer(const gfx_present_t *p)
{
	return p ? p->renderer : "";
}

/* ===================================================================== */
/* 13. The Wayland hand-off                                              */
/* ===================================================================== */

/*
 * Bind the globals the module needs on an existing wl_display and, when the
 * module owns the surface, give it an xdg-shell role so that the compositor
 * really renders it (and releases the buffer afterwards).
 */
static int gfx_wl_setup(struct gfx_present *p, int own_surface)
{
	struct wl_proxy *registry;
	uint32_t comp_version;

	registry = g_wl.proxy_marshal_flags((struct wl_proxy *)p->wl_display,
					    GFX_WL_DISPLAY_GET_REGISTRY,
					    g_wl.iface_registry, 1, 0, NULL);
	if (!registry) {
		gfx_errf("wl_display.get_registry failed");
		return -1;
	}
	p->reg_listener.global = gfx_reg_global;
	p->reg_listener.global_remove = gfx_reg_global_remove;
	g_wl.proxy_add_listener(registry, (void (**)(void))&p->reg_listener, p);
	if (g_wl.display_roundtrip(p->wl_display) < 0) {
		gfx_errf("wl_display.roundtrip failed");
		return -1;
	}
	if (!p->reg_dmabuf_name) {
		gfx_errf("the compositor does not expose zwp_linux_dmabuf_v1 — "
			 "it cannot import a dma-buf");
		return -1;
	}
	if (own_surface && !p->reg_comp_name) {
		gfx_errf("the compositor does not expose wl_compositor");
		return -1;
	}

	/* zwp_linux_dmabuf_v1 — version 1 is the one every compositor has and
	 * the one that still advertises the supported formats. */
	p->dmabuf_version = 1;
	p->wl_dmabuf = g_wl.proxy_marshal_flags(registry, GFX_WL_REGISTRY_BIND,
						 &g_dmabuf_iface, p->dmabuf_version,
						 0, p->reg_dmabuf_name,
						 "zwp_linux_dmabuf_v1",
						 p->dmabuf_version, NULL);
	if (!p->wl_dmabuf) {
		gfx_errf("wl_registry.bind(zwp_linux_dmabuf_v1) failed");
		return -1;
	}
	p->dm_listener.format = gfx_dmabuf_format;
	p->dm_listener.modifier = gfx_dmabuf_modifier;
	g_wl.proxy_add_listener(p->wl_dmabuf, (void (**)(void))&p->dm_listener, p);
	g_wl.display_roundtrip(p->wl_display);

	if (own_surface) {
		comp_version = p->reg_comp_version < 4 ? p->reg_comp_version : 4;
		p->compositor_version = comp_version;
		p->wl_compositor = g_wl.proxy_marshal_flags(registry,
							     GFX_WL_REGISTRY_BIND,
							     g_wl.iface_compositor,
							     comp_version, 0,
							     p->reg_comp_name,
							     "wl_compositor",
							     comp_version, NULL);
		if (!p->wl_compositor) {
			gfx_errf("wl_registry.bind(wl_compositor) failed");
			return -1;
		}
		p->wl_surface = g_wl.proxy_marshal_flags(p->wl_compositor,
							 GFX_WL_COMPOSITOR_CREATE_SURFACE,
							 g_wl.iface_surface,
							 comp_version, 0, NULL);
		if (!p->wl_surface) {
			gfx_errf("wl_compositor.create_surface failed");
			return -1;
		}
		p->wl_owns_surface = 1;

		/* an xdg-shell role: without it the compositor would never
		 * render the surface, and the "release" event would never
		 * come. */
		if (p->reg_wm_base_name) {
			uint32_t wm_version = p->reg_wm_base_version < 3 ?
					      p->reg_wm_base_version : 3;

			p->wl_xdg_wm_base = g_wl.proxy_marshal_flags(registry,
								      GFX_WL_REGISTRY_BIND,
								      &g_wm_base_iface,
								      wm_version, 0,
								      p->reg_wm_base_name,
								      "xdg_wm_base",
								      wm_version, NULL);
			if (p->wl_xdg_wm_base) {
				p->wl_xdg_surface =
					g_wl.proxy_marshal_flags(p->wl_xdg_wm_base,
								 GFX_XDG_WM_BASE_GET_XDG_SURFACE,
								 &g_xdg_surface_iface,
								 wm_version, 0, NULL,
								 p->wl_surface);
				if (p->wl_xdg_surface) {
					p->xs_listener.configure = gfx_xdg_surface_configure;
					g_wl.proxy_add_listener(p->wl_xdg_surface,
								(void (**)(void))&p->xs_listener,
								p);
					p->wl_xdg_toplevel =
						g_wl.proxy_marshal_flags(p->wl_xdg_surface,
								 GFX_XDG_SURFACE_GET_TOPLEVEL,
								 &g_toplevel_iface,
								 wm_version, 0, NULL);
					if (p->wl_xdg_toplevel) {
						p->tl_listener.configure = gfx_toplevel_configure;
						p->tl_listener.close = gfx_toplevel_close;
						g_wl.proxy_add_listener(p->wl_xdg_toplevel,
									(void (**)(void))&p->tl_listener,
									p);
						g_wl.proxy_marshal_flags(p->wl_xdg_toplevel,
								 GFX_XDG_TOPLEVEL_SET_TITLE,
								 NULL, wm_version, 0,
								 "NT-Box");
						/*
						 * Tell the compositor how big the
						 * window is: without it the
						 * xdg_toplevel configure
						 * carries a 0x0 size and the
						 * compositor never renders
						 * (and never releases) the
						 * buffer.
						 */
					}
				}
			}
			/* the empty commit asks the compositor for the
			 * first configure (which the listener acks) */
			g_wl.proxy_marshal_flags(p->wl_surface,
						 GFX_WL_SURFACE_COMMIT, NULL,
						 comp_version, 0);
			g_wl.display_roundtrip(p->wl_display);
		}
	}

	snprintf(p->compositor, sizeof p->compositor,
		 "%s (wl_compositor v%u, zwp_linux_dmabuf_v1 v%u, %d format(s))",
		 getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "wayland",
		 p->compositor_version, p->dmabuf_version, p->nformats);
	p->wl_ready = 1;
	return 0;
}

int gfx_present_wl_connect(gfx_present_t *p)
{
	const char *name;

	if (!p) {
		gfx_errf("gfx_present_wl_connect: no module");
		return -1;
	}
	if (p->wl_ready)
		return 0;
	if (gfx_wl_bind() < 0)
		return -1;

	name = getenv("WAYLAND_DISPLAY");
	p->wl_display = g_wl.display_connect(name && *name ? name : NULL);
	if (!p->wl_display) {
		gfx_errf("cannot connect to the Wayland display %s",
			 name && *name ? name : "(default)");
		return -1;
	}
	p->wl_owns_display = 1;
	if (gfx_wl_setup(p, 1) < 0) {
		g_wl.display_disconnect(p->wl_display);
		p->wl_display = NULL;
		p->wl_owns_display = 0;
		return -1;
	}
	return 0;
}

int gfx_present_wl_use(gfx_present_t *p, void *wl_display, void *wl_surface)
{
	if (!p || !wl_display || !wl_surface) {
		gfx_errf("gfx_present_wl_use: bad arguments");
		return -1;
	}
	if (p->wl_ready)
		return 0;
	if (gfx_wl_bind() < 0)
		return -1;
	p->wl_display = (struct wl_display *)wl_display;
	p->wl_owns_display = 0;
	p->wl_surface = (struct wl_proxy *)wl_surface;
	p->wl_owns_surface = 0;
	p->compositor_version = 4;
	if (gfx_wl_setup(p, 0) < 0)
		return -1;
	return 0;
}

int gfx_present_wl_ready(const gfx_present_t *p)
{
	return p ? p->wl_ready : 0;
}

void *gfx_present_wl_display(const gfx_present_t *p)
{
	return p ? (void *)p->wl_display : NULL;
}

void *gfx_present_wl_surface(const gfx_present_t *p)
{
	return p ? (void *)p->wl_surface : NULL;
}

const char *gfx_present_compositor(const gfx_present_t *p)
{
	return p ? p->compositor : "";
}

/* ===================================================================== */
/* 14. The frame                                                          */
/* ===================================================================== */

/* Free the wl_buffer proxies the compositor already released. */
static void gfx_sweep_buffers(struct gfx_present *p)
{
	int i;

	for (i = 0; i < GFX_BUF_SLOTS; i++) {
		if (p->slots[i].proxy && p->slots[i].released && !p->slots[i].current) {
			if (g_wl.proxy_destroy)
				g_wl.proxy_destroy(p->slots[i].proxy);
			p->slots[i].proxy = NULL;
		}
	}
}

int gfx_present_frame(gfx_present_t *p)
{
	struct gbm_bo *bo = NULL;
	struct wl_proxy *params = NULL;
	struct gfx_buf_slot *slot;
	int owned_window = 0;
	int fd = -1;
	uint32_t handle;
	int rc = -1;

	if (!p) {
		gfx_errf("gfx_present_frame: no module");
		return -1;
	}
	if (!p->wl_ready) {
		gfx_errf("gfx_present_frame: no Wayland connection — call "
			 "gfx_present_wl_connect() or gfx_present_wl_use() first");
		return -1;
	}
	if (!p->egl_swap_surf) {
		gfx_errf("gfx_present_frame: no EGL surface to swap");
		return -1;
	}

	if (!g_cp_window) {
		gfx_present_copy_window_begin(p);
		owned_window = 1;
	}

	/* ---- 1. the GPU hands the rendered frame back to GBM ---- */
	if (!g_egl.swap_buffers(p->dpy, p->egl_swap_surf)) {
		gfx_errf("eglSwapBuffers failed (EGL error 0x%x)",
			 g_egl.get_error ? g_egl.get_error() : 0);
		goto out;
	}

	/* ---- 2. lock the front buffer and export it as a dma-buf ---- */
	bo = g_gbm.lock_front_buffer(p->surface);
	if (!bo) {
		gfx_errf("gbm_surface_lock_front_buffer failed");
		goto out;
	}
	p->bo_w = (uint32_t)g_gbm.bo_get_width(bo);
	p->bo_h = (uint32_t)g_gbm.bo_get_height(bo);
	p->bo_stride = g_gbm.bo_get_stride(bo);
	p->modifier = g_gbm.bo_get_modifier ?
		      g_gbm.bo_get_modifier(bo) : GFX_DRM_FORMAT_MOD_INVALID;

	handle = g_gbm.bo_get_handle(bo);
	fd = gfx_drm_prime_to_fd(p->drm_fd, handle);
	if (fd < 0) {
		gfx_errf("DRM_IOCTL_PRIME_HANDLE_TO_FD failed: %s", strerror(errno));
		goto out;
	}
	/*
	 * The module keeps its own copy of the descriptor: the compositor gets
	 * its own through the Unix socket (SCM_RIGHTS), and the caller can use
	 * this one to prove what was handed over.  The previous frame's
	 * descriptor is closed here.
	 */
	if (p->dmabuf_fd >= 0)
		close(p->dmabuf_fd);
	p->dmabuf_fd = fd;

	/* ---- 3. register the scanout framebuffer (best effort) ---- */
	/*
	 * The KMS framebuffer of the previous frame is removed first: a box
	 * runs at 60 fps and an unbounded number of registered framebuffers
	 * would be a kernel resource leak.
	 */
	if (p->fb_id && p->primary_fd >= 0) {
		unsigned int old = p->fb_id;

		ioctl(p->primary_fd, GFX_DRM_IOCTL_MODE_RMFB, &old);
		p->fb_id = 0;
	}
	p->fb_id = 0;
	if (p->primary_fd < 0)
		p->primary_fd = gfx_open_primary_node(p->primary, sizeof p->primary);
	if (p->primary_fd >= 0) {
		uint32_t handle = 0;

		/*
		 * The KMS node needs a GEM HANDLE, not a descriptor: the
		 * dma-buf is imported into the handle space of the node that
		 * registers the framebuffer (a DRM handle belongs to the
		 * file that opened it).
		 */
		if (gfx_drm_fd_to_handle(p->primary_fd, fd, &handle) == 0) {
			struct gfx_drm_fb_cmd2 fb2;

			memset(&fb2, 0, sizeof fb2);
			fb2.width = p->bo_w;
			fb2.height = p->bo_h;
			fb2.pixel_format = GFX_DRM_FORMAT_ARGB8888;
			fb2.flags = 0;
			fb2.handles[0] = handle;
			fb2.pitches[0] = p->bo_stride;
			fb2.offsets[0] = 0;
			fb2.modifier[0] = p->modifier;
			if (ioctl(p->primary_fd, GFX_DRM_IOCTL_MODE_ADDFB2, &fb2) == 0) {
				p->fb_id = fb2.fb_id;
				snprintf(p->fb_method, sizeof p->fb_method,
					 "drmModeAddFB2(id=%u) on %s", p->fb_id,
					 p->primary);
			} else {
				struct gfx_drm_fb_cmd fb;

				memset(&fb, 0, sizeof fb);
				fb.width = p->bo_w;
				fb.height = p->bo_h;
				fb.pitch = p->bo_stride;
				fb.bpp = 32;
				fb.depth = 24;
				fb.handle = handle;
				if (ioctl(p->primary_fd, GFX_DRM_IOCTL_MODE_ADDFB, &fb) == 0) {
					p->fb_id = fb.fb_id;
					snprintf(p->fb_method, sizeof p->fb_method,
						 "drmModeAddFB(id=%u) on %s", p->fb_id,
						 p->primary);
				}
			}
		}
	}

	/* ---- 4. hand the dma-buf to the compositor (linux-dmabuf v1) ---- */
	params = g_wl.proxy_marshal_flags(p->wl_dmabuf, 1 /* create_params */,
					  &g_params_iface, p->dmabuf_version, 0, NULL);
	if (!params) {
		gfx_errf("zwp_linux_dmabuf_v1.create_params failed");
		goto out;
	}
	{
		p->par_listener.created = gfx_params_created;
		p->par_listener.failed = gfx_params_failed;
		g_wl.proxy_add_listener(params, (void (**)(void))&p->par_listener, p);
	}
	/* zwp_linux_buffer_params_v1.add(fd, plane, offset, stride, mod_hi, mod_lo) */
	g_wl.proxy_marshal_flags(params, 1 /* add */, NULL, p->dmabuf_version, 0,
				 fd, (uint32_t)0, (uint32_t)0, p->bo_stride,
				 (uint32_t)(p->modifier >> 32),
				 (uint32_t)(p->modifier & 0xffffffffu));
	/* zwp_linux_buffer_params_v1.create(w, h, format, flags) */
	g_wl.proxy_marshal_flags(params, 2 /* create */, NULL, p->dmabuf_version, 0,
				 (int32_t)p->bo_w, (int32_t)p->bo_h,
				 (uint32_t)GFX_DRM_FORMAT_ARGB8888, (uint32_t)0);

	if (g_wl.display_roundtrip(p->wl_display) < 0) {
		gfx_errf("wl_display.roundtrip after the dmabuf create failed");
		goto out;
	}
	/*
	 * linux-dmabuf v1: "Upon receiving this event, the client should
	 * destroy the zwp_linux_buffer_params_v1 object."  Without this the
	 * compositor keeps the imported dma-buf alive and every frame would
	 * leak a GPU allocation.
	 */
	g_wl.proxy_marshal_flags(params, 0 /* destroy */, NULL, p->dmabuf_version,
				 WL_MARSHAL_FLAG_DESTROY);
	params = NULL;
	if (!p->wl_buffer_new) {
		if (!g_gfx_err[0])
			gfx_errf("the compositor did not create a wl_buffer for "
				 "the dma-buf");
		goto out;
	}

	/* ---- 5. attach the buffer to the surface and commit ---- */
	gfx_sweep_buffers(p);
	slot = &p->slots[p->slot_next];
	p->slot_next = (p->slot_next + 1) % GFX_BUF_SLOTS;
	if (slot->proxy) {
		/*
		 * The slot is reused before the compositor released the
		 * buffer of the previous frame: drop the proxy (the
		 * compositor keeps its own reference to the dma-buf, so the
		 * pixels stay valid) instead of leaking one proxy per
		 * frame.
		 */
		if (g_wl.proxy_destroy)
			g_wl.proxy_destroy(slot->proxy);
		slot->proxy = NULL;
	}
	slot->owner = p;
	slot->proxy = p->wl_buffer_new;
	slot->current = 1;
	slot->released = 0;
	p->wl_buffer_new = NULL;

	p->buf_listener.release = gfx_buffer_release;
	g_wl.proxy_add_listener(slot->proxy, (void (**)(void))&p->buf_listener, slot);

	p->released = 0;
	g_wl.proxy_marshal_flags(p->wl_surface, GFX_WL_SURFACE_ATTACH, NULL,
				 p->compositor_version, 0, slot->proxy,
				 (int32_t)0, (int32_t)0);
	g_wl.proxy_marshal_flags(p->wl_surface, GFX_WL_SURFACE_DAMAGE, NULL,
				 p->compositor_version, 0, (int32_t)0, (int32_t)0,
				 (int32_t)p->bo_w, (int32_t)p->bo_h);
	g_wl.proxy_marshal_flags(p->wl_surface, GFX_WL_SURFACE_COMMIT, NULL,
				 p->compositor_version, 0);
	if (g_wl.display_flush(p->wl_display) < 0 && errno != EAGAIN) {
		gfx_errf("wl_display.flush failed: %s", strerror(errno));
		goto out;
	}
	/* the descriptor was sent with the request; the module keeps its own
	 * copy alive in p->dmabuf_fd */
	fd = -1;
	rc = 0;

out:
	if (bo) {
		/*
		 * The buffer object is NOT destroyed here: the EGL platform
		 * of Mesa keeps its own pointer to the front buffer
		 * (dri2_gbm_surface.base.b) and destroys it itself at the
		 * next eglSwapBuffers() or at eglDestroySurface().  Destroying
		 * it here as well would be a double free (the symptom is a
		 * crash inside gbm_bo_destroy() during the cleanup).
		 *
		 * The compositor holds its own reference to the same
		 * allocation through the dma-buf descriptor, so the pixels
		 * stay valid until the compositor is done with them.
		 */
		(void)bo;
	}
	if (fd >= 0 && fd != p->dmabuf_fd)
		close(fd);
	if (owned_window)
		gfx_present_copy_window_end(p);
	return rc;
}

/* ===================================================================== */
/* 15. Waiting for the compositor                                        */
/* ===================================================================== */

static long gfx_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int gfx_present_wait_release(gfx_present_t *p, int timeout_ms)
{
	long deadline;

	if (!p || !p->wl_display) {
		gfx_errf("gfx_present_wait_release: no Wayland connection");
		return -1;
	}
	deadline = gfx_now_ms() + (timeout_ms > 0 ? timeout_ms : 0);

	for (;;) {
		struct pollfd pfd;
		long remaining;
		int r;

		if (p->released)
			return 1;
		if (g_wl.display_dispatch_pending(p->wl_display) < 0) {
			gfx_errf("wl_display.dispatch_pending failed");
			return -1;
		}
		if (p->released)
			return 1;

		if (timeout_ms == 0)
			return 0;
		remaining = deadline - gfx_now_ms();
		if (timeout_ms > 0 && remaining <= 0)
			return 0;

		pfd.fd = g_wl.display_get_fd(p->wl_display);
		pfd.events = POLLIN;
		pfd.revents = 0;
		r = poll(&pfd, 1, timeout_ms < 0 ? -1 : (int)remaining);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			gfx_errf("poll on the Wayland socket failed: %s",
				 strerror(errno));
			return -1;
		}
		if (r == 0)
			return 0;

		while (g_wl.display_prepare_read(p->wl_display) != 0) {
			if (g_wl.display_dispatch_pending(p->wl_display) < 0)
				return -1;
			if (p->released)
				return 1;
		}
		if (g_wl.display_read_events(p->wl_display) < 0) {
			gfx_errf("wl_display.read_events failed");
			return -1;
		}
	}
}

int gfx_present_released(const gfx_present_t *p)
{
	return p ? p->released : 0;
}

int gfx_present_dmabuf_fd(const gfx_present_t *p)
{
	return p ? p->dmabuf_fd : -1;
}

int gfx_present_fb_id(const gfx_present_t *p)
{
	return p ? (int)p->fb_id : 0;
}

const char *gfx_present_fb_method(const gfx_present_t *p)
{
	return p ? p->fb_method : "";
}

uint32_t gfx_present_buffer_width(const gfx_present_t *p) { return p ? p->bo_w : 0; }
uint32_t gfx_present_buffer_height(const gfx_present_t *p) { return p ? p->bo_h : 0; }
uint32_t gfx_present_buffer_stride(const gfx_present_t *p) { return p ? p->bo_stride : 0; }

/* ===================================================================== */
/* 16. The copy accounting API                                           */
/* ===================================================================== */

void gfx_present_copy_window_begin(gfx_present_t *p)
{
	(void)p;
	g_cp_calls = g_cp_bytes = g_cp_big_calls = g_cp_big_bytes = 0;
	g_cp_window = 1;
}

void gfx_present_copy_window_end(gfx_present_t *p)
{
	if (!g_cp_window)
		return;
	g_cp_window = 0;
	if (!p)
		return;
	p->cp_last_calls = g_cp_calls;
	p->cp_last_bytes = g_cp_bytes;
	p->cp_last_big_calls = g_cp_big_calls;
	p->cp_last_big_bytes = g_cp_big_bytes;
	p->cp_calls += g_cp_calls;
	p->cp_bytes += g_cp_bytes;
	p->cp_big_calls += g_cp_big_calls;
	p->cp_big_bytes += g_cp_big_bytes;
}

unsigned long gfx_present_copies(const gfx_present_t *p)
{
	return p ? p->cp_big_calls : 0;
}

unsigned long gfx_present_copies_last(const gfx_present_t *p)
{
	return p ? p->cp_last_big_calls : 0;
}

unsigned long gfx_present_copy_bytes(const gfx_present_t *p)
{
	return p ? p->cp_bytes : 0;
}

unsigned long gfx_present_copy_bytes_last(const gfx_present_t *p)
{
	return p ? p->cp_last_bytes : 0;
}

unsigned long gfx_present_copy_big_bytes(const gfx_present_t *p)
{
	return p ? p->cp_big_bytes : 0;
}

unsigned long gfx_present_copy_big_bytes_last(const gfx_present_t *p)
{
	return p ? p->cp_last_big_bytes : 0;
}

unsigned long gfx_present_copy_calls(const gfx_present_t *p)
{
	return p ? p->cp_calls : 0;
}

unsigned long gfx_present_copy_calls_last(const gfx_present_t *p)
{
	return p ? p->cp_last_calls : 0;
}

/* ===================================================================== */
/* 17. GDI-dmabuf integration bridge                                     */
/* ===================================================================== */

/*
 * Link the gfx_present module to win32k GDI rendering.
 * This allows GDI operations to draw directly into the GPU buffer
 * managed by this module, eliminating CPU copies.
 *
 * Call this after gfx_present_create() and gfx_present_make_current()
 * have succeeded, but before any GDI drawing operations.
 */
int gfx_present_link_gdi(gfx_present_t *p)
{
	extern int win32k_gdi_gpu_init(void *egl_display, void *egl_config, 
				       void *egl_context) __attribute__((weak));

	if (!p) {
		gfx_errf("gfx_present_link_gdi: no module");
		return -1;
	}
	if (!p->dpy || !p->ctx || !p->egl_surf) {
		gfx_errf("gfx_present_link_gdi: EGL not initialized");
		return -1;
	}

	if (!win32k_gdi_gpu_init) {
		/* win32k not linked in this binary */
		return 0;
	}

	return win32k_gdi_gpu_init(p->dpy, p->cfg, p->ctx);
}
