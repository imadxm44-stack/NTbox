/*
 * d3d11_bridge.c — NT-Box: the D3D11 → EGL/GLES UMD bridge (master plan
 * W9a, T-214 … T-217 P0).  See d3d11_bridge.h for the contract.
 *
 * IMPLEMENTATION NOTE — "no SPIR-V" (the honest shader story)
 *   D3D11 shader bytecode is a DXBC container (bitcode of the d3dcompiler).
 *   Translating arbitrary DXBC to GLSL ES is a compiler project of its own;
 *   NO ONE here claims it exists.  What this file does instead (documented
 *   in the header too):
 *     * it DETECTS a DXBC container (magic "DXBC", chunk table parse) and
 *       reports it honestly: every time real DXBC bytecode arrives
 *       un-translated, a STUB lamp is printed and the documented
 *       pass-through shader pair is substituted.
 *     * applications that carry the NT-Box internal marker ("NTBA") get
 *       the pass-through pair without the STUB (that marker IS the tiny
 *       internal bytecode of this bridge).
 *     * anything else (tests that feed GLSL source text, ...) is treated
 *       as "kind unknown" and reported as such.
 *   What makes this still a REAL device to the application is that the
 *   attributes come from the input layout the app registered (semantics
 *   → GL attrib locations) and the buffers are real GPU buffers: the
 *   canned program is bound to exactly the layout the app described.
 *
 * THE ZERO-COPY CONTRACT
 *   The CPU copies of the PRESENTED frame are MEASURED, not assumed: the
 *   memcpy()/memmove() interposition inside gfx_present.c counts every call
 *   of the measurement window the CALLER opens around
 *   Clear/Draw/Update-in-frame/Present.  Present() itself only reports the
 *   dmabuf metadata and respects that window (it never opens/resets one of
 *   its own, see "Present()" below).
 *
 *   NTBOX_UMD_FORCE_COPY=1  — Present() performs a deliberate frame-sized
 *   CPU copy INSIDE the frame path; with the window open the counter MUST
 *   see it (the lamp then FAILs — proof that the counter measures and does
 *   not assume).
 *
 * Lamps printed here: GFX-031..GFX-038 (see the header for GFX-039/040 in
 * umd_egl.c and GFX-041..043 in the test).
 */

#define _GNU_SOURCE

#include "d3d11_bridge.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "umd_egl.h"

/* ===================================================================== */
/* Lamps                                                                 */
/* ===================================================================== */

#define D3D_TP(id, st, fmt, ...) \
    fprintf(stderr, "[ntbox][%s][%s] " fmt "\n", (id), (st), ##__VA_ARGS__)

#define D3D_OK(id, fmt, ...)   D3D_TP(id, "OK", fmt, ##__VA_ARGS__)
#define D3D_STUB(id, fmt, ...) D3D_TP(id, "STUB", fmt, ##__VA_ARGS__)
#define D3D_FAIL(id, fmt, ...) D3D_TP(id, "FAIL", fmt, ##__VA_ARGS__)
#define D3D_SKIP(id, fmt, ...) D3D_TP(id, "SKIP", fmt, ##__VA_ARGS__)

#define LAMP_DEVICE   "GFX-031"  /* D3D11CreateDevice -> real EGL/GLES dev */
#define LAMP_RES      "GFX-032"  /* CreateBuffer/CreateTexture2D real GPU  */
#define LAMP_SHADER   "GFX-033"  /* CreateVertexShader/CreatePixelShader   */
#define LAMP_PROGRAM  "GFX-034"  /* baked pass-through pair + attrib wiring */
#define LAMP_CLEAR    "GFX-035"  /* ClearRenderTargetView / Clear          */
#define LAMP_DRAW     "GFX-036"  /* Draw / DrawIndexed on the GPU          */
#define LAMP_UPDATE   "GFX-037"  /* UpdateSubresource + Map/Unmap uploads   */
#define LAMP_SWAPCHAIN "GFX-038" /* IDXGIFactory::CreateSwapChain -> gbm    */
#define LAMP_WIN32K_SC "WIN-021" /* win32k integration (kept from the seed) \
 */

/* ===================================================================== */
/* GLES2 bindings (dlopen — no headers on this machine)                  */
/* ===================================================================== */

static void *g_gl_lib = NULL;

static void gl_load(void) {
    if (g_gl_lib) return;
    g_gl_lib = dlopen("libGLESv2.so.2", RTLD_LAZY | RTLD_LOCAL);
    if (!g_gl_lib) g_gl_lib = dlopen("libGLESv2.so", RTLD_LAZY | RTLD_LOCAL);
}

#define GL_SYM(name, sym) \
    do { \
        gl_load(); \
        if (!g_gl_lib) return; \
        name = dlsym(g_gl_lib, sym); \
    } while(0)

/* GLES2 function pointers (only what the bridge really executes). */
static void (*gl_clear)(unsigned int);
static void (*gl_clear_color)(float, float, float, float);
static void (*gl_clear_depthf)(float);
static void (*gl_viewport)(int, int, int, int);
static unsigned int (*gl_create_shader)(unsigned int);
static void (*gl_delete_shader)(unsigned int);
static void (*gl_shader_source)(unsigned int, int, const char *const *, const int *);
static void (*gl_compile_shader)(unsigned int);
static void (*gl_get_shaderiv)(unsigned int, unsigned int, int *);
static void (*gl_get_shader_info_log)(unsigned int, int, int *, char *);
static unsigned int (*gl_create_program)(void);
static void (*gl_attach_shader)(unsigned int, unsigned int);
static void (*gl_link_program)(unsigned int);
static void (*gl_get_programiv)(unsigned int, unsigned int, int *);
static void (*gl_get_program_info_log)(unsigned int, int, int *, char *);
static void (*gl_use_program)(unsigned int);
static void (*gl_delete_program)(unsigned int);
static void (*gl_gen_buffers)(int, unsigned int *);
static void (*gl_delete_buffers)(int, const unsigned int *);
static void (*gl_bind_buffer)(unsigned int, unsigned int);
static void (*gl_buffer_data)(unsigned int, long, const void *, unsigned int);
static void (*gl_buffer_sub_data)(unsigned int, long, long, const void *);
static void (*gl_gen_textures)(int, unsigned int *);
static void (*gl_delete_textures)(int, const unsigned int *);
static void (*gl_bind_texture)(unsigned int, unsigned int);
static void (*gl_tex_image2d)(unsigned int, int, int, unsigned int, unsigned int, unsigned int, unsigned int, unsigned int, const void *);
static void (*gl_tex_sub_image2d)(unsigned int, int, int, int, unsigned int, unsigned int, unsigned int, unsigned int, const void *);
static void (*gl_tex_parameteri)(unsigned int, unsigned int, unsigned int);
static void (*gl_pixel_storei)(unsigned int, int);
static void (*gl_gen_framebuffers)(int, unsigned int *);
static void (*gl_delete_framebuffers)(int, const unsigned int *);
static void (*gl_bind_framebuffer)(unsigned int, unsigned int);
static void (*gl_framebuffer_texture2d)(unsigned int, unsigned int, unsigned int, unsigned int, int);
static unsigned int (*gl_check_framebuffer_status)(unsigned int);
static void (*gl_gen_renderbuffers)(int, unsigned int *);
static void (*gl_delete_renderbuffers)(int, const unsigned int *);
static void (*gl_bind_renderbuffer)(unsigned int, unsigned int);
static void (*gl_renderbuffer_storage)(unsigned int, unsigned int, int, int);
static void (*gl_framebuffer_renderbuffer)(unsigned int, unsigned int, unsigned int, unsigned int);
static void (*gl_vertex_attrib_pointer)(unsigned int, int, unsigned int, unsigned char, int, const void *);
static void (*gl_enable_vertex_attrib_array)(unsigned int);
static void (*gl_disable_vertex_attrib_array)(unsigned int);
static void (*gl_draw_arrays)(unsigned int, int, int);
static void (*gl_draw_elements)(unsigned int, int, unsigned int, const void *);
static void (*gl_depth_mask)(unsigned char);


/* GLES2 constants (local, matching the GL ES public ABI). */
#define GL_COLOR_BUFFER_BIT       0x00004000
#define GL_DEPTH_BUFFER_BIT       0x00000100
#define GL_FALSE                  0
#define GL_TRUE                   1
#define GL_TRIANGLES              0x0004
#define GL_TRIANGLE_STRIP         0x0005
#define GL_LINES                  0x0001
#define GL_LINE_STRIP             0x0003
#define GL_POINTS                 0x0000
#define GL_LINE_LOOP              0x0002
#define GL_UNSIGNED_BYTE          0x1401
#define GL_UNSIGNED_SHORT         0x1403
#define GL_UNSIGNED_INT           0x1405
#define GL_FLOAT                  0x1406
#define GL_HALF_FLOAT             0x140B
#define GL_VERTEX_SHADER          0x8B31
#define GL_FRAGMENT_SHADER        0x8B30
#define GL_COMPILE_STATUS         0x8B81
#define GL_LINK_STATUS            0x8B82
#define GL_INFO_LOG_LENGTH        0x8B84
#define GL_ARRAY_BUFFER           0x8892
#define GL_ELEMENT_ARRAY_BUFFER   0x8893
#define GL_STATIC_DRAW            0x88E4
#define GL_DYNAMIC_DRAW           0x88E8
#define GL_STREAM_DRAW            0x88E0
#define GL_NO_ERROR               0
#define GL_RENDERER               0x1F01
#define GL_TEXTURE_2D             0x0DE1
#define GL_TEXTURE_MIN_FILTER     0x2801
#define GL_TEXTURE_MAG_FILTER     0x2802
#define GL_NEAREST                0x2600
#define GL_RGBA                   0x1908
#define GL_BGRA                   0x80E1
#define GL_DEPTH_COMPONENT        0x1902
#define GL_DEPTH_COMPONENT24      0x81A6
#define GL_FRAMEBUFFER            0x8D40
#define GL_RENDERBUFFER           0x8D41
#define GL_COLOR_ATTACHMENT0      0x8CE0
#define GL_DEPTH_ATTACHMENT       0x8D20
#define GL_FRAMEBUFFER_COMPLETE   0x8CD5
#define GL_UNPACK_ALIGNMENT       0x0CF5

static void gl_load_all(void) {
    GL_SYM(gl_clear, "glClear");
    GL_SYM(gl_clear_color, "glClearColor");
    GL_SYM(gl_clear_depthf, "glClearDepthf");
    GL_SYM(gl_viewport, "glViewport");
    GL_SYM(gl_create_shader, "glCreateShader");
    GL_SYM(gl_delete_shader, "glDeleteShader");
    GL_SYM(gl_shader_source, "glShaderSource");
    GL_SYM(gl_compile_shader, "glCompileShader");
    GL_SYM(gl_get_shaderiv, "glGetShaderiv");
    GL_SYM(gl_get_shader_info_log, "glGetShaderInfoLog");
    GL_SYM(gl_create_program, "glCreateProgram");
    GL_SYM(gl_attach_shader, "glAttachShader");
    GL_SYM(gl_link_program, "glLinkProgram");
    GL_SYM(gl_get_programiv, "glGetProgramiv");
    GL_SYM(gl_get_program_info_log, "glGetProgramInfoLog");
    GL_SYM(gl_use_program, "glUseProgram");
    GL_SYM(gl_delete_program, "glDeleteProgram");
    GL_SYM(gl_gen_buffers, "glGenBuffers");
    GL_SYM(gl_delete_buffers, "glDeleteBuffers");
    GL_SYM(gl_bind_buffer, "glBindBuffer");
    GL_SYM(gl_buffer_data, "glBufferData");
    GL_SYM(gl_buffer_sub_data, "glBufferSubData");
    GL_SYM(gl_gen_textures, "glGenTextures");
    GL_SYM(gl_delete_textures, "glDeleteTextures");
    GL_SYM(gl_bind_texture, "glBindTexture");
    GL_SYM(gl_tex_image2d, "glTexImage2D");
    GL_SYM(gl_tex_sub_image2d, "glTexSubImage2D");
    GL_SYM(gl_tex_parameteri, "glTexParameteri");
    GL_SYM(gl_pixel_storei, "glPixelStorei");
    GL_SYM(gl_gen_framebuffers, "glGenFramebuffers");
    GL_SYM(gl_delete_framebuffers, "glDeleteFramebuffers");
    GL_SYM(gl_bind_framebuffer, "glBindFramebuffer");
    GL_SYM(gl_framebuffer_texture2d, "glFramebufferTexture2D");
    GL_SYM(gl_check_framebuffer_status, "glCheckFramebufferStatus");
    GL_SYM(gl_gen_renderbuffers, "glGenRenderbuffers");
    GL_SYM(gl_delete_renderbuffers, "glDeleteRenderbuffers");
    GL_SYM(gl_bind_renderbuffer, "glBindRenderbuffer");
    GL_SYM(gl_renderbuffer_storage, "glRenderbufferStorage");
    GL_SYM(gl_framebuffer_renderbuffer, "glFramebufferRenderbuffer");
    GL_SYM(gl_vertex_attrib_pointer, "glVertexAttribPointer");
    GL_SYM(gl_enable_vertex_attrib_array, "glEnableVertexAttribArray");
    GL_SYM(gl_disable_vertex_attrib_array, "glDisableVertexAttribArray");
    GL_SYM(gl_draw_arrays, "glDrawArrays");
    GL_SYM(gl_draw_elements, "glDrawElements");
    GL_SYM(gl_depth_mask, "glDepthMask");
}
/* ===================================================================== */
/* DXGI format maps                                                      */
/* ===================================================================== */

struct umd_tex_fmt { unsigned kind, fmt, type, bytes; };
/* kind: 0 = unsupported, 1 = rgba-ish texture, 2 = depth */

static int umd_tex_format(unsigned int dxgi, struct umd_tex_fmt *out) {
    memset(out, 0, sizeof *out);
    switch (dxgi) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        out->kind = 1;
        out->fmt = (dxgi == DXGI_FORMAT_B8G8R8A8_UNORM) ? GL_BGRA : GL_RGBA;
        out->type = GL_UNSIGNED_BYTE;
        out->bytes = 4;
        return 0;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        out->kind = 1;
        out->fmt = GL_RGBA;
        out->type = GL_FLOAT;
        out->bytes = 16;
        return 0;
    case DXGI_FORMAT_R16G16_FLOAT:
        out->kind = 1;
        out->fmt = GL_RGBA; /* 2 floats don't round-trip in ES2; honest doc */
        out->type = GL_HALF_FLOAT;
        out->bytes = 4;
        return 0;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        out->kind = 2;
        out->fmt = GL_DEPTH_COMPONENT24;
        out->type = GL_UNSIGNED_INT;
        out->bytes = 4;
        return 0;
    default:
        return -1;
    }
}

