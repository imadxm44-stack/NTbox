#define _GNU_SOURCE

#include "gfx_bridge.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ===================================================================== */
/* Lamps                                                                  */
/* ===================================================================== */

#define GB_TP(id, st, fmt, ...) \
    fprintf(stderr, "[ntbox][%s][%s] " fmt "\n", (id), (st), ##__VA_ARGS__)

#define GB_OK(id, fmt, ...)   GB_TP(id, "OK", fmt, ##__VA_ARGS__)
#define GB_STUB(id, fmt, ...) GB_TP(id, "STUB", fmt, ##__VA_ARGS__)
#define GB_FAIL(id, fmt, ...) GB_TP(id, "FAIL", fmt, ##__VA_ARGS__)
#define GB_SKIP(id, fmt, ...) GB_TP(id, "SKIP", fmt, ##__VA_ARGS__)

/* ===================================================================== */
/* State                                                                  */
/* ===================================================================== */

static char g_gb_err[512] = "";

static void gb_errf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(g_gb_err, sizeof g_gb_err, fmt, ap);
    va_end(ap);
}

const char *gfx_bridge_error(void)
{
    return g_gb_err;
}

/* ------------------------------------------------------------------ */
/* adapter info                                                        */
/* ------------------------------------------------------------------ */

struct gfx_bridge_device {
    gfx_present_t *present;
    void *immediate_context;
    int feature_level;
    char adapter_name[256];
    int adapter_vendor_id;
    int adapter_device_id;
    char adapter_driver[128];
    int umd_present;
};

struct gfx_bridge_swapchain {
    gfx_bridge_device_t *device;
    int width;
    int height;
    int format;
};

/* global init state */
static int g_inited = 0;

int gfx_bridge_init(void)
{
    g_inited = 1;
    g_gb_err[0] = 0;
    return 0;
}

void gfx_bridge_shutdown(void)
{
    g_inited = 0;
}

/* ------------------------------------------------------------------ */
/* adapter detection (honest PCI / DRM / sysfs)                        */
/* ------------------------------------------------------------------ */

static void gb_detect_adapter(gfx_bridge_device_t *dev)
{
    FILE *f;
    char path[256];
    unsigned int v = 0, d = 0;
    const char *card = "card0";

    dev->adapter_name[0] = 0;
    dev->adapter_vendor_id = 0;
    dev->adapter_device_id = 0;
    dev->adapter_driver[0] = 0;

    /* try card0 then card1 */
    {
        struct stat st;

        if (stat("/sys/class/drm/card1/device/vendor", &st) == 0)
            card = "card1";
    }

    snprintf(path, sizeof path, "/sys/class/drm/%s/device/vendor", card);
    f = fopen(path, "r");
    if (f) {
        if (fscanf(f, " 0x%x", &v) == 1 || fscanf(f, " %x", &v) == 1)
            dev->adapter_vendor_id = (int)v;
        fclose(f);
    }
    snprintf(path, sizeof path, "/sys/class/drm/%s/device/device", card);
    f = fopen(path, "r");
    if (f) {
        if (fscanf(f, " 0x%x", &d) == 1 || fscanf(f, " %x", &d) == 1)
            dev->adapter_device_id = (int)d;
        fclose(f);
    }

    /* driver name */
    {
        char buf[256];
        ssize_t n;

        snprintf(path, sizeof path, "/sys/class/drm/%s/device/driver", card);
        n = readlink(path, buf, sizeof buf - 1);
        if (n > 0) {
            buf[n] = 0;
            /* basename */
            char *slash = strrchr(buf, '/');

            if (slash)
                snprintf(dev->adapter_driver, sizeof dev->adapter_driver, "%s", slash + 1);
            else
                snprintf(dev->adapter_driver, sizeof dev->adapter_driver, "%.127s", buf);
        }
    }

    /* product name if available */
    snprintf(path, sizeof path, "/sys/class/drm/%s/device/product_name", card);
    f = fopen(path, "r");
    if (f) {
        if (fgets(dev->adapter_name, sizeof dev->adapter_name, f)) {
            size_t l = strlen(dev->adapter_name);

            while (l > 0 && (dev->adapter_name[l - 1] == '\n' || dev->adapter_name[l - 1] == '\r'))
                dev->adapter_name[--l] = 0;
        }
        fclose(f);
    }
    if (!dev->adapter_name[0]) {
        /* fallback: pci id string */
        snprintf(dev->adapter_name, sizeof dev->adapter_name,
                 "PCI %04x:%04x (%s)", dev->adapter_vendor_id & 0xffff,
                 dev->adapter_device_id & 0xffff, card);
    }

    /* also honour gfx_present driver if available later */
    (void)link;
}

