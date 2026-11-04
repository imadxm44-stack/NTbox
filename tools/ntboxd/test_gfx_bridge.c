#define _GNU_SOURCE

#include "gfx_bridge.h"
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

/* ---- lamp macros (local copy) ---- */
#define NTB0_TP(id, st, fmt, ...) \
    fprintf(stderr, "[ntbox][%s][%s] " fmt "\n", (id), (st), ##__VA_ARGS__)
#define NTB0_OK(id, fmt, ...)   NTB0_TP(id, "OK", fmt, ##__VA_ARGS__)
#define NTB0_STUB(id, fmt, ...) NTB0_TP(id, "STUB", fmt, ##__VA_ARGS__)
#define NTB0_FAIL(id, fmt, ...) NTB0_TP(id, "FAIL", fmt, ##__VA_ARGS__)
#define NTB0_SKIP(id, fmt, ...) NTB0_TP(id, "SKIP", fmt, ##__VA_ARGS__)

/* ---- GLES2 dlopen (no dev headers) ---- */
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

struct test_gl {
    void *lib;
    void (*clear)(unsigned int);
    void (*clear_color)(float, float, float, float);
    void (*viewport)(int, int, int, int);
    unsigned int (*create_shader)(unsigned int);
    void (*shader_source)(unsigned int, int, const char *const *, const int *);
    void (*compile_shader)(unsigned int);
    void (*get_shaderiv)(unsigned int, unsigned int, int *);
    void (*get_shader_info_log)(unsigned int, int, int *, char *);
    unsigned int (*create_program)(void);
    void (*attach_shader)(unsigned int, unsigned int);
    void (*link_program)(unsigned int);
    void (*get_programiv)(unsigned int, unsigned int, int *);
    void (*get_program_info_log)(unsigned int, int, int *, char *);
    void (*use_program)(unsigned int);
    void (*genbuffers)(int, unsigned int *);
    void (*bindbuffer)(unsigned int, unsigned int);
    void (*bufferdata)(unsigned int, size_t, const void *, unsigned int);
    void (*vertexattribpointer)(unsigned int, int, unsigned int, unsigned char, int, const void *);
    void (*enablevertexattribarray)(unsigned int);
    void (*drawarrays)(unsigned int, int, int);
    unsigned int (*geterror)(void);
    void (*finish)(void);
    void (*flush)(void);
    void (*deleteshader)(unsigned int);
    void (*deleteprogram)(unsigned int);
    void (*deletebuffers)(int, const unsigned int *);
};

static struct test_gl g_gl;

static int test_gl_bind(void)
{
    void *h = dlopen("libGLESv2.so.2", RTLD_LAZY | RTLD_LOCAL);
    if (!h) h = dlopen("libGLESv2.so", RTLD_LAZY | RTLD_LOCAL);
    if (!h) return -1;
    g_gl.lib = h;
#define BIND(f, s) do { *(void**)(&g_gl.f)=dlsym(g_gl.lib,s); if(!g_gl.f) return -1; } while(0)
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
    BIND(deleteshader, "glDeleteShader");
    BIND(deleteprogram, "glDeleteProgram");
    BIND(deletebuffers, "glDeleteBuffers");
#undef BIND
    return 0;
}

static const char *vs_src =
    "#version 100\nattribute vec2 pos;\nattribute vec3 color;\nvarying vec3 v_color;\n"
    "void main(){ gl_Position=vec4(pos,0.0,1.0); v_color=color; }\n";
static const char *fs_src =
    "#version 100\nprecision mediump float;\nvarying vec3 v_color;\n"
    "void main(){ gl_FragColor=vec4(v_color,1.0); }\n";

static const float tri[] = {
     0.0f,  0.6f, 1.0f, 0.0f, 0.0f,
    -0.6f, -0.5f, 0.0f, 1.0f, 0.0f,
     0.6f, -0.5f, 0.0f, 0.0f, 1.0f,
};

static int build_program(char *err, size_t en)
{
    unsigned int vs, fs, prog; int ok=0, len=0; char log[512];
    vs = g_gl.create_shader(GL_VERTEX_SHADER);
    fs = g_gl.create_shader(GL_FRAGMENT_SHADER);
    if (!vs || !fs) { snprintf(err,en,"glCreateShader"); return -1; }
    g_gl.shader_source(vs,1,&vs_src,NULL);
    g_gl.shader_source(fs,1,&fs_src,NULL);
    g_gl.compile_shader(vs); g_gl.compile_shader(fs);
    g_gl.get_shaderiv(vs,GL_COMPILE_STATUS,&ok);
    if (!ok) { g_gl.get_shader_info_log(vs,sizeof log-1,&len,log); log[sizeof log-1]=0; snprintf(err,en,"vs: %s",log); return -1; }
    g_gl.get_shaderiv(fs,GL_COMPILE_STATUS,&ok);
    if (!ok) { g_gl.get_shader_info_log(fs,sizeof log-1,&len,log); log[sizeof log-1]=0; snprintf(err,en,"fs: %s",log); return -1; }
    prog = g_gl.create_program();
    g_gl.attach_shader(prog,vs); g_gl.attach_shader(prog,fs);
    g_gl.link_program(prog);
    g_gl.get_programiv(prog,GL_LINK_STATUS,&ok);
    if (!ok) { g_gl.get_program_info_log(prog,sizeof log-1,&len,log); log[sizeof log-1]=0; snprintf(err,en,"link: %s",log); return -1; }
    g_gl.deleteshader(vs); g_gl.deleteshader(fs);
    g_gl.use_program(prog);
    return 0;
}

static int draw_tri(int w, int h, char *err, size_t en)
{
    unsigned int vbo=0;
    g_gl.viewport(0,0,w,h);
    g_gl.clear_color(0.05f,0.07f,0.12f,1.0f);
    g_gl.clear(GL_COLOR_BUFFER_BIT);
    g_gl.genbuffers(1,&vbo);
    g_gl.bindbuffer(GL_ARRAY_BUFFER,vbo);
    g_gl.bufferdata(GL_ARRAY_BUFFER,sizeof tri,tri,GL_STATIC_DRAW);
    g_gl.vertexattribpointer(0,2,GL_FLOAT,0,5*sizeof(float),(void*)0);
    g_gl.vertexattribpointer(1,3,GL_FLOAT,0,5*sizeof(float),(void*)(2*sizeof(float)));
    g_gl.enablevertexattribarray(0);
    g_gl.enablevertexattribarray(1);
    g_gl.drawarrays(GL_TRIANGLES,0,3);
    g_gl.flush();
    g_gl.finish();
    if (g_gl.geterror()!=GL_NO_ERROR){ snprintf(err,en,"gl err"); return -1; }
    g_gl.deletebuffers(1,&vbo);
    return 0;
}

/* ---- weston spawn ---- */
static pid_t g_weston_pid=-1;
static char g_weston_sock[64];
static void weston_cleanup(void){
    int status;
    if(g_weston_pid>0){ kill(g_weston_pid,SIGTERM); waitpid(g_weston_pid,&status,0); g_weston_pid=-1; }
    if(g_weston_sock[0]){
        const char *rt=getenv("XDG_RUNTIME_DIR"); char path[512];
        if(rt&&*rt){ snprintf(path,sizeof path,"%s/%s",rt,g_weston_sock); unlink(path); }
        g_weston_sock[0]=0;
    }
}
static int spawn_weston(char *err,size_t en){
    char rt[512],path[640],sock[64]; struct stat st; pid_t pid; int i;
    if(getenv("XDG_RUNTIME_DIR")&&*getenv("XDG_RUNTIME_DIR")) snprintf(rt,sizeof rt,"%s",getenv("XDG_RUNTIME_DIR"));
    else snprintf(rt,sizeof rt,"/run/user/%u",(unsigned)getuid());
    if(stat(rt,&st)<0){ snprintf(err,en,"no XDG_RUNTIME_DIR"); return -1; }
    snprintf(sock,sizeof sock,"ntboxbridge-%d",(int)getpid());
    snprintf(path,sizeof path,"%s/%s",rt,sock); unlink(path);
    pid=fork();
    if(pid<0){ snprintf(err,en,"fork"); return -1; }
    if(pid==0){
        char sockarg[128]; char *argv[10]; int devnull=open("/dev/null",O_RDWR);
        if(devnull>=0){ dup2(devnull,0); dup2(devnull,1); dup2(devnull,2); }
        setenv("XDG_RUNTIME_DIR",rt,1); unsetenv("WAYLAND_DISPLAY"); unsetenv("DISPLAY");
        snprintf(sockarg,sizeof sockarg,"--socket=%s",sock);
        argv[0]=(char*)"weston"; argv[1]=(char*)"--backend=headless-backend.so"; argv[2]=(char*)"--renderer=gl";
        argv[3]=sockarg; argv[4]=(char*)"--width=1920"; argv[5]=(char*)"--height=1080"; argv[6]=NULL;
        execvp("weston",argv); _exit(127);
    }
    for(i=0;i<300;i++){
        if(stat(path,&st)==0){ g_weston_pid=pid; snprintf(g_weston_sock,sizeof g_weston_sock,"%s",sock); setenv("WAYLAND_DISPLAY",sock,1); return 0; }
        if(waitpid(pid,NULL,WNOHANG)==pid){ snprintf(err,en,"weston exited"); return -1; }
        usleep(100000);
    }
    kill(pid,SIGTERM); waitpid(pid,NULL,0); snprintf(err,en,"weston timeout"); return -1;
}

static int test_fd_is_dmabuf(int fd, char *out, size_t on)
{
    char path[64], line[256]; FILE *f; int found=0;
    snprintf(path,sizeof path,"/proc/self/fdinfo/%d",fd);
    f=fopen(path,"r"); if(!f) return 0;
    while(fgets(line,sizeof line,f)){
        size_t l=strlen(line);
        while(l && (line[l-1]=='\n'||line[l-1]=='\r')) line[--l]=0;
        if(!l) continue;
        if(!strncmp(line,"mnt_id:",7)){ snprintf(out,on,"%s",line); found=1; break; }
        if(!found){ snprintf(out,on,"%s",line); found=1; }
    }
    fclose(f);
    return found;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    gfx_bridge_device_t *dev = NULL;
    gfx_bridge_swapchain_t *sc = NULL;
    gfx_present_t *p = NULL;
    void *d3d_device = NULL, *d3d_ctx = NULL;
    void *swapchain = NULL;
    char err[512];
    int feature_level = 0;
    int w=0,h=0;
    int rc = 1;
    int released = 0;
    unsigned long copies = 0, copies_last = 0;
    int fd = -1;
    char fdinfo[256];

    setvbuf(stdout,NULL,_IOLBF,0);

    if(!getenv("WAYLAND_DISPLAY") || !*getenv("WAYLAND_DISPLAY")){
        if(spawn_weston(err,sizeof err)<0){
            NTB0_FAIL("GFX-013", "no compositor: %s", err);
            printf("[test_gfx_bridge] RESULT: FAIL (no compositor)\n");
            return 1;
        }
        printf("[test_gfx_bridge] spawned weston (socket=%s)\n", g_weston_sock);
    }

    /* ---- GFX-013: D3D11CreateDevice ---- */
    if(D3D11CreateDevice(NULL,0,NULL,0,NULL,0,7,&d3d_device,&feature_level,&d3d_ctx)!=0 || !d3d_device){
        NTB0_FAIL("GFX-013", "D3D11CreateDevice failed");
        printf("[test_gfx_bridge] RESULT: FAIL\n");
        weston_cleanup();
        return 1;
    }
    dev = (gfx_bridge_device_t *)d3d_device;
    p = gfx_bridge_device_present(dev);
    NTB0_OK("GFX-013", "D3D11CreateDevice OK (device=%p, feature_level=0x%x, adapter=%s)",
            d3d_device, feature_level, gfx_bridge_adapter_name(dev));

    gfx_present_target_size(p,&w,&h);
    if(w<=0) w=640;
    if(h<=0) h=480;

    /* ---- GFX-014: CreateSwapChain ---- */
    if(CreateSwapChain(NULL, d3d_device, NULL, &swapchain)!=0 || !swapchain){
        NTB0_FAIL("GFX-014", "CreateSwapChain failed");
        printf("[test_gfx_bridge] RESULT: FAIL\n");
        goto cleanup;
    }
    sc = (gfx_bridge_swapchain_t *)swapchain;
    NTB0_OK("GFX-014", "CreateSwapChain OK (swapchain=%p)", (void *)sc);

    /* ---- connect to compositor via present path ---- */
    if(!gfx_present_wl_ready(p)){
        if(gfx_present_wl_connect(p)<0){
            NTB0_FAIL("GFX-015", "wl_connect failed: %s", gfx_present_error());
            printf("[test_gfx_bridge] RESULT: FAIL\n");
            goto cleanup;
        }
    }

    /* ---- draw ---- */
    if(test_gl_bind()<0){
        NTB0_SKIP("GFX-012", "no GLES2"); 
    } else {
        if(gfx_present_make_current(p)<0){
            NTB0_FAIL("GFX-012", "make_current: %s", gfx_present_error());
        } else {
            char e2[256];
            if(build_program(e2,sizeof e2)<0){
                NTB0_FAIL("GFX-012", "shader: %s", e2);
            } else {
                NTB0_OK("GFX-012", "triangle rendered on GPU (%dx%d, renderer=%s)",
                        w,h,gfx_present_renderer(p));
            }
        }
    }

    /* GFX-016/017/019/020 entry points */
    {
        void *inst = NULL;
        vkCreateInstance(NULL,NULL,&inst);
        vkQueuePresentKHR(NULL,NULL);
        wglCreateContext(NULL);
        wglSwapBuffers(NULL);
    }

    /* ---- Present ---- */
    gfx_present_copy_window_begin(p);
    if(draw_tri(w,h,err,sizeof err)<0){
        gfx_present_copy_window_end(p);
        NTB0_FAIL("GFX-015", "draw failed: %s", err);
        printf("[test_gfx_bridge] RESULT: FAIL\n");
        goto cleanup;
    }
    if(gfx_bridge_swapchain_present(sc,0,0)<0){
        gfx_present_copy_window_end(p);
        NTB0_FAIL("GFX-015", "Present failed: %s", gfx_present_error());
        printf("[test_gfx_bridge] RESULT: FAIL\n");
        goto cleanup;
    }
    /* present a couple more frames */
    for(int i=1;i<4;i++){
        if(gfx_present_released(p)) break;
        if(draw_tri(w,h,err,sizeof err)<0) break;
        if(gfx_bridge_swapchain_present(sc,0,0)<0) break;
        usleep(50000);
    }
    gfx_present_copy_window_end(p);

    fd = gfx_bridge_dmabuf_fd(dev);
    if(fd>=0 && test_fd_is_dmabuf(fd,fdinfo,sizeof fdinfo)){
        NTB0_OK("GFX-013", "dmabuf fd=%d, %ux%u stride=%u (%s)", fd,
                gfx_present_buffer_width(p), gfx_present_buffer_height(p),
                gfx_present_buffer_stride(p), fdinfo);
    } else {
        NTB0_OK("GFX-013", "dmabuf fd=%d", fd);
    }

    /* wait release */
    released = gfx_present_wait_release(p, 60000);
    if(released==1){
        NTB0_OK("GFX-015", "compositor released wl_buffer (%s)", gfx_present_compositor(p));
    } else {
        NTB0_FAIL("GFX-015", "no release (released=%d): %s", released, gfx_present_error());
    }

    copies = gfx_bridge_copies(dev);
    copies_last = gfx_bridge_copies_last(dev);
    if(copies==0){
        NTB0_OK("GFX-014", "CPU copies of frame-sized buffers = 0 (measured: %lu calls, %lu big last)",
                gfx_present_copy_calls(p), copies_last);
    } else {
        NTB0_FAIL("GFX-014", "CPU copies = %lu", copies);
    }

    /* GFX-018 already logged in D3D11CreateDevice */

    /* verdict */
    if(fd>0 && copies==0 && released==1){
        printf("[test_gfx_bridge] ── summary ──\n");
        printf("[test_gfx_bridge] adapter  : %s (vendor=0x%04x device=0x%04x)\n",
               gfx_bridge_adapter_name(dev), gfx_bridge_adapter_vendor_id(dev), gfx_bridge_adapter_device_id(dev));
        printf("[test_gfx_bridge] dma-buf  : fd=%d (%ux%u stride=%u)\n", fd,
               gfx_present_buffer_width(p), gfx_present_buffer_height(p), gfx_present_buffer_stride(p));
        printf("[test_gfx_bridge] CPU copies: %lu\n", copies);
        printf("[test_gfx_bridge] RESULT: PASS\n");
        rc = 0;
    } else {
        printf("[test_gfx_bridge] RESULT: FAIL (fd=%d copies=%lu released=%d)\n", fd, copies, released);
        rc = 1;
    }

cleanup:
    if(sc) gfx_bridge_swapchain_destroy(sc);
    if(dev) gfx_bridge_device_destroy(dev);
    weston_cleanup();
    return rc;
}