/* Vertex-attribute format map: -(elements, GL type, normalized, stride) */
struct umd_vat { int count; unsigned int type; unsigned char norm; int bytes; };

static int umd_vertex_format(unsigned int dxgi, struct umd_vat *out) {
    memset(out, 0, sizeof *out);
    switch (dxgi) {
    case DXGI_FORMAT_R32_FLOAT:            out->count = 1; out->type = GL_FLOAT;  out->bytes = 4;  break;
    case DXGI_FORMAT_R32G32_FLOAT:         out->count = 2; out->type = GL_FLOAT;  out->bytes = 8;  break;
    case DXGI_FORMAT_R32G32B32_FLOAT:      out->count = 3; out->type = GL_FLOAT;  out->bytes = 12; break;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:   out->count = 4; out->type = GL_FLOAT;  out->bytes = 16; break;
    case DXGI_FORMAT_R8G8B8A8_UNORM:       out->count = 4; out->type = GL_UNSIGNED_BYTE; out->norm = 1; out->bytes = 4; break;
    case DXGI_FORMAT_B8G8R8A8_UNORM:       out->count = 4; out->type = GL_UNSIGNED_BYTE; out->norm = 1; out->bytes = 4; break;
    case DXGI_FORMAT_R8G8B8A8_UINT:        out->count = 4; out->type = GL_UNSIGNED_BYTE; out->bytes = 4; break;
    case DXGI_FORMAT_R16G16_FLOAT:         out->count = 2; out->type = GL_HALF_FLOAT; out->bytes = 4; break;
    case DXGI_FORMAT_R16_UINT:             out->count = 1; out->type = GL_UNSIGNED_SHORT; out->bytes = 2; break;
    case DXGI_FORMAT_R32_UINT:             out->count = 1; out->type = GL_UNSIGNED_INT; out->bytes = 4; break;
    default: return -1;
    }
    return 0;
}

/* The GL draw mode of a D3D11 primitive topology (unsigned value). */
static unsigned int umd_topology_gl(unsigned int topology) {
    switch (topology) {
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST:  return GL_TRIANGLES;
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: return GL_TRIANGLE_STRIP;
    case D3D11_PRIMITIVE_TOPOLOGY_LINELIST:      return GL_LINES;
    case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP:     return GL_LINE_STRIP;
    case D3D11_PRIMITIVE_TOPOLOGY_POINTLIST:     return GL_POINTS;
    case 0x7: /* the historic "0x7" hack of the seed test = triangle list */
        return GL_TRIANGLES;
    default: return GL_TRIANGLES;
    }
}

/* ===================================================================== */
/* Internal structures                                                   */
/* ===================================================================== */

struct d3d11_device {
    unsigned long magic;
    gfx_present_t *present;
    int feature_level;
    d3d11_context_t *immediate_context;
    /* The baked (per-device) GL program of the pass-through VS/FS pair. */
    unsigned int program;
    int program_ok;
    char program_log[320];
    char renderer[128];
    int refcount;
};

struct d3d11_tex {
    unsigned long magic;
    d3d11_device_t *device;
    int width, height;
    unsigned int format;
    unsigned int bind_flags;
    unsigned int usage;
    unsigned int cpu_access;
    unsigned int gl_tex;   /* 0 when there is none (depth/staging) */
    unsigned int gl_rbo;   /* color/depth renderbuffer of an RT    */
    unsigned int gl_fbo;   /* the framebuffer of an RT             */
    int is_depth;
    unsigned int row_pitch;
    void *shadow;          /* Map() staging shadow (read/write)     */
    unsigned int shadow_size;
    int mapped;            /* an app currently holds the shadow pointer */
    int refcount;
};

struct d3d11_buffer {
    unsigned long magic;
    d3d11_device_t *device;
    unsigned int size;
    unsigned int bind_flags;
    unsigned int usage;
    unsigned int cpu_access;
    unsigned int gl_vbo;
    void *shadow;          /* Map() target (retained between the calls) */
    unsigned int shadow_size;
    int mapped;
    int refcount;
};

struct d3d11_inputlayout {
    unsigned long magic;
    d3d11_device_t *device;
    D3D11_INPUT_ELEMENT_DESC elems[16];
    unsigned int n;
    int refcount;
};

struct d3d11_shader {
    unsigned long magic;
    d3d11_device_t *device;
    int type;              /* 0 = vertex, 1 = pixel                  */
    int kind;              /* 0 = unknown, 1 = DXBC, 2 = NTBA, 3 = GLSL text */
    unsigned int bytes;
    int stub_reported;
    int refcount;
};

struct d3d11_rendertarget {
    unsigned long magic;
    d3d11_device_t *device;
    d3d11_swapchain_t *swapchain;  /* the default FB target, or NULL */
    struct d3d11_tex *tex;         /* offscreen target, or NULL      */
    int width, height;
    unsigned int format;
    unsigned int gl_fbo;           /* the FB Clear/Draw binds for it */
    int refcount;
};

struct d3d11_depthstencil {
    unsigned long magic;
    d3d11_device_t *device;
    struct d3d11_tex *tex;
    int width, height;
    unsigned int format;
    unsigned int gl_fbo;
    int refcount;
};

struct d3d11_swapchain {
    unsigned long magic;
    d3d11_device_t *device;
    HWND hwnd;
    int width, height;
    unsigned int format;
    unsigned int gl_depth_rbo; /* depth renderbuffer of the DSV-on-swapchain */
    int refcount;
    /* last present */
    int last_mode;     /* 0 = dry, 1 = wet, -1 = nothing presented yet */
    long last_present_us;
    long last_roundtrip_us;
    long last_release_us;
    struct umd_frame frame;
};

struct d3d11_context {
    unsigned long magic;
    d3d11_device_t *device;
    /* bound state */
    d3d11_rendertarget_t *rtv;
    d3d11_depthstencil_t *dsv;
    d3d11_inputlayout_t *input_layout;
    d3d11_shader_t *vs, *ps;
    d3d11_buffer_t *vbs[8];
    unsigned int vb_strides[8], vb_offsets[8];
    unsigned int num_vbs;
    d3d11_buffer_t *ib;
    unsigned int ib_format, ib_offset;
    unsigned int topology;
    D3D11_VIEWPORT viewport;
    float clear_color[4];
    unsigned int fb_bound; /* the FB the draw path last bound    */
    int refcount;
};

#define D3D11_DEVICE_MAGIC    0x44334431UL /* "D3D1" */
#define D3D11_CONTEXT_MAGIC   0x43545831UL /* "CTX1" */
#define D3D11_SWAPCHAIN_MAGIC 0x53434831UL /* "SCH1" */
#define D3D11_TEX_MAGIC       0x54455831UL /* "TEX1" */
#define D3D11_RTV_MAGIC       0x52545631UL /* "RTV1" */
#define D3D11_DSV_MAGIC       0x44535631UL /* "DSV1" */
#define D3D11_BUFFER_MAGIC    0x42554631UL /* "BUF1" */
#define D3D11_IL_MAGIC        0x494C5631UL /* "ILV1" */
#define D3D11_SHADER_MAGIC    0x53484431UL /* "SHD1" */