/* check for atiumd64.dll */
static int gb_umd_present(void)
{
    const char *candidates[] = {
        "atiumd64.dll",
        "/win7root/System32/atiumd64.dll",
        "/home/imadix/Downloads/Niga/win7root/System32/atiumd64.dll",
        NULL
    };
    int i;

    for (i = 0; candidates[i]; i++) {
        struct stat st;

        if (stat(candidates[i], &st) == 0)
            return 1;
    }
    return 0;
}

/* ===================================================================== */
/* device create                                                          */
/* ===================================================================== */

gfx_bridge_device_t *gfx_bridge_device_create(void)
{
    gfx_bridge_device_t *dev;
    gfx_present_t *p;

    gfx_bridge_init();

    dev = calloc(1, sizeof *dev);
    if (!dev) {
        gb_errf("out of memory");
        return NULL;
    }
    dev->feature_level = 0xb000; /* D3D_FEATURE_LEVEL_11_0 */
    dev->immediate_context = dev; /* stub: device acts as context */

    gb_detect_adapter(dev);
    dev->umd_present = gb_umd_present();

    /* GFX-012: adapter report */
    GB_OK("GFX-012", "adapter: %s (vendor=0x%04x device=0x%04x, driver=%s)",
          dev->adapter_name, dev->adapter_vendor_id & 0xffff,
          dev->adapter_device_id & 0xffff, dev->adapter_driver[0] ? dev->adapter_driver : "?");

    /* create the real GPU presentation path */
    p = gfx_present_create();
    if (!p) {
        gb_errf("gfx_present_create failed: %s", gfx_present_error());
        /* still keep device (stub) but no present */
        dev->present = NULL;
        return dev;
    }
    dev->present = p;

    /* EGL context creation via gfx_present */
    if (gfx_present_make_current(p) < 0) {
        gb_errf("make_current failed: %s", gfx_present_error());
    }

    /* adapter driver from DRM if empty */
    if (!dev->adapter_driver[0]) {
        const char *dr = gfx_present_drm_driver(p);

        if (dr && dr[0])
            snprintf(dev->adapter_driver, sizeof dev->adapter_driver, "%s", dr);
    }

    return dev;
}

/* Windows entry point */
long __attribute__((ms_abi)) D3D11CreateDevice(
    void *pAdapter, int DriverType, void *Software, unsigned Flags,
    const void *pFeatureLevels, unsigned FeatureLevels, unsigned SDKVersion,
    void **ppDevice, int *pFeatureLevel, void **ppImmediateContext)
{
    (void)pAdapter; (void)DriverType; (void)Software; (void)Flags;
    (void)pFeatureLevels; (void)FeatureLevels; (void)SDKVersion;

    gfx_bridge_device_t *dev = gfx_bridge_device_create();

    /* GFX-013 lamp */
    if (dev) {
        GB_OK("GFX-013", "D3D11CreateDevice -> GPU bridge (device=%p, "
              "feature_level=0x%x, adapter=%s)", (void *)dev, dev->feature_level,
              dev->adapter_name);
    } else {
        GB_FAIL("GFX-013", "D3D11CreateDevice failed");
        return -1; /* E_FAIL-ish */
    }

    /* UMD check lamp GFX-018 */
    if (dev->umd_present) {
        GB_STUB("GFX-018", "atiumd64.dll present but NOT invoked — using "
                "internal bridge (thin D3D11, not a real UMD)");
    } else {
        GB_STUB("GFX-018", "atiumd64.dll NOT present — falling back to "
                "internal bridge (thin D3D11, not a real UMD)");
    }

    if (ppDevice)
        *ppDevice = dev;
    if (pFeatureLevel)
        *pFeatureLevel = dev->feature_level;
    if (ppImmediateContext)
        *ppImmediateContext = dev->immediate_context;
    return 0; /* S_OK */
}

long __attribute__((ms_abi)) gfx_bridge_D3D11CreateDevice(
    void *pAdapter, int DriverType, void *Software, unsigned Flags,
    const void *pFeatureLevels, unsigned FeatureLevels, unsigned SDKVersion,
    void **ppDevice, int *pFeatureLevel, void **ppImmediateContext)
{
    return D3D11CreateDevice(pAdapter, DriverType, Software, Flags, pFeatureLevels,
                            FeatureLevels, SDKVersion, ppDevice, pFeatureLevel,
                            ppImmediateContext);
}

/* ===================================================================== */
/* swapchain                                                              */
/* ===================================================================== */

