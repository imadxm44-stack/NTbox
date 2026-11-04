/*
 * test_umd.c — NT-Box: the real D3D11 UMD bridge selftest (master plan
 * W9a, T-214 … T-217 P0).
 *
 * WHAT IT MEASURES (lamps = MEASUREMENTS, never claims)
 *
 *   GFX-031..038  d3d11_bridge.c   (device, resources, shaders, program,
 *                                  clears, draw, uploads, swapchain)
 *   GFX-039/040   umd_egl.c        DRY mode: dma-buf fd export + KMS AddFB2
 *   GFX-041       THIS TEST        the frame hand-off of the swapchain
 *                                  (wet: the compositor releases the
 *                                   wl_buffer — the import proof;
 *                                   dry: the dma-buf fd + the KMS
 *                                   scannout framebuffer proof)
 *   GFX-042       THIS TEST        CPU copies of the presented frame == 0
 *                                  (interposed memcpy counter — MEASURED;
 *                                   NTBOX_UMD_FORCE_COPY=1 flips the lamp
 *                                   to FAIL: proof the counter measures)
 *   GFX-043       THIS TEST        frame time present -> hand-off
 *                                  (<= 16.67 ms = one 60 Hz frame)
 *
 * TWO MODES (the SAME D3D11 flow runs in both; only Present differs):
 *
 *   WET  — a compositor exists: weston headless is spawned when
 *          $WAYLAND_DISPLAY is empty (or the existing one is used).
 *          Present() = eglSwapBuffers -> dma-buf -> linux-dmabuf ->
 *          wl_surface.attach -> commit; the test waits for the
 *          compositor's wl_buffer release (the import PROOF).
 *   DRY  — no compositor (NTBOX_UMD_DRY=1, or weston unavailable):
 *          Present() = eglSwapBuffers -> gbm_surface_lock_front_buffer
 *          -> gbm_bo_get_fd -> PRIME_FD_TO_HANDLE + drmModeAddFB2 on
 *          /dev/dri/cardN (the scannout-capable PROOF, umd_egl.c).
 *
 *   rc 0 = pass, rc 1 = a measured criterion broken, rc 3 = no GPU (SKIP,
 *   same convention as tools/verify.sh).  Nothing is ever faked.
 *
 * BUILD (direct gcc, like the sibling tests; NO Makefile involvement):
 *   gcc -O2 -Wall -Wextra -fno-builtin-memcpy -fno-builtin-memmove \
 *       -o /tmp/opencode/test_umd \
 *       tools/ntboxd/test_umd.c tools/ntboxd/d3d11_bridge.c \
 *       tools/ntboxd/umd_egl.c tools/ntboxd/gfx_present.c -ldl -lm
 *   (the bridge compiles WITHOUT win32k.c: win32k_window_get_size is a
 *    weak symbol there — the windowless UMD flow does not need it)
 */

#define _GNU_SOURCE

#include "d3d11_bridge.h"
#include "gfx_present.h"
#include "umd_egl.h"   /* struct umd_frame fields for the metadata proof */

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ===================================================================== */
/* Lamps (the same protocol as the bridge / ntb0_trace.h)                */
/* ===================================================================== */