static inline d3d11_device_t *dev_from_ptr(ID3D11Device dev) {
    d3d11_device_t *d = (d3d11_device_t *)dev;
    return (d && d->magic == D3D11_DEVICE_MAGIC) ? d : NULL;
}
static inline d3d11_context_t *ctx_from_ptr(ID3D11DeviceContext ctx) {
    d3d11_context_t *c = (d3d11_context_t *)ctx;
    return (c && c->magic == D3D11_CONTEXT_MAGIC) ? c : NULL;
}
static inline d3d11_swapchain_t *sc_from_ptr(IDXGISwapChain sc) {
    d3d11_swapchain_t *s = (d3d11_swapchain_t *)sc;
    return (s && s->magic == D3D11_SWAPCHAIN_MAGIC) ? s : NULL;
}
static inline struct d3d11_tex *tex_from_ptr(void *p) {
    struct d3d11_tex *t = (struct d3d11_tex *)p;
    return (t && t->magic == D3D11_TEX_MAGIC) ? t : NULL;
}
static inline d3d11_rendertarget_t *rtv_from_ptr(ID3D11RenderTargetView rtv) {
    d3d11_rendertarget_t *r = (d3d11_rendertarget_t *)rtv;
    return (r && r->magic == D3D11_RTV_MAGIC) ? r : NULL;
}
static inline d3d11_depthstencil_t *dsv_from_ptr(ID3D11DepthStencilView dsv) {
    d3d11_depthstencil_t *d = (d3d11_depthstencil_t *)dsv;
    return (d && d->magic == D3D11_DSV_MAGIC) ? d : NULL;
}
static inline d3d11_buffer_t *buf_from_ptr(ID3D11Buffer buf) {
    d3d11_buffer_t *b = (d3d11_buffer_t *)buf;
    return (b && b->magic == D3D11_BUFFER_MAGIC) ? b : NULL;
}
static inline d3d11_inputlayout_t *il_from_ptr(ID3D11InputLayout il) {
    d3d11_inputlayout_t *i = (d3d11_inputlayout_t *)il;
    return (i && i->magic == D3D11_IL_MAGIC) ? i : NULL;
}
static inline d3d11_shader_t *sh_from_ptr(void *shader) {
    d3d11_shader_t *s = (d3d11_shader_t *)shader;
    return (s && s->magic == D3D11_SHADER_MAGIC) ? s : NULL;
}

/* the honest idempotent printing flags (print each capability ONCE) */
struct umd_lamp_flags {
    int draw, clear, update, program, res_buf, res_tex, shader, il;
    int map_read, map_write;
    int present_wet, present_dry;
};
static struct umd_lamp_flags g_lamps;

/* ===================================================================== */
/* The baked pass-through shader pair (the "no-SPIR-V" translation)      */
/* ===================================================================== */

/*
 * a0 = the FIRST input-element (POSITION typically), a1 = the SECOND.
 * Un-supplied components come from the GL attribute defaults
 * (.z=0/.w=1 for size<=2, .w=1 for size==3) — which is exactly the fill
 * rule of D3D11 for completeness.
 */
static const char *umd_vs_src =
    "#version 100\n"
    "attribute vec4 a0;\n"
    "attribute vec4 a1;\n"
    "varying vec4 v1;\n"
    "void main() {\n"
    "  gl_Position = a0;\n"
    "  v1 = a1;\n"
    "}\n";

static const char *umd_fs_src =
    "#version 100\n"
    "precision mediump float;\n"
    "varying vec4 v1;\n"
    "void main() {\n"
    "  gl_FragColor = v1;\n"
    "}\n";

static void gl_bind_attrib_location(unsigned int prog, unsigned int loc, const char *name);

/* one attribute extra (bound by order) */
static void gl_bind_attrib_location(unsigned int prog, unsigned int loc, const char *name) {
    static void (*p)(unsigned int, unsigned int, const char *) = NULL;

    if (!p) {
        gl_load();
        if (!g_gl_lib) return;
        p = dlsym(g_gl_lib, "glBindAttribLocation");
    }
    if (p) p(prog, loc, name);
}

/* DXBC container detection (T-214 honesty): parse the chunk table. */
static int umd_shader_kind(const void *code, unsigned int len, char *desc, size_t descn)
{
    static const char *dxbc_chunks[] = {
        "DXIL", "SHDR", "SHDE", "ISGN", "OSGN", "ISG1", "OSG1", "SFI0", "STAT"
    };

    if (desc && descn)
        desc[0] = 0;
    if (!code || len < 4)
        return 0; /* unknown */
    if (!memcmp(code, "DXBC", 4)) {
        unsigned int ns, i, j;
        char seen[64];

        if (len < 32 || !desc) {
            if (desc && descn) snprintf(desc, descn, "DXBC (short container)");
            return 1;
        }
        ns = *(const unsigned int *)((const unsigned char *)code + 24);
        if (ns > 64) ns = 0; /* malformed: only report the magic */
        seen[0] = 0;
        for (i = 0; i < ns; i++) {
            uint32_t off = *(const uint32_t *)((const unsigned char *)code +
                                               28 + 4 * i);
            if ((off + 4) > len)
                continue;
            for (j = 0; j < sizeof dxbc_chunks / sizeof *dxbc_chunks; j++) {
                if (!memcmp((const unsigned char *)code + off,
                            dxbc_chunks[j], 4)) {
                    if (seen[0]) strncat(seen, ",", sizeof seen - strlen(seen) - 1);
                    strncat(seen, dxbc_chunks[j], sizeof seen - strlen(seen) - 1);
                    break;
                }
            }
        }
        snprintf(desc, descn, "DXBC container: %u chunks, bytes=%u chunks=%s",
                 ns, len, seen[0] ? seen : "unrecognized");
        return 1;
    }
    if (!memcmp(code, "NTBA", 4)) {
        if (desc && descn)
            snprintf(desc, descn, "NTBA (NT-Box internal bytecode)");
        return 2;
    }
    if (desc && descn)
        snprintf(desc, descn, "not a DXBC container (bytes=%u)", len);
    return 3;
}

static int umd_build_program(d3d11_device_t *dev)
{
    unsigned int vs, fs, prog;
    int ok = 0;
    char log[256];

    if (dev->program_ok)
        return 0;
    if (dev->present && gfx_present_make_current(dev->present) < 0)
        return -1;
    gl_load_all();
    if (!gl_create_shader || !gl_create_program)
        return -1;
    if (dev->present && gfx_present_renderer(dev->present) &&
        gfx_present_renderer(dev->present)[0])
        snprintf(dev->renderer, sizeof dev->renderer, "%s",
                 gfx_present_renderer(dev->present));

    vs = gl_create_shader(GL_VERTEX_SHADER);
    fs = gl_create_shader(GL_FRAGMENT_SHADER);
    if (!vs || !fs)
        return -1;
    gl_shader_source(vs, 1, &umd_vs_src, NULL);
    gl_shader_source(fs, 1, &umd_fs_src, NULL);
    gl_compile_shader(vs);
    gl_compile_shader(fs);
    gl_get_shaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        if (gl_get_shader_info_log)
            gl_get_shader_info_log(vs, (int)sizeof log - 1, NULL, log);
        snprintf(dev->program_log, sizeof dev->program_log, "VS: %s", log);
        goto fail;
    }
    gl_get_shaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        if (gl_get_shader_info_log)
            gl_get_shader_info_log(fs, (int)sizeof log - 1, NULL, log);
        snprintf(dev->program_log, sizeof dev->program_log, "FS: %s", log);
        goto fail;
    }
    prog = gl_create_program();
    if (!prog)
        goto fail;
    gl_attach_shader(prog, vs);
    gl_attach_shader(prog, fs);
    gl_bind_attrib_location(prog, 0, "a0");
    gl_bind_attrib_location(prog, 1, "a1");
    gl_link_program(prog);
    gl_get_programiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        if (gl_get_program_info_log)
            gl_get_program_info_log(prog, (int)sizeof log - 1, NULL, log);
        snprintf(dev->program_log, sizeof dev->program_log, "link: %s", log);
        gl_delete_program(prog);
        prog = 0;
        goto fail;
    }
    gl_delete_shader(vs);
    gl_delete_shader(fs);
    dev->program = prog;
    dev->program_ok = 1;
    return 0;

fail:
    if (vs) gl_delete_shader(vs);
    if (fs) gl_delete_shader(fs);
    return -1;
}

/* ===================================================================== */
/* Device (T-214 / GFX-031)                                              */
/* ===================================================================== */

long __attribute__((ms_abi)) D3D11CreateDevice(
    void *pAdapter, int DriverType, void *Software, unsigned Flags,
    const void *pFeatureLevels, unsigned FeatureLevels, unsigned SDKVersion,
    void **ppDevice, int *pFeatureLevel, void **ppImmediateContext)
{
    (void)pAdapter; (void)DriverType; (void)Software; (void)Flags;
    (void)pFeatureLevels; (void)FeatureLevels; (void)SDKVersion;

    d3d11_device_t *dev = calloc(1, sizeof *dev);

    if (!dev) {
        D3D_FAIL(LAMP_DEVICE, "D3D11CreateDevice: out of memory");
        return E_OUTOFMEMORY;
    }
    dev->magic = D3D11_DEVICE_MAGIC;
    dev->feature_level = D3D_FEATURE_LEVEL_11_0;
    dev->refcount = 1;

    /* The GPU device: GBM surface + EGL display + GLES2 context (LINEAR |
     * SCANOUT GBM surface inside gfx_present_create — the same one the
     * swapchain of T-216 uses; see the header). */
    dev->present = gfx_present_create();
    if (!dev->present) {
        D3D_FAIL(LAMP_DEVICE, "no GPU path: %s", gfx_present_error());
        free(dev);
        return DXGI_ERROR_DEVICE_REMOVED;
    }
    if (gfx_present_make_current(dev->present) < 0) {
        D3D_FAIL(LAMP_DEVICE, "eglMakeCurrent failed: %s", gfx_present_error());
        gfx_present_destroy(dev->present);
        free(dev);
        return DXGI_ERROR_DEVICE_REMOVED;
    }
    /*
     * The Wayland hand-off is explicit (gfx_present.h contract).  When a
     * compositor is available ($WAYLAND_DISPLAY) connect now; Present()
     * then runs the WET path.  Without it, Present() honestly runs the
     * DRY path (eglSwapBuffers + dma-buf export, umd_egl.c).
     */
    if (getenv("WAYLAND_DISPLAY") && *getenv("WAYLAND_DISPLAY") &&
        gfx_present_wl_connect(dev->present) < 0)
        D3D_STUB(LAMP_DEVICE,
                 "wl_connect failed (%s) — Present() falls back to the "
                 "DRY path", gfx_present_error());

    dev->immediate_context = calloc(1, sizeof *dev->immediate_context);
    if (!dev->immediate_context) {
        gfx_present_destroy(dev->present);
        free(dev);
        D3D_FAIL(LAMP_DEVICE, "D3D11CreateDevice: out of memory (context)");
        return E_OUTOFMEMORY;
    }
    dev->immediate_context->magic = D3D11_CONTEXT_MAGIC;
    dev->immediate_context->device = dev;
    dev->immediate_context->refcount = 1;
    dev->immediate_context->viewport.Width = 640.0f;
    dev->immediate_context->viewport.Height = 480.0f;
    dev->immediate_context->viewport.MaxDepth = 1.0f;

    D3D_OK(LAMP_DEVICE,
           "D3D11CreateDevice -> real GPU device (drm=%s drive=%s, "
           "GBM+EGL+%s, feature=0xB000, device=%p, context=%p)",
           gfx_present_drm_node(dev->present),
           gfx_present_drm_driver(dev->present),
           "GLES2",
           (void *)dev, (void *)dev->immediate_context);

    if (ppDevice) *ppDevice = dev;
    if (pFeatureLevel) *pFeatureLevel = dev->feature_level;
    if (ppImmediateContext) *ppImmediateContext = dev->immediate_context;
    return S_OK;
}

long __attribute__((ms_abi)) gfx_d3d11_create_device(
    void *pAdapter, int DriverType, void *Software, unsigned Flags,
    const void *pFeatureLevels, unsigned FeatureLevels, unsigned SDKVersion,
    void **ppDevice, int *pFeatureLevel, void **ppImmediateContext)
{
    return D3D11CreateDevice(pAdapter, DriverType, Software, Flags,
                             pFeatureLevels, FeatureLevels, SDKVersion,
                             ppDevice, pFeatureLevel, ppImmediateContext);
}

/* ===================================================================== */
/* Buffers + textures (GFX-032)                                          */
/* ===================================================================== */