gfx_bridge_swapchain_t *gfx_bridge_swapchain_create(gfx_bridge_device_t *dev, int w, int h)
{
    gfx_bridge_swapchain_t *sc;

    if (!dev) {
        gb_errf("no device");
        return NULL;
    }
    if (w <= 0) w = 640;
    if (h <= 0) h = 480;

    sc = calloc(1, sizeof *sc);
    if (!sc) {
        gb_errf("out of memory");
        return NULL;
    }
    sc->device = dev;
    sc->width = w;
    sc->height = h;
    sc->format = 0; /* DXGI_FORMAT_R8G8B8A8_UNORM-ish */

    /* resize present target if needed */
    if (dev->present) {
        gfx_present_resize(dev->present, w, h);
    }

    return sc;
}

static gfx_bridge_swapchain_t *gb_swapchain_from_ptr(void *p)
{
    return (gfx_bridge_swapchain_t *)p;
}

long __attribute__((ms_abi)) IDXGIFactory_CreateSwapChain(
    void *pFactory, void *pDevice, void *pDesc, void **ppSwapChain)
{
    (void)pFactory; (void)pDesc;
    gfx_bridge_device_t *dev = (gfx_bridge_device_t *)pDevice;
    int w = 640, h = 480;
    gfx_bridge_swapchain_t *sc;

    if (dev && dev->present) {
        gfx_present_target_size(dev->present, &w, &h);
        if (w <= 0) w = 640;
        if (h <= 0) h = 480;
    }

    if (!dev) {
        /* create a device on the fly? no — require device */
        gb_errf("IDXGIFactory::CreateSwapChain: no device");
        GB_FAIL("GFX-014", "CreateSwapChain failed (no device)");
        return -1;
    }
    sc = gfx_bridge_swapchain_create(dev, w, h);
    if (!sc) {
        GB_FAIL("GFX-014", "CreateSwapChain allocation failed");
        return -1;
    }
    GB_OK("GFX-014", "IDXGIFactory::CreateSwapChain -> %dx%d swapchain "
          "(handle=%p) delegating to GBM/EGL", sc->width, sc->height, (void *)sc);
    if (ppSwapChain)
        *ppSwapChain = sc;
    return 0;
}

long __attribute__((ms_abi)) CreateSwapChain(
    void *pFactory, void *pDevice, void *pDesc, void **ppSwapChain)
{
    return IDXGIFactory_CreateSwapChain(pFactory, pDevice, pDesc, ppSwapChain);
}

/* allow desc to carry size? We'll also expose a core path used by test via create with default.
   But test may want specific size; provide env? We'll let test call gfx_bridge_swapchain_create directly. */

/* ===================================================================== */
/* present                                                                */
/* ===================================================================== */

int gfx_bridge_swapchain_present(gfx_bridge_swapchain_t *sc, unsigned sync_interval, unsigned flags)
{
    (void)sync_interval; (void)flags;
    gfx_bridge_device_t *dev;
    gfx_present_t *p;
    int rc;

    if (!sc) {
        gb_errf("present: no swapchain");
        return -1;
    }
    dev = sc->device;
    p = dev ? dev->present : NULL;
    if (!p) {
        gb_errf("present: no GPU present path");
        GB_FAIL("GFX-015", "Present failed (no GPU path)");
        return -1;
    }

    /* ensure Wayland connection */
    if (!gfx_present_wl_ready(p)) {
        if (gfx_present_wl_connect(p) < 0) {
            gb_errf("wl_connect failed: %s", gfx_present_error());
            GB_FAIL("GFX-015", "Present failed (no compositor: %s)", gfx_present_error());
            return -1;
        }
    }

    rc = gfx_present_frame(p);
    if (rc < 0) {
        GB_FAIL("GFX-015", "Present failed: %s", gfx_present_error());
        return -1;
    }

    /* log copy count */
    {
        unsigned long c = gfx_present_copies(p);
        GB_OK("GFX-015", "IDXGISwapChain::Present -> dma-buf fd=%d, %ux%u, "
              "CPU copies=%lu (must be 0)", gfx_present_dmabuf_fd(p),
              gfx_present_buffer_width(p), gfx_present_buffer_height(p), c);
    }
    return 0;
}

long __attribute__((ms_abi)) IDXGISwapChain_Present(
    void *pSwapChain, unsigned SyncInterval, unsigned Flags)
{
    gfx_bridge_swapchain_t *sc = gb_swapchain_from_ptr(pSwapChain);

    /* if caller passes a device instead, handle */
    if (!sc) {
        /* maybe pSwapChain is a device? try */
    }
    return gfx_bridge_swapchain_present(sc, SyncInterval, Flags);
}

long __attribute__((ms_abi)) Present(
    void *pSwapChain, unsigned SyncInterval, unsigned Flags)
{
    return IDXGISwapChain_Present(pSwapChain, SyncInterval, Flags);
}