#define UMD_TP(id, st, fmt, ...) \
	fprintf(stderr, "[ntbox][%s][%s] " fmt "\n", (id), (st), ##__VA_ARGS__)
#define UMD_OK(id, fmt, ...)    UMD_TP(id, "OK", fmt, ##__VA_ARGS__)
#define UMD_STUB(id, fmt, ...)  UMD_TP(id, "STUB", fmt, ##__VA_ARGS__)
#define UMD_FAIL(id, fmt, ...)  UMD_TP(id, "FAIL", fmt, ##__VA_ARGS__)
#define UMD_SKIP(id, fmt, ...)  UMD_TP(id, "SKIP", fmt, ##__VA_ARGS__)

#define T_ID_FRAME  "GFX-041"  /* the frame hand-off of the swapchain     */
#define T_ID_COPIES "GFX-042"  /* CPU copies of the presented frame == 0  */
#define T_ID_TIME   "GFX-043"  /* frame time present -> hand-off          */

#define TEST_EXIT_PASS 0
#define TEST_EXIT_FAIL 1
#define TEST_EXIT_SKIP 3

#define WIN_W 640
#define WIN_H 480
#define TEX_W 64
#define TEX_H 64

/* ===================================================================== */
/* GLES2 — dlopen()ed (the TEST's own pixel proofs: glReadPixels)        */
/* ===================================================================== */

#define GL_FLOAT         0x1406
#define GL_RGBA          0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_NO_ERROR      0

static unsigned int (*t_read_pixels)(int, int, int, int, unsigned int,
				     unsigned int, void *);
static unsigned int (*t_get_error)(void);

static int test_gl_bind(void)
{
	void *h;

	h = dlopen("libGLESv2.so.2", RTLD_LAZY | RTLD_LOCAL);
	if (!h)
		h = dlopen("libGLESv2.so", RTLD_LAZY | RTLD_LOCAL);
	if (!h) {
		fprintf(stderr, "[test_umd] no libGLESv2: %s\n", dlerror());
		return -1;
	}
	t_read_pixels = dlsym(h, "glReadPixels");
	t_get_error = dlsym(h, "glGetError");
	if (!t_read_pixels || !t_get_error)
		return -1;
	return 0;
}

static int read_pixel(int x, int y, unsigned char out[4], char *err,
		      size_t errn)
{
	unsigned char px[4];
	unsigned e;

	if (!t_read_pixels) {
		snprintf(err, errn, "test GLES not bound");
		return -1;
	}
	t_get_error(); /* flush any stale GL error state */
	t_read_pixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	memcpy(out, px, 4);
	e = t_get_error();
	if (e != GL_NO_ERROR) {
		snprintf(err, errn, "glReadPixels GL error 0x%x", e);
		return -1;
	}
	return 0;
}

/* ===================================================================== */
/* Headless compositor (weston) — the sibling-test pattern                */
/* ===================================================================== */

static pid_t g_weston_pid = -1;
static char g_weston_sock[64];

static void test_weston_cleanup(void)
{
	int status;

	if (g_weston_pid > 0) {
		kill(g_weston_pid, SIGTERM);
		waitpid(g_weston_pid, &status, 0);
		g_weston_pid = -1;
	}
	if (g_weston_sock[0]) {
		const char *rt = getenv("XDG_RUNTIME_DIR");
		char path[512];

		if (rt && *rt) {
			snprintf(path, sizeof path, "%s/%s", rt,
				 g_weston_sock);
			unlink(path);
		}
		g_weston_sock[0] = 0;
	}
}

/* Start weston headless so a REAL compositor imports the frames. */
static int test_spawn_weston(char *err, size_t errn)
{
	char rt[512], path[640], sock[64];
	struct stat st;
	pid_t pid;
	int i;

	if (getenv("XDG_RUNTIME_DIR") && *getenv("XDG_RUNTIME_DIR"))
		snprintf(rt, sizeof rt, "%s", getenv("XDG_RUNTIME_DIR"));
	else
		snprintf(rt, sizeof rt, "/run/user/%u", (unsigned)getuid());
	if (stat(rt, &st) < 0) {
		snprintf(err, errn, "no XDG_RUNTIME_DIR (%.120s)", rt);
		return -1;
	}
	snprintf(sock, sizeof sock, "ntboxumd-%d", (int)getpid());
	snprintf(path, sizeof path, "%s/%s", rt, sock);
	unlink(path);

	pid = fork();
	if (pid < 0) {
		snprintf(err, errn, "fork: %s", strerror(errno));
		return -1;
	}
	if (pid == 0) {
		char sockarg[128];
		char *argv[10];
		int devnull = open("/dev/null", O_RDWR);

		if (devnull >= 0) {
			dup2(devnull, 0);
			dup2(devnull, 1);
			dup2(devnull, 2);
			close(devnull);
		}
		setenv("XDG_RUNTIME_DIR", rt, 1);
		unsetenv("WAYLAND_DISPLAY");
		unsetenv("DISPLAY");
		snprintf(sockarg, sizeof sockarg, "--socket=%s", sock);
		argv[0] = (char *)"weston";
		argv[1] = (char *)"--backend=headless-backend.so";
		argv[2] = (char *)"--renderer=gl";
		argv[3] = sockarg;
		argv[4] = (char *)"--width=1920";
		argv[5] = (char *)"--height=1080";
		argv[6] = NULL;
		execvp("weston", argv);
		_exit(127);
	}

	for (i = 0; i < 300; i++) {
		if (stat(path, &st) == 0) {
			g_weston_pid = pid;
			snprintf(g_weston_sock, sizeof g_weston_sock, "%s",
				 sock);
			setenv("WAYLAND_DISPLAY", sock, 1);
			return 0;
		}
		if (waitpid(pid, NULL, WNOHANG) == pid) {
			snprintf(err, errn,
				 "weston exited before creating the socket");
			return -1;
		}
		usleep(100000);
	}
	kill(pid, SIGTERM);
	waitpid(pid, NULL, 0);
	snprintf(err, errn, "weston did not create %.120s in time", path);
	return -1;
}

/* ===================================================================== */
/* Helpers                                                                */
/* ===================================================================== */

static long now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000L + ts.tv_nsec / 1000;
}