long __attribute__((ms_abi)) ID3D11Device_CreateBuffer(
    void *pDevice, const void *pDesc, const void *pInitialData, void **ppBuffer)
{
    d3d11_device_t *dev = dev_from_ptr(pDevice);
    const D3D11_BUFFER_DESC *desc = (const D3D11_BUFFER_DESC *)pDesc;
    d3d11_buffer_t *buf;

    if (!dev || !desc) return E_INVALIDARG;
    if (desc->ByteWidth == 0) {
        D3D_FAIL(LAMP_RES, "CreateBuffer: zero size");
        return E_INVALIDARG;
    }
    buf = calloc(1, sizeof *buf);
    if (!buf) return E_OUTOFMEMORY;
    buf->magic = D3D11_BUFFER_MAGIC;
    buf->device = dev;
    buf->size = desc->ByteWidth;
    buf->bind_flags = desc->BindFlags;
    buf->usage = (unsigned int)(uintptr_t)desc->Usage;
    buf->cpu_access = desc->CPUAccessFlags;
    buf->refcount = 1;

    /* STAGING buffers get no GL object: they are pure CPU memory (D3D11). */
    if (buf->usage == D3D11_USAGE_STAGING) {
        buf->shadow = malloc(buf->size);
        if (!buf->shadow) { free(buf); return E_OUTOFMEMORY; }
        buf->shadow_size = buf->size;
    } else {
        gl_load_all();
        if (!gl_gen_buffers || !gl_bind_buffer || !gl_buffer_data) {
            free(buf);
            return DXGI_ERROR_DEVICE_REMOVED;
        }
        if (buf->usage == D3D11_USAGE_DYNAMIC)
            buf->shadow = malloc(buf->size); /* the Map() target */
        if (buf->shadow) buf->shadow_size = buf->size;

        gl_gen_buffers(1, &buf->gl_vbo);
        gl_bind_buffer(GL_ARRAY_BUFFER, buf->gl_vbo);
        if (pInitialData) {
            const D3D11_SUBRESOURCE_DATA *init =
                (const D3D11_SUBRESOURCE_DATA *)pInitialData;

            if (init->pSysMem)
                gl_buffer_data(GL_ARRAY_BUFFER, (long)buf->size,
                               init->pSysMem, buf->usage == D3D11_USAGE_DYNAMIC ?
                               GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
        } else {
            gl_buffer_data(GL_ARRAY_BUFFER, (long)buf->size, NULL,
                           buf->usage == D3D11_USAGE_DYNAMIC ?
                           GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
        }
    }
    if (!g_lamps.res_buf) {
        g_lamps.res_buf = 1;
        D3D_OK(LAMP_RES,
               "CreateBuffer -> real GPU buffer (vbo=%u, %u bytes, usage=%u, "
               "bind=0x%x, handle=%p)",
               buf->gl_vbo, buf->size, buf->usage, buf->bind_flags,
               (void *)buf);
    }
    if (ppBuffer) *ppBuffer = buf;
    return S_OK;
}

long __attribute__((ms_abi)) ID3D11Device_CreateTexture2D(
    void *pDevice, const void *pDesc, const void *pInitialData, void **ppTexture2D)
{
    d3d11_device_t *dev = dev_from_ptr(pDevice);
    const D3D11_TEXTURE2D_DESC *desc = (const D3D11_TEXTURE2D_DESC *)pDesc;
    struct d3d11_tex *tex;
    struct umd_tex_fmt fm;

    if (!dev || !desc) return E_INVALIDARG;
    if (umd_tex_format((unsigned int)(uintptr_t)desc->Format, &fm) < 0) {
        D3D_FAIL(LAMP_RES, "CreateTexture2D: DXGI format %u is not mapped",
                 (unsigned int)(uintptr_t)desc->Format);
        return E_INVALIDARG;
    }
    tex = calloc(1, sizeof *tex);
    if (!tex) return E_OUTOFMEMORY;
    tex->magic = D3D11_TEX_MAGIC;
    tex->device = dev;
    tex->width = (int)desc->Width;
    tex->height = (int)desc->Height;
    tex->format = (unsigned int)(uintptr_t)desc->Format;
    tex->bind_flags = desc->BindFlags;
    tex->usage = (unsigned int)(uintptr_t)desc->Usage;
    tex->cpu_access = desc->CPUAccessFlags;
    tex->row_pitch = desc->Width * fm.bytes;
    tex->refcount = 1;
    if (fm.kind == 2) tex->is_depth = 1;

    gl_load_all();
    if (!gl_gen_textures || !gl_bind_texture || !gl_tex_image2d ||
        !gl_gen_framebuffers || !gl_bind_framebuffer ||
        !gl_framebuffer_texture2d || !gl_check_framebuffer_status ||
        !gl_gen_renderbuffers || !gl_bind_renderbuffer ||
        !gl_renderbuffer_storage || !gl_framebuffer_renderbuffer) {
        free(tex);
        return DXGI_ERROR_DEVICE_REMOVED;
    }

    if (fm.kind == 1) {
        gl_gen_textures(1, &tex->gl_tex);
        gl_bind_texture(GL_TEXTURE_2D, tex->gl_tex);
        gl_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl_tex_image2d(GL_TEXTURE_2D, 0, fm.fmt, (int)desc->Width,
                       (int)desc->Height, 0, fm.fmt, fm.type,
                       pInitialData ?
                       ((const D3D11_SUBRESOURCE_DATA *)pInitialData)->pSysMem :
                       NULL);
        if ((desc->BindFlags & D3D11_BIND_RENDER_TARGET) ||
            (desc->BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
            gl_gen_framebuffers(1, &tex->gl_fbo);
            gl_bind_framebuffer(GL_FRAMEBUFFER, tex->gl_fbo);
            gl_framebuffer_texture2d(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                     GL_TEXTURE_2D, tex->gl_tex, 0);
            if (gl_check_framebuffer_status(GL_FRAMEBUFFER) !=
                GL_FRAMEBUFFER_COMPLETE) {
                D3D_FAIL(LAMP_RES,
                         "CreateTexture2D %ux%u: FBO incomplete (0x%x)",
                         tex->width, tex->height,
                         gl_check_framebuffer_status(GL_FRAMEBUFFER));
                gl_bind_framebuffer(GL_FRAMEBUFFER, 0);
                if (ppTexture2D) *ppTexture2D = NULL;
                tex->magic = 0;
                free(tex);
                return DXGI_ERROR_DEVICE_REMOVED;
            }
            gl_bind_framebuffer(GL_FRAMEBUFFER, 0);
        }
    }
    /* depth textures carry their DSV through the renderbuffer */
    if (fm.kind == 2 && pInitialData == NULL) {
        gl_gen_renderbuffers(1, &tex->gl_rbo);
        gl_bind_renderbuffer(GL_RENDERBUFFER, tex->gl_rbo);
        gl_renderbuffer_storage(GL_RENDERBUFFER, fm.fmt,
                                (int)desc->Width, (int)desc->Height);
        gl_bind_renderbuffer(GL_RENDERBUFFER, 0);
    }
    if (!g_lamps.res_tex) {
        g_lamps.res_tex = 1;
        D3D_OK(LAMP_RES,
               "CreateTexture2D -> real GPU texture (tex=%u fbo=%u, %ux%u "
               "DXGI=%u is_depth=%d, handle=%p)",
               tex->gl_tex, tex->gl_fbo, tex->width, tex->height,
               tex->format, tex->is_depth, (void *)tex);
    } else {
        D3D_OK(LAMP_RES, "CreateTexture2D %ux%u DXGI=%u -> tex=%u fbo=%u rbo=%u",
               tex->width, tex->height, tex->format, tex->gl_tex,
               tex->gl_fbo, tex->gl_rbo);
    }
    if (ppTexture2D) *ppTexture2D = tex;
    return S_OK;
}

long __attribute__((ms_abi)) ID3D11Device_CreateRenderTargetView(
    void *pDevice, void *pResource, const void *pDesc, void **ppRTView)
{
    d3d11_device_t *dev = dev_from_ptr(pDevice);
    d3d11_rendertarget_t *rtv;

    if (!dev) return E_INVALIDARG;
    (void)pDesc;
    rtv = calloc(1, sizeof *rtv);
    if (!rtv) return E_OUTOFMEMORY;
    rtv->magic = D3D11_RTV_MAGIC;
    rtv->device = dev;
    rtv->refcount = 1;
    rtv->format = DXGI_FORMAT_R8G8B8A8_UNORM;

    if (sc_from_ptr((IDXGISwapChain)pResource)) {
        d3d11_swapchain_t *sc = sc_from_ptr((IDXGISwapChain)pResource);

        rtv->swapchain = sc;
        rtv->width = sc->width;
        rtv->height = sc->height;
        rtv->gl_fbo = 0; /* the swapchain target is the default framebuffer */
    } else {
        struct d3d11_tex *tex = tex_from_ptr(pResource);

        rtv->tex = tex;
        if (tex && tex->width > 0) {
            rtv->width = tex->width;
            rtv->height = tex->height;
            rtv->format = tex->format;
            rtv->gl_fbo = tex->gl_fbo;
            if (!tex->gl_fbo) {
                D3D_FAIL(LAMP_RES,
                         "CreateRenderTargetView on a texture without an "
                         "FBO (BindFlags without RENDER_TARGET?)");
                rtv->magic = 0;
                free(rtv);
                return E_INVALIDARG;
            }
        } else {
            D3D_FAIL(LAMP_RES, "CreateRenderTargetView: unknown resource");
            rtv->magic = 0;
            free(rtv);
            return E_INVALIDARG;
        }
        D3D_OK(LAMP_RES,
               "CreateRenderTargetView offscreen -> fbo=%u (%dx%d)",
               rtv->gl_fbo, rtv->width, rtv->height);
        if (ppRTView) *ppRTView = rtv;
        return S_OK;
    }
    D3D_OK(LAMP_RES, "CreateRenderTargetView swapchain -> default FB (%dx%d)",
           rtv->width, rtv->height);
    if (ppRTView) *ppRTView = rtv;
    return S_OK;
}

long __attribute__((ms_abi)) ID3D11Device_CreateDepthStencilView(
    void *pDevice, void *pResource, const void *pDesc, void **ppDepthStencilView)
{
    d3d11_device_t *dev = dev_from_ptr(pDevice);
    d3d11_depthstencil_t *dsv;
    struct d3d11_tex *tex = tex_from_ptr(pResource);
    d3d11_swapchain_t *sc = tex ? NULL : sc_from_ptr((IDXGISwapChain)pResource);
    unsigned int rbo = 0;

    (void)pDesc;
    if (!dev) return E_INVALIDARG;
    dsv = calloc(1, sizeof *dsv);
    if (!dsv) return E_OUTOFMEMORY;
    dsv->magic = D3D11_DSV_MAGIC;
    dsv->device = dev;
    dsv->refcount = 1;
    dsv->format = DXGI_FORMAT_D24_UNORM_S8_UINT;

    gl_load_all();
    if (gl_gen_renderbuffers && gl_bind_renderbuffer &&
        gl_renderbuffer_storage && gl_gen_framebuffers &&
        gl_bind_framebuffer && gl_framebuffer_renderbuffer) {
        if (tex && tex->width > 0) {
            /* a real depth renderbuffer of the texture (color textures
             * carry no rbo; the D24 one was made in CreateTexture2D) */
            dsv->tex = tex;
            dsv->width = tex->width;
            dsv->height = tex->height;
            rbo = tex->gl_rbo;
        } else if (sc) {
            /* the swapchain's depth surface: one shared depth RBO */
            dsv->width = sc->width;
            dsv->height = sc->height;
            if (!sc->gl_depth_rbo) {
                gl_gen_renderbuffers(1, &sc->gl_depth_rbo);
                gl_bind_renderbuffer(GL_RENDERBUFFER, sc->gl_depth_rbo);
                gl_renderbuffer_storage(GL_RENDERBUFFER,
                                        GL_DEPTH_COMPONENT24,
                                        sc->width, sc->height);
                gl_bind_renderbuffer(GL_RENDERBUFFER, 0);
            }
            rbo = sc->gl_depth_rbo;
        }
        /* the DSV owns a depth-only FBO: the depth clear runs there */
        if (rbo) {
            gl_gen_framebuffers(1, &dsv->gl_fbo);
            gl_bind_framebuffer(GL_FRAMEBUFFER, dsv->gl_fbo);
            gl_framebuffer_renderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                        GL_RENDERBUFFER, rbo);
            gl_bind_framebuffer(GL_FRAMEBUFFER, 0);
        }
    }

    if (!dsv->width) {
        D3D_FAIL(LAMP_RES, "CreateDepthStencilView: unknown resource");
        if (dsv->gl_fbo) gl_delete_framebuffers(1, &dsv->gl_fbo);
        dsv->magic = 0;
        free(dsv);
        return E_INVALIDARG;
    }
    D3D_OK(LAMP_RES, "CreateDepthStencilView %dx%d D24 (rbo=%u fbo=%u, "
           "handle=%p)",
           dsv->width, dsv->height, rbo, dsv->gl_fbo, (void *)dsv);
    if (ppDepthStencilView) *ppDepthStencilView = dsv;
    return S_OK;
}

/* ===================================================================== */
/* Input layout + shaders (GFX-033, GFX-034)                             */
/* ===================================================================== */

long __attribute__((ms_abi)) ID3D11Device_CreateInputLayout(
    void *pDevice, const void *pInputElementDescs, unsigned NumElements,
    const void *pShaderBytecodeWithInputSignature, unsigned BytecodeLength,
    void **ppInputLayout)
{
    d3d11_device_t *dev = dev_from_ptr(pDevice);
    const D3D11_INPUT_ELEMENT_DESC *in =
        (const D3D11_INPUT_ELEMENT_DESC *)pInputElementDescs;
    d3d11_inputlayout_t *il;

    if (!dev || !in || NumElements == 0) return E_INVALIDARG;
    if (NumElements > 2) {
        D3D_FAIL(LAMP_PROGRAM,
                 "CreateInputLayout: the pass-through program binds "
                 "exactly 2 attributes (a0, a1) — %u elements refused",
                 NumElements);
        return E_INVALIDARG;
    }
    il = calloc(1, sizeof *il);
    if (!il) return E_OUTOFMEMORY;
    il->magic = D3D11_IL_MAGIC;
    il->device = dev;
    il->refcount = 1;
    for (unsigned i = 0; i < NumElements; i++) {
        il->elems[i] = in[i];
        if (!in[i].SemanticName)
            continue;
        il->elems[i].SemanticName = strdup(in[i].SemanticName);
    }
    il->n = NumElements;
    {
        char desc[160];
        int kind = umd_shader_kind(pShaderBytecodeWithInputSignature,
                                   BytecodeLength, desc, sizeof desc);

        if (!g_lamps.il) {
            g_lamps.il = 1;
            D3D_OK(LAMP_PROGRAM,
                   "CreateInputLayout -> %u element(s): %s location a0, %s "
                   "location a1 (input signature source: %s)",
                   NumElements,
                   in[0].SemanticName ? in[0].SemanticName : "?",
                   NumElements > 1 && in[1].SemanticName ? in[1].SemanticName : "?",
                   desc);
        }
        (void)kind;
    }
    if (ppInputLayout) *ppInputLayout = il;
    return S_OK;
}

/* forward declaration: defined right after the two Create*Shader entry
 * points (they share the teardown of both). */
static long d3d11_create_shader(d3d11_device_t *dev, int type,
                                const void *code, unsigned int len,
                                void *class_linkage, void **out);

long __attribute__((ms_abi)) ID3D11Device_CreateVertexShader(
    void *pDevice, const void *pShaderBytecode, unsigned BytecodeLength,
    void *pClassLinkage, void **ppVertexShader)
{
    return (long)d3d11_create_shader(pDevice, 0, pShaderBytecode,
                                     BytecodeLength, pClassLinkage,
                                     ppVertexShader);
}

long __attribute__((ms_abi)) ID3D11Device_CreatePixelShader(
    void *pDevice, const void *pShaderBytecode, unsigned BytecodeLength,
    void *pClassLinkage, void **ppPixelShader)
{
    return (long)d3d11_create_shader(pDevice, 1, pShaderBytecode,
                                     BytecodeLength, pClassLinkage,
                                     ppPixelShader);
}

static long d3d11_create_shader(d3d11_device_t *dev, int type,
                                const void *code, unsigned int len,
                                void *class_linkage, void **out)
{
    d3d11_shader_t *sh;
    char desc[160];
    int kind;

    if (!dev_from_ptr((ID3D11Device)dev)) return E_INVALIDARG;
    sh = calloc(1, sizeof *sh);
    if (!sh) return E_OUTOFMEMORY;
    sh->magic = D3D11_SHADER_MAGIC;
    sh->device = dev_from_ptr((ID3D11Device)dev);
    sh->type = type;
    sh->refcount = 1;
    sh->bytes = len;
    kind = umd_shader_kind(code, len, desc, sizeof desc);
    sh->kind = kind;
    if (!out) {
        D3D_FAIL(LAMP_SHADER, "shaders: out pointer missing");
        free(sh);
        return E_INVALIDARG;
    }
    switch (kind) {
    case 1: /* real DXBC — translation NOT implemented, say so */
        sh->stub_reported = 1;
        D3D_STUB(LAMP_SHADER,
                 "Create%sShader: %s — MS bytecode is NOT translated "
                 "(the pass-through VS/FS pair is bound instead)",
                 type == 0 ? "Vertex" : "Pixel", desc);
        break;
    case 2: /* NTBA internal bytecode: documented tiny bytecode */
        D3D_OK(LAMP_SHADER,
               "Create%sShader: NTBA internal bytecode accepted -> the "
               "pass-through pair (bytes=%u)", type == 0 ? "Vertex" : "Pixel",
               len);
        break;
    case 3:
        D3D_OK(LAMP_SHADER,
               "Create%sShader: %s -> the pass-through pair is bound "
               "(bytes=%u, location wiring comes from the input layout)",
               type == 0 ? "Vertex" : "Pixel", desc, len);
        break;
    }
    (void)class_linkage;
    *out = sh;
    return S_OK;
}
/* ===================================================================== */
/* Immediate context: state plumbing (GFX-034..GFX-037)                   */
/* ===================================================================== */

static long umd_now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000L + (ts.tv_nsec / 1000);
}

static int umd_make_current(d3d11_device_t *dev)
{
    if (!dev || !dev->present)
        return 0;
    return gfx_present_make_current(dev->present);
}

/* The D3D11 index-buffer format -> the GL index element type.  Compat:
 * the old bridge test passes the bogus "28" (R8G8B8A8) as the index
 * format — the subset honestly defaults that to R16 ( UNSIGNED_SHORT ). */
static unsigned int umd_index_type(unsigned int dxgi_fmt)
{
    if (dxgi_fmt == DXGI_FORMAT_R32_UINT)
        return GL_UNSIGNED_INT;
    return GL_UNSIGNED_SHORT;
}

static unsigned int umd_index_size(unsigned int gl_type)
{
    return gl_type == GL_UNSIGNED_INT ? 4u : 2u;
}

/* Build the baked pass-through program once (GFX-034). */
static int umd_ensure_program(d3d11_device_t *dev)
{
    if (!dev)
        return -1;
    if (dev->program_ok)
        return 0;
    if (umd_build_program(dev) < 0)
        return -1;
    if (!g_lamps.program) {
        g_lamps.program = 1;
        D3D_OK(LAMP_PROGRAM,
               "baked pass-through pair compiled+linked (program=%u, "
               "a0 = layout element 0, a1 = layout element 1, renderer=%s)",
               dev->program, dev->renderer[0] ? dev->renderer : "unknown");
    }
    return 0;
}

static void umd_use_program(d3d11_device_t *dev)
{
    if (umd_ensure_program(dev) == 0 && dev->program_ok && gl_use_program)
        gl_use_program(dev->program);
}

/* --------------------------------------------------------------------- */

void __attribute__((ms_abi)) ID3D11DeviceContext_IASetInputLayout(
    void *pContext, void *pInputLayout)
{
    d3d11_context_t *ctx = ctx_from_ptr(pContext);
    d3d11_inputlayout_t *il = il_from_ptr(pInputLayout);

    if (!ctx)
        return;
    ctx->input_layout = il;
}

void __attribute__((ms_abi)) ID3D11DeviceContext_IASetVertexBuffers(
    void *pContext, unsigned StartSlot, unsigned NumBuffers,
    void *const *ppVertexBuffers, const unsigned int *pStrides,
    const unsigned int *pOffsets)
{
    d3d11_context_t *ctx = ctx_from_ptr(pContext);
    unsigned i;

    if (!ctx || StartSlot + NumBuffers > 8)
        return;
    for (i = 0; i < NumBuffers; i++) {
        ctx->vbs[StartSlot + i] = buf_from_ptr(ppVertexBuffers[i]);
        ctx->vb_strides[StartSlot + i] = pStrides ? pStrides[i] : 0;
        ctx->vb_offsets[StartSlot + i] = pOffsets ? pOffsets[i] : 0;
    }
    if (StartSlot + NumBuffers > ctx->num_vbs)
        ctx->num_vbs = StartSlot + NumBuffers;
}

void __attribute__((ms_abi)) ID3D11DeviceContext_IASetIndexBuffer(
    void *pContext, void *pIndexBuffer, void *Format, unsigned Offset)
{
    d3d11_context_t *ctx = ctx_from_ptr(pContext);

    if (!ctx)
        return;
    ctx->ib = buf_from_ptr(pIndexBuffer);
    ctx->ib_format = (unsigned int)(uintptr_t)Format;
    ctx->ib_offset = Offset;
}

void __attribute__((ms_abi)) ID3D11DeviceContext_IASetPrimitiveTopology(
    void *pContext, void *Topology)
{
    d3d11_context_t *ctx = ctx_from_ptr(pContext);

    if (!ctx)
        return;
    ctx->topology = (unsigned int)(uintptr_t)Topology;
}

void __attribute__((ms_abi)) ID3D11DeviceContext_VSSetShader(
    void *pContext, void *pVertexShader, void *const *ppClassInstances,
    unsigned NumClassInstances)
{
    d3d11_context_t *ctx = ctx_from_ptr(pContext);

    if (!ctx)
        return;
    ctx->vs = sh_from_ptr(pVertexShader);
    (void)ppClassInstances; (void)NumClassInstances;
}

void __attribute__((ms_abi)) ID3D11DeviceContext_PSSetShader(
    void *pContext, void *pPixelShader, void *const *ppClassInstances,
    unsigned NumClassInstances)
{
    d3d11_context_t *ctx = ctx_from_ptr(pContext);

    if (!ctx)
        return;
    ctx->ps = sh_from_ptr(pPixelShader);
    (void)ppClassInstances; (void)NumClassInstances;
}

void __attribute__((ms_abi)) ID3D11DeviceContext_OMSetRenderTargets(
    void *pContext, unsigned NumViews, void *const *ppRenderTargetViews,
    void *pDepthStencilView)
{
    d3d11_context_t *ctx = ctx_from_ptr(pContext);
    d3d11_rendertarget_t *rtv;

    if (!ctx)
        return;
    (void)NumViews;
    rtv = ppRenderTargetViews ? rtv_from_ptr(ppRenderTargetViews[0]) : NULL;
    ctx->rtv = rtv;
    ctx->dsv = dsv_from_ptr(pDepthStencilView);

    umd_make_current(ctx->device);
    gl_load_all();
    if (!gl_bind_framebuffer)
        return;
    if (rtv) {
        ctx->fb_bound = rtv->gl_fbo;
        gl_bind_framebuffer(GL_FRAMEBUFFER, rtv->gl_fbo);
        /* the offscreen path: attach the depth renderbuffer of the DSV
         * to the RT's framebuffer so depth writes actually land */
        if (ctx->dsv && ctx->dsv->tex && ctx->dsv->tex->gl_rbo &&
            rtv->gl_fbo && gl_framebuffer_renderbuffer)
            gl_framebuffer_renderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                        GL_RENDERBUFFER,
                                        ctx->dsv->tex->gl_rbo);
    } else {
        ctx->fb_bound = 0;
        gl_bind_framebuffer(GL_FRAMEBUFFER, 0);
    }
}

