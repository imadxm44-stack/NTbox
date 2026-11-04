/*
 * gfx_bridge.h
 */
#ifndef GFX_BRIDGE_H
#define GFX_BRIDGE_H
#include <stddef.h>
#include <stdint.h>
#include "gfx_present.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct gfx_bridge_device gfx_bridge_device_t;
typedef struct gfx_bridge_swapchain gfx_bridge_swapchain_t;
int gfx_bridge_init(void);
void gfx_bridge_shutdown(void);
const char *gfx_bridge_error(void);
const char *gfx_bridge_adapter_name(const gfx_bridge_device_t *dev);
int gfx_bridge_adapter_vendor_id(const gfx_bridge_device_t *dev);
int gfx_bridge_adapter_device_id(const gfx_bridge_device_t *dev);
const char *gfx_bridge_adapter_driver(const gfx_bridge_device_t *dev);
gfx_present_t *gfx_bridge_device_present(gfx_bridge_device_t *dev);
int gfx_bridge_dmabuf_fd(gfx_bridge_device_t *dev);
unsigned long gfx_bridge_copies(gfx_bridge_device_t *dev);
unsigned long gfx_bridge_copies_last(gfx_bridge_device_t *dev);
gfx_present_t *gfx_bridge_get_present(gfx_bridge_device_t *dev);
int gfx_bridge_get_dmabuf_fd(gfx_bridge_device_t *dev);
unsigned long gfx_bridge_get_copies(gfx_bridge_device_t *dev);
unsigned long gfx_bridge_copy_count(gfx_bridge_device_t *dev);
gfx_bridge_device_t *gfx_bridge_device_create(void);
gfx_bridge_swapchain_t *gfx_bridge_swapchain_create(gfx_bridge_device_t *dev, int w, int h);
int gfx_bridge_swapchain_present(gfx_bridge_swapchain_t *sc, unsigned sync_interval, unsigned flags);
int gfx_bridge_present(gfx_bridge_swapchain_t *sc, unsigned sync_interval, unsigned flags);
void gfx_bridge_swapchain_destroy(gfx_bridge_swapchain_t *sc);
void gfx_bridge_device_destroy(gfx_bridge_device_t *dev);
int gfx_bridge_vk_create_instance(const void *create_info, const void *allocator, void **instance);
int gfx_bridge_vk_queue_present(const void *queue, const void *present_info);
void *gfx_bridge_wgl_create_context(void *hdc);
int gfx_bridge_wgl_swap_buffers(void *hdc);
long __attribute__((ms_abi)) D3D11CreateDevice(void *pAdapter, int DriverType, void *Software, unsigned Flags, const void *pFeatureLevels, unsigned FeatureLevels, unsigned SDKVersion, void **ppDevice, int *pFeatureLevel, void **ppImmediateContext);
long __attribute__((ms_abi)) gfx_bridge_D3D11CreateDevice(void *pAdapter, int DriverType, void *Software, unsigned Flags, const void *pFeatureLevels, unsigned FeatureLevels, unsigned SDKVersion, void **ppDevice, int *pFeatureLevel, void **ppImmediateContext);
long __attribute__((ms_abi)) IDXGIFactory_CreateSwapChain(void *pFactory, void *pDevice, void *pDesc, void **ppSwapChain);
long __attribute__((ms_abi)) CreateSwapChain(void *pFactory, void *pDevice, void *pDesc, void **ppSwapChain);
long __attribute__((ms_abi)) IDXGISwapChain_Present(void *pSwapChain, unsigned SyncInterval, unsigned Flags);
long __attribute__((ms_abi)) Present(void *pSwapChain, unsigned SyncInterval, unsigned Flags);
long __attribute__((ms_abi)) vkCreateInstance(const void *pCreateInfo, const void *pAllocator, void **pInstance);
long __attribute__((ms_abi)) vkQueuePresentKHR(void *queue, const void *pPresentInfo);
void * __attribute__((ms_abi)) wglCreateContext(void *hdc);
int __attribute__((ms_abi)) wglSwapBuffers(void *hdc);
#ifdef __cplusplus
}
#endif
#endif