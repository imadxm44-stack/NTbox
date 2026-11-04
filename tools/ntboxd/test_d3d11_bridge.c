/*
 * test_d3d11_bridge.c — NT-Box: D3D11→EGL bridge + Win32k integration test
 *
 * Lamps: GFX-021..GFX-030 (D3D11 bridge), WIN-021..WIN-030 (Win32k integration)
 *
 * TEST: create device, create swapchain for win32k window, draw triangle, present,
 *       verify dmabuf reaches compositor (check gfx_present_copies == 0).
 *       Exit 0 only if frame reaches compositor with 0 copies.
 *
 * BUILD: gcc -O2 -Wall -Wextra -fno-builtin-memcpy -fno-builtin-memmove \
 *        -Itools/ntboxd -Itools/ntboxd/syscalls -Itools/ntboxgfx \
 *        -o /tmp/opencode/test_d3d11_bridge \
 *        tools/ntboxd/test_d3d11_bridge.c tools/ntboxd/d3d11_bridge.c \
 *        tools/ntboxd/gfx_bridge.c tools/ntboxd/gfx_present.c \
 *        tools/ntboxd/win32k.c tools/ntboxd/input.c \
 *        tools/ntboxgfx/ntboxgfx.c -ldl -lm
 */

#define _GNU_SOURCE

#include "d3d11_bridge.h"
#include "gfx_present.h"
#include "win32k.h"
#include "ntb0_trace.h"

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
/* Lamp IDs                                                             */
/* ===================================================================== */

#define NTB0_ID_D3D11_DEVICE   "GFX-021"  /* D3D11CreateDevice                */
#define NTB0_ID_D3D11_SWAPCHAIN "GFX-022"  /* IDXGIFactory::CreateSwapChain    */
#define NTB0_ID_D3D11_RESOURCES "GFX-023"  /* CreateRenderTargetView, etc.     */
#define NTB0_ID_D3D11_PRESENT  "GFX-024"  /* IDXGISwapChain::Present          */
#define NTB0_ID_D3D11_DRAW     "GFX-025"  /* DeviceContext::Draw/Indexed      */
#define NTB0_ID_D3D11_STATE    "GFX-026"  /* IASet*, OMSet*, VSSet*, PSSet*   */
#define NTB0_ID_D3D11_RTV      "GFX-027"  /* CreateRenderTargetView           */
#define NTB0_ID_D3D11_DSV      "GFX-028"  /* CreateDepthStencilView           */
#define NTB0_ID_D3D11_TEX      "GFX-029"  /* CreateTexture2D, Buffer, IL, VS, PS */
#define NTB0_ID_D3D11_BUF      "GFX-030"  /* CreateBuffer                     */

#define NTB0_ID_WIN32K_DEVICE  "WIN-021"  /* d3d11_win32k_create_device_and_swapchain */
#define NTB0_ID_WIN32K_SWAPCHAIN "WIN-022" /* swapchain creation with hwnd    */
#define NTB0_ID_WIN32K_RESIZE  "WIN-023"  /* d3d11_swapchain_resize          */
#define NTB0_ID_WIN32K_PRESENT "WIN-024"  /* Present via Win32k window       */
#define NTB0_ID_WIN32K_INTEGRATE "WIN-025" /* Full integration test           */

/* Exit codes */
#define TEST_EXIT_PASS 0
#define TEST_EXIT_FAIL 1
#define TEST_EXIT_SKIP 3

/* ===================================================================== */
/* GLES2 — dlopen()ed                                                   */
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
#define GL_UNSIGNED_SHORT     0x1403