void __attribute__((ms_abi)) ID3D11DeviceContext_RSSetViewports(
    void *pContext, unsigned NumViewports, const void *pViewports)
{
    d3d11_context_t *ctx = ctx_from_ptr(pContext);
    const D3D11_VIEWPORT *vp = (const D3D11_VIEWPORT *)pViewports;

    if (!ctx || !vp || NumViewports == 0)
        return;
    ctx->viewport = vp[0];
    umd_make_current(ctx->device);
    gl_load_all();
    if (gl_viewport)
        gl_viewport((int)vp[0].TopLeftX, (int)vp[0].TopLeftY,
                    (int)(vp[0].Width  > 0 ? vp[0].Width  : 1),
                    (int)(vp[0].Height > 0 ? vp[0].Height : 1));
}

/* --------------------------------------------------------------------- */
/* Clear (GFX-035)                                                        */
/* --------------------------------------------------------------------- */

void __attribute__((ms_abi)) ID3D11DeviceContext_ClearRenderTargetView(
    void *pContext, void *pRenderTargetView, const float color[4])
{
    d3d11_context_t *ctx = ctx_from_ptr(pContext);
    d3d11_rendertarget_t *rtv = rtv_from_ptr(pRenderTargetView);

    if (!ctx || !rtv || !color)
        return;
    umd_make_current(ctx->device);
    gl_load_all();
    if (!gl_clear || !gl_clear_color || !gl_bind_framebuffer)
        return;
    gl_bind_framebuffer(GL_FRAMEBUFFER, rtv->gl_fbo);
    gl_clear_color(color[0], color[1], color[2], color[3]);
    gl_clear(GL_COLOR_BUFFER_BIT);
    gl_bind_framebuffer(GL_FRAMEBUFFER, ctx->fb_bound);
    if (!g_lamps.clear) {
        g_lamps.clear = 1;
        D3D_OK(LAMP_CLEAR,
               "ClearRenderTargetView -> glClear(GL_COLOR_BUFFER_BIT) on "
               "FB %u (color %.2f %.2f %.2f %.2f) [the present clears use "
               "the same GL path]",
               rtv->gl_fbo, color[0], color[1], color[2], color[3]);
    }
}

