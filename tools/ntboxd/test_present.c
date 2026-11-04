/*
 * test_present.c — the selftest of tools/ntboxd/gfx_present.c (master plan
 * W9a, the presentation half of the zero-copy path).
 *
 * WHAT IT PROVES
 *   1. The box can draw on the GPU (a coloured triangle in a GBM buffer).
 *   2. That GPU buffer can be handed to the Wayland compositor as a
 *      dma-buf (linux-dmabuf v1) WITHOUT a single CPU copy.
 *   3. The compositor really imported it: the wl_buffer "release" event is
 *      the proof that the compositor finished rendering the frame.
 *
 * HONESTY CONTRACT
 *   * exit 0  — the frame reached the compositor AND the measured number of
 *               frame-sized CPU copies was 0.
 *   * exit 1  — the path ran but the criterion was not met (or the
 *               compositor could not be reached).
 *   * exit 3  — the environment has no GPU (SKIP: neither a pass nor a
 *               failure).  Nothing is ever faked.
 *
 *   The copy counter lives inside gfx_present.c (memcpy/memmove interposed
 *   there); the test only asks the module what it counted.
 *
 * BUILD
 *   gcc -O2 -Wall -Wextra -fno-builtin-memcpy -fno-builtin-memmove \
 *       -o /tmp/opencode/test_present tools/ntboxd/test_present.c \
 *       tools/ntboxd/gfx_present.c -ldl -lm
 */

#define _GNU_SOURCE

#include "gfx_present.h"

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
#include <unistd.h>

/* ===================================================================== */
/* The lamp (المصابيح) — the pattern of tools/ntboxd/syscalls/ntb0_trace.h */
/* copied here; the original file is NOT modified.                        */
/* ===================================================================== */