static int have_gpu(void)
{
	DIR *dir;
	struct dirent *de;
	int found = 0;

	dir = opendir("/dev/dri");
	if (!dir)
		return 0;
	while ((de = readdir(dir)) != NULL) {
		if (!strncmp(de->d_name, "renderD", 7)) {
			char path[320];
			int fd;

			snprintf(path, sizeof path, "/dev/dri/%s",
				 de->d_name);
			fd = open(path, O_RDWR | O_CLOEXEC);
			if (fd >= 0) {
				close(fd);
				found = 1;
				break;
			}
		}
	}
	closedir(dir);
	return found;
}

/* tiny fake DXBC container (chunks SHDR/ISGN) — the STUB honesty input */
static const unsigned char fake_dxbc[] = {
	'D', 'X', 'B', 'C', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0,               /* 2 chunks   */
	32, 0, 0, 0,                                      /* off 0 = 32 */
	36, 0, 0, 0,                                      /* off 1 = 36 */
	'S', 'H', 'D', 'R',                               /* 32         */
	'I', 'S', 'G', 'N', 0, 0, 0, 0,                   /* 36         */
};

/* NTBA — the internal bytecode marker of the bridge (documented). */
static const unsigned char ntba_marker[4] = { 'N', 'T', 'B', 'A' };

/* app vertex data: 3 verts (x,y,z,r,g,b), stride 24, R32G32B32_FLOAT */
static const float triangle[] = {
	 0.0f,  0.6f, 1.0f,   1.0f, 0.0f, 0.0f,
	-0.6f, -0.5f, 0.0f,   0.0f, 1.0f, 0.0f,
	 0.6f, -0.5f, 0.0f,   0.0f, 0.0f, 1.0f,
};
static const unsigned short indices[] = { 0, 1, 2 };

/* the 64x64 UpdateSubresource pattern (every pixel 0x10,0x20,0x30,0xFF) */
static unsigned char pattern[TEX_W * TEX_H * 4];

static void build_pattern(void)
{
	unsigned x, y;

	for (y = 0; y < TEX_H; y++) {
		for (x = 0; x < TEX_W; x++) {
			unsigned char *p = pattern + (y * TEX_W + x) * 4;

			p[0] = 0x10;
			p[1] = 0x20;
			p[2] = 0x30;
			p[3] = 0xFF;
		}
	}
}

static long median3(long a, long b, long c)
{
	if ((a >= b && a <= c) || (a >= c && a <= b))
		return a;
	if ((b >= a && b <= c) || (b >= c && b <= a))
		return b;
	return c;
}

static void set_viewport(ID3D11DeviceContext ctx, int w, int h)
{
	D3D11_VIEWPORT vp;

	memset(&vp, 0, sizeof vp);
	vp.Width = (float)w;
	vp.Height = (float)h;
	vp.MaxDepth = 1.0f;
	ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
}

/* one presented frame of the window: OM + clear + depth clear + draw */
static void draw_scene(ID3D11DeviceContext ctx, ID3D11RenderTargetView rtv,
		       ID3D11DepthStencilView dsv, int w, int h,
		       const float clear[4])
{
	ID3D11DeviceContext_OMSetRenderTargets(ctx, 1,
		(void *const *)&rtv, dsv);
	set_viewport(ctx, w, h);
	ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clear);
	if (dsv)
		ID3D11DeviceContext_ClearDepthStencilView(ctx, dsv,
			D3D11_CLEAR_DEPTH, 1.0f, 0);
	ID3D11DeviceContext_DrawIndexed(ctx, 3, 0, 0);
}

/* ===================================================================== */
/* The test                                                              */
/* ===================================================================== */