/*
 * HONEST NOTE ON THE DEPTH CLEAR: GLES2 has no glDepth on the default FB.
 * The DSV owns a depth renderbuffer (wrapped in its own FBO at
 * CreateDepthStencilView); the depth clear runs in the DSV's own FBO and —
 * for a texture RT — OMSetRenderTargets attaches the same renderbuffer to
 * the draw FBO, so depth writes of the draw land in the same buffer.
 */
void __attribute__((ms_abi)) ID3D11DeviceContext_ClearDepthStencilView(
    void *pContext, void *pDepthStencilView, unsigned ClearFlags,
    float Depth, unsigned char Stencil)
{
    d3d11_context_t *ctx = ctx_from_ptr(pContext);
    d3d11_depthstencil_t *dsv = dsv_from_ptr(pDepthStencilView);
    unsigned int fb;

    if (!ctx || !dsv)
        return;
    (void)Stencil;
    umd_make_current(ctx->device);
    gl_load_all();
    if (!gl_clear || !gl_clear_depthf || !gl_depth_mask || !gl_bind_framebuffer)
        return;
    fb = dsv->gl_fbo ? dsv->gl_fbo : ctx->fb_bound;
    if (ClearFlags & D3D11_CLEAR_DEPTH) {
        gl_depth_mask(GL_TRUE);
        gl_clear_depthf(Depth);
        gl_bind_framebuffer(GL_FRAMEBUFFER, fb);
        gl_clear(GL_DEPTH_BUFFER_BIT);
        gl_bind_framebuffer(GL_FRAMEBUFFER, ctx->fb_bound);
        if (!g_lamps.clear) {
            g_lamps.clear = 1;
            D3D_OK(LAMP_CLEAR,
                   "ClearDepthStencilView -> glClear(GL_DEPTH_BUFFER_BIT, "
                   "%.2f) on FB %u (the DSV depth renderbuffer)",
                   Depth, fb);
        }
    }
    (void)ClearFlags; /* stencil clear: ES2 has no GL_STENCIL via rbo D24 */
}

/* The D3D9-style Clear on the context: clears the CURRENTLY bound RT. */
void __attribute__((ms_abi)) ID3D11DeviceContext_Clear(
    void *pContext, const float color[4])
{
    d3d11_context_t *ctx = ctx_from_ptr(pContext);

    if (!ctx || !color)
        return;
    umd_make_current(ctx->device);
    gl_load_all();
    if (!gl_clear || !gl_clear_color)
        return;
    gl_clear_color(color[0], color[1], color[2], color[3]);
    gl_clear(GL_COLOR_BUFFER_BIT);
    if (!g_lamps.clear) {
        g_lamps.clear = 1;
        D3D_OK(LAMP_CLEAR,
               "Clear -> glClear(GL_COLOR_BUFFER_BIT) on FB %u "
               "(color %.2f %.2f %.2f %.2f)",
               ctx->fb_bound, color[0], color[1], color[2], color[3]);
    }
}

/* --------------------------------------------------------------------- */
/* Draw (GFX-036) — the pass-through program with the app's input layout   */
/* --------------------------------------------------------------------- */

static void umd_draw_common(d3d11_context_t *ctx, unsigned int mode,
                            int indexed, unsigned int count,
                            unsigned int start, int base_vertex)
{
    d3d11_device_t *dev = ctx->device;
    d3d11_inputlayout_t *il = ctx->input_layout;
    unsigned int i;

    umd_make_current(dev);
    gl_load_all();
    if (!gl_draw_arrays || !gl_draw_elements || !gl_vertex_attrib_pointer ||
        !gl_enable_vertex_attrib_array || !gl_disable_vertex_attrib_array ||
        !gl_bind_buffer)
        return;
    umd_use_program(dev);
    if (!dev->program_ok)
        return;

    if (!il) {
        D3D_FAIL(LAMP_DRAW, "Draw without an input layout (IASetInputLayout "
                 "required by the pass-through program)");
        return;
    }
    /* bind the app's layout: semantics -> a0/a1 with REAL GPU buffers */
    for (i = 0; i < il->n; i++) {
        unsigned int slot = il->elems[i].InputSlot;
        d3d11_buffer_t *buf = (slot < 8) ? ctx->vbs[slot] : NULL;
        struct umd_vat vat;
        unsigned int stride;
        size_t off;

        if (!buf || buf->magic != D3D11_BUFFER_MAGIC) {
            D3D_FAIL(LAMP_DRAW,
                     "Draw: input element %u refers to a vertex buffer "
                     "slot %u that is not bound", i, slot);
            return;
        }
        if (umd_vertex_format((unsigned int)(uintptr_t)il->elems[i].Format,
                              &vat) < 0) {
            D3D_FAIL(LAMP_DRAW,
                     "Draw: input element %u format %u is not mapped",
                     i, (unsigned)(uintptr_t)il->elems[i].Format);
            return;
        }
        stride = ctx->vb_strides[slot] ? ctx->vb_strides[slot]
                                       : (unsigned int)vat.bytes;
        off = (size_t)il->elems[i].AlignedByteOffset;
        if (indexed && base_vertex != 0)
            off += (size_t)(base_vertex * (int)stride); /* emulated */
        gl_bind_buffer(GL_ARRAY_BUFFER, buf->gl_vbo);
        gl_vertex_attrib_pointer(i, vat.count, vat.type, vat.norm,
                                 (int)stride, (const void *)off);
        gl_enable_vertex_attrib_array(i);
    }
    for (i = il->n; i < 8; i++)
        gl_disable_vertex_attrib_array(i);

    if (indexed) {
        struct d3d11_buffer *ib = ctx->ib;
        unsigned int itype = umd_index_type(ctx->ib_format);
        size_t idx_off;

        if (!ib) {
            D3D_FAIL(LAMP_DRAW, "DrawIndexed without an index buffer");
            return;
        }
        idx_off = (size_t)ctx->ib_offset + (size_t)start * umd_index_size(itype);
        gl_bind_buffer(GL_ELEMENT_ARRAY_BUFFER, ib->gl_vbo);
        gl_draw_elements(mode, (int)count, itype, (const void *)idx_off);
        gl_bind_buffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    } else {
        if (ctx->num_vbs == 0 || !ctx->vbs[0]) {
            D3D_FAIL(LAMP_DRAW, "Draw without a vertex buffer");
            return;
        }
        gl_bind_buffer(GL_ARRAY_BUFFER, ctx->vbs[0]->gl_vbo);
        gl_draw_arrays(mode, (int)start, (int)count);
    }

    if (!g_lamps.draw) {
        g_lamps.draw = 1;
        D3D_OK(LAMP_DRAW,
               "%s -> REAL GPU draw (mode=tris, count=%u, start=%u, "
               "program=%u, vbo=%u%s) — the app's buffers/geometry, no CPU "
               "shadow of the frame",
               indexed ? "DrawIndexed" : "Draw", count, start, dev->program,
               indexed && ctx->ib ? ctx->ib->gl_vbo : ctx->vbs[0]->gl_vbo,
               indexed ? " ib" : "");
    }
}

void __attribute__((ms_abi)) ID3D11DeviceContext_Draw(
    void *pContext, unsigned VertexCount, unsigned StartVertexLocation)
{
    d3d11_context_t *ctx = ctx_from_ptr(pContext);

    if (!ctx)
        return;
    umd_draw_common(ctx, umd_topology_gl(ctx->topology), 0,
                    VertexCount, StartVertexLocation, 0);
}

void __attribute__((ms_abi)) ID3D11DeviceContext_DrawIndexed(
    void *pContext, unsigned IndexCount, unsigned StartIndexLocation,
    int BaseVertexLocation)
{
    d3d11_context_t *ctx = ctx_from_ptr(pContext);

    if (!ctx)
        return;
    umd_draw_common(ctx, umd_topology_gl(ctx->topology), 1,
                    IndexCount, StartIndexLocation, BaseVertexLocation);
}

/* --------------------------------------------------------------------- */
/* UpdateSubresource + Map/Unmap (GFX-037)                                */
/* --------------------------------------------------------------------- */