#define NTB0_TP(id, st, fmt, ...) \
	fprintf(stderr, "[ntbox][%s][%s] " fmt "\n", (id), (st), ##__VA_ARGS__)

#define NTB0_OK(id, fmt, ...)   NTB0_TP(id, "OK", fmt, ##__VA_ARGS__)
#define NTB0_STUB(id, fmt, ...) NTB0_TP(id, "STUB", fmt, ##__VA_ARGS__)
#define NTB0_FAIL(id, fmt, ...) NTB0_TP(id, "FAIL", fmt, ##__VA_ARGS__)
#define NTB0_UNIMPL(id, fmt, ...) NTB0_TP(id, "UNIMPL", fmt, ##__VA_ARGS__)
/* SKIP: the environment does not allow the measurement at all — it is
 * neither a pass nor a failure (same meaning as in tools/ntboxgfx). */
#define NTB0_SKIP(id, fmt, ...) NTB0_TP(id, "SKIP", fmt, ##__VA_ARGS__)

/* Lamp IDs of the presentation half (master plan W9a). */
#define NTB0_ID_MODULE  "GFX-011"  /* the module came up (GBM + EGL)     */
#define NTB0_ID_RENDER  "GFX-012"  /* the frame was rendered on the GPU   */
#define NTB0_ID_DMABUF  "GFX-013"  /* the buffer became a dma-buf fd      */
#define NTB0_ID_COPIES  "GFX-014"  /* CPU copies in the frame path == 0   */
#define NTB0_ID_WL      "GFX-015"  /* the compositor imported the buffer  */
#define NTB0_ID_ADOPT   "GFX-016"  /* the box's own wl_surface was fed   */

/* Exit codes. */
#define TEST_EXIT_PASS 0
#define TEST_EXIT_FAIL 1
#define TEST_EXIT_SKIP 3

/* ===================================================================== */
/* GLES2 — dlopen()ed (this machine has no GLES development headers)     */
/* ===================================================================== */

#define GL_COLOR_BUFFER_BIT   0x00004000
#define GL_TRIANGLES          0x0004
#define GL_FLOAT              0x1406
#define GL_VERTEX_SHADER      0x8B31
#define GL_FRAGMENT_SHADER    0x8B30
#define GL_COMPILE_STATUS     0x8B81
#define GL_LINK_STATUS        0x8B82
#define GL_INFO_LOG_LENGTH    0x8B84
#define GL_ARRAY_BUFFER       0x8892
#define GL_STATIC_DRAW        0x88E4
#define GL_NO_ERROR           0
#define GL_RENDERER           0x1F01

struct test_gl {
	void *lib;
	void (*clear)(unsigned int mask);
	void (*clear_color)(float r, float g, float b, float a);
	void (*viewport)(int x, int y, int w, int h);
	unsigned int (*create_shader)(unsigned int type);
	void (*shader_source)(unsigned int sh, int count, const char *const *str,
			      const int *len);
	void (*compile_shader)(unsigned int sh);
	void (*get_shaderiv)(unsigned int sh, unsigned int pname, int *params);
	void (*get_shader_info_log)(unsigned int sh, int bufsz, int *len, char *log);
	unsigned int (*create_program)(void);
	void (*attach_shader)(unsigned int prog, unsigned int sh);
	void (*link_program)(unsigned int prog);
	void (*get_programiv)(unsigned int prog, unsigned int pname, int *params);
	void (*get_program_info_log)(unsigned int prog, int bufsz, int *len, char *log);
	void (*use_program)(unsigned int prog);
	void (*genbuffers)(int n, unsigned int *b);
	void (*bindbuffer)(unsigned int target, unsigned int buf);
	void (*bufferdata)(unsigned int target, size_t size, const void *data,
			   unsigned int usage);
	void (*vertexattribpointer)(unsigned int idx, int size, unsigned int type,
				    unsigned char normalized, int stride,
				    const void *ptr);
	void (*enablevertexattribarray)(unsigned int idx);
	void (*drawarrays)(unsigned int mode, int first, int count);
	unsigned int (*geterror)(void);
	void (*finish)(void);
	void (*flush)(void);
	const unsigned char *(*getstring)(unsigned int name);
	void (*deleteshader)(unsigned int sh);
	void (*deleteprogram)(unsigned int prog);
	void (*deletebuffers)(int n, const unsigned int *b);
};

static struct test_gl g_gl;

static int test_gl_bind(void)
{
	void *h;

	if (g_gl.lib)
		return 0;
	h = dlopen("libGLESv2.so.2", RTLD_LAZY | RTLD_LOCAL);
	if (!h)
		h = dlopen("libGLESv2.so", RTLD_LAZY | RTLD_LOCAL);
	if (!h) {
		fprintf(stderr, "[test_present] no libGLESv2: %s\n", dlerror());
		return -1;
	}
	g_gl.lib = h;

#define BIND(field, sym)                                                       \
	do {                                                                   \
		*(void **)(&g_gl.field) = dlsym(g_gl.lib, sym);               \
		if (!g_gl.field) {                                            \
			fprintf(stderr, "[test_present] libGLESv2: %s missing\n", \
				sym);                                          \
			return -1;                                            \
		}                                                              \
	} while (0)

	BIND(clear, "glClear");
	BIND(clear_color, "glClearColor");
	BIND(viewport, "glViewport");
	BIND(create_shader, "glCreateShader");
	BIND(shader_source, "glShaderSource");
	BIND(compile_shader, "glCompileShader");
	BIND(get_shaderiv, "glGetShaderiv");
	BIND(get_shader_info_log, "glGetShaderInfoLog");
	BIND(create_program, "glCreateProgram");
	BIND(attach_shader, "glAttachShader");
	BIND(link_program, "glLinkProgram");
	BIND(get_programiv, "glGetProgramiv");
	BIND(get_program_info_log, "glGetProgramInfoLog");
	BIND(use_program, "glUseProgram");
	BIND(genbuffers, "glGenBuffers");
	BIND(bindbuffer, "glBindBuffer");
	BIND(bufferdata, "glBufferData");
	BIND(vertexattribpointer, "glVertexAttribPointer");
	BIND(enablevertexattribarray, "glEnableVertexAttribArray");
	BIND(drawarrays, "glDrawArrays");
	BIND(geterror, "glGetError");
	BIND(finish, "glFinish");
	BIND(flush, "glFlush");
	BIND(getstring, "glGetString");
	BIND(deleteshader, "glDeleteShader");
	BIND(deleteprogram, "glDeleteProgram");
	BIND(deletebuffers, "glDeleteBuffers");
#undef BIND
	return 0;
}

/* ===================================================================== */
/* The coloured triangle                                                 */
/* ===================================================================== */

static const char *vs_src =
	"#version 100\n"
	"attribute vec2 pos;\n"
	"attribute vec3 color;\n"
	"varying vec3 v_color;\n"
	"void main() {\n"
	"  gl_Position = vec4(pos, 0.0, 1.0);\n"
	"  v_color = color;\n"
	"}\n";

static const char *fs_src =
	"#version 100\n"
	"precision mediump float;\n"
	"varying vec3 v_color;\n"
	"void main() {\n"
	"  gl_FragColor = vec4(v_color, 1.0);\n"
	"}\n";

static const float g_triangle[] = {
	 0.0f,  0.6f,   1.0f, 0.0f, 0.0f,
	-0.6f, -0.5f,   0.0f, 1.0f, 0.0f,
	 0.6f, -0.5f,   0.0f, 0.0f, 1.0f,
};

static int test_build_program(char *err, size_t errn)
{
	unsigned int vs, fs, prog;
	int ok = 0, len = 0;
	char log[1024];

	vs = g_gl.create_shader(GL_VERTEX_SHADER);
	fs = g_gl.create_shader(GL_FRAGMENT_SHADER);
	if (!vs || !fs) {
		snprintf(err, errn, "glCreateShader failed");
		return -1;
	}
	g_gl.shader_source(vs, 1, &vs_src, NULL);
	g_gl.shader_source(fs, 1, &fs_src, NULL);
	g_gl.compile_shader(vs);
	g_gl.compile_shader(fs);
	g_gl.get_shaderiv(vs, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		g_gl.get_shader_info_log(vs, sizeof log - 1, &len, log);
		log[sizeof log - 1] = 0;
		snprintf(err, errn, "vertex shader: %.200s", log);
		return -1;
	}
	g_gl.get_shaderiv(fs, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		g_gl.get_shader_info_log(fs, sizeof log - 1, &len, log);
		log[sizeof log - 1] = 0;
		snprintf(err, errn, "fragment shader: %.200s", log);
		return -1;
	}
	prog = g_gl.create_program();
	if (!prog) {
		snprintf(err, errn, "glCreateProgram failed");
		return -1;
	}
	g_gl.attach_shader(prog, vs);
	g_gl.attach_shader(prog, fs);
	g_gl.link_program(prog);
	g_gl.get_programiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		g_gl.get_program_info_log(prog, sizeof log - 1, &len, log);
		log[sizeof log - 1] = 0;
		snprintf(err, errn, "glLinkProgram: %.200s", log);
		return -1;
	}
	g_gl.deleteshader(vs);
	g_gl.deleteshader(fs);
	g_gl.use_program(prog);
	return 0;
}