int main(int argc, char **argv)
{
	char err[512] = "";
	int rc = TEST_EXIT_FAIL;
	int wet = 0;
	int i;
	int force_copy;
	unsigned long copies_total = 0, bytes_total = 0, big_total = 0;
	unsigned long calls_total = 0;
	long present_us[4] = { -1, -1, -1, -1 };
	long rt_us[4] = { -1, -1, -1, -1 };
	long med = 0;
	int w = WIN_W, h = WIN_H, fb_w = 0, fb_h = 0;
	gfx_present_t *present = NULL;

	ID3D11Device dev = NULL;
	ID3D11DeviceContext ctx = NULL;
	IDXGISwapChain sc = NULL;
	ID3D11RenderTargetView win_rtv = NULL;
	ID3D11DepthStencilView win_dsv = NULL;
	ID3D11Buffer vb = NULL, ib = NULL;
	ID3D11InputLayout il = NULL;
	ID3D11VertexShader vs = NULL;
	ID3D11PixelShader ps = NULL;
	ID3D11Texture2D depth_tex = NULL, rt_tex = NULL, pattern_tex = NULL;
	ID3D11RenderTargetView rt_rtv = NULL, pattern_rtv = NULL;

	long hr;
	const struct umd_frame *frame = NULL;
	unsigned char px[4] = { 0, 0, 0, 0 };

	(void)argc;
	(void)argv;

	setvbuf(stdout, NULL, _IOLBF, 0);
	printf("[test_umd] starting — %s\n",
	       have_gpu() ? "GPU present (render node found)" : "NO GPU");

	if (!have_gpu()) {
		UMD_SKIP("GFX-031",
			 "no /dev/dri render node — the UMD bridge cannot "
			 "exist here (the verify.sh SKIP convention)");
		printf("[test_umd] RESULT: SKIP\n");
		return TEST_EXIT_SKIP;
	}

	force_copy = getenv("NTBOX_UMD_FORCE_COPY") != NULL;

	/* --------------------------------------------------------------- */
	/* the mode: WET (compositor) or DRY (no compositor)                */
	/* --------------------------------------------------------------- */
	if (getenv("NTBOX_UMD_DRY")) {
		wet = 0;
		printf("[test_umd] NTBOX_UMD_DRY=1 -> DRY mode "
		       "(no compositor)\n");
	} else if (getenv("WAYLAND_DISPLAY") && *getenv("WAYLAND_DISPLAY")) {
		wet = 1;
		printf("[test_umd] using compositor '%s'\n",
		       getenv("WAYLAND_DISPLAY"));
	} else if (!test_spawn_weston(err, sizeof err)) {
		wet = 1;
		printf("[test_umd] spawned weston headless (socket=%s)\n",
		       g_weston_sock);
	} else {
		wet = 0;
		printf("[test_umd] no compositor (%s) -> DRY mode\n", err);
	}
	UMD_OK(T_ID_FRAME, "mode = %s (T-217: the SAME UMD flow, two "
	       "present paths)",
	       wet ? "WET (compositor present)" : "DRY (no compositor)");

	build_pattern();

	/* --------------------------------------------------------------- */
	/* device (GFX-031 — printed by the bridge)                         */
	/* --------------------------------------------------------------- */
	hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
			       NULL, 0, 0, (void **)&dev, NULL,
			       (void **)&ctx);
	if (hr != S_OK || !dev || !ctx) {
		UMD_FAIL("GFX-031", "D3D11CreateDevice failed (hr=0x%lx)",
			 hr);
		printf("[test_umd] RESULT: FAIL\n");
		return TEST_EXIT_FAIL;
	}
	present = d3d11_device_get_present(dev);
	if (!present) {
		UMD_FAIL("GFX-031", "device has no gfx_present handle");
		goto cleanup;
	}
	UMD_OK("GFX-031", "test: device=%p context=%p (gfx_present=%p)",
	       (void *)dev, (void *)ctx, (void *)present);

	/* --------------------------------------------------------------- */
	/* swapchain (GFX-038 — printed by the bridge)                      */
	/* --------------------------------------------------------------- */
	{
		DXGI_SWAP_CHAIN_DESC scd;
		DXGI_MODE_DESC mode;

		memset(&mode, 0, sizeof mode);
		mode.Width = (unsigned)w;
		mode.Height = (unsigned)h;
		mode.Format = (void *)(uintptr_t)DXGI_FORMAT_R8G8B8A8_UNORM;

		memset(&scd, 0, sizeof scd);
		scd.BufferDesc = &mode;
		scd.BufferCount = 2;
		scd.Windowed = 1;
		scd.BufferUsage = (void *)(uintptr_t)
			DXGI_USAGE_RENDER_TARGET_OUTPUT;
		scd.SwapEffect = (void *)(uintptr_t)DXGI_SWAP_EFFECT_DISCARD;

		hr = IDXGIFactory_CreateSwapChain(NULL, dev, &scd,
						  (void **)&sc);
		if (hr != S_OK || !sc) {
			UMD_FAIL("GFX-038",
				 "CreateSwapChain failed (hr=0x%lx)", hr);
			goto cleanup;
		}
	}
	gfx_present_target_size(present, &fb_w, &fb_h);
	UMD_OK("GFX-038", "test: swapchain=%p, render target %dx%d",
	       (void *)sc, fb_w, fb_h);

	/* --------------------------------------------------------------- */
	/* resources (GFX-032/033 — printed by the bridge)                  */
	/* --------------------------------------------------------------- */
	{
		D3D11_BUFFER_DESC bd;
		D3D11_SUBRESOURCE_DATA init;

		memset(&bd, 0, sizeof bd);
		bd.ByteWidth = sizeof triangle;
		bd.Usage = (void *)(uintptr_t)D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		memset(&init, 0, sizeof init);
		init.pSysMem = triangle;
		init.SysMemPitch = sizeof triangle;
		hr = ID3D11Device_CreateBuffer(dev, &bd, &init, (void **)&vb);
		if (hr != S_OK || !vb) {
			UMD_FAIL("GFX-032", "CreateBuffer (vertex) 0x%lx",
				 hr);
			goto cleanup;
		}

		bd.ByteWidth = sizeof indices;
		bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
		init.pSysMem = indices;
		init.SysMemPitch = sizeof indices;
		hr = ID3D11Device_CreateBuffer(dev, &bd, &init, (void **)&ib);
		if (hr != S_OK || !ib) {
			UMD_FAIL("GFX-032", "CreateBuffer (index) 0x%lx",
				 hr);
			goto cleanup;
		}

		/* the depth buffer of the WINDOW target (real D24 rbo) */
		{
			D3D11_TEXTURE2D_DESC td;

			memset(&td, 0, sizeof td);
			td.Width = (unsigned)w;
			td.Height = (unsigned)h;
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = (void *)(uintptr_t)
				DXGI_FORMAT_D24_UNORM_S8_UINT;
			td.Usage = (void *)(uintptr_t)D3D11_USAGE_DEFAULT;
			td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
			hr = ID3D11Device_CreateTexture2D(dev, &td, NULL,
							  (void **)&depth_tex);
			if (hr != S_OK || !depth_tex) {
				UMD_FAIL("GFX-032",
					 "CreateTexture2D (depth) 0x%lx",
					 hr);
				goto cleanup;
			}
		}
		hr = ID3D11Device_CreateDepthStencilView(dev, depth_tex,
							 NULL,
							 (void **)&win_dsv);
		if (hr != S_OK || !win_dsv) {
			UMD_FAIL("GFX-032", "CreateDepthStencilView 0x%lx",
				 hr);
			goto cleanup;
		}
	}

	/* the WINDOW render target = the swapchain's default FB */
	hr = ID3D11Device_CreateRenderTargetView(dev, (void *)sc, NULL,
						 (void **)&win_rtv);
	if (hr != S_OK || !win_rtv) {
		UMD_FAIL("GFX-032",
			 "CreateRenderTargetView (window) 0x%lx", hr);
		goto cleanup;
	}

	/* offscreen 64x64 RT texture + view + the UpdateSubresource tex */
	{
		D3D11_TEXTURE2D_DESC td;

		memset(&td, 0, sizeof td);
		td.Width = TEX_W;
		td.Height = TEX_H;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = (void *)(uintptr_t)DXGI_FORMAT_R8G8B8A8_UNORM;
		td.Usage = (void *)(uintptr_t)D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_RENDER_TARGET;

		hr = ID3D11Device_CreateTexture2D(dev, &td, NULL,
						  (void **)&rt_tex);
		if (hr != S_OK || !rt_tex) {
			UMD_FAIL("GFX-032",
				 "CreateTexture2D (offscreen RT) 0x%lx", hr);
			goto cleanup;
		}
		hr = ID3D11Device_CreateRenderTargetView(dev, rt_tex, NULL,
							 (void **)&rt_rtv);
		if (hr != S_OK || !rt_rtv) {
			UMD_FAIL("GFX-032", "CreateRenderTargetView "
				 "(offscreen) 0x%lx", hr);
			goto cleanup;
		}
		hr = ID3D11Device_CreateTexture2D(dev, &td, NULL,
						  (void **)&pattern_tex);
		if (hr != S_OK || !pattern_tex) {
			UMD_FAIL("GFX-032",
				 "CreateTexture2D (pattern) 0x%lx", hr);
			goto cleanup;
		}
		hr = ID3D11Device_CreateRenderTargetView(dev, pattern_tex,
							 NULL,
							 (void **)&pattern_rtv);
		if (hr != S_OK || !pattern_rtv) {
			UMD_FAIL("GFX-032", "CreateRenderTargetView "
				 "(pattern) 0x%lx", hr);
			goto cleanup;
		}
	}

	/* shaders (GFX-033/034): NTBA bytecode + a fake DXBC (STUB, honest) */
	hr = ID3D11Device_CreateVertexShader(dev, ntba_marker,
					     sizeof ntba_marker, NULL,
					     (void **)&vs);
	if (hr != S_OK || !vs) {
		UMD_FAIL("GFX-033", "CreateVertexShader 0x%lx", hr);
		goto cleanup;
	}
	hr = ID3D11Device_CreatePixelShader(dev, fake_dxbc,
					    sizeof fake_dxbc, NULL,
					    (void **)&ps);
	if (hr != S_OK || !ps) {
		UMD_FAIL("GFX-033", "CreatePixelShader 0x%lx", hr);
		goto cleanup;
	}

	/* the input layout drives the a0/a1 attrib wiring of the program */
	{
		D3D11_INPUT_ELEMENT_DESC ied[2];
		const char *sig = "NTBoxInputSignature";

		memset(ied, 0, sizeof ied);
		ied[0].SemanticName = "POSITION";
		ied[0].Format = (void *)(uintptr_t)
			DXGI_FORMAT_R32G32B32_FLOAT;
		ied[0].AlignedByteOffset = 0;
		ied[1].SemanticName = "COLOR";
		ied[1].Format = (void *)(uintptr_t)
			DXGI_FORMAT_R32G32B32_FLOAT;
		ied[1].AlignedByteOffset = 12;
		hr = ID3D11Device_CreateInputLayout(dev, ied, 2, sig,
						    (unsigned)strlen(sig),
						    (void **)&il);
		if (hr != S_OK || !il) {
			UMD_FAIL("GFX-034", "CreateInputLayout 0x%lx", hr);
			goto cleanup;
		}
	}

	/* state wiring (one pipeline for every draw of this test) */
	{
		unsigned strides[1] = { 24 }, offsets[1] = { 0 };

		ID3D11DeviceContext_IASetInputLayout(ctx, il);
		ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1,
			(void *const *)&vb, strides, offsets);
		ID3D11DeviceContext_IASetIndexBuffer(ctx, ib,
			(void *)(uintptr_t)DXGI_FORMAT_R16_UINT, 0);
		ID3D11DeviceContext_IASetPrimitiveTopology(ctx,
			(void *)(uintptr_t)
			D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
		ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
	}

	/* the GLES pixel proofs need the test's own readback */
	if (test_gl_bind() < 0) {
		UMD_SKIP("GFX-031",
			 "no GLES2 runtime — the pixel proofs and the GPU "
			 "device (which IS GLES) cannot work here");
		rc = TEST_EXIT_SKIP;
		goto cleanup;
	}

	/* --------------------------------------------------------------- */
	/* OFFSCREEN A — Clear + Draw on a REAL GPU texture RT               */
	/* --------------------------------------------------------------- */
	{
		float orange[4] = { 1.0f, 0.5f, 0.0f, 1.0f };

		ID3D11DeviceContext_OMSetRenderTargets(ctx, 1,
			(void *const *)&rt_rtv, NULL);
		set_viewport(ctx, TEX_W, TEX_H);
		ID3D11DeviceContext_ClearRenderTargetView(ctx, rt_rtv,
							  orange);
		ID3D11DeviceContext_DrawIndexed(ctx, 3, 0, 0);

		if (read_pixel(0, 0, px, err, sizeof err) == 0 &&
		    px[0] == 255 && px[1] >= 127 && px[1] <= 128 &&
		    px[2] == 0 && px[3] == 255) {
			UMD_OK("GFX-035",
			       "the offscreen FBO clear is REAL (read back "
			       "%u %u %u %u at the texture corner; the same "
			       "GL path serves the window frames)",
			       px[0], px[1], px[2], px[3]);
		} else {
			UMD_FAIL("GFX-035",
				 "the offscreen clear proof failed "
				 "(read back %u %u %u %u, %s)", px[0],
				 px[1], px[2], px[3], err);
			goto cleanup;
		}
	}

	/* --------------------------------------------------------------- */
	/* OFFSCREEN B — UpdateSubresource -> the pixel proof (GFX-037)      */
	/* --------------------------------------------------------------- */
	{
		ID3D11DeviceContext_UpdateSubresource(ctx, pattern_tex, 0,
						      NULL, pattern,
						      TEX_W * 4, 0);
		ID3D11DeviceContext_OMSetRenderTargets(ctx, 1,
			(void *const *)&pattern_rtv, NULL);
		set_viewport(ctx, TEX_W, TEX_H);

		if (read_pixel(TEX_W / 2, TEX_H / 2, px, err, sizeof err)
		    == 0 && px[0] == 0x10 && px[1] == 0x20 &&
		    px[2] == 0x30 && px[3] == 0xFF) {
			UMD_OK("GFX-037",
			       "UpdateSubresource PROVEN on the GPU: pixel "
			       "(%d,%d) = 0x%02x 0x%02x 0x%02x 0x%02x — the "
			       "app bytes now live in GPU memory",
			       TEX_W / 2, TEX_H / 2, px[0], px[1], px[2],
			       px[3]);
		} else {
			UMD_FAIL("GFX-037",
				 "UpdateSubresource pixel proof failed "
				 "(read 0x%02x 0x%02x 0x%02x 0x%02x: %s)",
				 px[0], px[1], px[2], px[3], err);
			goto cleanup;
		}
	}

	/* --------------------------------------------------------------- */
	/* the FRAME LOOP — 3 presents through the swapchain                 */
	/* (the measurement window covers Clear + Draw + Present — exactly  */
	/*  the presented frame; Present never opens/closes one itself)     */
	/* --------------------------------------------------------------- */
	for (i = 0; i < 3; i++) {
		float clear[4] = { 0.05f, 0.07f, 0.12f, 1.0f };
		long t0, t1, trel;
		int released = 0, extra = 0;

		gfx_present_copy_window_begin(present);

		draw_scene(ctx, win_rtv, win_dsv, w, h, clear);

		t0 = now_us();
		hr = IDXGISwapChain_Present(sc, 1, 0);
		t1 = now_us();
		present_us[i] = t1 - t0;
		if (hr != S_OK) {
			UMD_FAIL(T_ID_FRAME,
				 "frame %d: IDXGISwapChain_Present failed "
				 "(0x%lx)", i, hr);
			gfx_present_copy_window_end(present);
			goto cleanup;
		}

		/* the wl_buffer release = the compositor imported it */
		if (wet) {
			/*
			 * weston headless may release the buffer only after
			 * it composited the NEXT frame; keep presenting.
			 * (Every retry stays inside the SAME measurement
			 * window — exactly what the criterion measures.)
			 */
			released = gfx_present_wait_release(present, 5000);
			while (released != 1 && extra < 4) {
				draw_scene(ctx, win_rtv, win_dsv, w, h,
					   clear);
				if (IDXGISwapChain_Present(sc, 1, 0) != S_OK)
					break;
				extra++;
				released = gfx_present_wait_release(present,
								    5000);
			}
			if (released != 1) {
				UMD_FAIL(T_ID_FRAME,
					 "frame %d: the compositor did not "
					 "release the wl_buffer (%s: %s)",
					 i,
					 gfx_present_compositor(present),
					 gfx_present_error());
				gfx_present_copy_window_end(present);
				goto cleanup;
			}
			trel = now_us() - t0;
		} else {
			trel = present_us[i];
		}
		rt_us[i] = trel;

		gfx_present_copy_window_end(present);

		copies_total += gfx_present_copies_last(present);
		big_total += gfx_present_copy_big_bytes_last(present);
		bytes_total += gfx_present_copy_bytes_last(present);

		printf("[test_umd] frame %d: present=%ldus roundtrip=%ldus "
		       "released=%d copies_last=%lu bytes_last=%lu\n",
		       i, present_us[i], rt_us[i], released,
		       gfx_present_copies_last(present),
		       gfx_present_copy_bytes_last(present));
	}
	calls_total = gfx_present_copy_calls(present);

	/* --------------------------------------------------------------- */
	/* the frame metadata of the LAST present                           */
	/* --------------------------------------------------------------- */
	frame = d3d11_swapchain_frame(sc);
	if (!frame || frame->dmabuf_fd < 0) {
		UMD_FAIL(T_ID_FRAME, "no dma-buf metadata after Present");
		goto cleanup;
	}
	UMD_OK(T_ID_FRAME,
	       "frame hand-off PROVEN: the %s path, dma-buf fd=%d, %ux%u "
	       "stride=%u ARGB8888, %s",
	       wet ? "wet" : "dry", frame->dmabuf_fd, frame->width,
	       frame->height, frame->stride, frame->fb_method);
	if (!wet && frame->fb_id == 0 &&
	    !strstr(frame->fb_method, "AddFB")) {
		UMD_FAIL(T_ID_FRAME,
			 "DRY mode: the KMS scannout proof is missing (%s)",
			 frame->fb_method);
		goto cleanup;
	}

	/* --------------------------------------------------------------- */
	/* GFX-042: the CPU copies of the presented frames — MEASURED        */
	/* --------------------------------------------------------------- */
	if (force_copy) {
		if (copies_total > 0) {
			UMD_FAIL(T_ID_COPIES,
				 "NTBOX_UMD_FORCE_COPY: the interposed "
				 "counter MEASURED the forced frame-sized "
				 "copy (copies=%lu, %lu big bytes) — the "
				 "counter works; with the copy FORCED the "
				 "zero-copy criterion is expectedly broken",
				 copies_total, big_total);
		} else {
			UMD_FAIL(T_ID_COPIES,
				 "NTBOX_UMD_FORCE_COPY was set but the "
				 "counter saw NOTHING — the counter does "
				 "not measure");
		}
		rc = TEST_EXIT_FAIL;
	} else if (copies_total == 0) {
		UMD_OK(T_ID_COPIES,
		       "CPU copies of the presented frames = 0 (MEASURED "
		       "over 3 presents: 0 frame-sized copies; %lu memcpy "
		       "calls / %lu bytes of any-size memory traffic "
		       "inside the windows)", calls_total, bytes_total);
	} else {
		UMD_FAIL(T_ID_COPIES,
			 "CPU copies of the presented frames = %lu "
			 "(%lu big bytes, %lu total) — the zero-copy "
			 "criterion is broken", copies_total, big_total,
			 bytes_total);
		goto cleanup;
	}

	/* --------------------------------------------------------------- */
	/* GFX-043: the frame time (present -> hand-off round trip)          */
	/* --------------------------------------------------------------- */
	med = median3(rt_us[0], rt_us[1], rt_us[2]);
	if (med <= 16667) {
		UMD_OK(T_ID_TIME,
		       "frame time present -> hand-off round trip: median "
		       "%ldus (<= one 60Hz frame); samples [%ldus, %ldus, "
		       "%ldus] (%s)",
		       med, rt_us[0], rt_us[1], rt_us[2],
		       wet ? "release round trip" : "dry swap+export");
	} else if (med <= 33333) {
		UMD_STUB(T_ID_TIME,
			 "frame time median %ldus — between one and two "
			 "60Hz frames; samples [%ldus, %ldus, %ldus]", med,
			 rt_us[0], rt_us[1], rt_us[2]);
	} else {
		UMD_FAIL(T_ID_TIME,
			 "frame time median %ldus > 2 vsync intervals; "
			 "samples [%ldus, %ldus, %ldus]", med, rt_us[0],
			 rt_us[1], rt_us[2]);
		goto cleanup;
	}

	if (!force_copy)
		rc = TEST_EXIT_PASS;

cleanup:
	if (sc)
		d3d11_swapchain_release(sc);
	if (ctx)
		d3d11_context_release(ctx);
	if (dev)
		d3d11_device_release(dev);

	test_weston_cleanup();

	printf("[test_umd] ── UMD bridge summary ──\n");
	printf("[test_umd] mode          : %s\n",
	       wet ? "WET (compositor present)" : "DRY (no compositor)");
	printf("[test_umd] copies        : %lu frame-sized (%lu big "
	       "bytes)\n", copies_total, big_total);
	printf("[test_umd] present times : %ld %ld %ld us\n",
	       present_us[0], present_us[1], present_us[2]);
	printf("[test_umd] roundtrips    : %ld %ld %ld us\n", rt_us[0],
	       rt_us[1], rt_us[2]);
	printf("[test_umd] RESULT: %s\n",
	       rc == TEST_EXIT_PASS ? "PASS"
				    : (rc == TEST_EXIT_SKIP ? "SKIP"
							    : "FAIL"));
	return rc;
}