static void umd_upload_texture(struct d3d11_tex *tex, const void *src,
                               unsigned int pitch)
{
    struct umd_tex_fmt fm;

    if (umd_tex_format(tex->format, &fm) < 0 || fm.kind != 1 || !tex->gl_tex)
        return;
    gl_bind_texture(GL_TEXTURE_2D, tex->gl_tex);
    if (pitch == tex->row_pitch || pitch == 0) {
        gl_tex_sub_image2d(GL_TEXTURE_2D, 0, 0, 0, tex->width, tex->height,
                           fm.fmt, fm.type, src);
        return;
    }
    /* pitch mismatch: honest row-by-row upload (app data, NOT frame data) */
    {
        unsigned int r;
        const unsigned char *p = (const unsigned char *)src;

        gl_pixel_storei(GL_UNPACK_ALIGNMENT, 1);
        for (r = 0; r < (unsigned)tex->height; r++)
            gl_tex_sub_image2d(GL_TEXTURE_2D, 0, 0, (int)r, tex->width, 1,
                               fm.fmt, fm.type, p + (size_t)r * pitch);
        gl_pixel_storei(GL_UNPACK_ALIGNMENT, 4);
    }
}

void __attribute__((ms_abi)) ID3D11DeviceContext_UpdateSubresource(
    void *pContext, void *pDstResource, unsigned DstSubresource,
    const void *pDstBox, const void *pSrcData, unsigned SrcRowPitch,
    unsigned SrcDepthPitch)
{
    d3d11_context_t *ctx = ctx_from_ptr(pContext);
    struct d3d11_tex *tex = tex_from_ptr(pDstResource);
    d3d11_buffer_t *buf = tex ? NULL : buf_from_ptr(pDstResource);

    if (!ctx || (!tex && !buf) || !pSrcData)
        return;
    (void)DstSubresource; (void)SrcDepthPitch;
    umd_make_current(ctx->device);
    gl_load_all();

    if (tex) {
        umd_upload_texture(tex, pSrcData, SrcRowPitch);
        if (!g_lamps.update) {
            g_lamps.update = 1;
            D3D_OK(LAMP_UPDATE,
                   "UpdateSubresource -> glTexSubImage2D (%ux%u tex=%u, "
                   "pitch=%u; the app->GPU upload — NOT in the presentation "
                   "path)",
                   tex->width, tex->height, tex->gl_tex, SrcRowPitch);
        }
        return;
    }
    /* buffer path */
    if (buf->usage == D3D11_USAGE_STAGING) {
        unsigned int off = 0;

        if (pDstBox) {
            const struct { unsigned int left, top, front, right, bottom, back; }
                *box = (const void *)pDstBox;

            off = box->left;
        }
        if (off < buf->shadow_size && buf->shadow)
            memcpy((unsigned char *)buf->shadow + off, pSrcData,
                   (size_t)buf->shadow_size - off);
        if (!g_lamps.update) {
            g_lamps.update = 1;
            D3D_OK(LAMP_UPDATE,
                   "UpdateSubresource -> staging shadow copy (%u bytes; "
                   "staging = CPU memory by definition)",
                   buf->shadow_size);
        }
        return;
    }
    if (buf->gl_vbo) {
        unsigned int off = 0;

        if (pDstBox) {
            const struct { unsigned int left, top, front, right, bottom, back; }
                *box = (const void *)pDstBox;

            off = box->left;
        }
        gl_bind_buffer(GL_ARRAY_BUFFER, buf->gl_vbo);
        gl_buffer_sub_data(GL_ARRAY_BUFFER, (long)off,
                           (long)buf->size - (long)off, pSrcData);
        if (!g_lamps.update) {
            g_lamps.update = 1;
            D3D_OK(LAMP_UPDATE,
                   "UpdateSubresource -> glBufferSubData (vbo=%u, %u bytes "
                   "+off=%u; the app->GPU upload — NOT the presentation path)",
                   buf->gl_vbo, buf->size, off);
        }
    }
}

long __attribute__((ms_abi)) ID3D11DeviceContext_Map(
    void *pContext, void *pResource, unsigned Subresource, void *MapType,
    unsigned MapFlags, void *pMappedResource)
{
    d3d11_context_t *ctx = ctx_from_ptr(pContext);
    struct d3d11_tex *tex = tex_from_ptr(pResource);
    d3d11_buffer_t *buf = tex ? NULL : buf_from_ptr(pResource);
    D3D11_MAPPED_SUBRESOURCE *m = (D3D11_MAPPED_SUBRESOURCE *)pMappedResource;
    unsigned int type = (unsigned int)(uintptr_t)MapType;

    (void)Subresource; (void)MapFlags;

    if (!ctx || !m)
        return E_INVALIDARG;
    if (!tex && !buf)
        return E_INVALIDARG;
    gl_load_all();

    if (tex) {
        if (type == D3D11_MAP_READ) {
            /* HONESTY: GPU readback is not translated (no staging
             * semantics yet). */
            if (!g_lamps.map_read) {
                g_lamps.map_read = 1;
                D3D_STUB(LAMP_UPDATE,
                         "Map(READ) on a GPU texture -> E_FAIL (GPU "
                         "readback is NOT translated; USE staging or "
                         "UpdateSubresource instead)");
            }
            return E_FAIL;
        }
        tex->shadow_size = tex->row_pitch * (unsigned)tex->height;
        if (!tex->shadow) {
            free(tex->shadow);
            tex->shadow = malloc(tex->shadow_size);
        }
        if (!tex->shadow)
            return E_OUTOFMEMORY;
        tex->mapped = 1;
        m->pData = tex->shadow;
        m->RowPitch = tex->row_pitch;
        m->DepthPitch = tex->shadow_size;
        if (!g_lamps.map_write) {
            g_lamps.map_write = 1;
            D3D_OK(LAMP_UPDATE,
                   "Map(WRITE) -> shadow upload at Unmap (tex=%u, %u "
                   "bytes shadow; the app-side pointer, GPU upload at "
                   "Unmap)",
                   tex->gl_tex, tex->shadow_size);
        }
        return S_OK;
    }
    /* buffer */
    if (type == D3D11_MAP_READ) {
        if (buf->shadow && buf->usage == D3D11_USAGE_STAGING) {
            m->pData = buf->shadow;
            m->RowPitch = buf->size;
            m->DepthPitch = buf->size;
            return S_OK;
        }
        if (!g_lamps.map_read) {
            g_lamps.map_read = 1;
            D3D_STUB(LAMP_UPDATE,
                     "Map(READ) on a GPU buffer -> E_FAIL (GPU readback is "
                     "NOT translated; staging only)");
        }
        return E_FAIL;
    }
    if (!buf->shadow || (type == D3D11_MAP_WRITE_DISCARD)) {
        free(buf->shadow);
        buf->shadow = malloc(buf->size);
        buf->shadow_size = buf->size;
    }
    if (!buf->shadow)
        return E_OUTOFMEMORY;
    buf->mapped = 1;
    m->pData = buf->shadow;
    m->RowPitch = buf->size;
    m->DepthPitch = buf->size;
    if (!g_lamps.map_write) {
        g_lamps.map_write = 1;
        D3D_OK(LAMP_UPDATE,
               "Map(WRITE) -> shadow upload at Unmap (vbo=%u, %u bytes)",
               buf->gl_vbo, buf->size);
    }
    return S_OK;
}

long __attribute__((ms_abi)) ID3D11DeviceContext_Unmap(
    void *pContext, void *pResource, unsigned Subresource)
{
    d3d11_context_t *ctx = ctx_from_ptr(pContext);
    struct d3d11_tex *tex = tex_from_ptr(pResource);
    d3d11_buffer_t *buf = tex ? NULL : buf_from_ptr(pResource);

    (void)Subresource;
    if (!ctx)
        return E_INVALIDARG;
    umd_make_current(ctx->device);
    gl_load_all();

    if (tex) {
        if (tex->mapped && tex->shadow) {
            umd_upload_texture(tex, tex->shadow, tex->row_pitch);
            tex->mapped = 0;
        }
        return S_OK;
    }
    if (buf && buf->mapped && buf->shadow) {
        if (buf->usage == D3D11_USAGE_STAGING) {
            /* nothing: the staging buffer IS the shadow */
        } else if (buf->gl_vbo) {
            gl_bind_buffer(GL_ARRAY_BUFFER, buf->gl_vbo);
            gl_buffer_sub_data(GL_ARRAY_BUFFER, 0,
                               (long)buf->shadow_size, buf->shadow);
        }
        buf->mapped = 0;
        return S_OK;
    }
    return DXGI_ERROR_INVALID_CALL;
}

/* ===================================================================== */
/* SwapChain (GFX-038)                                                    */
/* ===================================================================== */

static long d3d11_create_swapchain(d3d11_device_t *dev, void *pDesc,
                                   void **ppSwapChain)
{
    DXGI_SWAP_CHAIN_DESC *desc = (DXGI_SWAP_CHAIN_DESC *)pDesc;
    d3d11_swapchain_t *sc;
    int w = 640, h = 480;

    if (ppSwapChain) *ppSwapChain = NULL;
    if (!dev || dev->magic != D3D11_DEVICE_MAGIC)
        return E_INVALIDARG;

    if (desc) {
        DXGI_MODE_DESC *md = (DXGI_MODE_DESC *)desc->BufferDesc;

        if (md && md->Width && md->Height) {
            w = (int)md->Width;
            h = (int)md->Height;
        }
    }
    /* the swapchain target IS the gfx_present render target */
    if (gfx_present_resize(dev->present, w, h) < 0) {
        D3D_FAIL(LAMP_SWAPCHAIN, "CreateSwapChain resize %dx%d: %s",
                 w, h, gfx_present_error());
        return E_FAIL;
    }
    gfx_present_target_size(dev->present, &w, &h);

    sc = calloc(1, sizeof *sc);
    if (!sc)
        return E_OUTOFMEMORY;
    sc->magic = D3D11_SWAPCHAIN_MAGIC;
    sc->device = dev;
    sc->hwnd = desc ? (HWND)desc->OutputWindow : NULL;
    sc->width = w;
    sc->height = h;
    sc->format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc->refcount = 1;
    sc->last_mode = -1;              /* nothing presented yet */
    sc->frame.dmabuf_fd = -1;        /* dry export protocol */

    D3D_OK(LAMP_SWAPCHAIN,
           "CreateSwapChain -> gbm/EGL surface resized to %dx%d "
           "(usage LINEAR|SCANOUT, hwnd=%p, handle=%p)",
           sc->width, sc->height, (void *)sc->hwnd, (void *)sc);
    if (sc->hwnd)
        D3D_OK("WIN-022", "CreateSwapChain for the win32k window %p "
               "(%dx%d)", (void *)sc->hwnd, sc->width, sc->height);

    if (ppSwapChain) *ppSwapChain = sc;
    return S_OK;
}

long __attribute__((ms_abi)) IDXGIFactory_CreateSwapChain(
    void *pFactory, void *pDevice, void *pDesc, void **ppSwapChain)
{
    (void)pFactory;
    return d3d11_create_swapchain((d3d11_device_t *)pDevice, pDesc,
                                  ppSwapChain);
}

long __attribute__((ms_abi)) CreateSwapChain(
    void *pFactory, void *pDevice, void *pDesc, void **ppSwapChain)
{
    return IDXGIFactory_CreateSwapChain(pFactory, pDevice, pDesc,
                                        ppSwapChain);
}

/* ===================================================================== */
/* Present (T-217): wet = gfx_present (compositor), dry = umd_egl         */
/* ===================================================================== */