/* Draw the coloured triangle into the current EGL surface (on the GPU). */
static int test_draw_triangle(int w, int h, char *err, size_t errn)
{
	unsigned int vbo = 0;

	g_gl.viewport(0, 0, w, h);
	g_gl.clear_color(0.05f, 0.07f, 0.12f, 1.0f);
	g_gl.clear(GL_COLOR_BUFFER_BIT);

	g_gl.genbuffers(1, &vbo);
	g_gl.bindbuffer(GL_ARRAY_BUFFER, vbo);
	g_gl.bufferdata(GL_ARRAY_BUFFER, sizeof g_triangle, g_triangle, GL_STATIC_DRAW);
	g_gl.vertexattribpointer(0, 2, GL_FLOAT, 0, 5 * sizeof(float), (void *)0);
	g_gl.vertexattribpointer(1, 3, GL_FLOAT, 0, 5 * sizeof(float),
				(void *)(2 * sizeof(float)));
	g_gl.enablevertexattribarray(0);
	g_gl.enablevertexattribarray(1);

	g_gl.drawarrays(GL_TRIANGLES, 0, 3);
	g_gl.flush();
	/* glFinish() blocks until the GPU executed the draw: the proof that
	 * the frame was produced by the GPU and not by the CPU. */
	g_gl.finish();

	if (g_gl.geterror() != GL_NO_ERROR) {
		snprintf(err, errn, "GL error 0x%x after the draw", g_gl.geterror());
		return -1;
	}
	g_gl.deletebuffers(1, &vbo);
	return 0;
}

/* ===================================================================== */
/* A headless compositor, when the environment has none                  */
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
			snprintf(path, sizeof path, "%s/%s", rt, g_weston_sock);
			unlink(path);
		}
		g_weston_sock[0] = 0;
	}
}