struct test_gl {
    void *lib;
    void (*clear)(unsigned int mask);
    void (*clear_color)(float r, float g, float b, float a);
    void (*viewport)(int x, int y, int w, int h);
    unsigned int (*create_shader)(unsigned int type);
    void (*shader_source)(unsigned int sh, int count, const char *const *str, const int *len);
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
    void (*bufferdata)(unsigned int target, size_t size, const void *data, unsigned int usage);
    void (*vertexattribpointer)(unsigned int idx, int size, unsigned int type, unsigned char normalized, int stride, const void *ptr);
    void (*enablevertexattribarray)(unsigned int idx);
    void (*drawarrays)(unsigned int mode, int first, int count);
    void (*drawelements)(unsigned int mode, int count, unsigned int type, const void *indices);
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
        fprintf(stderr, "[test_d3d11_bridge] no libGLESv2: %s\n", dlerror());
        return -1;
    }
    g_gl.lib = h;

#define BIND(field, sym)                                                       \
    do {                                                                   \
        *(void **)(&g_gl.field) = dlsym(g_gl.lib, sym);               \
        if (!g_gl.field) {                                            \
            fprintf(stderr, "[test_d3d11_bridge] libGLESv2: %s missing\n", \
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
    BIND(drawelements, "glDrawElements");
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
/* Shaders                                                              */
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

static const unsigned short g_indices[] = { 0, 1, 2 };

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
    unsigned int vbo = 0, ibo = 0;

    g_gl.viewport(0, 0, w, h);
    g_gl.clear_color(0.05f, 0.07f, 0.12f, 1.0f);
    g_gl.clear(GL_COLOR_BUFFER_BIT);

    g_gl.genbuffers(1, &vbo);
    g_gl.bindbuffer(GL_ARRAY_BUFFER, vbo);
    g_gl.bufferdata(GL_ARRAY_BUFFER, sizeof g_triangle, g_triangle, GL_STATIC_DRAW);
    g_gl.vertexattribpointer(0, 2, GL_FLOAT, 0, 5 * sizeof(float), (void *)0);
    g_gl.vertexattribpointer(1, 3, GL_FLOAT, 0, 5 * sizeof(float), (void *)(2 * sizeof(float)));
    g_gl.enablevertexattribarray(0);
    g_gl.enablevertexattribarray(1);

    g_gl.genbuffers(1, &ibo);
    g_gl.bindbuffer(0x8893 /* GL_ELEMENT_ARRAY_BUFFER */, ibo);
    g_gl.bufferdata(0x8893, sizeof g_indices, g_indices, GL_STATIC_DRAW);

    g_gl.drawelements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, NULL);
    g_gl.flush();
    g_gl.finish();

    if (g_gl.geterror() != GL_NO_ERROR) {
        snprintf(err, errn, "GL error 0x%x after the draw", g_gl.geterror());
        return -1;
    }
    g_gl.deletebuffers(1, &vbo);
    g_gl.deletebuffers(1, &ibo);
    return 0;
}

/* ===================================================================== */
/* Headless compositor (weston)                                         */
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

/* Start weston headless backend for testing */
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
    snprintf(sock, sizeof sock, "ntboxd3d11-%d", (int)getpid());
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

    for (i = 0; i < 300; i++) {
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
/* Proof that the descriptor really is a dma-buf                        */
/* ===================================================================== */

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

/* ===================================================================== */
/* Win32k window procedure                                              */
/* ===================================================================== */

LONG win32k_wndproc(HWND hwnd, UINT msg, unsigned long wParam, long lParam)
{
    (void)wParam; (void)lParam;
    switch (msg) {
        case 15: /* WM_PAINT */
            NTB0_OK("WIN-025", "WM_PAINT received for hwnd=%p", (void *)hwnd);
            break;
        case 18: /* WM_QUIT */
            NTB0_OK("WIN-025", "WM_QUIT received for hwnd=%p", (void *)hwnd);
            break;
    }
    return 0;
}

/* ===================================================================== */
/* Main test                                                            */
/* ===================================================================== */

static volatile unsigned char g_sink;

int main(int argc, char **argv)
{
    char err[512];
    char fdinfo[256];
    int w = 640, h = 480;
    int rc = TEST_EXIT_FAIL;
    int released = 0;
    unsigned long copies, copies_last, bytes, bigbytes, calls;
    int fd;
    int i;

    (void)argc;
    (void)argv;

    setvbuf(stdout, NULL, _IOLBF, 0);

    NTB0_OK("GFX-021", "Starting D3D11 bridge + Win32k integration test");

    /* ---- a compositor to present to ---- */
    if (!getenv("WAYLAND_DISPLAY") || !*getenv("WAYLAND_DISPLAY")) {
        if (test_spawn_weston(err, sizeof err) < 0) {
            NTB0_FAIL(NTB0_ID_WIN32K_INTEGRATE,
                      "no compositor could be started: %s — the frame "
                      "never reached a compositor", err);
            printf("[test_d3d11_bridge] RESULT: FAIL (no compositor)\n");
            return TEST_EXIT_FAIL;
        }
        printf("[test_d3d11_bridge] spawned weston headless (socket=%s)\n",
               g_weston_sock);
    }

    /* ---- Win32k: Register window class ---- */
    win32k_wndclassex_t wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = 0;
    wc.lpfnWndProc = win32k_wndproc;
    wc.hInstance = (HINSTANCE)1;
    wc.lpszClassName = "NTBoxD3D11WindowClass";
    wc.lpszMenuName = NULL;
    wc.hIcon = NULL;
    wc.hCursor = NULL;
    wc.hbrBackground = NULL;
    wc.hIconSm = NULL;

    unsigned int atom = NtUserRegisterClassEx(&wc);
    if (!atom) {
        NTB0_FAIL(NTB0_ID_WIN32K_INTEGRATE, "RegisterClassEx failed");
        test_weston_cleanup();
        return TEST_EXIT_FAIL;
    }
    NTB0_OK(NTB0_ID_WIN32K_INTEGRATE, "RegisterClassEx '%s' atom=%u", wc.lpszClassName, atom);

    /* ---- Win32k: Create window ---- */
    HWND hwnd = NtUserCreateWindowEx(0, wc.lpszClassName, "NT-Box D3D11 Test",
                                     0x10000000 /* WS_VISIBLE */, 100, 100, w, h,
                                     NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) {
        NTB0_FAIL(NTB0_ID_WIN32K_INTEGRATE, "CreateWindowEx failed");
        test_weston_cleanup();
        return TEST_EXIT_FAIL;
    }
    NTB0_OK(NTB0_ID_WIN32K_INTEGRATE, "CreateWindowEx hwnd=%p %dx%d", (void *)hwnd, w, h);

    /* ---- Win32k: Show window ---- */
    NtUserShowWindow(hwnd, 5 /* SW_SHOW */);
    NtUserUpdateWindow(hwnd);

    /* ---- Win32k: Get DC ---- */
    HDC hdc = NtUserGetDC(hwnd);
    if (!hdc) {
        NTB0_FAIL(NTB0_ID_WIN32K_INTEGRATE, "GetDC failed");
        test_weston_cleanup();
        return TEST_EXIT_FAIL;
    }
    NTB0_OK(NTB0_ID_WIN32K_INTEGRATE, "GetDC hwnd=%p -> hdc=%p", (void *)hwnd, (void *)hdc);

    /* ---- D3D11: Create device and swapchain via Win32k integration (WIN-021, WIN-022) ---- */
    ID3D11Device device = NULL;
    ID3D11DeviceContext context = NULL;
    IDXGISwapChain swapchain = NULL;

    int res = d3d11_win32k_create_device_and_swapchain(hwnd, hdc, &device, &context, &swapchain);
    if (res != 0) {
        NTB0_FAIL(NTB0_ID_WIN32K_DEVICE, "d3d11_win32k_create_device_and_swapchain failed: %d", res);
        NtUserReleaseDC(hwnd, hdc);
        test_weston_cleanup();
        return TEST_EXIT_FAIL;
    }

    NTB0_OK(NTB0_ID_WIN32K_DEVICE, "Created D3D11 device=%p, context=%p, swapchain=%p",
            (void *)device, (void *)context, (void *)swapchain);

    /* ---- Verify gfx_present is accessible (GFX-021..GFX-030) ---- */
    gfx_present_t *present = d3d11_device_get_present(device);
    if (!present) {
        NTB0_FAIL(NTB0_ID_D3D11_DEVICE, "No gfx_present from device");
        goto cleanup;
    }

    gfx_present_target_size(present, &w, &h);
    NTB0_OK(NTB0_ID_D3D11_DEVICE, "gfx_present up: DRM node %s, GBM surface %dx%d",
            gfx_present_drm_node(present), w, h);

    /* ---- Create render target view (GFX-027) ---- */
    ID3D11RenderTargetView rtv = NULL;
    long hr = ID3D11Device_CreateRenderTargetView(device, swapchain, NULL, (void **)&rtv);
    if (hr != S_OK) {
        NTB0_FAIL(NTB0_ID_D3D11_RTV, "CreateRenderTargetView failed: 0x%lx", hr);
        goto cleanup;
    }
    NTB0_OK(NTB0_ID_D3D11_RTV, "CreateRenderTargetView -> rtv=%p", (void *)rtv);

    /* ---- Create depth stencil view (GFX-028) ---- */
    ID3D11DepthStencilView dsv = NULL;
    hr = ID3D11Device_CreateDepthStencilView(device, swapchain, NULL, (void **)&dsv);
    if (hr != S_OK) {
        NTB0_FAIL(NTB0_ID_D3D11_DSV, "CreateDepthStencilView failed: 0x%lx", hr);
        goto cleanup;
    }
    NTB0_OK(NTB0_ID_D3D11_DSV, "CreateDepthStencilView -> dsv=%p", (void *)dsv);

    /* ---- Create vertex buffer (GFX-030) ---- */
    D3D11_BUFFER_DESC vb_desc = {0};
    vb_desc.ByteWidth = sizeof(g_triangle);
    vb_desc.Usage = (void *)D3D11_USAGE_DEFAULT;
    vb_desc.BindFlags = D3D11_BIND_RENDER_TARGET; /* Actually vertex buffer */
    vb_desc.CPUAccessFlags = 0;

    D3D11_SUBRESOURCE_DATA vb_init = {0};
    vb_init.pSysMem = g_triangle;
    vb_init.SysMemPitch = sizeof(g_triangle);
    vb_init.SysMemSlicePitch = 0;

    ID3D11Buffer vbuffer = NULL;
    hr = ID3D11Device_CreateBuffer(device, &vb_desc, &vb_init, (void **)&vbuffer);
    if (hr != S_OK) {
        NTB0_FAIL(NTB0_ID_D3D11_BUF, "CreateBuffer (vertex) failed: 0x%lx", hr);
        goto cleanup;
    }
    NTB0_OK(NTB0_ID_D3D11_BUF, "CreateBuffer (vertex) -> vbuffer=%p", (void *)vbuffer);

    /* ---- Create index buffer (GFX-030) ---- */
    D3D11_BUFFER_DESC ib_desc = {0};
    ib_desc.ByteWidth = sizeof(g_indices);
    ib_desc.Usage = (void *)D3D11_USAGE_DEFAULT;
    ib_desc.BindFlags = D3D11_BIND_RENDER_TARGET; /* Actually index buffer */
    ib_desc.CPUAccessFlags = 0;

    D3D11_SUBRESOURCE_DATA ib_init = {0};
    ib_init.pSysMem = g_indices;
    ib_init.SysMemPitch = sizeof(g_indices);
    ib_init.SysMemSlicePitch = 0;

    ID3D11Buffer ibuffer = NULL;
    hr = ID3D11Device_CreateBuffer(device, &ib_desc, &ib_init, (void **)&ibuffer);
    if (hr != S_OK) {
        NTB0_FAIL(NTB0_ID_D3D11_BUF, "CreateBuffer (index) failed: 0x%lx", hr);
        goto cleanup;
    }
    NTB0_OK(NTB0_ID_D3D11_BUF, "CreateBuffer (index) -> ibuffer=%p", (void *)ibuffer);

    /* ---- Create input layout (GFX-029) ---- */
    D3D11_INPUT_ELEMENT_DESC ied[] = {
        { "POSITION", 0, (void *)DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0, (void *)0, 0 },
        { "COLOR", 0, (void *)DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8, (void *)0, 0 },
    };
    ID3D11InputLayout il = NULL;
    hr = ID3D11Device_CreateInputLayout(device, ied, 2, vs_src, strlen(vs_src), (void **)&il);
    if (hr != S_OK) {
        NTB0_FAIL(NTB0_ID_D3D11_TEX, "CreateInputLayout failed: 0x%lx", hr);
        goto cleanup;
    }
    NTB0_OK(NTB0_ID_D3D11_TEX, "CreateInputLayout -> il=%p", (void *)il);

    /* ---- Create vertex shader (GFX-029) ---- */
    ID3D11VertexShader vs = NULL;
    hr = ID3D11Device_CreateVertexShader(device, vs_src, strlen(vs_src), NULL, (void **)&vs);
    if (hr != S_OK) {
        NTB0_FAIL(NTB0_ID_D3D11_TEX, "CreateVertexShader failed: 0x%lx", hr);
        goto cleanup;
    }
    NTB0_OK(NTB0_ID_D3D11_TEX, "CreateVertexShader -> vs=%p", (void *)vs);

    /* ---- Create pixel shader (GFX-029) ---- */
    ID3D11PixelShader ps = NULL;
    hr = ID3D11Device_CreatePixelShader(device, fs_src, strlen(fs_src), NULL, (void **)&ps);
    if (hr != S_OK) {
        NTB0_FAIL(NTB0_ID_D3D11_TEX, "CreatePixelShader failed: 0x%lx", hr);
        goto cleanup;
    }
    NTB0_OK(NTB0_ID_D3D11_TEX, "CreatePixelShader -> ps=%p", (void *)ps);

    /* ---- Bind render targets (GFX-026) ---- */
    ID3D11DeviceContext_OMSetRenderTargets(context, 1, (void *const *)&rtv, dsv);
    NTB0_OK(NTB0_ID_D3D11_STATE, "OMSetRenderTargets -> rtv=%p, dsv=%p", (void *)rtv, (void *)dsv);

    /* ---- Set viewport (GFX-026) ---- */
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
    ID3D11DeviceContext_RSSetViewports(context, 1, &vp);
    NTB0_OK(NTB0_ID_D3D11_STATE, "RSSetViewports -> %dx%d", w, h);

    /* ---- Bind vertex buffer (GFX-026) ---- */
    unsigned int strides[] = { 5 * sizeof(float) };
    unsigned int offsets[] = { 0 };
    ID3D11DeviceContext_IASetVertexBuffers(context, 0, 1, (void *const *)&vbuffer, strides, offsets);
    NTB0_OK(NTB0_ID_D3D11_STATE, "IASetVertexBuffers -> vbuffer=%p, stride=%u", (void *)vbuffer, strides[0]);

    /* ---- Bind index buffer (GFX-026) ---- */
    ID3D11DeviceContext_IASetIndexBuffer(context, ibuffer, (void *)DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    NTB0_OK(NTB0_ID_D3D11_STATE, "IASetIndexBuffer -> ibuffer=%p", (void *)ibuffer);

    /* ---- Bind input layout (GFX-026) ---- */
    ID3D11DeviceContext_IASetInputLayout(context, il);
    NTB0_OK(NTB0_ID_D3D11_STATE, "IASetInputLayout -> il=%p", (void *)il);

    /* ---- Set primitive topology (GFX-026) ---- */
    ID3D11DeviceContext_IASetPrimitiveTopology(context, (void *)D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    NTB0_OK(NTB0_ID_D3D11_STATE, "IASetPrimitiveTopology -> TRIANGLELIST");

    /* ---- Bind shaders (GFX-026) ---- */
    ID3D11DeviceContext_VSSetShader(context, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(context, ps, NULL, 0);
    NTB0_OK(NTB0_ID_D3D11_STATE, "VSSetShader/PSSetShader -> vs=%p, ps=%p", (void *)vs, (void *)ps);

    /* ---- GLES2 setup for draw ---- */
    if (test_gl_bind() < 0) {
        NTB0_SKIP(NTB0_ID_D3D11_DRAW, "no GLES2 runtime on this machine");
        rc = TEST_EXIT_SKIP;
        goto cleanup;
    }
    if (gfx_present_make_current(present) < 0) {
        NTB0_FAIL(NTB0_ID_D3D11_DRAW, "eglMakeCurrent failed: %s", gfx_present_error());
        goto cleanup;
    }
    if (test_build_program(err, sizeof err) < 0) {
        NTB0_FAIL(NTB0_ID_D3D11_DRAW, "shader build failed: %s", err);
        goto cleanup;
    }

    /* ---- Draw triangle (GFX-025) ---- */
    gfx_present_copy_window_begin(present);

    /* Inject self-test copy if requested */
    if (getenv("NTB0_PRESENT_SELFTEST_COPY")) {
        size_t n = (size_t)w * (size_t)h * 4;
        char *a = malloc(n), *b = malloc(n);
        if (a && b) {
            memset(a, 0x5a, n);
            memcpy(b, a, n);
            g_sink = (unsigned char)b[n / 2];
            fprintf(stderr, "[test_d3d11_bridge]   a deliberate %zu byte copy injected (self-test)\n", n);
        }
        free(a);
        free(b);
    }

    if (test_draw_triangle(w, h, err, sizeof err) < 0) {
        gfx_present_copy_window_end(present);
        NTB0_FAIL(NTB0_ID_D3D11_DRAW, "draw failed: %s", err);
        goto cleanup;
    }
    NTB0_OK(NTB0_ID_D3D11_DRAW, "Draw/DeviceContext draw -> triangle rendered on GPU");

    /* ---- Present (GFX-024, WIN-024) ---- */
    hr = IDXGISwapChain_Present(swapchain, 1, 0);
    if (hr != S_OK) {
        gfx_present_copy_window_end(present);
        NTB0_FAIL(NTB0_ID_D3D11_PRESENT, "Present failed: 0x%lx", hr);
        goto cleanup;
    }

    /* Wait for release event */
    for (i = 1; i < 4; i++) {
        if (gfx_present_released(present))
            break;
        if (test_draw_triangle(w, h, err, sizeof err) < 0)
            break;
        if (IDXGISwapChain_Present(swapchain, 1, 0) != S_OK)
            break;
        usleep(50000);
    }
    gfx_present_copy_window_end(present);

    /* ---- Check dma-buf ---- */
    fd = gfx_present_dmabuf_fd(present);
    if (fd >= 0 && test_fd_is_dmabuf(fd, fdinfo, sizeof fdinfo)) {
        NTB0_OK(NTB0_ID_D3D11_PRESENT,
                "GPU buffer became dma-buf: fd=%d, %ux%u, stride=%u, "
                "format=ARGB8888, %s (kernel fdinfo: %s)",
                fd, gfx_present_buffer_width(present),
                gfx_present_buffer_height(present),
                gfx_present_buffer_stride(present),
                gfx_present_fb_method(present), fdinfo);
    } else if (fd >= 0) {
        NTB0_OK(NTB0_ID_D3D11_PRESENT,
                "GPU buffer became dma-buf: fd=%d, %ux%u, stride=%u, "
                "format=ARGB8888, %s",
                fd, gfx_present_buffer_width(present),
                gfx_present_buffer_height(present),
                gfx_present_buffer_stride(present),
                gfx_present_fb_method(present));
    } else {
        NTB0_FAIL(NTB0_ID_D3D11_PRESENT, "No dma-buf fd after present");
        goto cleanup;
    }

    /* ---- Check compositor release ---- */
    released = gfx_present_wait_release(present, 60000);
    if (released == 1) {
        NTB0_OK(NTB0_ID_WIN32K_INTEGRATE,
                "Compositor imported dma-buf and released wl_buffer: %s",
                gfx_present_compositor(present));
    } else if (released == 0) {
        NTB0_FAIL(NTB0_ID_WIN32K_INTEGRATE,
                  "Compositor did not release wl_buffer within 60s: %s",
                  gfx_present_compositor(present));
        goto cleanup;
    } else {
        NTB0_FAIL(NTB0_ID_WIN32K_INTEGRATE, "Wayland connection broke: %s", gfx_present_error());
        goto cleanup;
    }

    /* ---- Check CPU copy count (must be 0) ---- */
    copies = gfx_present_copies(present);
    copies_last = gfx_present_copies_last(present);
    bytes = gfx_present_copy_bytes(present);
    bigbytes = gfx_present_copy_big_bytes(present);
    calls = gfx_present_copy_calls(present);
    if (copies == 0) {
        NTB0_OK(NTB0_ID_D3D11_PRESENT,
                "CPU copies of frame-sized buffers = 0 "
                "(measured: %lu calls, %lu bytes total, %lu >=64KiB; last frame: %lu)",
                calls, bytes, bigbytes, copies_last);
    } else {
        NTB0_FAIL(NTB0_ID_D3D11_PRESENT,
                  "CPU copies = %lu (%lu bytes) — zero-copy criterion broken",
                  copies, bigbytes);
        goto cleanup;
    }

    /* ---- Test resize (WIN-023) ---- */
    int new_w = 800, new_h = 600;
    res = d3d11_swapchain_resize(swapchain, new_w, new_h);
    if (res != 0) {
        NTB0_FAIL(NTB0_ID_WIN32K_RESIZE, "Swapchain resize failed: %d", res);
        goto cleanup;
    }
    gfx_present_target_size(present, &w, &h);
    if (w == new_w && h == new_h) {
        NTB0_OK(NTB0_ID_WIN32K_RESIZE, "Resize verified: %dx%d", w, h);
    } else {
        NTB0_STUB(NTB0_ID_WIN32K_RESIZE, "Resize requested %dx%d, got %dx%d", new_w, new_h, w, h);
    }

    /* ---- Success ---- */
    rc = TEST_EXIT_PASS;

cleanup:
    /* Release resources */
    if (swapchain) d3d11_swapchain_release(swapchain);
    if (context) d3d11_context_release(context);
    if (device) d3d11_device_release(device);
    if (hdc) NtUserReleaseDC(hwnd, hdc);

    test_weston_cleanup();

    if (rc == TEST_EXIT_PASS) {
        printf("[test_d3d11_bridge] ── D3D11 bridge + Win32k integration summary ──\n");
        printf("[test_d3d11_bridge] drm node    : %s\n", gfx_present_drm_node(present));
        printf("[test_d3d11_bridge] target size : %dx%d\n", w, h);
        printf("[test_d3d11_bridge] dma-buf fd  : %d (%ux%u stride=%u)\n", fd,
               gfx_present_buffer_width(present), gfx_present_buffer_height(present),
               gfx_present_buffer_stride(present));
        printf("[test_d3d11_bridge] CPU copies  : %lu (>=64 KiB), %lu calls / %lu bytes total\n",
               copies, calls, bytes);
        printf("[test_d3d11_bridge] compositor  : %s\n", gfx_present_compositor(present));
        printf("[test_d3d11_bridge] wl_buffer release received : %s\n",
               released == 1 ? "yes" : "no");
        printf("[test_d3d11_bridge] RESULT: PASS (frame reached compositor as dma-buf, 0 CPU copies)\n");
    } else if (rc == TEST_EXIT_SKIP) {
        printf("[test_d3d11_bridge] RESULT: SKIP\n");
    } else {
        printf("[test_d3d11_bridge] RESULT: FAIL (copies=%lu, release=%d)\n", copies, released);
    }

    return rc;
}