/*
 * CONTRACT: Present NEVER opens/uses its own copy-measurement window.
 * The caller opens gfx_present_copy_window_begin() around Clear + Draw +
 * Present so that the counted number covers exactly the presented frame.
 */
static long d3d11_swapchain_present_impl(d3d11_swapchain_t *sc,
                                         unsigned SyncInterval, unsigned Flags)
{
    d3d11_device_t *dev;
    gfx_present_t *p;
    long t0, t1;

    if (!sc || sc->magic != D3D11_SWAPCHAIN_MAGIC || !sc->device ||
        sc->device->magic != D3D11_DEVICE_MAGIC)
        return DXGI_ERROR_INVALID_CALL;
    dev = sc->device;
    p = dev->present;
    if (!p)
        return DXGI_ERROR_DEVICE_REMOVED;
    (void)SyncInterval; (void)Flags;

    /*
     * NTBOX_UMD_FORCE_COPY — the deliberate frame-sized CPU copy INSIDE
     * the present path.  With the caller's window open the memcpy
     * interposition MUST count it (lamp FAIL in the test): the proof that
     * the counter MEASURES and does not assume.
     */
    if (getenv("NTBOX_UMD_FORCE_COPY")) {
        static volatile unsigned char sink;
        size_t n = (size_t)sc->width * (size_t)sc->height * 4;
        unsigned char *a = malloc(n);

        if (a) {
            unsigned char *b = malloc(n);

            if (b) {
                memset(a, 0x5a, n);
                memcpy(b, a, n);  /* the FORCED frame-sized CPU copy */
                sink = b[n / 2];
                (void)sink;
                free(b);
            }
            free(a);
        }
    }

    if (gfx_present_wl_ready(p)) {
        /* ---- WET: linux-dmabuf -> wl_surface.attach -> commit ---- */
        t0 = umd_now_us();
        if (gfx_present_frame(p) < 0) {
            D3D_FAIL(LAMP_SWAPCHAIN, "Present (wet) failed: %s",
                     gfx_present_error());
            return DXGI_ERROR_DEVICE_REMOVED;
        }
        t1 = umd_now_us();
        sc->last_mode = 1;
        sc->last_present_us = t1 - t0;
        sc->last_roundtrip_us = t1 - t0;
        sc->frame.dmabuf_fd = gfx_present_dmabuf_fd(p);
        sc->frame.width = gfx_present_buffer_width(p);
        sc->frame.height = gfx_present_buffer_height(p);
        sc->frame.stride = gfx_present_buffer_stride(p);
        sc->frame.fb_id = gfx_present_fb_id(p);
        snprintf(sc->frame.fb_method, sizeof sc->frame.fb_method, "%s",
                 gfx_present_fb_method(p));
        sc->frame.usage = UMD_GBM_USE_LINEAR | UMD_GBM_USE_SCANOUT;
        if (!g_lamps.present_wet) {
            g_lamps.present_wet = 1;
            D3D_OK(LAMP_SWAPCHAIN,
                   "Present (wet) -> gfx_present_frame: dma-buf fd=%d, "
                   "%ux%u stride=%u, %s (present=%ldus)",
                   sc->frame.dmabuf_fd, sc->frame.width, sc->frame.height,
                   sc->frame.stride, gfx_present_compositor(p),
                   sc->last_present_us);
        }
        return S_OK;
    }

    /* ---- DRY: no compositor — eglSwapBuffers + dma-buf export proof ---- */
    t0 = umd_now_us();
    if (umd_egl_backend() < 0) {
        D3D_FAIL(LAMP_SWAPCHAIN, "Present (dry): %s", umd_egl_error());
        return DXGI_ERROR_DEVICE_REMOVED;
    }
    if (umd_egl_swap(gfx_present_egl(p), gfx_present_egl_surface(p)) < 0) {
        D3D_FAIL(LAMP_SWAPCHAIN, "Present (dry): eglSwapBuffers: %s",
                 umd_egl_error());
        return DXGI_ERROR_DEVICE_REMOVED;
    }
    if (umd_egl_lock_export(gfx_present_egl_window(p), &sc->frame) < 0) {
        D3D_FAIL(LAMP_SWAPCHAIN, "Present (dry): %s", umd_egl_error());
        return DXGI_ERROR_DEVICE_REMOVED;
    }
    t1 = umd_now_us();
    sc->last_mode = 0;
    sc->last_present_us = t1 - t0;
    sc->last_roundtrip_us = t1 - t0; /* no compositor: the swap itself */
    if (!g_lamps.present_dry) {
        g_lamps.present_dry = 1;
        D3D_OK(LAMP_SWAPCHAIN,
               "Present (dry) -> eglSwapBuffers + dma-buf export: fd=%d, "
               "%ux%u stride=%u, %s (present=%ldus)",
               sc->frame.dmabuf_fd, sc->frame.width, sc->frame.height,
               sc->frame.stride, sc->frame.fb_method, sc->last_present_us);
    }
    return S_OK;
}

long __attribute__((ms_abi)) IDXGISwapChain_Present(
    void *pSwapChain, unsigned SyncInterval, unsigned Flags)
{
    return d3d11_swapchain_present_impl((d3d11_swapchain_t *)pSwapChain,
                                        SyncInterval, Flags);
}

long __attribute__((ms_abi)) Present(
    void *pSwapChain, unsigned SyncInterval, unsigned Flags)
{
    return IDXGISwapChain_Present(pSwapChain, SyncInterval, Flags);
}

int d3d11_swapchain_resize(IDXGISwapChain swapchain, int width, int height)
{
    d3d11_swapchain_t *sc = sc_from_ptr(swapchain);

    if (!sc || !sc->device || width <= 0 || height <= 0)
        return -1;
    if (gfx_present_resize(sc->device->present, width, height) < 0) {
        D3D_FAIL("WIN-023", "swapchain resize: %s", gfx_present_error());
        return -1;
    }
    gfx_present_target_size(sc->device->present, &width, &height);
    sc->width = width;
    sc->height = height;
    D3D_OK("WIN-023", "swapchain resized to %dx%d", width, height);
    return 0;
}

int d3d11_swapchain_present_mode(IDXGISwapChain swapchain)
{
    d3d11_swapchain_t *sc = sc_from_ptr(swapchain);

    return sc ? sc->last_mode : -1;
}

const struct umd_frame *d3d11_swapchain_frame(IDXGISwapChain swapchain)
{
    d3d11_swapchain_t *sc = sc_from_ptr(swapchain);

    return sc ? &sc->frame : NULL;
}

/* ===================================================================== */
/* Releases + accessors                                                   */
/* ===================================================================== */

void d3d11_swapchain_release(IDXGISwapChain swapchain)
{
    d3d11_swapchain_t *sc = sc_from_ptr(swapchain);

    if (!sc)
        return;
    /*
     * Fd ownership: in the DRY mode this swapchain owns the dma-buf fd it
     * exported itself.  In the WET mode gfx_present owns its fd; do NOT
     * close it here (a stale fd in the metadata is harmless).
     */
    if (sc->last_mode == 0)
        umd_frame_release(&sc->frame);
    else
        sc->frame.dmabuf_fd = -1;
    sc->magic = 0;
    free(sc);
}

void d3d11_context_release(ID3D11DeviceContext context)
{
    d3d11_context_t *ctx = ctx_from_ptr(context);

    if (!ctx)
        return;
    if (ctx->device && ctx->device->immediate_context == ctx)
        ctx->device->immediate_context = NULL;
    ctx->magic = 0;
    free(ctx);
}

void d3d11_device_release(ID3D11Device device)
{
    d3d11_device_t *dev = dev_from_ptr(device);

    if (!dev)
        return;
    if (dev->immediate_context)
        d3d11_context_release(dev->immediate_context);
    gfx_present_destroy(dev->present);
    dev->present = NULL;
    dev->magic = 0;
    free(dev);
}

gfx_present_t *d3d11_device_get_present(ID3D11Device device)
{
    d3d11_device_t *dev = dev_from_ptr(device);

    return dev ? dev->present : NULL;
}

gfx_present_t *d3d11_swapchain_get_present(IDXGISwapChain swapchain)
{
    d3d11_swapchain_t *sc = sc_from_ptr(swapchain);

    return (sc && sc->device) ? sc->device->present : NULL;
}

d3d11_device_t *d3d11_device_from_ptr(ID3D11Device dev)
{
    return dev_from_ptr(dev);
}
d3d11_context_t *d3d11_context_from_ptr(ID3D11DeviceContext ctx)
{
    return ctx_from_ptr(ctx);
}
d3d11_swapchain_t *d3d11_swapchain_from_ptr(IDXGISwapChain sc)
{
    return sc_from_ptr(sc);
}

/* ===================================================================== */
/* Win32k integration (WIN-021..WIN-023; kept from the seed)              */
/* ===================================================================== */

/*
 * Weak so the UMD test binary can link WITHOUT win32k.c (the bridge does
 * not need the window manager for its own present path).  When win32k.c
 * is linked (the seed d3d11 test, ntboxd itself), the strong definition
 * wins automatically.
 */
__attribute__((weak)) int win32k_window_get_size(HWND hwnd, int *width,
                                                 int *height)
{
    (void)hwnd;
    if (width) *width = 640;
    if (height) *height = 480;
    return 0;
}

int d3d11_win32k_create_device_and_swapchain(
    HWND hwnd, HDC hdc,
    ID3D11Device *ppDevice, ID3D11DeviceContext *ppContext,
    IDXGISwapChain *ppSwapChain)
{
    d3d11_device_t *dev;
    ID3D11Device device = NULL;
    ID3D11DeviceContext context = NULL;
    DXGI_SWAP_CHAIN_DESC desc;
    DXGI_MODE_DESC mode;
    int w = 640, h = 480;
    long hr;

    (void)hdc;
    memset(&desc, 0, sizeof desc);
    memset(&mode, 0, sizeof mode);

    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                           NULL, 0, 0, (void **)&device, NULL,
                           (void **)&context);
    if (hr != S_OK || !device) {
        D3D_FAIL(LAMP_WIN32K_SC, "win32k: D3D11CreateDevice failed (0x%lx)",
                 hr);
        return -1;
    }
    dev = dev_from_ptr(device);

    if (win32k_window_get_size(hwnd, &w, &h) != 0 || w <= 0 || h <= 0) {
        w = 640;
        h = 480;
    }
    mode.Width = (unsigned)w;
    mode.Height = (unsigned)h;
    mode.Format = (void *)(uintptr_t)DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferDesc = &mode;
    desc.BufferCount = 2;
    desc.OutputWindow = hwnd;
    desc.Windowed = 1;
    desc.BufferUsage = (void *)(uintptr_t)
        (DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT);
    desc.SwapEffect = (void *)(uintptr_t)DXGI_SWAP_EFFECT_DISCARD;

    hr = IDXGIFactory_CreateSwapChain(NULL, dev, &desc, (void **)ppSwapChain);
    if (hr != S_OK || !*ppSwapChain) {
        D3D_FAIL(LAMP_WIN32K_SC, "win32k: CreateSwapChain failed (0x%lx)",
                 hr);
        d3d11_device_release(device);
        return -1;
    }
    if (ppDevice) *ppDevice = device;
    if (ppContext) *ppContext = context;

    D3D_OK(LAMP_WIN32K_SC,
           "win32k integration: hwnd=%p %dx%d -> device=%p, context=%p, "
           "swapchain=%p (this device IS the window's device)",
           (void *)hwnd, w, h, (void *)device, (void *)context,
           *ppSwapChain);
    return 0;
}
/* ==PART2== */