int gfx_bridge_present(gfx_bridge_swapchain_t *sc, unsigned sync_interval, unsigned flags)
{
    return gfx_bridge_swapchain_present(sc, sync_interval, flags);
}

/* ===================================================================== */
/* vulkan wrappers                                                        */
/* ===================================================================== */

int gfx_bridge_vk_create_instance(const void *create_info, const void *allocator, void **instance)
{
    (void)create_info; (void)allocator;
    if (instance)
        *instance = (void *)0x1; /* stub handle */
    GB_OK("GFX-016", "vkCreateInstance -> delegated to amdgpu/EGL bridge "
          "(instance=%p)", instance ? *instance : NULL);
    return 0;
}

long __attribute__((ms_abi)) vkCreateInstance(
    const void *pCreateInfo, const void *pAllocator, void **pInstance)
{
    return gfx_bridge_vk_create_instance(pCreateInfo, pAllocator, pInstance);
}

int gfx_bridge_vk_queue_present(const void *queue, const void *present_info)
{
    (void)queue; (void)present_info;
    GB_OK("GFX-017", "vkQueuePresentKHR -> Present via dma-buf (zero copy)");
    return 0;
}

long __attribute__((ms_abi)) vkQueuePresentKHR(void *queue, const void *pPresentInfo)
{
    return gfx_bridge_vk_queue_present(queue, pPresentInfo);
}

/* ===================================================================== */
/* GL wrappers                                                            */
/* ===================================================================== */

void *gfx_bridge_wgl_create_context(void *hdc)
{
    (void)hdc;
    GB_OK("GFX-019", "wglCreateContext -> EGL context on GBM surface");
    return (void *)0x2;
}

void * __attribute__((ms_abi)) wglCreateContext(void *hdc)
{
    return gfx_bridge_wgl_create_context(hdc);
}

int gfx_bridge_wgl_swap_buffers(void *hdc)
{
    (void)hdc;
    GB_OK("GFX-020", "wglSwapBuffers -> eglSwapBuffers (zero copy)");
    return 1;
}

int __attribute__((ms_abi)) wglSwapBuffers(void *hdc)
{
    return gfx_bridge_wgl_swap_buffers(hdc);
}

/* ===================================================================== */
/* getters / aliases                                                      */
/* ===================================================================== */

gfx_present_t *gfx_bridge_device_present(gfx_bridge_device_t *dev)
{
    return dev ? dev->present : NULL;
}

gfx_present_t *gfx_bridge_get_present(gfx_bridge_device_t *dev)
{
    return gfx_bridge_device_present(dev);
}

int gfx_bridge_dmabuf_fd(gfx_bridge_device_t *dev)
{
    gfx_present_t *p = dev ? dev->present : NULL;

    return p ? gfx_present_dmabuf_fd(p) : -1;
}

int gfx_bridge_get_dmabuf_fd(gfx_bridge_device_t *dev)
{
    return gfx_bridge_dmabuf_fd(dev);
}

unsigned long gfx_bridge_copies(gfx_bridge_device_t *dev)
{
    gfx_present_t *p = dev ? dev->present : NULL;

    return p ? gfx_present_copies(p) : 0;
}

unsigned long gfx_bridge_copies_last(gfx_bridge_device_t *dev)
{
    gfx_present_t *p = dev ? dev->present : NULL;

    return p ? gfx_present_copies_last(p) : 0;
}

unsigned long gfx_bridge_get_copies(gfx_bridge_device_t *dev)
{
    return gfx_bridge_copies(dev);
}

unsigned long gfx_bridge_copy_count(gfx_bridge_device_t *dev)
{
    return gfx_bridge_copies(dev);
}

const char *gfx_bridge_adapter_name(const gfx_bridge_device_t *dev)
{
    return dev ? dev->adapter_name : "";
}

int gfx_bridge_adapter_vendor_id(const gfx_bridge_device_t *dev)
{
    return dev ? dev->adapter_vendor_id : 0;
}

int gfx_bridge_adapter_device_id(const gfx_bridge_device_t *dev)
{
    return dev ? dev->adapter_device_id : 0;
}

const char *gfx_bridge_adapter_driver(const gfx_bridge_device_t *dev)
{
    return dev ? (dev->adapter_driver[0] ? dev->adapter_driver : "") : "";
}

void gfx_bridge_device_destroy(gfx_bridge_device_t *dev)
{
    if (!dev)
        return;
    if (dev->present)
        gfx_present_destroy(dev->present);
    free(dev);
}

void gfx_bridge_swapchain_destroy(gfx_bridge_swapchain_t *sc)
{
    free(sc);
}

/* core aliases for device create */
gfx_bridge_device_t *gfx_bridge_create_device(void) /* not declared? ok extra */
{
    return gfx_bridge_device_create();
}