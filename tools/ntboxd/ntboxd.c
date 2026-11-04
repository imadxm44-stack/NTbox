/*
 * ntboxd — minimal user-space PE loader (M6b stepping stone)
 * Usage: ntboxd <file.exe>
 * Loads a PE (x64) into user space, resolves kernel32/ntdll imports to local stubs, calls entry.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/syscall.h>
#include <linux/prctl.h> /* ARCH_SET_GS */
#include <asm/prctl.h>
#include <stdint.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "syscalls/ntb0_trap.h"   /* T-063: مصيدة syscalls */
#include "syscalls/ntb0_path.h"  /* T-063: جذر ويندوز للمترجم */
#include "pe_export.h"  /* T-063: تصديرات ntdll الحقيقي */
#include "ntboxd_libc.h" /* msvcrt: طبقة C صغيرة */
#include "dll_loader.h"  /* T-120: محمّل DLL الحقيقي */
#include "apiset.h"      /* T-127: عقد api-set */
#include "syswow64.h"    /* T-XXX: SysWOW64 / PE32 support */

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

/* Async I/O stubs for linker (weak) */
__attribute__((weak)) void ntb0_async_cancel(uint64_t handle) { (void)handle; }
__attribute__((weak)) long ntb0_async_submit_read_ex(uint64_t handle, uint64_t event_handle, void *buf, size_t len, uint64_t apc_context, void *cb, void *ctx) {
    (void)handle; (void)event_handle; (void)buf; (void)len; (void)apc_context; (void)cb; (void)ctx;
    return 0;
}
__attribute__((weak)) long ntb0_async_submit_write_ex(uint64_t handle, uint64_t event_handle, const void *buf, size_t len, uint64_t apc_context, void *cb, void *ctx) {
    (void)handle; (void)event_handle; (void)buf; (void)len; (void)apc_context; (void)cb; (void)ctx;
    return 0;
}

/* ============ M7: Wayland framebuffer (shared with gdi32/user32 stubs) ============ */
#define NTBOXD_WL_FB_W 640
#define NTBOXD_WL_FB_H 480
#define NTBOXD_WL_FB_SIZE (NTBOXD_WL_FB_W * NTBOXD_WL_FB_H * 4)

static uint32_t ntboxd_wl_fb[NTBOXD_WL_FB_W * NTBOXD_WL_FB_H];
static int ntboxd_wl_fb_dirty = 0;
static pthread_mutex_t ntboxd_wl_fb_lock = PTHREAD_MUTEX_INITIALIZER;

/* Wayland objects (created on first ShowWindow) */
static struct wl_display *wl_display;
static struct wl_compositor *wl_compositor;
static struct wl_shm *wl_shm;
static struct xdg_wm_base *wl_xdg_wm_base;
static struct wl_surface *wl_surface;
static struct xdg_surface *wl_xdg_surface;
static struct xdg_toplevel *wl_xdg_toplevel;
static struct wl_buffer *wl_buffer;
static int wl_configured = 0;
static pthread_t wl_thread;
static int wl_running = 0;
static int wl_width = NTBOXD_WL_FB_W;
static int wl_height = NTBOXD_WL_FB_H;

static void wl_xdg_surface_configure(void *data, struct xdg_surface *s, uint32_t serial) {
    xdg_surface_ack_configure(s, serial);
    wl_configured = 1;
}
static const struct xdg_surface_listener wl_xdg_surface_listener = { .configure = wl_xdg_surface_configure };

static void wl_xdg_toplevel_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) {
    if (w > 0 && h > 0) { wl_width = w; wl_height = h; }
}
static void wl_xdg_toplevel_close(void *data, struct xdg_toplevel *t) { wl_running = 0; }
static const struct xdg_toplevel_listener wl_xdg_toplevel_listener = {
    .configure = wl_xdg_toplevel_configure, .close = wl_xdg_toplevel_close
};

static void wl_xdg_wm_base_ping(void *data, struct xdg_wm_base *wm, uint32_t serial) {
    xdg_wm_base_pong(wm, serial);
}
static const struct xdg_wm_base_listener wl_xdg_wm_base_listener = { .ping = wl_xdg_wm_base_ping };

static void wl_registry_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver) {
    if (strcmp(iface, "wl_compositor") == 0)
        wl_compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    else if (strcmp(iface, "wl_shm") == 0)
        wl_shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (strcmp(iface, "xdg_wm_base") == 0) {
        wl_xdg_wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(wl_xdg_wm_base, &wl_xdg_wm_base_listener, NULL);
    }
}
static const struct wl_registry_listener wl_registry_listener = { .global = wl_registry_global };