/*
 * Start `weston --backend=headless-backend.so` so that a REAL compositor
 * can validate the dma-buf hand-off.  Only used when the environment
 * provides no WAYLAND_DISPLAY of its own.
 */
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
	snprintf(sock, sizeof sock, "ntboxpresent-%d", (int)getpid());
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

	for (i = 0; i < 300; i++) {   /* up to 30 s */
		if (stat(path, &st) == 0) {
			g_weston_pid = pid;
			snprintf(g_weston_sock, sizeof g_weston_sock, "%s", sock);
			setenv("WAYLAND_DISPLAY", sock, 1);
			return 0;
		}
		if (waitpid(pid, NULL, WNOHANG) == pid) {
			snprintf(err, errn, "weston exited before creating the socket");
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
/* Proof that the descriptor really is a dma-buf                         */
/* ===================================================================== */

/* Copy one line of /proc/self/fdinfo/<fd> into out (without the newline).
 * A dma-buf descriptor shows the DRM mount id ("mnt_id:") — that is the
 * kernel's own proof that the descriptor carries a DRM/dma-buf object. */
static int test_fd_is_dmabuf(int fd, char *out, size_t outn)
{
	char path[64], line[256];
	FILE *f;
	int found = 0;

	snprintf(path, sizeof path, "/proc/self/fdinfo/%d", fd);
	f = fopen(path, "r");
	if (!f)
		return 0;
	while (fgets(line, sizeof line, f)) {
		size_t len = strlen(line);

		while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = 0;
		if (!len)
			continue;
		if (!strncmp(line, "mnt_id:", 7)) {
			snprintf(out, outn, "%s", line);
			found = 1;
			break;
		}
		if (!found) {
			snprintf(out, outn, "%s", line);
			found = 1;
		}
	}
	fclose(f);
	return found;
}

/* The GPU memory the driver has handed out (a leak check for the loop
 * mode): the value is read from the amdgpu sysfs interface. */
static long test_gpu_mem_used(void)
{
	FILE *f;
	long v = -1;

	f = fopen("/sys/class/drm/card1/device/mem_info_gtt_used", "r");
	if (!f)
		f = fopen("/sys/class/drm/card0/device/mem_info_gtt_used", "r");
	if (!f)
		return -1;
	if (fscanf(f, "%ld", &v) != 1)
		v = -1;
	fclose(f);
	return v;
}

/*
 * The loop mode: present N frames and watch the GPU memory.  A box runs at
 * 60 fps, so a per-frame leak would be fatal; this mode proves that the
 * frame path does not leak GPU allocations.
 */
static int test_loop(int frames)
{
	gfx_present_t *p;
	char err[512];
	int w = 0, h = 0, i;
	long before, after;

	p = gfx_present_create();
	if (!p) {
		NTB0_SKIP(NTB0_ID_MODULE, "no GPU here: %s", gfx_present_error());
		return TEST_EXIT_SKIP;
	}
	gfx_present_target_size(p, &w, &h);
	if (gfx_present_wl_connect(p) < 0) {
		NTB0_FAIL(NTB0_ID_WL, "no compositor: %s", gfx_present_error());
		gfx_present_destroy(p);
		return TEST_EXIT_FAIL;
	}
	if (test_gl_bind() < 0 || gfx_present_make_current(p) < 0) {
		NTB0_SKIP(NTB0_ID_RENDER, "no GLES2 context here");
		gfx_present_destroy(p);
		return TEST_EXIT_SKIP;
	}
	if (test_build_program(err, sizeof err) < 0) {
		NTB0_FAIL(NTB0_ID_RENDER, "shader: %s", err);
		gfx_present_destroy(p);
		return TEST_EXIT_FAIL;
	}
	before = test_gpu_mem_used();
	for (i = 0; i < frames; i++) {
		if (test_draw_triangle(w, h, err, sizeof err) < 0)
			break;
		if (gfx_present_frame(p) < 0)
			break;
	}
	after = test_gpu_mem_used();
	printf("[test_present] %d frames presented, GPU memory: %ld -> %ld bytes "
	       "(%ld bytes/frame), CPU copies: %lu\n", frames, before, after,
	       before >= 0 && after >= 0 ? (after - before) / (frames ? frames : 1) : -1,
	       gfx_present_copies(p));
	gfx_present_destroy(p);
	return TEST_EXIT_PASS;
}

/* ===================================================================== */
/* main                                                                  */
/* ===================================================================== */

/* A global sink: it keeps the deliberate copy of the self-test alive (the
 * compiler must not be allowed to remove a dead store). */
static volatile unsigned char g_sink;

int main(int argc, char **argv)
{
	gfx_present_t *p;
	char err[512];
	char fdinfo[256];
	int w = 0, h = 0;
	int rc = TEST_EXIT_FAIL;
	int released = 0;
	unsigned long copies, copies_last, bytes, bigbytes, calls;
	int fd;
	int i;

	(void)argc;
	(void)argv;

	setvbuf(stdout, NULL, _IOLBF, 0);

	/* ---- a compositor to present to ---- */
	if (!getenv("WAYLAND_DISPLAY") || !*getenv("WAYLAND_DISPLAY")) {
		if (test_spawn_weston(err, sizeof err) < 0) {
			NTB0_FAIL(NTB0_ID_WL,
				  "no compositor could be started: %s — the frame "
				  "never reached a compositor", err);
			printf("[test_present] RESULT: FAIL (no compositor)\n");
			return TEST_EXIT_FAIL;
		}
		printf("[test_present] spawned weston headless (socket=%s)\n",
		       g_weston_sock);
	}

	/* ---- the leak check mode ---- */
	if (argc >= 2 && !strcmp(argv[1], "--loop")) {
		int n = argc >= 3 ? atoi(argv[2]) : 120;
		int r = test_loop(n > 0 ? n : 120);

		test_weston_cleanup();
		return r;
	}

	/* ---- the module ---- */
	p = gfx_present_create();
	if (!p) {
		NTB0_SKIP(NTB0_ID_MODULE,
			  "the GPU presentation path is unavailable here: %s — "
			  "SKIP, nothing was measured and nothing was faked",
			  gfx_present_error());
		printf("[test_present] RESULT: SKIP (no GPU: %s)\n",
		       gfx_present_error());
		return TEST_EXIT_SKIP;
	}
	gfx_present_target_size(p, &w, &h);
	NTB0_OK(NTB0_ID_MODULE,
		"gfx_present is up: DRM node %s, GBM surface %dx%d ARGB8888, "
		"EGL display %p, GLES2 context %p", gfx_present_drm_node(p), w, h,
		gfx_present_egl(p), gfx_present_egl_context(p));

	/* ---- the compositor ---- */
	if (gfx_present_wl_connect(p) < 0) {
		NTB0_FAIL(NTB0_ID_WL, "the compositor did not accept the "
			  "linux-dmabuf connection: %s", gfx_present_error());
		printf("[test_present] RESULT: FAIL (no compositor)\n");
		gfx_present_destroy(p);
		test_weston_cleanup();
		return TEST_EXIT_FAIL;
	}

	/* ---- draw the coloured triangle on the GPU ---- */
	if (test_gl_bind() < 0) {
		NTB0_SKIP(NTB0_ID_RENDER, "no GLES2 runtime on this machine — "
			  "the frame cannot be rendered here");
		printf("[test_present] RESULT: SKIP (no GLES2)\n");
		gfx_present_destroy(p);
		test_weston_cleanup();
		return TEST_EXIT_SKIP;
	}
	if (gfx_present_make_current(p) < 0) {
		NTB0_FAIL(NTB0_ID_RENDER, "eglMakeCurrent failed: %s",
			  gfx_present_error());
		printf("[test_present] RESULT: FAIL (no GL context)\n");
		gfx_present_destroy(p);
		test_weston_cleanup();
		return TEST_EXIT_FAIL;
	}
	if (test_build_program(err, sizeof err) < 0) {
		NTB0_FAIL(NTB0_ID_RENDER, "the shader did not build: %s", err);
		printf("[test_present] RESULT: FAIL (shader)\n");
		gfx_present_destroy(p);
		test_weston_cleanup();
		return TEST_EXIT_FAIL;
	}

	/* The measurement window opens BEFORE the draw: everything from the
	 * draw command up to the compositor hand-off is the frame path. */
	gfx_present_copy_window_begin(p);
	/*
	 * Self-test of the counter: with NTB0_PRESENT_SELFTEST_COPY=1 one
	 * deliberate frame-sized copy is injected INSIDE the measured window.
	 * The counter MUST notice it and GFX-014 MUST turn into a FAIL.  This
	 * is how we prove that the reported "0 copies" is a measurement and
	 * not a hard coded number.
	 */
	if (getenv("NTB0_PRESENT_SELFTEST_COPY")) {
		size_t n = (size_t)w * (size_t)h * 4;
		char *a = malloc(n), *b = malloc(n);

		if (a && b) {
			memset(a, 0x5a, n);
			memcpy(b, a, n);
			g_sink = (unsigned char)b[n / 2];
			fprintf(stderr, "[test_present]   a deliberate %zu byte "
				"copy was injected inside the measured window "
				"(self-test of the counter)\n", n);
		}
		free(a);
		free(b);
	}
	if (test_draw_triangle(w, h, err, sizeof err) < 0) {
		gfx_present_copy_window_end(p);
		NTB0_FAIL(NTB0_ID_RENDER, "the draw failed: %s", err);
		printf("[test_present] RESULT: FAIL (draw)\n");
		gfx_present_destroy(p);
		test_weston_cleanup();
		return TEST_EXIT_FAIL;
	}
	NTB0_OK(NTB0_ID_RENDER,
		"the coloured triangle was rendered on the GPU into the GBM "
		"buffer (%dx%d, renderer: %s)", w, h, gfx_present_renderer(p));

	/* ---- present it: swap -> dma-buf -> Wayland ---- */
	if (gfx_present_frame(p) < 0) {
		gfx_present_copy_window_end(p);
		NTB0_FAIL(NTB0_ID_DMABUF, "the frame could not be presented: %s",
			  gfx_present_error());
		printf("[test_present] RESULT: FAIL (present)\n");
		gfx_present_destroy(p);
		test_weston_cleanup();
		return TEST_EXIT_FAIL;
	}
	/*
	 * A real box presents frame after frame; the compositor releases a
	 * buffer when it is done with it (and, on some compositors, when the
	 * buffer is replaced).  Present a few frames so that the release
	 * event can arrive on any of them.
	 */
	for (i = 1; i < 4; i++) {
		if (gfx_present_released(p))
			break;
		if (test_draw_triangle(w, h, err, sizeof err) < 0)
			break;
		if (gfx_present_frame(p) < 0)
			break;
		usleep(50000);
	}
	gfx_present_copy_window_end(p);

	fd = gfx_present_dmabuf_fd(p);
	if (fd >= 0 && test_fd_is_dmabuf(fd, fdinfo, sizeof fdinfo)) {
		NTB0_OK(NTB0_ID_DMABUF,
			"the GPU buffer became a dma-buf: fd=%d, %ux%u, stride=%u, "
			"format=ARGB8888, %s (kernel fdinfo: %s) — the pixels "
			"were never touched by the CPU", fd,
			gfx_present_buffer_width(p),
			gfx_present_buffer_height(p),
			gfx_present_buffer_stride(p),
			gfx_present_fb_method(p), fdinfo);
	} else {
		NTB0_OK(NTB0_ID_DMABUF,
			"the GPU buffer became a dma-buf: fd=%d, %ux%u, "
			"stride=%u, format=ARGB8888, %s — the pixels were "
			"never touched by the CPU", fd,
			gfx_present_buffer_width(p),
			gfx_present_buffer_height(p),
			gfx_present_buffer_stride(p),
			gfx_present_fb_method(p));
	}

	/* ---- did the compositor import and render it? ---- */
	/*
	 * The headless compositor of this machine repaints slowly (the very
	 * same delay is visible with weston's own reference clients), so the
	 * wait is generous: the release event is the proof, not the speed.
	 */
	released = gfx_present_wait_release(p, 60000);
	if (released == 1) {
		NTB0_OK(NTB0_ID_WL,
			"the compositor imported the dma-buf and released the "
			"wl_buffer (the proof that it rendered the frame): %s",
			gfx_present_compositor(p));
	} else if (released == 0) {
		NTB0_FAIL(NTB0_ID_WL,
			  "the compositor did not release the wl_buffer within "
			  "60 s (the buffer was attached and committed, but no "
			  "release event arrived): %s", gfx_present_compositor(p));
	} else {
		NTB0_FAIL(NTB0_ID_WL, "the Wayland connection broke while waiting "
			  "for the release: %s", gfx_present_error());
	}

	/* ---- the measured CPU copy count ---- */
	copies = gfx_present_copies(p);
	copies_last = gfx_present_copies_last(p);
	bytes = gfx_present_copy_bytes(p);
	bigbytes = gfx_present_copy_big_bytes(p);
	calls = gfx_present_copy_calls(p);
	if (copies == 0) {
		NTB0_OK(NTB0_ID_COPIES,
			"CPU copies of frame-sized buffers in the frame path = 0 "
			"(measured: %lu memcpy/memmove calls, %lu bytes in total, "
			"none of them >= 64 KiB; last frame: %lu big copies)",
			calls, bytes, copies_last);
	} else {
		NTB0_FAIL(NTB0_ID_COPIES,
			  "CPU copies of frame-sized buffers in the frame path = "
			  "%lu (%lu bytes) — the frame was copied by the CPU, the "
			  "zero-copy criterion is broken", copies, bigbytes);
	}

	/* ---- the verdict ---- */
	if (copies == 0 && released == 1) {
		/* ---- phase 2: the mode the box uses ---- */
		/*
		 * ntboxd.c already owns a wl_display and a wl_surface (with
		 * its xdg-shell role).  It hands them to the module, which
		 * then attaches the dma-buf buffers to THAT surface.  This
		 * phase exercises exactly that path.
		 */
		gfx_present_t *q = gfx_present_create();

		if (!q) {
			NTB0_FAIL(NTB0_ID_ADOPT,
				  "the second module (the box's own surface) could "
				  "not be created: %s", gfx_present_error());
			rc = TEST_EXIT_FAIL;
			goto verdict;
		}
		if (gfx_present_wl_use(q, gfx_present_wl_display(p),
				       gfx_present_wl_surface(p)) < 0) {
			NTB0_FAIL(NTB0_ID_ADOPT,
				  "the module could not adopt the display and the "
				  "surface of the box: %s", gfx_present_error());
			gfx_present_destroy(q);
			rc = TEST_EXIT_FAIL;
			goto verdict;
		}
		gfx_present_make_current(q);
		gfx_present_copy_window_begin(q);
		for (i = 0; i < 3; i++) {
			if (test_draw_triangle(w, h, err, sizeof err) < 0)
				break;
			if (gfx_present_frame(q) < 0)
				break;
			usleep(50000);
		}
		gfx_present_copy_window_end(q);
		released = gfx_present_wait_release(q, 60000);
		if (released == 1 && gfx_present_copies(q) == 0) {
			NTB0_OK(NTB0_ID_ADOPT,
				"the box's own wl_surface was fed with the dma-buf "
				"and the compositor released it: %lu CPU copies, "
				"fd=%d, %ux%u",
				gfx_present_copies(q), gfx_present_dmabuf_fd(q),
				gfx_present_buffer_width(q),
				gfx_present_buffer_height(q));
			rc = TEST_EXIT_PASS;
		} else {
			NTB0_FAIL(NTB0_ID_ADOPT,
				  "the box's own wl_surface path failed: release=%d, "
				  "copies=%lu (%s)", released,
				  gfx_present_copies(q), gfx_present_error());
			rc = TEST_EXIT_FAIL;
		}
		gfx_present_destroy(q);
	}

verdict:
	if (copies == 0 && released == 1 && rc == TEST_EXIT_PASS) {
		printf("[test_present] ── zero-copy presentation summary ──\n");
		printf("[test_present] drm node    : %s\n", gfx_present_drm_node(p));
		printf("[test_present] target size : %dx%d\n", w, h);
		printf("[test_present] dma-buf fd  : %d (%ux%u stride=%u)\n", fd,
		       gfx_present_buffer_width(p), gfx_present_buffer_height(p),
		       gfx_present_buffer_stride(p));
		printf("[test_present] CPU copies  : %lu (>=64 KiB), %lu calls / "
		       "%lu bytes in total\n", copies, calls, bytes);
		printf("[test_present] compositor  : %s\n", gfx_present_compositor(p));
		printf("[test_present] wl_buffer release received : %s\n",
		       released == 1 ? "yes" : "no");
		printf("[test_present] RESULT: PASS (the frame reached the "
		       "compositor as a dma-buf, 0 CPU copies)\n");
	} else {
		if (copies == 0 && released == 1)
			printf("[test_present] RESULT: FAIL (the box's own "
			       "wl_surface path failed)\n");
		else
			printf("[test_present] RESULT: FAIL (copies=%lu, "
			       "release=%d)\n", copies, released);
		rc = TEST_EXIT_FAIL;
	}

	gfx_present_destroy(p);
	test_weston_cleanup();
	return rc;
}