static void *wl_thread_func(void *arg) {
    (void)arg;
    wl_display = wl_display_connect(NULL);
    if (!wl_display) { fprintf(stderr, "[wayland] cannot connect to display\n"); return NULL; }
    struct wl_registry *registry = wl_display_get_registry(wl_display);
    wl_registry_add_listener(registry, &wl_registry_listener, NULL);
    wl_display_roundtrip(wl_display);
    if (!wl_compositor || !wl_shm || !wl_xdg_wm_base) {
        fprintf(stderr, "[wayland] missing globals\n");
        return NULL;
    }

    wl_surface = wl_compositor_create_surface(wl_compositor);
    wl_xdg_surface = xdg_wm_base_get_xdg_surface(wl_xdg_wm_base, wl_surface);
    xdg_surface_add_listener(wl_xdg_surface, &wl_xdg_surface_listener, NULL);
    wl_xdg_toplevel = xdg_surface_get_toplevel(wl_xdg_surface);
    xdg_toplevel_add_listener(wl_xdg_toplevel, &wl_xdg_toplevel_listener, NULL);
    xdg_toplevel_set_title(wl_xdg_toplevel, "NT-Box M7");
    wl_surface_commit(wl_surface);

    /* create shm buffer */
    int fd = memfd_create("ntboxwlfb", MFD_CLOEXEC);
    ftruncate(fd, NTBOXD_WL_FB_SIZE);
    void *shm_data = mmap(NULL, NTBOXD_WL_FB_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    struct wl_shm_pool *pool = wl_shm_create_pool(wl_shm, fd, NTBOXD_WL_FB_SIZE);
    wl_buffer = wl_shm_pool_create_buffer(pool, 0, wl_width, wl_height, wl_width*4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    while (!wl_configured) wl_display_dispatch(wl_display);

    wl_surface_attach(wl_surface, wl_buffer, 0, 0);
    wl_surface_damage(wl_surface, 0, 0, wl_width, wl_height);
    wl_surface_commit(wl_surface);
    wl_display_flush(wl_display);

    while (wl_running) {
        pthread_mutex_lock(&ntboxd_wl_fb_lock);
        if (ntboxd_wl_fb_dirty) {
            memcpy(shm_data, ntboxd_wl_fb, NTBOXD_WL_FB_SIZE);
            ntboxd_wl_fb_dirty = 0;
        }
        pthread_mutex_unlock(&ntboxd_wl_fb_lock);

        wl_surface_attach(wl_surface, wl_buffer, 0, 0);
        wl_surface_damage(wl_surface, 0, 0, wl_width, wl_height);
        wl_surface_commit(wl_surface);
        wl_display_flush(wl_display);

        /* poll for events non-blocking */
        while (wl_display_dispatch_pending(wl_display) > 0) {}
        wl_display_flush(wl_display);
        usleep(16666); /* ~60fps */
    }
    return NULL;
}

static int wl_init(void) {
    if (wl_running) return 0;
    wl_running = 1;
    if (pthread_create(&wl_thread, NULL, wl_thread_func, NULL) != 0) {
        wl_running = 0;
        return -1;
    }
    return 0;
}

static void wl_render_dirty(void) {
    pthread_mutex_lock(&ntboxd_wl_fb_lock);
    ntboxd_wl_fb_dirty = 1;
    pthread_mutex_unlock(&ntboxd_wl_fb_lock);
}

/* ---- local stubs (user-mode) ---- */
static void * __attribute__((ms_abi)) ntboxd_GetStdHandle(unsigned long n) {
    if (n == (unsigned long)-10) return (void*)0; /* STD_INPUT_HANDLE */
    if (n == (unsigned long)-11) return (void*)1; /* STD_OUTPUT_HANDLE */
    if (n == (unsigned long)-12) return (void*)2; /* STD_ERROR_HANDLE */
    return (void*)1;
}
static int __attribute__((ms_abi)) ntboxd_WriteFile(void *h, const void *buf, unsigned long n, unsigned long *written, void *ov) {
    (void)ov;
    ssize_t r = write((int)(long)h, buf, n);
    if (r < 0) r = 0;
    if (written) *written = (unsigned long)r;
    if (buf && n && (int)(long)h == 1) { /* ensure stdout visible */
        /* already written via write() */
    }
    return 1;
}
static int __attribute__((ms_abi)) ntboxd_ReadFile(void *h, void *buf, unsigned long n, unsigned long *readn, void *ov) {
    (void)ov;
    ssize_t r = read((int)(long)h, buf, n);
    if (r < 0) r = 0;
    if (readn) *readn = (unsigned long)r;
    return 1;
}
static void * __attribute__((ms_abi)) ntboxd_VirtualAlloc(void *addr, size_t size, unsigned long type, unsigned long protect) {
    (void)type; (void)protect;
    size_t psz = (size + 4095) & ~((size_t)4095);
    if (psz == 0) psz = 4096;
    void *p = mmap(addr, psz, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    return p;
}
static int __attribute__((ms_abi)) ntboxd_VirtualFree(void *addr, size_t size, unsigned long type) {
    (void)type;
    size_t psz = size ? ((size + 4095) & ~((size_t)4095)) : 4096;
    if (psz == 0) psz = 4096;
    if (munmap(addr, psz) != 0) return 0;
    return 1;
}
static void * __attribute__((ms_abi)) ntboxd_HeapAlloc(void *heap, unsigned long flags, size_t size) {
    (void)heap; (void)flags;
    return calloc(1, size ? size : 1);
}
static int __attribute__((ms_abi)) ntboxd_HeapFree(void *heap, unsigned long flags, void *ptr) {
    (void)heap; (void)flags;
    free(ptr);
    return 1;
}

typedef unsigned long (__attribute__((ms_abi)) *NTBOXD_THREADPROC)(void *);

typedef struct { NTBOXD_THREADPROC start; void *param; } ntboxd_thread_ctx_t;

static _Thread_local void *ntboxd_current_teb = NULL;

typedef struct _NTBOXD_PEB {
    unsigned char InheritedAddressSpace;
    unsigned char ReadImageFileExecOptions;
    unsigned char BeingDebugged;
    unsigned char BitField;
    void *Mutant;
    void *ImageBaseAddress;
    void *Ldr;
    void *ProcessParameters;
    void *SubSystemData;
    void *ProcessHeap;
} NTBOXD_PEB;

typedef struct _NTBOXD_TEB {
    void *NtTibExceptionList;
    void *NtTibStackBase;
    void *NtTibStackLimit;
    void *NtTibSubSystemTib;
    void *NtTibFiberData;
    void *NtTibArbitraryUserPointer;
    struct _NTBOXD_TEB *NtTibSelf;
    void *EnvironmentPointer;
    void *ClientIdUniqueProcess;
    void *ClientIdUniqueThread;
    void *RpcHandle;
    void *ThreadLocalStoragePointer;
    NTBOXD_PEB *ProcessEnvironmentBlock;
    /* TLS slots appended dynamically */
} NTBOXD_TEB;

static void *ntboxd_alloc_teb(void) {
    NTBOXD_PEB *peb = calloc(1, sizeof(NTBOXD_PEB));
    NTBOXD_TEB *teb = calloc(1, sizeof(NTBOXD_TEB));
    if (!peb || !teb) { free(peb); free(teb); return NULL; }
    teb->NtTibSelf = teb;
    teb->ProcessEnvironmentBlock = peb;
    /* allocate TLS array */
    teb->ThreadLocalStoragePointer = calloc(64, sizeof(void *));
    return teb;
}

static void ntboxd_thread_wrapper_set_teb(void *teb) {
    ntboxd_current_teb = teb;
    syscall(SYS_arch_prctl, ARCH_SET_GS, (unsigned long)teb);
}

static void *ntboxd_thread_wrapper(void *p) {
    ntboxd_thread_ctx_t *c = (ntboxd_thread_ctx_t *)p;
    NTBOXD_THREADPROC start = c->start;
    void *param = c->param;
    void *teb = ntboxd_alloc_teb();
    if (teb) ntboxd_thread_wrapper_set_teb(teb);
    unsigned long r = 0;
    if (start) r = start(param);
    free(c);
    /* keep teb until thread exit */
    return (void *)(unsigned long)r;
}

static void * __attribute__((ms_abi)) ntboxd_CreateThread(void *sec, size_t stack, void *start, void *param, unsigned long flags, unsigned long *tid) {
    (void)sec; (void)stack; (void)flags;
    ntboxd_thread_ctx_t *c = calloc(1, sizeof(*c));
    c->start = (NTBOXD_THREADPROC)start;
    c->param = param;
    pthread_t th;
    if (pthread_create(&th, NULL, ntboxd_thread_wrapper, c) != 0) {
        free(c);
        return NULL;
    }
    if (tid) *tid = (unsigned long)th;
    return (void *)th;
}

static unsigned int __attribute__((ms_abi)) ntboxd_GetLastError(void) { return 0; }
static void __attribute__((ms_abi)) ntboxd_SetLastError(unsigned int e) { (void)e; }
static void __attribute__((ms_abi)) ntboxd_Sleep(unsigned long ms) { usleep(ms * 1000); }

/* ---- M6c: critical sections ---- */
typedef struct {
    void *DebugInfo;
    long LockCount;
    long RecursionCount;
    void *OwningThread;
    void *LockSemaphore;
    unsigned long SpinCount;
} ntboxd_critsec_t;

static void __attribute__((ms_abi)) ntboxd_InitializeCriticalSection(void *cs) {
    if (!cs) return;
    memset(cs, 0, sizeof(ntboxd_critsec_t));
    pthread_mutex_t *m = calloc(1, sizeof(pthread_mutex_t));
    if (!m) return;
    pthread_mutex_init(m, NULL);
    ((ntboxd_critsec_t *)cs)->DebugInfo = m;
    ((ntboxd_critsec_t *)cs)->LockCount = -1;
}

static void __attribute__((ms_abi)) ntboxd_DeleteCriticalSection(void *cs) {
    if (!cs) return;
    pthread_mutex_t *m = (pthread_mutex_t *)((ntboxd_critsec_t *)cs)->DebugInfo;
    if (m) {
        pthread_mutex_destroy(m);
        free(m);
    }
    memset(cs, 0, sizeof(ntboxd_critsec_t));
}

static void __attribute__((ms_abi)) ntboxd_EnterCriticalSection(void *cs) {
    if (!cs) return;
    pthread_mutex_t *m = (pthread_mutex_t *)((ntboxd_critsec_t *)cs)->DebugInfo;
    if (m) pthread_mutex_lock(m);
}

static void __attribute__((ms_abi)) ntboxd_LeaveCriticalSection(void *cs) {
    if (!cs) return;
    pthread_mutex_t *m = (pthread_mutex_t *)((ntboxd_critsec_t *)cs)->DebugInfo;
    if (m) pthread_mutex_unlock(m);
}

static unsigned long __attribute__((ms_abi)) ntboxd_TryEnterCriticalSection(void *cs) {
    if (!cs) return 1;
    pthread_mutex_t *m = (pthread_mutex_t *)((ntboxd_critsec_t *)cs)->DebugInfo;
    if (!m) return 1;
    return (pthread_mutex_trylock(m) == 0) ? 1 : 0;
}

/* ---- M6c: TLS ---- */
static unsigned long ntboxd_tls_next_slot = 0;
static pthread_mutex_t ntboxd_tls_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned long __attribute__((ms_abi)) ntboxd_TlsAlloc(void) {
    pthread_mutex_lock(&ntboxd_tls_lock);
    unsigned long idx = ntboxd_tls_next_slot++;
    pthread_mutex_unlock(&ntboxd_tls_lock);
    return idx;
}

static unsigned long __attribute__((ms_abi)) ntboxd_TlsFree(unsigned long idx) {
    (void)idx;
    return 1;
}

static void * __attribute__((ms_abi)) ntboxd_TlsGetValue(unsigned long idx) {
    NTBOXD_TEB *teb = (NTBOXD_TEB *)ntboxd_current_teb;
    if (!teb || !teb->ThreadLocalStoragePointer) return NULL;
    void **arr = (void **)teb->ThreadLocalStoragePointer;
    if (idx >= 64) return NULL;
    return arr[idx];
}

static unsigned long __attribute__((ms_abi)) ntboxd_TlsSetValue(unsigned long idx, void *val) {
    NTBOXD_TEB *teb = (NTBOXD_TEB *)ntboxd_current_teb;
    if (!teb) return 0;
    if (!teb->ThreadLocalStoragePointer)
        teb->ThreadLocalStoragePointer = calloc(64, sizeof(void *));
    void **arr = (void **)teb->ThreadLocalStoragePointer;
    if (!arr || idx >= 64) return 0;
    arr[idx] = val;
    return 1;
}

/* ---- M6c: VirtualProtect / VirtualQuery ---- */
static unsigned long __attribute__((ms_abi)) ntboxd_VirtualProtect(void *addr, size_t size, unsigned long newProtect, unsigned long *oldProtect) {
    (void)newProtect;
    if (oldProtect) *oldProtect = 0x40; /* PAGE_EXECUTE_READWRITE */
    size_t ps = 4096;
    void *a = (void *)((unsigned long)addr & ~(ps - 1));
    size_t l = size + ((unsigned long)addr - (unsigned long)a);
    size_t al = (l + ps - 1) & ~(ps - 1);
    if (al == 0) al = ps;
    mprotect(a, al, PROT_READ | PROT_WRITE | PROT_EXEC);
    return 1;
}

typedef struct {
    void *BaseAddress;
    void *AllocationBase;
    unsigned long AllocationProtect;
    unsigned long _pad;
    size_t RegionSize;
    unsigned long State;
    unsigned long Protect;
    unsigned long Type;
} ntboxd_mbi_t;

static size_t __attribute__((ms_abi)) ntboxd_VirtualQuery(void *addr, void *buf, size_t len) {
    if (!buf || len < sizeof(ntboxd_mbi_t)) return 0;
    ntboxd_mbi_t *mbi = (ntboxd_mbi_t *)buf;
    memset(mbi, 0, sizeof(*mbi));
    mbi->BaseAddress = addr;
    mbi->AllocationBase = addr;
    mbi->AllocationProtect = 0x40;
    mbi->RegionSize = 4096;
    mbi->State = 0x1000; /* MEM_COMMIT */
    mbi->Protect = 0x40;
    mbi->Type = 0x20000; /* MEM_PRIVATE */
    return sizeof(*mbi);
}

/* ---- M6c: string conversion ---- */
static int __attribute__((ms_abi)) ntboxd_MultiByteToWideChar(unsigned int cp, unsigned long flags, const char *src, int cb, unsigned short *dst, int cch) {
    (void)cp; (void)flags;
    if (cb < 0) {
        cb = 0; if (src) while (src[cb]) cb++;
    }
    if (!dst || cch == 0) return cb;
    int n = 0;
    for (n = 0; n < cb && n < cch - 1; n++) {
        unsigned char ch = src ? (unsigned char)src[n] : 0;
        dst[n] = (ch < 0x80) ? ch : (unsigned short)'?';
    }
    if (cch > 0) dst[n < cch ? n : cch - 1] = 0;
    return n;
}

static int __attribute__((ms_abi)) ntboxd_WideCharToMultiByte(unsigned int cp, unsigned long flags, const unsigned short *src, int cch, char *dst, int cb, const char *def, int *used) {
    (void)cp; (void)flags; (void)def;
    if (used) *used = 0;
    if (cch < 0) {
        cch = 0; if (src) while (src[cch]) cch++;
    }
    if (!dst || cb == 0) return cch;
    int n = 0;
    for (n = 0; n < cch && n < cb - 1; n++) {
        unsigned int ch = src ? src[n] : 0;
        dst[n] = (ch < 0x80) ? (char)ch : '?';
    }
    if (cb > 0) dst[n < cb ? n : cb - 1] = 0;
    return n;
}

static unsigned long __attribute__((ms_abi)) ntboxd_IsDBCSLeadByteEx(unsigned int cp, unsigned char c) {
    (void)cp; (void)c;
    return 0;
}

/* ---- M6c: misc ---- */
typedef long (__attribute__((ms_abi)) *ntboxd_uef_t)(void *);
static ntboxd_uef_t ntboxd_uef = NULL;
static ntboxd_uef_t __attribute__((ms_abi)) ntboxd_SetUnhandledExceptionFilter(ntboxd_uef_t f) {
    ntboxd_uef_t prev = ntboxd_uef;
    ntboxd_uef = f;
    return prev;
}

static unsigned long __attribute__((ms_abi)) ntboxd_GetExitCodeThread(void *h, unsigned long *code) {
    if (code) *code = 0;
    (void)h;
    return 1;
}

/* ============ M6c: user32.dll stubs (M7 window foundation) ============ */

#define NTBOXD_HWND_MAGIC  0x57494E44UL   /* "WIND" */
#define NTBOXD_HDC_MAGIC   0x44434844UL   /* "DCHD" */
#define NTBOXD_HBITMAP_MAGIC 0x424D4948UL /* "HBMB" */

typedef struct {
    unsigned long magic;
    int width, height;
    const char *title;
    int shown;
} ntboxd_hwnd_t;

typedef struct {
    unsigned long magic;
    int width, height;
} ntboxd_hdc_t;

typedef struct {
    unsigned long magic;
    int width, height;
} ntboxd_hbitmap_t;

static void * __attribute__((ms_abi)) ntboxd_MessageBoxA(void *hWnd, const char *text, const char *caption, unsigned long type) {
    (void)hWnd; (void)type;
    fprintf(stderr, "[user32] MessageBox: %s%s%s\n",
            caption ? caption : "", caption ? " - " : "", text ? text : "");
    return (void *)1; /* IDOK */
}
static void * __attribute__((ms_abi)) ntboxd_DefWindowProcA(void *hwnd, unsigned int msg, unsigned long wp, unsigned long lp) {
    (void)hwnd; (void)msg; (void)wp; (void)lp;
    return (void *)0;
}
static unsigned long __attribute__((ms_abi)) ntboxd_RegisterClassA(const void *wndclass) {
    (void)wndclass;
    return 1; /* atom */
}
/* ------------------------------------------------------------------
 * W-variants (stubs خرجت من api_stubs.c بمسارها؛ المحوّل UTF-16→ASCII
 * داخلها. نعلن هنا للربط في resolve()). جميعها ms_abi.
 * ------------------------------------------------------------------ */
extern unsigned int __attribute__((ms_abi)) ntboxd_RegisterClassW(const void *wc);
extern void * __attribute__((ms_abi)) ntboxd_CreateWindowExW(
        unsigned long exstyle, const void *classname, const void *title,
        unsigned long style, int x, int y, int nw, int nh,
        void *parent, void *menu, void *hinst, void *param);
extern long __attribute__((ms_abi)) ntboxd_DefWindowProcW(
        void *hwnd, unsigned int msg, unsigned long wp, long lp);
extern unsigned int __attribute__((ms_abi)) ntboxd_MessageBoxW(
        void *h, const void *text, const void *caption, unsigned int type);
static void * __attribute__((ms_abi)) ntboxd_CreateWindowExA(unsigned long exstyle, const char *classname, const char *title,
                                  unsigned long style, int x, int y, int nw, int nh,
                                  void *parent, void *menu, void *hinst, void *param) {
    (void)exstyle; (void)classname; (void)style; (void)x; (void)y; (void)parent;
    (void)menu; (void)hinst; (void)param;
    ntboxd_hwnd_t *win = calloc(1, sizeof(*win));
    if (!win) return NULL;
    win->magic = NTBOXD_HWND_MAGIC;
    win->width = nw > 0 ? nw : 640;
    win->height = nh > 0 ? nh : 480;
    win->title = title ? title : "NT-Box Window";
    win->shown = 0;
    fprintf(stderr, "[user32] CreateWindowEx: '%s' %dx%d\n", win->title, win->width, win->height);
    return (void *)win;
}
static int __attribute__((ms_abi)) ntboxd_ShowWindow(void *hwnd, int nCmdShow) {
    ntboxd_hwnd_t *w = (ntboxd_hwnd_t *)hwnd;
    if (!w || w->magic != NTBOXD_HWND_MAGIC) return 0;
    w->shown = 1;
    fprintf(stderr, "[user32] ShowWindow: %s (cmd=%d)\n", w->title, nCmdShow);
    if (nCmdShow == 5 /* SW_SHOW */ && wl_init() != 0)
        fprintf(stderr, "[user32] Wayland init failed\n");
    return 1;
}
static int __attribute__((ms_abi)) ntboxd_UpdateWindow(void *hwnd) {
    (void)hwnd;
    return 1;
}
static int __attribute__((ms_abi)) ntboxd_GetMessageA(void *msg, void *hwnd, unsigned int min, unsigned int max) {
    (void)msg; (void)hwnd; (void)min; (void)max;
    usleep(100000); /* 100ms poll */
    return 0; /* WM_QUIT -> stop loop */
}
static int __attribute__((ms_abi)) ntboxd_TranslateMessage(const void *msg) {
    (void)msg;
    return 1;
}
static unsigned long __attribute__((ms_abi)) ntboxd_DispatchMessageA(const void *msg) {
    (void)msg;
    return 0;
}
static void __attribute__((ms_abi)) ntboxd_PostQuitMessage(int nExitCode) {
    (void)nExitCode;
    fprintf(stderr, "[user32] PostQuitMessage(%d)\n", nExitCode);
}
typedef struct {
    void *hdc;
    int fErase;
    int rcPaint[4];
    int fRestore;
    int fIncUpdate;
    unsigned char rgbReserved[32];
} NTBOXD_PAINTSTRUCT;

static long __attribute__((ms_abi)) ntboxd_GetWindowTextA(void *hwnd, char *lpString, int nMaxCount) {
    if (!lpString || nMaxCount <= 0) return 0;
    ntboxd_hwnd_t *w = (ntboxd_hwnd_t *)hwnd;
    const char *t = (w && w->magic == NTBOXD_HWND_MAGIC && w->title) ? w->title : "";
    int i = 0;
    for (; i < nMaxCount - 1 && t[i]; i++) lpString[i] = t[i];
    if (nMaxCount > 0) lpString[i] = ' ';
    return i;
}
static int __attribute__((ms_abi)) ntboxd_SetWindowTextW(void *hwnd, const void *lpString) {
    (void)hwnd; (void)lpString;
    return 1;
}
static int __attribute__((ms_abi)) ntboxd_SetWindowTextA(void *hwnd, const char *lpString) {
    (void)hwnd; (void)lpString;
    return 1;
}

static void * __attribute__((ms_abi)) ntboxd_BeginPaint(void *hwnd, void *lpPaint) {
    ntboxd_hwnd_t *w = (ntboxd_hwnd_t *)hwnd;
    if (!w || w->magic != NTBOXD_HWND_MAGIC) return NULL;
    if (lpPaint) {
        NTBOXD_PAINTSTRUCT *ps = (NTBOXD_PAINTSTRUCT *)lpPaint;
        memset(ps, 0, sizeof(*ps));
        ps->hdc = ntboxd_GetDC(hwnd);
        ps->rcPaint[0]=0; ps->rcPaint[1]=0; ps->rcPaint[2]=w->width; ps->rcPaint[3]=w->height;
    }
    return ntboxd_GetDC(hwnd);
}
static int __attribute__((ms_abi)) ntboxd_EndPaint(void *hwnd, void *lpPaint) {
    (void)lpPaint;
    return 1;
}
static int __attribute__((ms_abi)) ntboxd_GetWindowTextW(void *hwnd, void *lpString, int nMaxCount) {
    ntboxd_hwnd_t *w = (ntboxd_hwnd_t *)hwnd;
    if (!lpString || nMaxCount <= 0) return 0;
    unsigned short *ws = (unsigned short *)lpString;
    const char *t = (w && w->magic == NTBOXD_HWND_MAGIC && w->title) ? w->title : "";
    int i = 0;
    for (; i < nMaxCount - 1 && t[i]; i++) ws[i] = (unsigned char)t[i];
    if (nMaxCount > 0) ws[i] = 0;
    return i;
}
static unsigned long __attribute__((ms_abi)) ntboxd_SetTimer(void *hwnd, unsigned long nIDEvent, unsigned int uElapse, void *lpTimerFunc) {
    (void)hwnd; (void)nIDEvent; (void)lpTimerFunc;
    return nIDEvent ? nIDEvent : 1;
}
static int __attribute__((ms_abi)) ntboxd_KillTimer(void *hwnd, unsigned long uIDEvent) {
    (void)hwnd; (void)uIDEvent;
    return 1;
}
static int __attribute__((ms_abi)) ntboxd_FillRect(void *hdc, const void *lprc, void *hbr) {
    (void)hdc; (void)lprc; (void)hbr;
    return 1;
}

static int __attribute__((ms_abi)) ntboxd_GetClientRect(void *hwnd, void *rect) {
    ntboxd_hwnd_t *w = (ntboxd_hwnd_t *)hwnd;
    if (!w || w->magic != NTBOXD_HWND_MAGIC || !rect) return 0;
    int *r = (int *)rect;
    r[0]=0; r[1]=0; r[2]=w->width; r[3]=w->height;
    return 1;
}
static void * __attribute__((ms_abi)) ntboxd_GetDC(void *hwnd) {
    ntboxd_hwnd_t *w = (ntboxd_hwnd_t *)hwnd;
    if (!w || w->magic != NTBOXD_HWND_MAGIC) return NULL;
    ntboxd_hdc_t *dc = calloc(1, sizeof(*dc));
    if (!dc) return NULL;
    dc->magic = NTBOXD_HDC_MAGIC;
    dc->width = w->width;
    dc->height = w->height;
    return (void *)dc;
}
static int __attribute__((ms_abi)) ntboxd_ReleaseDC(void *hwnd, void *dc) {
    (void)hwnd;
    if (dc) free(dc);
    return 1;
}

/* ============ M6c: gdi32.dll stubs ============ */

/*
 * سجل كائنات GDI: كل كائن تحجزه الـ stubs يُسجّل هنا، و DeleteObject
 * لا يستدعي free() إلا لما هو في السجل — المؤشرات الضيفية أو المقبضات
 * المرميّة (sentinels) لا تُحرر أبداً (منع غشّ الكومي).
 */
static void *g_gdi_objs[256];
static int g_gdi_register(void *p) {
    int i;
    for (i = 0; i < 256; i++)
        if (!g_gdi_objs[i]) { g_gdi_objs[i] = p; return 1; }
    return 0;
}
static int g_gdi_unregister(void *p) {
    int i;
    for (i = 0; i < 256; i++)
        if (g_gdi_objs[i] == p) { g_gdi_objs[i] = NULL; return 1; }
    return 0;
}

static void * __attribute__((ms_abi)) ntboxd_CreateCompatibleDC(void *dc) {
    (void)dc;
    ntboxd_hdc_t *cdc = calloc(1, sizeof(*cdc));
    if (!cdc) return NULL;
    cdc->magic = NTBOXD_HDC_MAGIC;
    cdc->width = 640; cdc->height = 480;
    g_gdi_register(cdc);
    return (void *)cdc;
}
static void * __attribute__((ms_abi)) ntboxd_CreateCompatibleBitmap(void *dc, int w, int h) {
    (void)dc;
    ntboxd_hbitmap_t *bm = calloc(1, sizeof(*bm));
    if (!bm) return NULL;
    bm->magic = NTBOXD_HBITMAP_MAGIC;
    bm->width = w > 0 ? w : 640;
    bm->height = h > 0 ? h : 480;
    g_gdi_register(bm);
    return (void *)bm;
}
/* CreatePen / CreateSolidBrush — أجسام قلم/فرشاة صغيرة بـ magic خاص
 * (تُستخدم بمرور القلم ضيفيًا إلى SelectObject/DeleteObject بأمان) */
typedef struct { unsigned long magic; int style; int width; unsigned long color; } ntboxd_hpen_t;
#define NTBOXD_HPEN_MAGIC   0x50454E31UL  /* "PEN1" */
#define NTBOXD_HBRUSH_MAGIC 0x42525533UL  /* "BRU3" */
static void * __attribute__((ms_abi)) ntboxd_CreatePen(int style, int width, unsigned long color) {
    ntboxd_hpen_t *p = calloc(1, sizeof(*p));
    (void)style; (void)width; (void)color;
    if (!p) return NULL;
    p->magic = NTBOXD_HPEN_MAGIC;
    p->style = style; p->width = width; p->color = color;
    fprintf(stderr, "[gdi32] CreatePen: style=%d w=%d color=0x%lx\n", style, width, color);
    if (!g_gdi_register(p)) { free(p); p = NULL; }
    return (void *)p;
}
static void * __attribute__((ms_abi)) ntboxd_CreateSolidBrush(unsigned long color) {
    ntboxd_hpen_t *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->magic = NTBOXD_HBRUSH_MAGIC;
    b->color = color;
    fprintf(stderr, "[gdi32] CreateSolidBrush: 0x%lx\n", color);
    if (!g_gdi_register(b)) { free(b); b = NULL; }
    return (void *)b;
}
static void * __attribute__((ms_abi)) ntboxd_SelectObject(void *dc, void *obj) {
    (void)dc;
    /* return previous object (dummy) */
    return obj;
}
static int __attribute__((ms_abi)) ntboxd_DeleteDC(void *dc) {
    if (dc) free(dc);
    return 1;
}
static int __attribute__((ms_abi)) ntboxd_DeleteObject(void *obj) {
    /*
     * آمن: نحرر فقط ما أنشأته صناديق إنشاء GDI وسجلها. المؤشرات الضيفية
     * (مقبضات حقيقية، sentinels) لا تُمس — إجراءً أي فراغ لغير مؤشر ours
     * كان سيُفسد حجر كومة المُضيف عند وصول قلم/فرشاة من heap الضيف.
     */
    if (obj && g_gdi_unregister(obj))
        free(obj);
    return 1;
}
static int __attribute__((ms_abi)) ntboxd_BitBlt(void *destDC, int dx, int dy, int dw, int dh,
                                                  void *srcDC, int sx, int sy, unsigned long rop) {
    (void)destDC; (void)dx; (void)dy; (void)dw; (void)dh;
    (void)srcDC; (void)sx; (void)sy; (void)rop;
    return 1;
}
static void wl_putpixel(int x, int y, uint32_t color) {
    if (x < 0 || x >= NTBOXD_WL_FB_W || y < 0 || y >= NTBOXD_WL_FB_H) return;
    ntboxd_wl_fb[y * NTBOXD_WL_FB_W + x] = color;
}

static int __attribute__((ms_abi)) ntboxd_Rectangle(void *dc, int left, int top, int right, int bottom) {
    (void)dc;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > NTBOXD_WL_FB_W) right = NTBOXD_WL_FB_W;
    if (bottom > NTBOXD_WL_FB_H) bottom = NTBOXD_WL_FB_H;
    uint32_t color = 0xFFFFFFFF; /* white */
    for (int x = left; x < right; x++) { wl_putpixel(x, top, color); wl_putpixel(x, bottom-1, color); }
    for (int y = top; y < bottom; y++) { wl_putpixel(left, y, color); wl_putpixel(right-1, y, color); }
    fprintf(stderr, "[gdi32] Rectangle: %d,%d -> %d,%d\n", left, top, right, bottom);
    wl_render_dirty();
    return 1;
}
static int __attribute__((ms_abi)) ntboxd_TextOutA(void *dc, int x, int y, const char *str, int count) {
    (void)dc;
    if (!str) return 0;
    int n = count > 0 ? count : (int)strlen(str);
    uint32_t color = 0xFFFF0000; /* red */
    for (int i = 0; i < n; i++) {
        char c = str[i];
        /* simple 5x7 bitmap font for A-Z, a-z, 0-9, space */
        static const unsigned char font[95][5] = {
            {0x00,0x00,0x00,0x00,0x00}, /* space */
            {0x00,0x00,0x2f,0x00,0x00}, /* ! */
            {0x00,0x07,0x00,0x07,0x00}, /* " */
            {0x14,0x7e,0x14,0x7e,0x14}, /* # */
            {0x00,0x24,0x2e,0x2a,0x12}, /* $ */
            {0x00,0x42,0x4e,0x4a,0x24}, /* % */
            {0x3c,0x52,0x52,0x52,0x3c}, /* ( */
            {0x3c,0x4a,0x4a,0x4a,0x3c}, /* ) */
            {0x00,0x00,0x7e,0x00,0x00}, /* * */
            {0x00,0x08,0x3e,0x08,0x00}, /* + */
            {0x00,0x00,0x00,0x60,0x20}, /* , */
            {0x00,0x00,0x3e,0x00,0x00}, /* - */
            {0x00,0x00,0x00,0x00,0x00}, /* . */
            {0x00,0x40,0x40,0x20,0x00}, /* / */
            {0x3e,0x41,0x49,0x49,0x3e}, /* 0 */
            {0x00,0x00,0x41,0x7f,0x40}, /* 1 */
            {0x42,0x61,0x51,0x49,0x46}, /* 2 */
            {0x22,0x41,0x49,0x49,0x36}, /* 3 */
            {0x18,0x14,0x12,0x7f,0x10}, /* 4 */
            {0x27,0x49,0x49,0x49,0x31}, /* 5 */
            {0x3e,0x49,0x49,0x49,0x32}, /* 6 */
            {0x01,0x71,0x09,0x05,0x03}, /* 7 */
            {0x36,0x49,0x49,0x49,0x36}, /* 8 */
            {0x26,0x49,0x49,0x49,0x3e}, /* 9 */
            {0x00,0x36,0x00,0x00,0x00}, /* : */
            {0x00,0x36,0x00,0x00,0x40}, /* ; */
            {0x00,0x10,0x28,0x44,0x82}, /* < */
            {0x00,0x00,0x3e,0x00,0x00}, /* = */
            {0x82,0x44,0x28,0x10,0x00}, /* > */
            {0x00,0x02,0x01,0x51,0x00}, /* ? */
            {0x3e,0x41,0x5d,0x4d,0x3e}, /* @ */
            {0x7c,0x12,0x11,0x12,0x7c}, /* A */
            {0x7f,0x49,0x49,0x49,0x36}, /* B */
            {0x3e,0x41,0x41,0x41,0x22}, /* C */
            {0x7f,0x41,0x41,0x22,0x1c}, /* D */
            {0x7f,0x49,0x49,0x49,0x41}, /* E */
            {0x7f,0x09,0x09,0x09,0x01}, /* F */
            {0x3e,0x41,0x49,0x49,0x3a}, /* G */
            {0x7f,0x08,0x08,0x08,0x7f}, /* H */
            {0x00,0x41,0x7f,0x41,0x00}, /* I */
            {0x20,0x40,0x41,0x41,0x3f}, /* J */
            {0x7f,0x08,0x14,0x22,0x41}, /* K */
            {0x7f,0x40,0x40,0x40,0x40}, /* L */
            {0x7f,0x02,0x0c,0x02,0x7f}, /* M */
            {0x7f,0x04,0x08,0x10,0x7f}, /* N */
            {0x3e,0x41,0x41,0x41,0x3e}, /* O */
            {0x7f,0x09,0x09,0x09,0x06}, /* P */
            {0x3e,0x41,0x51,0x21,0x1e}, /* Q */
            {0x7f,0x09,0x19,0x29,0x46}, /* R */
            {0x26,0x49,0x49,0x49,0x32}, /* S */
            {0x01,0x01,0x7f,0x01,0x01}, /* T */
            {0x3f,0x40,0x40,0x40,0x3f}, /* U */
            {0x1f,0x20,0x40,0x20,0x1f}, /* V */
            {0x3f,0x40,0x20,0x40,0x3f}, /* W */
            {0x63,0x14,0x08,0x14,0x63}, /* X */
            {0x07,0x08,0x70,0x08,0x07}, /* Y */
            {0x61,0x51,0x49,0x47,0x41}, /* Z */
        };
        int fi = c - 32;
        if (fi < 0 || fi >= 95) fi = 0;
        for (int col = 0; col < 5; col++) {
            unsigned char bits = font[fi][col];
            for (int row = 0; row < 7; row++) {
                if (bits & (1 << row)) {
                    wl_putpixel(x + col*6 + i*32 + col, y + row, color);
                }
            }
        }
        x += 30; /* advance */
    }
    fprintf(stderr, "[gdi32] TextOut at %d,%d: %.*s\n", x, y, count > 0 ? count : (int)strlen(str), str);
    wl_render_dirty();
    return 1;
}
static int __attribute__((ms_abi)) ntboxd_SetBkMode(void *dc, int mode) {
    (void)dc; (void)mode;
    return 1;
}
static unsigned long __attribute__((ms_abi)) ntboxd_SetTextColor(void *dc, unsigned long color) {
    (void)dc; (void)color;
    return 0xFFFFFF;
}

/* ============ M8: d3d11.dll stubs (GPU translation → Wayland fb) ============ */
static void * __attribute__((ms_abi)) ntboxd_D3D11CreateDevice(void *adapter, int driver_type, void *software,
                                                                int flags, const int *feature_levels, int num_levels,
                                                                int sdk_version, void **device, void **context) {
    (void)adapter; (void)driver_type; (void)software; (void)flags;
    (void)feature_levels; (void)num_levels; (void)sdk_version;
    void *dev = calloc(1, sizeof(void*));
    void *ctx = calloc(1, sizeof(void*));
    if (device) *device = dev;
    if (context) *context = ctx;
    wl_init(); /* ensure wayland window exists */
    fprintf(stderr, "[d3d11] D3D11CreateDevice: device=%p context=%p\n", dev, ctx);
    return 0; /* S_OK */
}
static void * __attribute__((ms_abi)) ntboxd_D3D11CreateDeviceAndSwapChain(void *adapter, int driver_type, void *software,
                                                                            int flags, const int *feature_levels, int num_levels,
                                                                            int sdk_version, const void *desc,
                                                                            void **swap_chain, void **device, void **context) {
    (void)adapter; (void)driver_type; (void)software; (void)flags;
    (void)feature_levels; (void)num_levels; (void)sdk_version; (void)desc;
    void *dev = calloc(1, sizeof(void*));
    void *ctx = calloc(1, sizeof(void*));
    void *sc = calloc(1, sizeof(void*));
    if (swap_chain) *swap_chain = sc;
    if (device) *device = dev;
    if (context) *context = ctx;
    wl_init();
    fprintf(stderr, "[d3d11] D3D11CreateDeviceAndSwapChain: device=%p context=%p swapchain=%p\n", dev, ctx, sc);
    return 0;
}

/* ============ M8: dxgi.dll stubs ============ */
static void * __attribute__((ms_abi)) ntboxd_CreateDXGIFactory(void *riid, void **factory) {
    (void)riid;
    void *f = calloc(1, sizeof(void*));
    if (factory) *factory = f;
    fprintf(stderr, "[dxgi] CreateDXGIFactory: factory=%p\n", f);
    return 0;
}
static void * __attribute__((ms_abi)) ntboxd_CreateDXGIFactory1(void *riid, void **factory) {
    return ntboxd_CreateDXGIFactory(riid, factory);
}

/* ============ M8/Game DLL stubs (so real Windows games load without modification) ============ */

/* --- version.dll --- */
static unsigned long __attribute__((ms_abi)) ntboxd_GetFileVersionInfoSizeW(const unsigned short *fname, unsigned long *handle) {
    (void)fname; (void)handle;
    return 0;
}
static int __attribute__((ms_abi)) ntboxd_GetFileVersionInfoW(const unsigned short *fname, unsigned long handle, unsigned long size, void *buf) {
    (void)fname; (void)handle; (void)size; (void)buf;
    return 1;
}
static int __attribute__((ms_abi)) ntboxd_VerQueryValueA(const void *buf, const char *subblock, void **buffer, unsigned long *len) {
    (void)buf; (void)subblock; (void)buffer; (void)len;
    return 0;
}
static int __attribute__((ms_abi)) ntboxd_VerQueryValueW(const void *buf, const unsigned short *subblock, void **buffer, unsigned long *len) {
    (void)buf; (void)subblock; (void)buffer; (void)len;
    return 0;
}

/* --- d3d9.dll --- */
static void * __attribute__((ms_abi)) ntboxd_Direct3DCreate9(unsigned long sdk_version) {
    (void)sdk_version;
    fprintf(stderr, "[d3d9] Direct3DCreate9(%lu)\n", sdk_version);
    return calloc(1, sizeof(void*));
}

/* --- ws2_32.dll --- */
static int __attribute__((ms_abi)) ntboxd_WSACleanup(void) { return 0; }
static int __attribute__((ms_abi)) ntboxd_WSACloseEvent(void *h) { (void)h; return 1; }
static void * __attribute__((ms_abi)) ntboxd_WSACreateEvent(void) { return calloc(1, sizeof(void*)); }
static int __attribute__((ms_abi)) ntboxd_WSAEnumNetworkEvents(void *s, void *e, void *events) { (void)s;(void)e;(void)events; return 0; }
static int __attribute__((ms_abi)) ntboxd_WSAEventSelect(void *s, void *e, unsigned long l) { (void)s;(void)e;(void)l; return 0; }
static int __attribute__((ms_abi)) ntboxd_WSAIoctl(void *s, unsigned long cmd, void *inbuf, unsigned long inlen, void *outbuf, unsigned long outlen, unsigned long *ret, void *ov, void *comp) {
    (void)s;(void)cmd;(void)inbuf;(void)inlen;(void)outbuf;(void)outlen;(void)ret;(void)ov;(void)comp;
    return 0;
}

/* --- shell32.dll additions --- */
static int __attribute__((ms_abi)) ntboxd_CommandLineToArgvW(const unsigned short *cmdline, int *argc) {
    (void)cmdline; (void)argc;
    return 0;
}
static void __attribute__((ms_abi)) ntboxd_DragFinish(void *h) { (void)h; }
static unsigned long __attribute__((ms_abi)) ntboxd_DragQueryFileA(void *h, unsigned long i, char *buf, unsigned long cch) {
    (void)h;(void)i;(void)buf;(void)cch;
    return 0;
}
static unsigned long __attribute__((ms_abi)) ntboxd_DragQueryPoint(void *h, void *pt) {
    (void)h;(void)pt;
    return 0;
}

/* --- ole32.dll --- */
static unsigned long __attribute__((ms_abi)) ntboxd_CoCreateGuid(void *guid) {
    (void)guid;
    return 0;
}
static unsigned long __attribute__((ms_abi)) ntboxd_CoCreateInstance(void *clsid, void *outer, unsigned long ctx, void *iid, void **obj) {
    (void)clsid;(void)outer;(void)ctx;(void)iid;(void)obj;
    return 0;
}
static unsigned long __attribute__((ms_abi)) ntboxd_CoInitialize(void *reserved) {
    (void)reserved;
    return 0;
}
static unsigned long __attribute__((ms_abi)) ntboxd_CoInitializeEx(void *reserved, unsigned long coinit) {
    (void)reserved;(void)coinit;
    return 0;
}

/* WaitForSingleObject defined after sync structs (see below) */
/* forward decls */
static unsigned long long __attribute__((ms_abi)) ntboxd_generic_stub(void);
static void *resolve(const char *dll, const char *name);

/* ---- M6c.4: sync objects + files + registry ---- */
#define NTBOXD_EVENT_MAGIC  0x45564E54UL
#define NTBOXD_MUTEX_MAGIC  0x4D555458UL

typedef struct {
    unsigned long magic;
    pthread_mutex_t m;
    pthread_cond_t c;
    int signaled;
    int manual;
} ntboxd_event_t;

typedef struct {
    unsigned long magic;
    pthread_mutex_t m;
    int owned;
} ntboxd_mutex_t;

static void ntboxd_path_convert(const char *src, char *dst, size_t cap) {
    size_t i = 0, j = 0;
    if (!src) { dst[0]=0; return; }
    /* strip drive letter "C:" */
    if (src[0] && src[1] == ':') src += 2;
    while (src[0] == '\\' || src[0] == '/') src++;
    /* ensure leading slash */
    if (j < cap-1) dst[j++] = '/';
    for (i = 0; src[i] && j < cap-1; i++) {
        dst[j++] = (src[i] == '\\') ? '/' : src[i];
    }
    dst[j] = 0;
}

static void * __attribute__((ms_abi)) ntboxd_CreateFileA(const char *name, unsigned long access, unsigned long share, void *sa, unsigned long creation, unsigned long attrs, void *tmpl) {
    (void)share; (void)sa; (void)attrs; (void)tmpl;
    char path[4096];
    ntboxd_path_convert(name, path, sizeof(path));
    int flags = O_RDONLY;
    if (access & 0x40000000UL) { /* GENERIC_WRITE */
        flags = O_WRONLY | O_CREAT;
        if (creation == 2) flags |= O_TRUNC; /* CREATE_ALWAYS */
        else if (creation == 4) flags |= O_APPEND; /* OPEN_ALWAYS approx */
    }
    if (creation == 1) flags = O_RDONLY; /* CREATE_NEW -> open existing? simplify */
    if (creation == 3) flags = O_RDONLY; /* OPEN_EXISTING */
    int fd = open(path, flags, 0644);
    if (fd < 0) return (void *)-1; /* INVALID_HANDLE_VALUE */
    return (void *)(long)fd;
}

static void * __attribute__((ms_abi)) ntboxd_CreateFileW(const unsigned short *name, unsigned long access, unsigned long share, void *sa, unsigned long creation, unsigned long attrs, void *tmpl) {
    char buf[4096]; size_t i=0,j=0;
    if (name) {
        while (name[i] && j < sizeof(buf)-1) {
            unsigned int cp = name[i++];
            if (cp < 0x80) buf[j++] = (char)cp;
            else buf[j++] = '?';
        }
    }
    buf[j]=0;
    return ntboxd_CreateFileA(buf, access, share, sa, creation, attrs, tmpl);
}

static int __attribute__((ms_abi)) ntboxd_CloseHandle(void *h) {
    /* event/mutex */
    if (h && (unsigned long)h >= 4096) {
        unsigned long *m = (unsigned long *)h;
        if (*m == NTBOXD_EVENT_MAGIC) {
            ntboxd_event_t *e = (ntboxd_event_t *)h;
            pthread_cond_destroy(&e->c);
            pthread_mutex_destroy(&e->m);
            free(e);
            return 1;
        }
        if (*m == NTBOXD_MUTEX_MAGIC) {
            ntboxd_mutex_t *mx = (ntboxd_mutex_t *)h;
            pthread_mutex_destroy(&mx->m);
            free(mx);
            return 1;
        }
    }
    /* file handle (fd) */
    int fd = (int)(long)h;
    if (fd >= 3) close(fd);
    return 1;
}

static void * __attribute__((ms_abi)) ntboxd_CreateEventA(void *sa, int manual, int initial, const char *name) {
    (void)sa; (void)name;
    ntboxd_event_t *e = calloc(1, sizeof(*e));
    e->magic = NTBOXD_EVENT_MAGIC;
    pthread_mutex_init(&e->m, NULL);
    pthread_cond_init(&e->c, NULL);
    e->manual = manual;
    e->signaled = initial ? 1 : 0;
    return (void *)e;
}
static void * __attribute__((ms_abi)) ntboxd_CreateEventW(void *sa, int manual, int initial, const unsigned short *name) {
    (void)name;
    return ntboxd_CreateEventA(sa, manual, initial, NULL);
}
static int __attribute__((ms_abi)) ntboxd_SetEvent(void *h) {
    ntboxd_event_t *e = (ntboxd_event_t *)h;
    if (!e || e->magic != NTBOXD_EVENT_MAGIC) return 0;
    pthread_mutex_lock(&e->m);
    e->signaled = 1;
    pthread_cond_broadcast(&e->c);
    pthread_mutex_unlock(&e->m);
    return 1;
}
static int __attribute__((ms_abi)) ntboxd_ResetEvent(void *h) {
    ntboxd_event_t *e = (ntboxd_event_t *)h;
    if (!e || e->magic != NTBOXD_EVENT_MAGIC) return 0;
    pthread_mutex_lock(&e->m);
    e->signaled = 0;
    pthread_mutex_unlock(&e->m);
    return 1;
}

static void * __attribute__((ms_abi)) ntboxd_CreateMutexA(void *sa, int initial, const char *name) {
    (void)sa; (void)name;
    ntboxd_mutex_t *mx = calloc(1, sizeof(*mx));
    mx->magic = NTBOXD_MUTEX_MAGIC;
    pthread_mutex_init(&mx->m, NULL);
    mx->owned = initial ? 1 : 0;
    if (initial) pthread_mutex_lock(&mx->m);
    return (void *)mx;
}
static int __attribute__((ms_abi)) ntboxd_ReleaseMutex(void *h) {
    ntboxd_mutex_t *mx = (ntboxd_mutex_t *)h;
    if (!mx || mx->magic != NTBOXD_MUTEX_MAGIC) return 0;
    pthread_mutex_unlock(&mx->m);
    mx->owned = 0;
    return 1;
}

/* WaitForSingleObject already exists; extend to handle event/mutex */
/* We'll replace function below */

static long __attribute__((ms_abi)) ntboxd_RegOpenKeyExA(void *hKey, const char *subKey, unsigned long options, unsigned long sam, void **result) {
    (void)hKey; (void)subKey; (void)options; (void)sam;
    if (result) *result = NULL;
    return 2; /* ERROR_FILE_NOT_FOUND */
}
static long __attribute__((ms_abi)) ntboxd_RegQueryValueExA(void *hKey, const char *value, unsigned long *reserved, unsigned long *type, unsigned char *data, unsigned long *size) {
    (void)hKey; (void)value; (void)reserved; (void)type; (void)data; (void)size;
    return 2;
}
static long __attribute__((ms_abi)) ntboxd_RegCloseKey(void *hKey) {
    (void)hKey;
    return 0;
}
static void * __attribute__((ms_abi)) ntboxd_GetModuleHandleA(const char *name) {
    (void)name;
    return (void *)0x10000000; /* fake base */
}
static void * __attribute__((ms_abi)) ntboxd_GetProcAddress(void *mod, const char *name) {
    (void)mod;
    if (!name) return NULL;
    /* try kernel32/ntdll, then user32/gdi32, then advapi32 */
    void *r = resolve("kernel32.dll", name);
    if (r == (void *)ntboxd_generic_stub) {
        r = resolve("ntdll.dll", name);
    }
    if (r == (void *)ntboxd_generic_stub) {
        r = resolve("user32.dll", name);
    }
    if (r == (void *)ntboxd_generic_stub) {
        r = resolve("gdi32.dll", name);
    }
    if (r == (void *)ntboxd_generic_stub) {
        r = resolve("advapi32.dll", name);
    }
    if (r == (void *)ntboxd_generic_stub) {
        r = resolve("d3d11.dll", name);
    }
    if (r == (void *)ntboxd_generic_stub) {
        r = resolve("dxgi.dll", name);
    }
    if (r == (void *)ntboxd_generic_stub) {
        r = resolve("shell32.dll", name);
    }
    if (r == (void *)ntboxd_generic_stub) {
        r = resolve("version.dll", name);
    }
    if (r == (void *)ntboxd_generic_stub) {
        r = resolve("d3d9.dll", name);
    }
    if (r == (void *)ntboxd_generic_stub) {
        r = resolve("ws2_32.dll", name);
    }
    if (r == (void *)ntboxd_generic_stub) {
        r = resolve("ole32.dll", name);
    }
    return r;
}
static void __attribute__((ms_abi, noreturn)) ntboxd_ExitProcess(unsigned int code) {
    exit((int)code);
}

/* ---- PE loader (user space) ---- */
static int rva_to_off(u8 *b, size_t len, u32 rva, u32 *out) {
    u32 e_lfanew; u16 nsec; size_t i; u8 *nt,*sec;
    if (len < 0x40) return -1;
    e_lfanew = *(u32*)(b+0x3C);
    if (e_lfanew + 4 + 20 > len) return -1;
    nt = b + e_lfanew;
    if (nt[0]!='P'||nt[1]!='E') return -1;
    nsec = *(u16*)(nt+6);
    sec = nt + 24 + *(u16*)(nt+20);
    for (i=0;i<nsec;i++) {
        u8 *s = sec + i*40;
        u32 va=*(u32*)(s+12), vsz=*(u32*)(s+8), raw=*(u32*)(s+20), rsz=*(u32*)(s+16);
        u32 span = vsz?vsz:rsz; if(!span) span=rsz;
        if (rva>=va && rva < va+span) { *out = raw + (rva-va); return 0; }
    }
    if (rva < *(u32*)(nt+24+60)) { *out = rva; return 0; }
    return -1;
}

static unsigned long __attribute__((ms_abi)) ntboxd_WaitForSingleObject(void *h, unsigned long ms) {
    if (!h) return 0;
    unsigned long *m = (unsigned long *)h;
    if (*m == NTBOXD_EVENT_MAGIC) {
        ntboxd_event_t *e = (ntboxd_event_t *)h;
        pthread_mutex_lock(&e->m);
        while (!e->signaled) {
            if (ms == 0) { pthread_mutex_unlock(&e->m); return 0x102; /* WAIT_TIMEOUT */ }
            pthread_cond_wait(&e->c, &e->m);
        }
        if (!e->manual) e->signaled = 0;
        pthread_mutex_unlock(&e->m);
        return 0; /* WAIT_OBJECT_0 */
    }
    if (*m == NTBOXD_MUTEX_MAGIC) {
        ntboxd_mutex_t *mx = (ntboxd_mutex_t *)h;
        pthread_mutex_lock(&mx->m);
        mx->owned = 1;
        pthread_mutex_unlock(&mx->m);
        return 0;
    }
    if (ms == 0xFFFFFFFFUL) ms = 1000;
    (void)ms;
    pthread_join((pthread_t)h, NULL);
    return 0;
}

static unsigned long long __attribute__((ms_abi)) ntboxd_generic_stub(void) {
    fprintf(stderr, "[ntboxd] called UNIMPLEMENTED import\n");
    return 0;
}

/*
 * msvcrt دوال حالية تعيد مؤشرات — الاحالة إلى ntboxd_generic_stub تعيد 0
 * ويقرأها الضيف عبر مؤشر فارغ (مثل _errno: call ثم mov (%rax),%eax).
 */
static int * __attribute__((ms_abi)) ntboxd_msvcrt__errno(void) {
    static int e = 0;
    return &e;
}
static int __attribute__((ms_abi)) ntboxd_msvcrt___lc_codepage_func(void) {
    return 1252; /* ANSI codepage MW */
}
static int __attribute__((ms_abi)) ntboxd_msvcrt___mb_cur_max_func(void) {
    return 1;
}
static void * __attribute__((ms_abi)) ntboxd_msvcrt__iob_func(void) {
    static void *iob[3]; /* stderr/stdout/stdin خانات فارغة */
    return iob;
}
static void * __attribute__((ms_abi)) ntboxd_msvcrt_localeconv(void) {
    static const char *decimal_point = ".";
    static const char *thousands_sep = "";
    static struct { const char *dp; const char *ts; unsigned char pad[64]; } lc;
    lc.dp = decimal_point; lc.ts = thousands_sep;
    return (void *)&lc;
}
static const char * __attribute__((ms_abi)) ntboxd_msvcrt_strerror(int e) {
    (void)e;
    return "";
}
static int * __attribute__((ms_abi)) ntboxd_msvcrt__p__fmode(void);
static int * __attribute__((ms_abi)) ntboxd_msvcrt__p__commode(void);
static void ** __attribute__((ms_abi)) ntboxd_msvcrt__p___initenv(void);
static void *ntboxd_msvcrt_func(const char *name)
{
    if (!strcmp(name, "_errno"))               return (void *)ntboxd_msvcrt__errno;
    if (!strcmp(name, "__doserrno"))           return (void *)ntboxd_msvcrt__errno;
    if (!strcmp(name, "___lc_codepage_func"))  return (void *)ntboxd_msvcrt___lc_codepage_func;
    if (!strcmp(name, "___lc_collate_cp_func"))return (void *)ntboxd_msvcrt___lc_codepage_func;
    if (!strcmp(name, "___mb_cur_max_func"))   return (void *)ntboxd_msvcrt___mb_cur_max_func;
    if (!strcmp(name, "__iob_func"))           return (void *)ntboxd_msvcrt__iob_func;
    if (!strcmp(name, "__iob"))                return ntboxd_msvcrt__iob_func();
    if (!strcmp(name, "localeconv"))           return (void *)ntboxd_msvcrt_localeconv;
    if (!strcmp(name, "strerror"))             return (void *)ntboxd_msvcrt_strerror;
    if (!strcmp(name, "__p__fmode"))           return (void *)ntboxd_msvcrt__p__fmode;
    if (!strcmp(name, "__p___fmode"))          return (void *)ntboxd_msvcrt__p__fmode;
    if (!strcmp(name, "__p__commode"))         return (void *)ntboxd_msvcrt__p__commode;
    if (!strcmp(name, "__p___commode"))        return (void *)ntboxd_msvcrt__p__commode;
    if (!strcmp(name, "__p___initenv"))        return (void *)ntboxd_msvcrt__p___initenv;
    return NULL;
}

/*
 * msvcrt صادرات البيانات (متغيرات لا دوال) — تصدير البيانات: __initenv.
 * mingw CRT Startup يكتب عبر __imp___initenv (envp) — الاحالة إلى
 * ntboxd_generic_stub تعطي عنوان صفحة كود للقراءة فقط ⇒ كتابة الضيف
 * في نص المضيف ⇒ segfault صامت. نعيدها خلايا ذاكرة قابلة للكتابة.
 */
static void *ntboxd_msvcrt_data_slot(const char *name)
{
    static void *initenv, *winitenv, *fmode, *commode;

    if (!strcmp(name, "__initenv")) {
        if (!initenv) initenv = calloc(1, sizeof(void *));
        return initenv;
    }
    if (!strcmp(name, "_winitenv")) {
        if (!winitenv) winitenv = calloc(1, sizeof(void *));
        return winitenv;
    }
    if (!strcmp(name, "_fmode")) {
        if (!fmode) fmode = calloc(1, sizeof(int));
        return fmode;
    }
    if (!strcmp(name, "_commode")) {
        if (!commode) commode = calloc(1, sizeof(int));
        return commode;
    }
    return NULL;
}

/*
 * __p__fmode / __p__commode — دوال تعيد عنوان نفس الخلايا أعلاه حتى
 * تتطابق قراءة .refptr._fmode (عبر IAT) مع ما تعيده __p__*.
 */
static int * __attribute__((ms_abi)) ntboxd_msvcrt__p__fmode(void) {
    static void *cell;
    if (!cell) cell = calloc(1, sizeof(int));
    return (int *)cell;
}
static int * __attribute__((ms_abi)) ntboxd_msvcrt__p__commode(void) {
    static void *cell;
    if (!cell) cell = calloc(1, sizeof(int));
    return (int *)cell;
}
static void ** __attribute__((ms_abi)) ntboxd_msvcrt__p___initenv(void) {
    static void *cell;
    if (!cell) cell = calloc(1, sizeof(void *));
    return (void **)cell;
}

/*
 * ============ T-063: ntdll.dll الحقيقي داخل الصندوق ============
 *
 * الفكرة: نحمّل ntdll الحقيقي من شجرة Win7، نعدّل ستوبات الـ syscall
 * **في مكانها** (E9 rel32 ⇒ نفس مصيدة test_trap)، ثم نحلّ استيرادات
 * التطبيق من ntdll إلى **العناوين الحقيقية داخل الصورة المعدَّلة**.
 *
 * النتيجة: كود ويندوز الحقيقي داخل الصندوق ⇒ نداء Nt* ⇒ المصيدة ⇒
 * المترجم ⇒ نواة لينكس. (تطبيقات kernel32 تحتاج محمّل DLL — T-120.)
 */
static struct ntb0_image ntboxd_ntdll_img;
static int               ntboxd_ntdll_ready = 0;
static ntb0_pe          *ntboxd_ntdll_pe = NULL;
static char              ntboxd_ntdll_path[1024];

/* يبني مساراً داخل جذر ويندوز 7 ($NTBOX_ROOT أو الافتراضي) */
static int ntboxd_win7_file(const char *rel, char *out, size_t len)
{
    const char *root = getenv("NTBOX_ROOT");
    const char *home = getenv("HOME");
    char cand[1024];

    if (!root || !*root) {
        // Prioritize the specified /home/imadix/Downloads/Niga/win7root
        if (access("/home/imadix/Downloads/Niga/win7root/Windows/System32", R_OK) == 0)
            root = "/home/imadix/Downloads/Niga/win7root";
        // Then check the other default paths
        else if (access("/tmp/opencode/win7root/Windows/System32", R_OK) == 0)
            root = "/tmp/opencode/win7root";
        else if (access("/home/imadix/Downloads/Niga/win7full/Windows/System32", R_OK) == 0)
            root = "/home/imadix/Downloads/Niga/win7full";
        else if (home && *home) {
            snprintf(cand, sizeof(cand), "%s/win7root", home);
            root = cand;
        } else {
            root = "win7root";
        }
    }

    snprintf(out, len, "%s/%s", root, rel);
    if (access(out, R_OK) == 0)
        return 0;
    /* fallback: try to replace System32 with SysWOW64 for 32-bit libs if appropriate */
    if (strstr(rel, "System32/")) {
        char rel2[1024];
        snprintf(rel2, sizeof(rel2), "%s", rel);
        char *q = strstr(rel2, "System32/");
        if (q) {
            memcpy(q, "SysWOW64/", 9);
            snprintf(out, len, "%s/%s", root, rel2);
            if (access(out, R_OK) == 0)
                return 0;
        }
    }
    return -1;
}

/*
 * ============ T-120: kernel32.dll الحقيقي (بالمحمّل العام) ============
 *
 * الخطوات:
 *   1) نحمّل ntdll الحقيقي عبر المحمّل (⇒ **إعادة توجيه** مطبَّقة).
 *   2) نضع قفزة الاعتراض **قريبة** من الصورة (قيد E9 rel32: ±2GB)
 *      ونعدّل الستوبات الـ400 في مكانها.
 *   3) نحمّل kernel32.dll الحقيقي ⇒ استيراداته (891) تُحلّ عبر
 *      ntdld الحقيقي (المعدَّل) ⇒ فتصير نداءات kernel32 حقيقية.
 *   4) استيرادات التطبيق من kernel32 ⇒ الصورة الحقيقية ⇒ كود ويندوز
 *      حقيقي داخل الصندوق.
 */
static ntb0_dll_loader  *ntboxd_loader = NULL;
static ntb0_dll_module  *ntboxd_mod_ntdll = NULL;
static ntb0_dll_module  *ntboxd_mod_kernel32 = NULL;

/* يحجز صفحة قفزة ضمن ±2GB عن base (لقيد E9 rel32) */
static void *ntboxd_alloc_near(uint64_t base, size_t len)
{
    uint64_t page = 4096;
    uint64_t anchor = base & ~(page - 1);
    int step;

    for (step = 0; step < 512; step++) {
        int sign = (step & 1) ? -1 : 1;
        uint64_t off = (uint64_t)(step / 2 + 1) * 0x10000000ull; /* 256MB */
        uint64_t addr = sign > 0 ? anchor + off : (anchor > off ? anchor - off : 0);
        void *p;

        if (addr == 0)
            continue;
        p = mmap((void *)(uintptr_t)addr, len, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED)
            return p;
        if (errno != ENOMEM)
            break;
    }

    /* الاحتياط: mmap عادي (قد يتجاوز ±2GB ⇒ يفشل التعديل ⇒ يُبلَّغ) */
    return mmap(NULL, len, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

/* يحمّل ntdll الحقيقي + يعدّل ستوباته (يعيد 1 عند النجاح) */
static int ntboxd_load_real_ntdll(void)
{
    if (ntboxd_win7_file("Windows/System32/ntdll.dll", ntboxd_ntdll_path,
                         sizeof(ntboxd_ntdll_path)) < 0) {
        fprintf(stderr, "[ntboxd] real ntdll not found (NTBOX_ROOT?)\n");
        return 0;
    }

    if (ntb0_map_pe(ntboxd_ntdll_path, &ntboxd_ntdll_img) < 0) {
        fprintf(stderr, "[ntboxd] cannot map the real ntdll\n");
        return 0;
    }

    if (ntb0_trap_patch(&ntboxd_ntdll_img) < 0) {
        fprintf(stderr, "[ntboxd] cannot patch the ntdll stubs\n");
        return 0;
    }

    ntboxd_ntdll_pe = ntb0_pe_open(ntboxd_ntdll_path);

    fprintf(stderr,
            "[ntboxd] real ntdll at %p — %u syscall stubs patched in place\n",
            ntboxd_ntdll_img.base, ntboxd_ntdll_img.stubs_patched);
    ntboxd_ntdll_ready = 1;
    return 1;
}

/* يحلّ اسم تصدير من ntdll الحقيقي ⇒ عنوان داخل الصورة المعدَّلة */
static void *ntboxd_real_ntdll_export(const char *name)
{
    ntb0_pe_export ex;

    if (!ntboxd_ntdll_ready || !ntboxd_ntdll_pe)
        return NULL;
    if (!ntb0_pe_find_by_name(ntboxd_ntdll_pe, name, &ex))
        return NULL;
    if (ex.forwarded)
        return NULL;

    return (void *)((char *)ntboxd_ntdll_img.base + ex.rva);
}

/*
 * ============ T-120: محمّل DLL الحقيقي + kernel32 ============
 */

/*callback: حلّ استيراد (dll, func) ⇒ عنوان */
static int ntboxd_resolve_cb(void *ctx, const char *dll, const char *func,
                             uint64_t *out)
{
    void *p = NULL;

    (void)ctx;

    if (!dll || !func || !out)
        return 0;

    /* 1) الوحدات الحقيقية المحمّلة */
    if (ntboxd_mod_kernel32 && !strcasecmp(dll, "kernel32.dll"))
        p = ntb0_dll_get_proc(ntboxd_loader, ntboxd_mod_kernel32, func);
    if (!p && ntboxd_mod_ntdll && !strcasecmp(dll, "ntdll.dll"))
        p = ntb0_dll_get_proc(ntboxd_loader, ntboxd_mod_ntdll, func);

    if (p) {
        *out = (uint64_t)(uintptr_t)p;
        return 0;   /* عقد المحمّل: 0 = نجاح */
    }

    /* 2) stubs الصندوق */
    p = resolve(dll, func);
    if (p && p != (void *)ntboxd_generic_stub) {
        *out = (uint64_t)(uintptr_t)p;
        return 0;
    }

    return -1;      /* تعذّر الحلّ */
}

/* يحمّل ntdll + kernel32 الحقيقيين (المحمّل + إعادة توجيه + تعديل الستوبات) */
static int ntboxd_load_real_modules(void)
{
    char k32path[1024];
    char sysdir[1024];
    void *tramp;
    long patched;

    ntboxd_loader = ntb0_dll_loader_new();
    if (!ntboxd_loader) {
        fprintf(stderr, "[ntboxd] cannot create the DLL loader\n");
        return 0;
    }

    if (ntboxd_win7_file("Windows/System32", sysdir, sizeof(sysdir)) == 0) {
        ntb0_dll_loader_set_sysdir(ntboxd_loader, sysdir);
        /* Do NOT call ntb0_path_set_root(sysdir) — it breaks disk.c file resolution
         * by setting root to System32 instead of win7root. The DLL loader's sysdir
         * is sufficient for finding system DLLs. Disk operations need the original root. */
    }
    
    /* Set SysWOW64 as a full peer to System32 for 32-bit system DLLs (T-XXX) */
    char syswow64_dir[1024];
    if (ntboxd_win7_file("Windows/SysWOW64", syswow64_dir, sizeof(syswow64_dir)) == 0) {
        ntb0_dll_loader_set_syswow64_sysdir(ntboxd_loader, syswow64_dir);
        fprintf(stderr, "[ntboxd] SysWOW64 peer configured at %s\n", syswow64_dir);
    }
    
    ntb0_dll_loader_set_resolver(ntboxd_loader, ntboxd_resolve_cb, NULL);

    /* api-set: Win7 بلا مخطّط ⇒ الاشتقاق بالقياس من الملفات الحقيقية
     * (362 عقد api-ms-win-* تُعاد توجيهها إلى وحدتها المضيفة) */
    {
        static ntb0_apiset *ntboxd_apiset;

        ntboxd_apiset = ntb0_apiset_new();
        if (ntboxd_apiset) {
            if (ntboxd_win7_file("Windows/System32", sysdir, sizeof(sysdir)) == 0)
                ntb0_apiset_set_sysdir(ntboxd_apiset, sysdir);
            {
                char soft[1024];

                if (ntboxd_win7_file("Windows/System32/config/SOFTWARE", soft,
                                     sizeof(soft)) == 0)
                    ntb0_apiset_load_schema(ntboxd_apiset, soft);
            }
            ntb0_dll_loader_set_apiset(ntboxd_loader, ntboxd_apiset);
        }
    }

    /* 1) ntdll الحقيقي عبر المحمّل (مع إعادة التوجيه) */
    if (ntboxd_win7_file("Windows/System32/ntdll.dll", ntboxd_ntdll_path,
                         sizeof(ntboxd_ntdll_path)) == 0) {
        ntboxd_mod_ntdll = ntb0_dll_load(ntboxd_loader, ntboxd_ntdll_path, 0);
    }

    if (!ntboxd_mod_ntdll) {
        fprintf(stderr, "[ntboxd] cannot load the real ntdll: %s\n",
                ntb0_dll_last_error(ntboxd_loader));
        return 0;
    }

    /* 2) قفزة قريبة من الصورة + تعديل الستوبات في مكانها */
    tramp = ntboxd_alloc_near(ntb0_dll_module_base(ntboxd_mod_ntdll), 0x1000);
    if (!tramp || tramp == MAP_FAILED) {
        fprintf(stderr, "[ntboxd] cannot place the trampoline\n");
        return 0;
    }
    if (ntb0_trampoline_write(tramp) < 0)
        return 0;

    ntb0_dll_module_make_writable(ntboxd_mod_ntdll);
    patched = ntb0_patch_stubs((void *)(uintptr_t)ntb0_dll_module_base(ntboxd_mod_ntdll),
                                ntb0_dll_module_size(ntboxd_mod_ntdll), tramp);
    ntb0_dll_module_restore_protections(ntboxd_mod_ntdll);

    fprintf(stderr,
            "[ntboxd] real ntdll at 0x%llx — %ld syscall stubs patched in place "
            "(relocs %u/%u applied)\n",
            (unsigned long long)ntb0_dll_module_base(ntboxd_mod_ntdll), patched,
            ntb0_dll_module_reloc_applied(ntboxd_mod_ntdll),
            ntb0_dll_module_reloc_entries(ntboxd_mod_ntdll));
    ntboxd_ntdll_ready = 1;

    /* 3) kernel32 الحقيقي ⇒ استيراداته تُحلّ عبر ntdll المعدَّل */
    if (ntboxd_win7_file("Windows/System32/kernel32.dll", k32path,
                         sizeof(k32path)) == 0) {
        ntboxd_mod_kernel32 = ntb0_dll_load_at(ntboxd_loader, k32path,
                                               0x78d20000,
                                               NTB0_DLL_FLAG_AUTO_DEPS);
    }

    if (ntboxd_mod_kernel32) {
        fprintf(stderr,
                "[ntboxd] real kernel32 at 0x%llx — %u imports resolved, "
                "%u unresolved\n",
                (unsigned long long)ntb0_dll_module_base(ntboxd_mod_kernel32),
                ntb0_dll_module_imports_resolved(ntboxd_mod_kernel32),
                ntb0_dll_module_imports_unresolved(ntboxd_mod_kernel32));
    } else {
        fprintf(stderr, "[ntboxd] real kernel32 not loaded: %s\n",
                ntb0_dll_last_error(ntboxd_loader));
    }

    return 1;
}

static void *resolve(const char *dll, const char *name) {
    /* External declarations for API stubs from api_stubs.c */
    extern void     *ntboxd_GetCurrentProcess(void);
    extern uint32_t ntboxd_GetCurrentProcessId(void);
    extern int      ntboxd_GetExitCodeProcess(void *, uint32_t *);
    extern int      ntboxd_CreateProcessA(const char *, char *, void *, void *, int, uint32_t, void *, const char *, void *, void *);
    extern int      ntboxd_CreateProcessW(const uint16_t *, uint16_t *, void *, void *, int, uint32_t, void *, const uint16_t *, void *, void *);
    extern int      ntboxd_TerminateProcess(void *, uint32_t);
    extern void     *ntboxd_OpenProcess(uint32_t, int, uint32_t);
    extern void     *ntboxd_GetCurrentThread(void);
    extern uint32_t ntboxd_GetCurrentThreadId(void);
    extern uint32_t ntboxd_ExitThread(uint32_t);
    extern int      ntboxd_SetThreadPriority(void *, int);
    extern uint32_t ntboxd_ResumeThread(void *);
    extern uint32_t ntboxd_SuspendThread(void *);
    extern void     *ntboxd_HeapCreate(uint32_t, size_t, size_t);
    extern int      ntboxd_HeapDestroy(void *);
    extern size_t   ntboxd_HeapSize(void *, uint32_t, void *);
    extern int      ntboxd_HeapValidate(void *, uint32_t, void *);
    extern size_t   ntboxd_HeapCompact(void *, uint32_t);
    extern int      ntboxd_HeapWalk(void *, void *);
    extern void     *ntboxd_LocalAlloc(uint32_t, size_t);
    extern void     *ntboxd_LocalFree(void *);
    extern void     *ntboxd_LocalReAlloc(void *, size_t, uint32_t);
    extern size_t   ntboxd_LocalSize(void *);
    extern void     *ntboxd_GlobalAlloc(uint32_t, size_t);
    extern void     *ntboxd_GlobalFree(void *);
    extern void     *ntboxd_GlobalReAlloc(void *, size_t, uint32_t);
    extern size_t   ntboxd_GlobalSize(void *);
    extern int      ntboxd_DuplicateHandle(void *, void *, void *, void *, uint32_t, int, uint32_t);
    extern uint32_t ntboxd_GetEnvironmentVariableA(const char *, char *, uint32_t);
    extern uint32_t ntboxd_GetEnvironmentVariableW(const uint16_t *, uint16_t *, uint32_t);
    extern int      ntboxd_SetEnvironmentVariableA(const char *, const char *);
    extern int      ntboxd_SetEnvironmentVariableW(const uint16_t *, const uint16_t *);
    extern uint32_t ntboxd_ExpandEnvironmentStringsA(const char *, char *, uint32_t);
    extern uint32_t ntboxd_ExpandEnvironmentStringsW(const uint16_t *, uint16_t *, uint32_t);
    extern uint32_t ntboxd_GetCurrentDirectoryA(uint32_t, char *);
    extern uint32_t ntboxd_GetCurrentDirectoryW(uint32_t, uint16_t *);
    extern int      ntboxd_SetCurrentDirectoryA(const char *);
    extern int      ntboxd_SetCurrentDirectoryW(const uint16_t *);
    extern uint32_t ntboxd_GetSystemDirectoryA(char *, uint32_t);
    extern uint32_t ntboxd_GetSystemDirectoryW(uint16_t *, uint32_t);
    extern uint32_t ntboxd_GetWindowsDirectoryA(char *, uint32_t);
    extern uint32_t ntboxd_GetWindowsDirectoryW(uint16_t *, uint32_t);
    extern int32_t  ntboxd_UnhandledExceptionFilter(void *);
    extern uint32_t ntboxd_SetErrorMode(uint32_t);
    extern uint32_t ntboxd_GetTickCount(void);
    extern void     ntboxd_GetSystemTimeAsFileTime(uint64_t *);
    extern int      ntboxd_QueryPerformanceCounter(uint64_t *);
    extern int      ntboxd_QueryPerformanceFrequency(uint64_t *);
    extern void     ntboxd_GetSystemInfo(void *);
    extern void     ntboxd_GetLocalTime(void *);
    extern int      ntboxd_SetLocalTime(const void *);
    extern int      ntboxd_GetVersionExA(void *);
    extern int      ntboxd_GetVersionExW(void *);
    extern int32_t  ntboxd_InterlockedIncrement(int32_t *);
    extern int32_t  ntboxd_InterlockedDecrement(int32_t *);
    extern int32_t  ntboxd_InterlockedExchange(int32_t *, int32_t);
    extern int32_t  ntboxd_InterlockedCompareExchange(int32_t *, int32_t, int32_t);
    extern uint64_t ntboxd_InterlockedCompareExchange64(uint64_t *, uint64_t, uint64_t);
    extern uint32_t ntboxd_GetModuleFileNameA(void *, char *, uint32_t);
    extern uint32_t ntboxd_GetModuleFileNameW(void *, uint16_t *, uint32_t);
    extern int      ntboxd_GetModuleHandleExA(uint32_t, const char *, void **);
    extern int      ntboxd_GetModuleHandleExW(uint32_t, const uint16_t *, void **);
    extern void     ntboxd_FreeLibraryAndExitThread(void *, uint32_t);
    extern int      ntboxd_DisableThreadLibraryCalls(void *);
    extern void     *ntboxd_LoadLibraryExA(const char *, void *, uint32_t);
    extern void     *ntboxd_LoadLibraryExW(const uint16_t *, void *, uint32_t);
    extern void     *ntboxd_LoadLibraryW(const uint16_t *);
    extern int      ntboxd_MulDiv(int, int, int);
    extern int      ntboxd_IsDBCSLeadByte(unsigned char);
    extern uint32_t ntboxd_GetACP(void);
    extern uint32_t ntboxd_GetOEMCP(void);
    extern uint32_t ntboxd_GetUserDefaultLCID(void);
    extern int      ntboxd_IsValidLocale(uint32_t, uint32_t);
    extern uint32_t ntboxd_ConvertDefaultLocale(uint32_t);
    extern char     *ntboxd_GetCommandLineA(void);
    extern uint16_t *ntboxd_GetCommandLineW(void);
    extern uint32_t ntboxd_GetEnvironmentStringsA(void);
    extern uint32_t ntboxd_GetEnvironmentStringsW(void);
    extern int      ntboxd_FreeEnvironmentStringsA(void *);
    extern int      ntboxd_FreeEnvironmentStringsW(void *);
    extern int32_t  ntboxd_RegQueryValueExW(void *, const uint16_t *, uint32_t *, uint32_t *, unsigned char *, uint32_t *);
    extern int32_t  ntboxd_RegEnumValueA(void *, uint32_t, char *, uint32_t *, uint32_t *, uint32_t *, unsigned char *, uint32_t *);
    extern int32_t  ntboxd_RegEnumValueW(void *, uint32_t, uint16_t *, uint32_t *, uint32_t *, uint32_t *, unsigned char *, uint32_t *);
    extern int32_t  ntboxd_RegQueryInfoKeyA(void *, char *, uint32_t *, uint32_t *, uint32_t *, uint32_t *, uint32_t *, uint32_t *, uint32_t *, uint32_t *, uint32_t *, void *);
    extern int32_t  ntboxd_RegQueryInfoKeyW(void *, uint16_t *, uint32_t *, uint32_t *, uint32_t *, uint32_t *, uint32_t *, uint32_t *, uint32_t *, uint32_t *, uint32_t *, void *);
    extern void     ntboxd_GetStartupInfoA(void *);
    extern void     ntboxd_GetStartupInfoW(void *);
    extern uint32_t ntboxd_FlsAlloc(void *);
    extern void     *ntboxd_FlsGetValue(uint32_t);
    extern int      ntboxd_FlsSetValue(uint32_t, void *);
    extern int      ntboxd_FlsFree(uint32_t);
    
    /* ExitProcess حقيقي ⇒ تفكيك داخل ntdll الحقيقي يقرأ هيكل Loader/PEB المصنوع
     * ⇒ segfault. طلب طرفي (نهاية العملية) — ترجمته داخل الصندوق مباشرة. */
    if (!strcasecmp(dll, "kernel32.dll") || !strcasecmp(dll, "kernelbase.dll") ||
        !strcasecmp(dll, "ntdll.dll") || !strcasecmp(dll, "msvcrt.dll")) {
        if (!strcmp(name, "ExitProcess") || !strcmp(name, "TerminateProcess")) {
            return (void *)ntboxd_ExitProcess;
        }
    }
    /* ---- الوحدات الحقيقية أولاً (T-063 · T-120) ---- */
    if (ntboxd_mod_kernel32 && !strcasecmp(dll, "kernel32.dll")) {
        void *r = ntb0_dll_get_proc(ntboxd_loader, ntboxd_mod_kernel32, name);
        if (r) {
            fprintf(stderr, "[ntboxd] real kernel32!%s -> %p\n", name, r);
            return r;
        }
    }
    if (ntboxd_mod_ntdll && !strcasecmp(dll, "ntdll.dll")) {
        void *r = ntb0_dll_get_proc(ntboxd_loader, ntboxd_mod_ntdll, name);
        if (r) {
            fprintf(stderr, "[ntboxd] real ntdll!%s -> %p\n", name, r);
            return r;
        }
    }
    if (ntboxd_ntdll_ready && !strcasecmp(dll, "ntdll.dll")) {
        void *r = ntboxd_real_ntdll_export(name);
        if (r) {
            fprintf(stderr, "[ntboxd] real ntdll!%s -> %p\n", name, r);
            return r;
        }
    }
    /* ---- msvcrt: طبقة C صغيرة بدل الـ stub العام (Libc-001) ---- */
    if (!strcasecmp(dll, "msvcrt.dll")) {
        /*
         * صادرات البيانات أولاً (GUEST-WRITE): _fmode/_commode استيراد بيانات
         * ويكتب الضيف عبر IAT مباشرة — لا يجوز حلّها كدالة (صفحة كود ر أو Roch).
         */
        void *d = ntboxd_msvcrt_data_slot(name);
        if (d)
            return d;
        void *r = ntboxd_libc_lookup(name);
        if (r)
            return r;
        /* دوال تعيد مؤشرات حالة (GUEST-DEREF): لا تُترك للـ stub العام */
        r = ntboxd_msvcrt_func(name);
        if (r)
            return r;
                /* CATCH-ALL SAFE STUB FOR USER32/GDI32 TO PREVENT CRASH */
        fprintf(stderr, "[safe-stub] %s!%s -> auto-handled\n", dll, name);
        return (void *)ntboxd_swarm_ole32_dll_GetWindowRect; /* safe non-crashing function returning 1 */
    }
    if (!strcasecmp(dll,"user32.dll")) {
        if (!strcmp(name,"MessageBoxA")) return (void*)ntboxd_MessageBoxA;
        if (!strcmp(name,"DefWindowProcA")) return (void*)ntboxd_DefWindowProcA;
        if (!strcmp(name,"RegisterClassA")) return (void*)ntboxd_RegisterClassA;
        if (!strcmp(name,"CreateWindowExA")) return (void*)ntboxd_CreateWindowExA;
        /* W-variants — api_stubs.c ترجع داخلياً UTF-16 → ASCII ثم تبني
         * hwnd بهيكل "WIND" المطابق، فتعمل مع ShowWindow/Wayland: */
        if (!strcmp(name,"RegisterClassW")) return (void*)ntboxd_RegisterClassW;
        if (!strcmp(name,"CreateWindowExW")) return (void*)ntboxd_CreateWindowExW;
        if (!strcmp(name,"DefWindowProcW")) return (void*)ntboxd_DefWindowProcW;
        if (!strcmp(name,"MessageBoxW")) return (void*)ntboxd_MessageBoxW;
        if (!strcmp(name,"ShowWindow")) return (void*)ntboxd_ShowWindow;
        if (!strcmp(name,"UpdateWindow")) return (void*)ntboxd_UpdateWindow;
        if (!strcmp(name,"GetMessageA")) return (void*)ntboxd_GetMessageA;
        if (!strcmp(name,"TranslateMessage")) return (void*)ntboxd_TranslateMessage;
        if (!strcmp(name,"DispatchMessageA")) return (void*)ntboxd_DispatchMessageA;
        if (!strcmp(name,"PostQuitMessage")) return (void*)ntboxd_PostQuitMessage;
        if (!strcmp(name,"GetClientRect")) return (void*)ntboxd_GetClientRect;
        if (!strcmp(name,"GetDC")) return (void*)ntboxd_GetDC;
        if (!strcmp(name,"ReleaseDC")) return (void*)ntboxd_ReleaseDC;
        if (!strcmp(name,"BeginPaint")) return (void*)ntboxd_BeginPaint;
        if (!strcmp(name,"EndPaint")) return (void*)ntboxd_EndPaint;
        if (!strcmp(name,"GetWindowTextW")) return (void*)ntboxd_GetWindowTextW;
        if (!strcmp(name,"SetTimer")) return (void*)ntboxd_SetTimer;
        if (!strcmp(name,"KillTimer")) return (void*)ntboxd_KillTimer;
        if (!strcmp(name,"FillRect")) return (void*)ntboxd_FillRect;
        if (!strcmp(name,"BeginPaint")) return (void*)ntboxd_BeginPaint;
        if (!strcmp(name,"EndPaint")) return (void*)ntboxd_EndPaint;
        if (!strcmp(name,"GetWindowTextW")) return (void*)ntboxd_GetWindowTextW;
        if (!strcmp(name,"GetWindowTextA")) return (void*)ntboxd_GetWindowTextA;
        if (!strcmp(name,"SetTimer")) return (void*)ntboxd_SetTimer;
        if (!strcmp(name,"KillTimer")) return (void*)ntboxd_KillTimer;
                /* CATCH-ALL SAFE STUB FOR USER32/GDI32 TO PREVENT CRASH */
        fprintf(stderr, "[safe-stub] %s!%s -> auto-handled\n", dll, name);
        return (void *)ntboxd_swarm_ole32_dll_GetWindowRect; /* safe non-crashing function returning 1 */
    }
    if (!strcasecmp(dll,"gdi32.dll")) {
        if (!strcmp(name,"CreateCompatibleDC")) return (void*)ntboxd_CreateCompatibleDC;
        if (!strcmp(name,"CreateCompatibleBitmap")) return (void*)ntboxd_CreateCompatibleBitmap;
        if (!strcmp(name,"SelectObject")) return (void*)ntboxd_SelectObject;
        if (!strcmp(name,"DeleteDC")) return (void*)ntboxd_DeleteDC;
        if (!strcmp(name,"DeleteObject")) return (void*)ntboxd_DeleteObject;
        if (!strcmp(name,"BitBlt")) return (void*)ntboxd_BitBlt;
        if (!strcmp(name,"Rectangle")) return (void*)ntboxd_Rectangle;
        if (!strcmp(name,"TextOutA")) return (void*)ntboxd_TextOutA;
        if (!strcmp(name,"SetBkMode")) return (void*)ntboxd_SetBkMode;
        if (!strcmp(name,"SetTextColor")) return (void*)ntboxd_SetTextColor;
        /* الأقلام/الفرش — تعمل مباشرة، و DeleteObject محمين (سجل الأجسام) */
        if (!strcmp(name,"CreatePen")) return (void*)ntboxd_CreatePen;
        if (!strcmp(name,"CreateSolidBrush")) return (void*)ntboxd_CreateSolidBrush;
                /* CATCH-ALL SAFE STUB FOR USER32/GDI32 TO PREVENT CRASH */
        fprintf(stderr, "[safe-stub] %s!%s -> auto-handled\n", dll, name);
        return (void *)ntboxd_swarm_ole32_dll_GetWindowRect; /* safe non-crashing function returning 1 */
    }
    /* ============ M8: d3d11.dll stubs (GPU translation → Wayland fb) ============ */
    if (!strcasecmp(dll,"d3d11.dll")) {
        if (!strcmp(name,"D3D11CreateDevice")) return (void*)ntboxd_D3D11CreateDevice;
        if (!strcmp(name,"D3D11CreateDeviceAndSwapChain")) return (void*)ntboxd_D3D11CreateDeviceAndSwapChain;
                /* CATCH-ALL SAFE STUB FOR USER32/GDI32 TO PREVENT CRASH */
        fprintf(stderr, "[safe-stub] %s!%s -> auto-handled\n", dll, name);
        return (void *)ntboxd_swarm_ole32_dll_GetWindowRect; /* safe non-crashing function returning 1 */
    }
    /* ============ M8: dxgi.dll stubs ============ */
    if (!strcasecmp(dll,"dxgi.dll")) {
        if (!strcmp(name,"CreateDXGIFactory")) return (void*)ntboxd_CreateDXGIFactory;
        if (!strcmp(name,"CreateDXGIFactory1")) return (void*)ntboxd_CreateDXGIFactory1;
                /* CATCH-ALL SAFE STUB FOR USER32/GDI32 TO PREVENT CRASH */
        fprintf(stderr, "[safe-stub] %s!%s -> auto-handled\n", dll, name);
        return (void *)ntboxd_swarm_ole32_dll_GetWindowRect; /* safe non-crashing function returning 1 */
    }
    /* ============ M8/Game DLL stubs ============ */
    if (!strcasecmp(dll,"shell32.dll")) {
        if (!strcmp(name,"MessageBoxA")) return (void*)ntboxd_MessageBoxA;
        if (!strcmp(name,"CommandLineToArgvW")) return (void*)ntboxd_CommandLineToArgvW;
        if (!strcmp(name,"DragFinish")) return (void*)ntboxd_DragFinish;
        if (!strcmp(name,"DragQueryFileA")) return (void*)ntboxd_DragQueryFileA;
        if (!strcmp(name,"DragQueryPoint")) return (void*)ntboxd_DragQueryPoint;
                /* CATCH-ALL SAFE STUB FOR USER32/GDI32 TO PREVENT CRASH */
        fprintf(stderr, "[safe-stub] %s!%s -> auto-handled\n", dll, name);
        return (void *)ntboxd_swarm_ole32_dll_GetWindowRect; /* safe non-crashing function returning 1 */
    }
    if (!strcasecmp(dll,"version.dll")) {
        if (!strcmp(name,"GetFileVersionInfoSizeW")) return (void*)ntboxd_GetFileVersionInfoSizeW;
        if (!strcmp(name,"GetFileVersionInfoW")) return (void*)ntboxd_GetFileVersionInfoW;
        if (!strcmp(name,"VerQueryValueA")) return (void*)ntboxd_VerQueryValueA;
        if (!strcmp(name,"VerQueryValueW")) return (void*)ntboxd_VerQueryValueW;
                /* CATCH-ALL SAFE STUB FOR USER32/GDI32 TO PREVENT CRASH */
        fprintf(stderr, "[safe-stub] %s!%s -> auto-handled\n", dll, name);
        return (void *)ntboxd_swarm_ole32_dll_GetWindowRect; /* safe non-crashing function returning 1 */
    }
    if (!strcasecmp(dll,"d3d9.dll")) {
        if (!strcmp(name,"Direct3DCreate9")) return (void*)ntboxd_Direct3DCreate9;
                /* CATCH-ALL SAFE STUB FOR USER32/GDI32 TO PREVENT CRASH */
        fprintf(stderr, "[safe-stub] %s!%s -> auto-handled\n", dll, name);
        return (void *)ntboxd_swarm_ole32_dll_GetWindowRect; /* safe non-crashing function returning 1 */
    }
    if (!strcasecmp(dll,"ws2_32.dll")) {
        if (!strcmp(name,"WSACleanup")) return (void*)ntboxd_WSACleanup;
        if (!strcmp(name,"WSACloseEvent")) return (void*)ntboxd_WSACloseEvent;
        if (!strcmp(name,"WSACreateEvent")) return (void*)ntboxd_WSACreateEvent;
        if (!strcmp(name,"WSAEnumNetworkEvents")) return (void*)ntboxd_WSAEnumNetworkEvents;
        if (!strcmp(name,"WSAEventSelect")) return (void*)ntboxd_WSAEventSelect;
        if (!strcmp(name,"WSAIoctl")) return (void*)ntboxd_WSAIoctl;
                /* CATCH-ALL SAFE STUB FOR USER32/GDI32 TO PREVENT CRASH */
        fprintf(stderr, "[safe-stub] %s!%s -> auto-handled\n", dll, name);
        return (void *)ntboxd_swarm_ole32_dll_GetWindowRect; /* safe non-crashing function returning 1 */
    }
    if (!strcasecmp(dll,"ole32.dll")) {
        if (!strcmp(name,"CoCreateGuid")) return (void*)ntboxd_CoCreateGuid;
        if (!strcmp(name,"CoCreateInstance")) return (void*)ntboxd_CoCreateInstance;
        if (!strcmp(name,"CoInitialize")) return (void*)ntboxd_CoInitialize;
        if (!strcmp(name,"CoInitializeEx")) return (void*)ntboxd_CoInitializeEx;
                /* CATCH-ALL SAFE STUB FOR USER32/GDI32 TO PREVENT CRASH */
        fprintf(stderr, "[safe-stub] %s!%s -> auto-handled\n", dll, name);
        return (void *)ntboxd_swarm_ole32_dll_GetWindowRect; /* safe non-crashing function returning 1 */
    }
    if (!strcasecmp(dll,"kernel32.dll") || !strcasecmp(dll,"ntdll.dll") || !strcasecmp(dll,"advapi32.dll") || !strcasecmp(dll,"kernelbase.dll")) {
        if (!strcmp(name,"GetStdHandle")) return (void*)ntboxd_GetStdHandle;
        if (!strcmp(name,"WriteFile")) return (void*)ntboxd_WriteFile;
        if (!strcmp(name,"ReadFile")) return (void*)ntboxd_ReadFile;
        if (!strcmp(name,"VirtualAlloc")) return (void*)ntboxd_VirtualAlloc;
        if (!strcmp(name,"VirtualFree")) return (void*)ntboxd_VirtualFree;
        if (!strcmp(name,"HeapAlloc")) return (void*)ntboxd_HeapAlloc;
        if (!strcmp(name,"HeapFree")) return (void*)ntboxd_HeapFree;
        if (!strcmp(name,"CloseHandle")) return (void*)ntboxd_CloseHandle;
        if (!strcmp(name,"ExitProcess")) return (void*)ntboxd_ExitProcess;
        if (!strcmp(name,"CreateThread")) return (void*)ntboxd_CreateThread;
        if (!strcmp(name,"CreateFileA")) return (void*)ntboxd_CreateFileA;
        if (!strcmp(name,"CreateFileW")) return (void*)ntboxd_CreateFileW;
        if (!strcmp(name,"CreateEventA")) return (void*)ntboxd_CreateEventA;
        if (!strcmp(name,"CreateEventW")) return (void*)ntboxd_CreateEventW;
        if (!strcmp(name,"SetEvent")) return (void*)ntboxd_SetEvent;
        if (!strcmp(name,"ResetEvent")) return (void*)ntboxd_ResetEvent;
        if (!strcmp(name,"InitializeCriticalSection")) return (void*)ntboxd_InitializeCriticalSection;
        if (!strcmp(name,"DeleteCriticalSection")) return (void*)ntboxd_DeleteCriticalSection;
        if (!strcmp(name,"EnterCriticalSection")) return (void*)ntboxd_EnterCriticalSection;
        if (!strcmp(name,"LeaveCriticalSection")) return (void*)ntboxd_LeaveCriticalSection;
        if (!strcmp(name,"TryEnterCriticalSection")) return (void*)ntboxd_TryEnterCriticalSection;
        if (!strcmp(name,"TlsAlloc")) return (void*)ntboxd_TlsAlloc;
        if (!strcmp(name,"TlsFree")) return (void*)ntboxd_TlsFree;
        if (!strcmp(name,"TlsGetValue")) return (void*)ntboxd_TlsGetValue;
        if (!strcmp(name,"TlsSetValue")) return (void*)ntboxd_TlsSetValue;
        if (!strcmp(name,"VirtualProtect")) return (void*)ntboxd_VirtualProtect;
        if (!strcmp(name,"VirtualQuery")) return (void*)ntboxd_VirtualQuery;
        if (!strcmp(name,"MultiByteToWideChar")) return (void*)ntboxd_MultiByteToWideChar;
        if (!strcmp(name,"WideCharToMultiByte")) return (void*)ntboxd_WideCharToMultiByte;
        if (!strcmp(name,"IsDBCSLeadByteEx")) return (void*)ntboxd_IsDBCSLeadByteEx;
        if (!strcmp(name,"SetUnhandledExceptionFilter")) return (void*)ntboxd_SetUnhandledExceptionFilter;
        if (!strcmp(name,"GetExitCodeThread")) return (void*)ntboxd_GetExitCodeThread;
        if (!strcmp(name,"CreateMutexA")) return (void*)ntboxd_CreateMutexA;
        if (!strcmp(name,"ReleaseMutex")) return (void*)ntboxd_ReleaseMutex;
        if (!strcmp(name,"RegOpenKeyExA")) return (void*)ntboxd_RegOpenKeyExA;
        if (!strcmp(name,"RegQueryValueExA")) return (void*)ntboxd_RegQueryValueExA;
        if (!strcmp(name,"RegCloseKey")) return (void*)ntboxd_RegCloseKey;
        if (!strcmp(name,"GetModuleHandleA")) return (void*)ntboxd_GetModuleHandleA;
        if (!strcmp(name,"GetProcAddress")) return (void*)ntboxd_GetProcAddress;
        if (!strcmp(name,"GetLastError")) return (void*)ntboxd_GetLastError;
        if (!strcmp(name,"SetLastError")) return (void*)ntboxd_SetLastError;
        if (!strcmp(name,"Sleep")) return (void*)ntboxd_Sleep;
        if (!strcmp(name,"WaitForSingleObject")) return (void*)ntboxd_WaitForSingleObject;
        if (!strcmp(name,"NtAllocateVirtualMemory")) return (void*)ntboxd_VirtualAlloc;
        if (!strcmp(name,"NtFreeVirtualMemory")) return (void*)ntboxd_VirtualFree;
        if (!strcmp(name,"NtReadFile")) return (void*)ntboxd_ReadFile;
        if (!strcmp(name,"NtWriteFile")) return (void*)ntboxd_WriteFile;
        if (!strcmp(name,"NtTerminateProcess")) return (void*)ntboxd_ExitProcess;
    }
    
    /* ========== NEW COMPREHENSIVE STUBS (EXTENDED API COVERAGE) ========== */
    
    /* Process Management */
    if (!strcasecmp(dll, "kernel32.dll") || !strcasecmp(dll, "kernelbase.dll")) {
        if (!strcmp(name, "GetCurrentProcess")) return (void*)ntboxd_GetCurrentProcess;
        if (!strcmp(name, "GetCurrentProcessId")) return (void*)ntboxd_GetCurrentProcessId;
        if (!strcmp(name, "GetExitCodeProcess")) return (void*)ntboxd_GetExitCodeProcess;
        if (!strcmp(name, "CreateProcessA")) return (void*)ntboxd_CreateProcessA;
        if (!strcmp(name, "CreateProcessW")) return (void*)ntboxd_CreateProcessW;
        if (!strcmp(name, "TerminateProcess")) return (void*)ntboxd_TerminateProcess;
        if (!strcmp(name, "OpenProcess")) return (void*)ntboxd_OpenProcess;
    }
    
    /* Thread Management */
    if (!strcasecmp(dll, "kernel32.dll") || !strcasecmp(dll, "kernelbase.dll")) {
        if (!strcmp(name, "GetCurrentThread")) return (void*)ntboxd_GetCurrentThread;
        if (!strcmp(name, "GetCurrentThreadId")) return (void*)ntboxd_GetCurrentThreadId;
        if (!strcmp(name, "ExitThread")) return (void*)ntboxd_ExitThread;
        if (!strcmp(name, "SetThreadPriority")) return (void*)ntboxd_SetThreadPriority;
        if (!strcmp(name, "ResumeThread")) return (void*)ntboxd_ResumeThread;
        if (!strcmp(name, "SuspendThread")) return (void*)ntboxd_SuspendThread;
    }
    
    /* Memory Management */
    if (!strcasecmp(dll, "kernel32.dll") || !strcasecmp(dll, "kernelbase.dll")) {
        if (!strcmp(name, "HeapCreate")) return (void*)ntboxd_HeapCreate;
        if (!strcmp(name, "HeapDestroy")) return (void*)ntboxd_HeapDestroy;
        if (!strcmp(name, "HeapSize")) return (void*)ntboxd_HeapSize;
        if (!strcmp(name, "HeapValidate")) return (void*)ntboxd_HeapValidate;
        if (!strcmp(name, "HeapCompact")) return (void*)ntboxd_HeapCompact;
        if (!strcmp(name, "HeapWalk")) return (void*)ntboxd_HeapWalk;
        if (!strcmp(name, "LocalAlloc")) return (void*)ntboxd_LocalAlloc;
        if (!strcmp(name, "LocalFree")) return (void*)ntboxd_LocalFree;
        if (!strcmp(name, "LocalReAlloc")) return (void*)ntboxd_LocalReAlloc;
        if (!strcmp(name, "LocalSize")) return (void*)ntboxd_LocalSize;
        if (!strcmp(name, "GlobalAlloc")) return (void*)ntboxd_GlobalAlloc;
        if (!strcmp(name, "GlobalFree")) return (void*)ntboxd_GlobalFree;
        if (!strcmp(name, "GlobalReAlloc")) return (void*)ntboxd_GlobalReAlloc;
        if (!strcmp(name, "GlobalSize")) return (void*)ntboxd_GlobalSize;
    }
    
    /* Handle Management */
    if (!strcasecmp(dll, "kernel32.dll") || !strcasecmp(dll, "kernelbase.dll")) {
        if (!strcmp(name, "DuplicateHandle")) return (void*)ntboxd_DuplicateHandle;
    }
    
    /* Environment & Directory */
    if (!strcasecmp(dll, "kernel32.dll") || !strcasecmp(dll, "kernelbase.dll")) {
        if (!strcmp(name, "GetEnvironmentVariableA")) return (void*)ntboxd_GetEnvironmentVariableA;
        if (!strcmp(name, "GetEnvironmentVariableW")) return (void*)ntboxd_GetEnvironmentVariableW;
        if (!strcmp(name, "SetEnvironmentVariableA")) return (void*)ntboxd_SetEnvironmentVariableA;
        if (!strcmp(name, "SetEnvironmentVariableW")) return (void*)ntboxd_SetEnvironmentVariableW;
        if (!strcmp(name, "ExpandEnvironmentStringsA")) return (void*)ntboxd_ExpandEnvironmentStringsA;
        if (!strcmp(name, "ExpandEnvironmentStringsW")) return (void*)ntboxd_ExpandEnvironmentStringsW;
        if (!strcmp(name, "GetCurrentDirectoryA")) return (void*)ntboxd_GetCurrentDirectoryA;
        if (!strcmp(name, "GetCurrentDirectoryW")) return (void*)ntboxd_GetCurrentDirectoryW;
        if (!strcmp(name, "SetCurrentDirectoryA")) return (void*)ntboxd_SetCurrentDirectoryA;
        if (!strcmp(name, "SetCurrentDirectoryW")) return (void*)ntboxd_SetCurrentDirectoryW;
        if (!strcmp(name, "GetSystemDirectoryA")) return (void*)ntboxd_GetSystemDirectoryA;
        if (!strcmp(name, "GetSystemDirectoryW")) return (void*)ntboxd_GetSystemDirectoryW;
        if (!strcmp(name, "GetWindowsDirectoryA")) return (void*)ntboxd_GetWindowsDirectoryA;
        if (!strcmp(name, "GetWindowsDirectoryW")) return (void*)ntboxd_GetWindowsDirectoryW;
    }
    
    /* Error Handling */
    if (!strcasecmp(dll, "kernel32.dll") || !strcasecmp(dll, "kernelbase.dll")) {
        if (!strcmp(name, "GetLastError")) return (void*)ntboxd_GetLastError;
        if (!strcmp(name, "SetLastError")) return (void*)ntboxd_SetLastError;
        if (!strcmp(name, "UnhandledExceptionFilter")) return (void*)ntboxd_UnhandledExceptionFilter;
        if (!strcmp(name, "SetUnhandledExceptionFilter")) return (void*)ntboxd_SetUnhandledExceptionFilter;
        if (!strcmp(name, "SetErrorMode")) return (void*)ntboxd_SetErrorMode;
    }
    
    /* String Conversion */
    if (!strcasecmp(dll, "kernel32.dll") || !strcasecmp(dll, "kernelbase.dll")) {
        if (!strcmp(name, "MultiByteToWideChar")) return (void*)ntboxd_MultiByteToWideChar;
        if (!strcmp(name, "WideCharToMultiByte")) return (void*)ntboxd_WideCharToMultiByte;
    }
    
    /* System Information */
    if (!strcasecmp(dll, "kernel32.dll") || !strcasecmp(dll, "kernelbase.dll")) {
        if (!strcmp(name, "GetTickCount")) return (void*)ntboxd_GetTickCount;
        if (!strcmp(name, "GetSystemTimeAsFileTime")) return (void*)ntboxd_GetSystemTimeAsFileTime;
        if (!strcmp(name, "QueryPerformanceCounter")) return (void*)ntboxd_QueryPerformanceCounter;
        if (!strcmp(name, "QueryPerformanceFrequency")) return (void*)ntboxd_QueryPerformanceFrequency;
        if (!strcmp(name, "GetSystemInfo")) return (void*)ntboxd_GetSystemInfo;
        if (!strcmp(name, "GetLocalTime")) return (void*)ntboxd_GetLocalTime;
        if (!strcmp(name, "SetLocalTime")) return (void*)ntboxd_SetLocalTime;
        if (!strcmp(name, "GetVersionExA")) return (void*)ntboxd_GetVersionExA;
        if (!strcmp(name, "GetVersionExW")) return (void*)ntboxd_GetVersionExW;
        if (!strcmp(name, "GetStartupInfoA")) return (void*)ntboxd_GetStartupInfoA;
        if (!strcmp(name, "GetStartupInfoW")) return (void*)ntboxd_GetStartupInfoW;
    }
    
    /* Interlocked Operations */
    if (!strcasecmp(dll, "kernel32.dll") || !strcasecmp(dll, "kernelbase.dll")) {
        if (!strcmp(name, "InterlockedIncrement")) return (void*)ntboxd_InterlockedIncrement;
        if (!strcmp(name, "InterlockedDecrement")) return (void*)ntboxd_InterlockedDecrement;
        if (!strcmp(name, "InterlockedExchange")) return (void*)ntboxd_InterlockedExchange;
        if (!strcmp(name, "InterlockedCompareExchange")) return (void*)ntboxd_InterlockedCompareExchange;
        if (!strcmp(name, "InterlockedCompareExchange64")) return (void*)ntboxd_InterlockedCompareExchange64;
    }
    
    /* DLL Loading & Module Management */
    if (!strcasecmp(dll, "kernel32.dll") || !strcasecmp(dll, "kernelbase.dll")) {
        if (!strcmp(name, "GetModuleFileNameA")) return (void*)ntboxd_GetModuleFileNameA;
        if (!strcmp(name, "GetModuleFileNameW")) return (void*)ntboxd_GetModuleFileNameW;
        if (!strcmp(name, "GetModuleHandleExA")) return (void*)ntboxd_GetModuleHandleExA;
        if (!strcmp(name, "GetModuleHandleExW")) return (void*)ntboxd_GetModuleHandleExW;
        if (!strcmp(name, "FreeLibraryAndExitThread")) return (void*)ntboxd_FreeLibraryAndExitThread;
        if (!strcmp(name, "DisableThreadLibraryCalls")) return (void*)ntboxd_DisableThreadLibraryCalls;
        if (!strcmp(name, "LoadLibraryExA")) return (void*)ntboxd_LoadLibraryExA;
        if (!strcmp(name, "LoadLibraryExW")) return (void*)ntboxd_LoadLibraryExW;
        if (!strcmp(name, "LoadLibraryW")) return (void*)ntboxd_LoadLibraryW;
    }
    
    /* Misc Utilities */
    if (!strcasecmp(dll, "kernel32.dll") || !strcasecmp(dll, "kernelbase.dll")) {
        if (!strcmp(name, "MulDiv")) return (void*)ntboxd_MulDiv;
        if (!strcmp(name, "IsDBCSLeadByte")) return (void*)ntboxd_IsDBCSLeadByte;
        if (!strcmp(name, "GetACP")) return (void*)ntboxd_GetACP;
        if (!strcmp(name, "GetOEMCP")) return (void*)ntboxd_GetOEMCP;
        if (!strcmp(name, "GetUserDefaultLCID")) return (void*)ntboxd_GetUserDefaultLCID;
        if (!strcmp(name, "IsValidLocale")) return (void*)ntboxd_IsValidLocale;
        if (!strcmp(name, "ConvertDefaultLocale")) return (void*)ntboxd_ConvertDefaultLocale;
        if (!strcmp(name, "FlsAlloc")) return (void*)ntboxd_FlsAlloc;
        if (!strcmp(name, "FlsGetValue")) return (void*)ntboxd_FlsGetValue;
        if (!strcmp(name, "FlsSetValue")) return (void*)ntboxd_FlsSetValue;
        if (!strcmp(name, "FlsFree")) return (void*)ntboxd_FlsFree;
        if (!strcmp(name, "GetCommandLineA")) return (void*)ntboxd_GetCommandLineA;
        if (!strcmp(name, "GetCommandLineW")) return (void*)ntboxd_GetCommandLineW;
        if (!strcmp(name, "GetEnvironmentStrings")) return (void*)ntboxd_GetEnvironmentStringsA;
        if (!strcmp(name, "GetEnvironmentStringsA")) return (void*)ntboxd_GetEnvironmentStringsA;
        if (!strcmp(name, "GetEnvironmentStringsW")) return (void*)ntboxd_GetEnvironmentStringsW;
        if (!strcmp(name, "FreeEnvironmentStringsA")) return (void*)ntboxd_FreeEnvironmentStringsA;
        if (!strcmp(name, "FreeEnvironmentStringsW")) return (void*)ntboxd_FreeEnvironmentStringsW;
    }
    
    /* Registry Operations */
    if (!strcasecmp(dll, "kernel32.dll") || !strcasecmp(dll, "advapi32.dll")) {
        if (!strcmp(name, "RegQueryValueExA")) return (void*)ntboxd_RegQueryValueExA;
        if (!strcmp(name, "RegQueryValueExW")) return (void*)ntboxd_RegQueryValueExW;
        if (!strcmp(name, "RegEnumValueA")) return (void*)ntboxd_RegEnumValueA;
        if (!strcmp(name, "RegEnumValueW")) return (void*)ntboxd_RegEnumValueW;
        if (!strcmp(name, "RegQueryInfoKeyA")) return (void*)ntboxd_RegQueryInfoKeyA;
        if (!strcmp(name, "RegQueryInfoKeyW")) return (void*)ntboxd_RegQueryInfoKeyW;
    }
    
    /* عام: دوال مستخدمة بكثرة عبر DLLs مختلفة */
    if (!strcmp(name,"BeginPaint")) { extern void *ntboxd_user32_BeginPaint(void*,void*); return ntboxd_user32_BeginPaint; }
    if (!strcmp(name,"EndPaint")) { extern int ntboxd_user32_EndPaint(void*,const void*); return ntboxd_user32_EndPaint; }
    if (!strcmp(name,"GetWindowTextW")) { extern int ntboxd_user32_GetWindowTextW(void*,void*,int); return ntboxd_user32_GetWindowTextW; }
    if (!strcmp(name,"GetWindowTextA")) { extern int ntboxd_user32_GetWindowTextA(void*,char*,int); return ntboxd_user32_GetWindowTextA; }
    if (!strcmp(name,"SetTimer")) { extern unsigned long ntboxd_user32_SetTimer(void*,unsigned long,unsigned int,void*); return ntboxd_user32_SetTimer; }
    if (!strcmp(name,"KillTimer")) { extern int ntboxd_user32_KillTimer(void*,unsigned long); return ntboxd_user32_KillTimer; }
    if (!strcmp(name,"TranslateMessage")) { extern int ntboxd_user32_TranslateMessage(const void*); return ntboxd_user32_TranslateMessage; }
    if (!strcmp(name,"FillRect")) return (void*)ntboxd_FillRect;
    if (!strcmp(name,"GetMessageA")) { extern int ntboxd_user32_GetMessageA(void*,void*,unsigned int,unsigned int); return ntboxd_user32_GetMessageA; }
    if (!strcmp(name,"DispatchMessageA")) { extern long ntboxd_user32_DispatchMessageA(const void*); return ntboxd_user32_DispatchMessageA; }
    if (!strcmp(name,"PostQuitMessage")) { extern void ntboxd_user32_PostQuitMessage(int); return ntboxd_user32_PostQuitMessage; }
    if (!strcmp(name,"BeginPaint")) return (void*)ntboxd_BeginPaint;
    if (!strcmp(name,"EndPaint")) return (void*)ntboxd_EndPaint;
    if (!strcmp(name,"GetWindowTextW")) return (void*)ntboxd_GetWindowTextW;
    if (!strcmp(name,"GetWindowTextA")) return (void*)ntboxd_GetWindowTextA;
    if (!strcmp(name,"SetWindowTextW")) return (void*)ntboxd_SetWindowTextW;
    if (!strcmp(name,"SetTimer")) return (void*)ntboxd_SetTimer;
    if (!strcmp(name,"KillTimer")) return (void*)ntboxd_KillTimer;
    if (!strcmp(name,"TranslateMessage")) return (void*)ntboxd_TranslateMessage;
    if (!strcmp(name,"DispatchMessageA")) return (void*)ntboxd_DispatchMessageA;
    if (!strcmp(name,"FillRect")) return (void*)ntboxd_FillRect;
    if (!strcmp(name,"GetMessageA")) return (void*)ntboxd_GetMessageA;
    if (!strcmp(name,"GetMessageW")) return (void*)ntboxd_GetMessageA;
    if (!strcmp(name,"PeekMessageA")) return (void*)ntboxd_GetMessageA;
    if (!strcmp(name,"PeekMessageW")) return (void*)ntboxd_GetMessageA;
    if (!strcmp(name,"DefWindowProcA")) return (void*)ntboxd_DefWindowProcA;
    if (!strcmp(name,"DefWindowProcW")) return (void*)ntboxd_DefWindowProcW;
    if (!strcmp(name,"PostQuitMessage")) return (void*)ntboxd_PostQuitMessage;
    if (!strcmp(name,"ShowWindow")) return (void*)ntboxd_ShowWindow;
    if (!strcmp(name,"UpdateWindow")) return (void*)ntboxd_UpdateWindow;
    if (!strcmp(name,"GetDC")) return (void*)ntboxd_GetDC;
    if (!strcmp(name,"ReleaseDC")) return (void*)ntboxd_ReleaseDC;

    
    {
        /* AUTO SWARM DLSYM RESOLVER */
        char sym[512];
        char d_clean[64];
        int idx=0; while(dll[idx] && idx<60) { d_clean[idx] = (dll[idx]=='.') ? '_' : dll[idx]; idx++; }
        d_clean[idx] = '\0';
        snprintf(sym, sizeof(sym), "ntboxd_swarm_%s_%s", d_clean, name);
        void *fn = dlsym(RTLD_DEFAULT, sym);
        if (fn) {
            return fn;
        }
    }
    fprintf(stderr, "UNRESOLVED %s!%s -> generic stub\n", dll, name);
    return (void *)ntboxd_generic_stub;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr,"usage: %s <file.exe>\n",argv[0]); return 1; }

    /*
     * T-063: ntdll الحقيقي + المصيدة (قبل تحميل التطبيق).
     * يُعطَّل بـ NTBOX_NO_REAL_NTDLL=1 (للتشخيص).
     */
    if (!getenv("NTBOX_NO_REAL_NTDLL")) {
        char root[1024];

        if (ntboxd_win7_file("Windows/System32", root, sizeof(root)) == 0)
            ntb0_path_set_root(root);
        ntboxd_load_real_modules();
    }

    FILE *f = fopen(argv[1],"rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f,0,SEEK_END); size_t sz=ftell(f); fseek(f,0,SEEK_SET);
    u8 *file = malloc(sz); fread(file,1,sz,f); fclose(f);

    u32 e_lfanew = *(u32*)(file+0x3C);
    u8 *nt = file+e_lfanew;
    u16 nsec = *(u16*)(nt+6);
    u32 optsize = *(u16*)(nt+20);
    u8 *opt = nt+24;
    u16 magic = *(u16*)opt;
    if (magic == 0x10b) {
        /* PE32 (32-bit) - route to SysWOW64/PE32 path */
        fprintf(stderr, "[ntboxd] detected PE32 (32-bit) - using SysWOW64 translation path\n");
        u32 entry_rva = *(u32*)(opt+16);
        u32 image_base32 = *(u32*)(opt+28);
        u32 size_of_image = *(u32*)(opt+56);
        u32 size_of_headers = *(u32*)(opt+60);
        u32 import_rva = *(u32*)(opt+112+8);
        u32 reloc_rva = *(u32*)(opt+112+5*8);
        u32 reloc_size = *(u32*)(opt+112+5*8+4);
        u8 *sections = opt + optsize;

        size_t pagesz = 4096;
        size_t allocsz = (size_of_image + pagesz - 1) & ~(pagesz-1);
        u8 *image = mmap(NULL, allocsz, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (image == MAP_FAILED) { perror("mmap"); free(file); return 1; }
        size_t hc = size_of_headers;
        if (hc > sz) hc = sz;
        if (hc > size_of_image) hc = size_of_image;
        memcpy(image, file, hc);
        for (size_t i = 0; i < nsec; i++) {
            u8 *sh = sections + i * 40;
            u32 vsize = *(u32 *)(sh + 8), vaddr = *(u32 *)(sh + 12);
            u32 rawsize = *(u32 *)(sh + 16), rawptr = *(u32 *)(sh + 20);
            u32 tocopy = rawsize;
            if (tocopy > vsize && vsize) tocopy = vsize;
            if (!rawptr || !tocopy) continue;
            if (rawptr + tocopy > sz) continue;
            if (vaddr + tocopy > size_of_image) continue;
            memcpy(image + vaddr, file + rawptr, tocopy);
        }

        u64 new_base = (u64)image;
        u64 delta = (u64)new_base - (u64)image_base32;
        fprintf(stderr, "[ntboxd] PE32: before reloc, delta=%llx\n", (unsigned long long)delta); fflush(stderr);

        /* relocations - PE32 uses HIGHLOW (type 3) primarily */
        if (delta && reloc_rva && reloc_size) {
            fprintf(stderr, "[ntboxd] PE32: doing reloc\n"); fflush(stderr);
            u8 *rel = image + reloc_rva;
            u32 rem = reloc_size;
            while (rem >= 8) {
                u32 page_rva = *(u32 *)rel, block_size = *(u32 *)(rel + 4);
                if (block_size < 8 || block_size > rem) break;
                u32 nent = (block_size - 8) / 2;
                u16 *ents = (u16 *)(rel + 8);
                for (u32 e = 0; e < nent; e++) {
                    u16 entry = ents[e];
                    u8 type = entry >> 12;
                    u32 off = entry & 0xFFF;
                    if (type == 0) continue;
                    if (type == 3) { /* IMAGE_REL_BASED_HIGHLOW */
                        u32 *t = (u32 *)(image + page_rva + off);
                        *t += (u32)delta;
                    }
                    else if (type == 10) { /* though rare on PE32 */
                        u64 *t = (u64 *)(image + page_rva + off);
                        *t += delta;
                    }
                }
                rel += block_size;
                rem -= block_size;
            }
            fprintf(stderr, "[ntboxd] PE32: reloc done\n"); fflush(stderr);
        }
        fprintf(stderr, "[ntboxd] PE32_AFTER_RELOC\n"); fflush(stderr);
        fprintf(stderr, "[ntboxd] PE32: after reloc block, import_rva=%x\n", import_rva);
        fflush(stderr);
        fprintf(stderr, "DEBUG1\n"); fflush(stderr);
        fprintf(stderr, "[ntboxd] PE32_BEFORE_IMPORTS\n"); fflush(stderr);
        if (import_rva) {
            u8 *imp = image + import_rva;
            while (1) {
                u32 orig_thunk = *(u32 *)imp, name_rva = *(u32 *)(imp + 12), first_thunk = *(u32 *)(imp + 16);
                if (!name_rva && !first_thunk && !orig_thunk) break;
                char *dll = name_rva ? (char *)(image + name_rva) : "?";
                fprintf(stderr, "[ntboxd] PE32 import: %s\n", dll);
                fflush(stderr);
                u32 *thunk = first_thunk ? (u32 *)(image + first_thunk) : NULL;
                u32 *orig = orig_thunk ? (u32 *)(image + orig_thunk) : thunk;
                if (thunk) {
                    size_t idx = 0;
                    while (1) {
                        u32 t = orig ? orig[idx] : thunk[idx];
                        if (!t) break;
                        void *resolved = NULL;
                        if (!(t & 0x80000000UL)) {
                            const char *name = (const char *)(image + t + 2);
                            resolved = resolve(dll, name);
                            fprintf(stderr, "resolved %s!%s -> %p\n", dll, name, resolved);
                            if (!resolved) { fprintf(stderr, "UNRESOLVED %s!%s\n", dll, name); resolved = (void *)ntboxd_generic_stub; }
                        }
                        thunk[idx] = (u32)(uintptr_t)resolved;
                        idx++;
                    }
                }
                imp += 20;
            }
        }
        fprintf(stderr, "[ntboxd] PE32: imports done\n");
        fflush(stderr);

        /* PEB/TEB scaffolding (basic) */
        fprintf(stderr, "[ntboxd] PE32_MARK1\n"); fflush(stderr);
        fprintf(stderr, "[ntboxd] PE32: before PEB/TEB setup, import_rva=%x\n", import_rva); fflush(stderr);
        {
            NTBOXD_PEB *peb = calloc(1, sizeof(NTBOXD_PEB));
            NTBOXD_TEB *teb = calloc(1, sizeof(NTBOXD_TEB));
            if (peb && teb) {
                teb->NtTibSelf = teb;
                teb->ProcessEnvironmentBlock = peb;
                teb->ThreadLocalStoragePointer = calloc(64, sizeof(void *));
                peb->ImageBaseAddress = (void *)image;
                if (syscall(SYS_arch_prctl, ARCH_SET_GS, (unsigned long)teb) == 0) {
                    ntboxd_current_teb = teb;
                    fprintf(stderr, "[ntboxd] PE32: PEB=%p TEB=%p GS set\n", (void *)peb, (void *)teb);
                } else {
                    fprintf(stderr, "[ntboxd] PE32: arch_prctl failed\n");
                }
            } else {
                fprintf(stderr, "[ntboxd] PE32: PEB/TEB alloc failed\n");
            }
        }
        fflush(stderr);

        fprintf(stderr, "[ntboxd] PE32: about to mprotect allocsz=%zu\n", allocsz);
        fflush(stderr);
        mprotect(image, allocsz, PROT_READ | PROT_WRITE | PROT_EXEC);
        fprintf(stderr, "[ntboxd] PE32: mprotect done, entry_rva=%x image=%p\n", entry_rva, (void *)image);
        fflush(stderr);
        fprintf(stderr, "[ntboxd] PE32: entry addr=%p\n", (void *)(image + entry_rva));
        fflush(stderr);
        typedef void (*ENTRY)(void);
        ENTRY entry = (ENTRY)(image + entry_rva);
        printf("[ntboxd] PE32 entry_rva=0x%x image=%p\n", entry_rva, (void *)image);
        fflush(stdout);
        fflush(stderr);
        entry();
        fprintf(stderr, "[ntboxd] PE32 entry returned\n");
        fflush(stderr);
        munmap(image, allocsz);
        free(file);
        return 0;
    } else if (magic != 0x20B) {
        fprintf(stderr, "not PE32+ or PE32\n");
        return 1;
    }
    u32 entry_rva = *(u32*)(opt+16);
    u64 image_base = *(u64*)(opt+24);
    u32 size_of_image = *(u32*)(opt+56);
    u32 size_of_headers = *(u32*)(opt+60);
    u32 import_rva = *(u32*)(opt+112+8);
    u32 reloc_rva = *(u32*)(opt+112+5*8);
    u32 reloc_size = *(u32*)(opt+112+5*8+4);
    u8 *sections = opt+optsize;

    size_t pagesz = 4096;
    size_t allocsz = (size_of_image + pagesz - 1) & ~(pagesz-1);
    u8 *image = mmap(NULL, allocsz, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    size_t hc = size_of_headers; if (hc>sz) hc=sz; if (hc>size_of_image) hc=size_of_image;
    memcpy(image,file,hc);
    for (size_t i=0;i<nsec;i++) {
        u8 *sh = sections + i*40;
        u32 vsize=*(u32*)(sh+8), vaddr=*(u32*)(sh+12), rawsize=*(u32*)(sh+16), rawptr=*(u32*)(sh+20);
        u32 tocopy = rawsize; if (tocopy>vsize && vsize) tocopy=vsize;
        if (!rawptr || !tocopy) continue;
        if (rawptr+tocopy>sz) continue;
        if (vaddr+tocopy>size_of_image) continue;
        memcpy(image+vaddr, file+rawptr, tocopy);
    }

    u64 new_base = (u64)image;
    u64 delta = new_base - image_base;

    /* relocations */
    if (delta && reloc_rva && reloc_size) {
        u8 *rel = image+reloc_rva; u32 rem = reloc_size;
        while (rem >= 8) {
            u32 page_rva=*(u32*)rel, block_size=*(u32*)(rel+4);
            if (block_size<8 || block_size>rem) break;
            u32 nent=(block_size-8)/2; u16 *ents=(u16*)(rel+8);
            for (u32 e=0;e<nent;e++) {
                u16 entry=ents[e]; u8 type=entry>>12; u32 off=entry&0xFFF;
                if (type==0) continue;
                if (type==10) { u64 *t=(u64*)(image+page_rva+off); *t+=delta; }
                else if (type==3) { u32 *t=(u32*)(image+page_rva+off); *t+=(u32)delta; }
            }
            rel += block_size; rem -= block_size;
        }
    }

    /* imports */
    if (import_rva) {
        u8 *imp = image+import_rva;
        while (1) {
            u32 orig_thunk=*(u32*)imp, name_rva=*(u32*)(imp+12), first_thunk=*(u32*)(imp+16);
            if (!name_rva && !first_thunk && !orig_thunk) break;
            char *dll = name_rva ? (char*)(image+name_rva) : "?";
            u64 *thunk = first_thunk ? (u64*)(image+first_thunk) : NULL;
            u64 *orig = orig_thunk ? (u64*)(image+orig_thunk) : thunk;
            if (thunk) {
                size_t idx=0;
                while (1) {
                    u64 t = orig?orig[idx]:thunk[idx];
                    if (!t) break;
                    void *resolved=NULL;
                    if (!(t & 0x8000000000000000ULL)) {
                        const char *name = (const char*)(image + (u32)t + 2);
                        resolved = resolve(dll,name);
                        fprintf(stderr,"resolved %s!%s -> %p\n",dll,name,resolved);
                        if (!resolved) { fprintf(stderr,"UNRESOLVED %s!%s\n",dll,name); resolved = (void *)ntboxd_generic_stub; }
                    }
                    thunk[idx]=(u64)resolved;
                    idx++;
                }
            }
            imp += 20;
        }
    }

    /* M6c.1: minimal PEB/TEB (user-mode scaffolding) */
    {
        NTBOXD_PEB *peb = calloc(1, sizeof(NTBOXD_PEB));
        NTBOXD_TEB *teb = calloc(1, sizeof(NTBOXD_TEB));
        if (peb && teb) {
            teb->NtTibSelf = teb;
            teb->ProcessEnvironmentBlock = peb;
            teb->ThreadLocalStoragePointer = calloc(64, sizeof(void *));
            peb->ImageBaseAddress = (void*)image;
            if (syscall(SYS_arch_prctl, ARCH_SET_GS, (unsigned long)teb) != 0) {
                perror("arch_prctl ARCH_SET_GS");
            } else {
                ntboxd_current_teb = teb;
                fprintf(stderr, "[ntboxd] M6c.1 PEB=%p TEB=%p GS set (PEB@GS:0x60)\n",
                        (void*)peb, (void*)teb);
            }
        }
        /* keep allocated until process exit */
        (void)peb; (void)teb;
    }

    /* make executable */
    mprotect(image, allocsz, PROT_READ|PROT_WRITE|PROT_EXEC);

    /* call entry */
    typedef void (*ENTRY)(void);
    ENTRY entry = (ENTRY)(image+entry_rva);
    printf("[ntboxd] entry_rva=0x%x size_of_image=%u image=%p\n", entry_rva, size_of_image, (void*)image);
    fprintf(stderr, "[ntboxd] calling entry at %p (entry_rva=0x%x)\n", (void*)(image+entry_rva), entry_rva);
    fflush(stderr);
    entry();
    fprintf(stderr, "[ntboxd] entry returned\n");
    fflush(stderr);

    munmap(image, allocsz); free(file);
    return 0;
}
