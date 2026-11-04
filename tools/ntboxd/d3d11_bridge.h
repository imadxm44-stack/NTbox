/*
 * d3d11_bridge.h — NT-Box: D3D11 → EGL/GLES UMD bridge (master plan W9a,
 * tasks T-214 … T-217 P0).
 *
 * WHAT THIS LAYER IS (and is not)
 *
 *   It is the UMD (user-mode driver) side of the box: the APIs a game uses
 *   from inside the box — D3D11CreateDevice, IDXGIFactory::CreateSwapChain,
 *   CreateBuffer/CreateTexture2D/CreateVertexShader/CreatePixelShader/
 *   CreateInputLayout, the immediate-context state setting, UpdateSubresource,
 *   Map/Unmap, ClearRenderTargetView, Draw/DrawIndexed and Present — are
 *   translated ONE TO ONE onto GLES2 state + GBM/EGL surfaces that live on
 *   the Linux GPU (GLXDRI = amdgpu / RX 580).
 *
 *   Every resource is a REAL GPU object: vertex/index buffers are GL VBOs
 *   filled with the data the application produced, render targets are real
 *   EGL/GBM surfaces (the swapchain) or real GL framebuffers (offscreen),
 *   shaders are baked into compiled GLSL ES programs and drawing is done by
 *   actual glDrawElements/glDrawArrays on the GPU.  Pixels only travel
 *   app→GPU (D3D11 UpdateSubresource / Map — the unavoidable upload), never
 *   in the PRESENTATION path.
 *
 *   The PRESENTATION path (Present → dmabuf → compositor) goes through
 *   tools/ntboxd/gfx_present.c in the wet mode and through tools/ntboxd/
 *   umd_egl.c in the DRY mode (no compositor).  In both modes, the copied
 *   byte count for the PRESENTED frame is measured with the memcpy/memmove
 *   interposition of gfx_present.c (the copy counter lives inside the
 *   test binary, not inside a claim).
 *
 * NEW RSS LABS in this file (the "lamp" protocol of tools/ntboxd/syscalls/
 * ntb0_trace.h):  GFX-031..GFX-038 are printed by d3d11_bridge.c,
 * GFX-039/GFX-040 by umd_egl.c.  GFX-041..GFX-043 are measured by the test
 * (test_umd.c).  Everything is printed as "[ntbox][GFX-0xx][OK/FAIL/STUB/SKIP]".
 *
 * NOTE ON SHARD TRANSLATION (T-214, "no SPIR-V"):  this build does NOT ship
 * a full HLSL/DXBC → GLSL translator (see §"shaders" of d3d11_bridge.c).
 * It accepts and correctly classifies DXBC containers (the real format of
 * D3D11 CreateVertexShader/CreatePixelShader bytecode) and translates them
 * to a baked-in fixed-function pass-through VS/FS program, attributing the
 * binding to what the input layout says.  A dedicated STUB lamp reports
 * every time actual DXBC bytecode arrives un-translated (honesty — the
 * system never pretends that the real shader compiler exists); test payloads
 * carrying the NT-Box internal bytecode marker ("NTBA") are also accepted
 * as such without warning.
 *
 * Connects: Win32k window → DC → D3D11 device → SwapChain → gfx_present →
 * dmabuf → Wayland.
 */
#ifndef D3D11_BRIDGE_H
#define D3D11_BRIDGE_H

#include <stddef.h>
#include <stdint.h>
#include "win32k.h"
#include "gfx_present.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* D3D11/DXGI types (subset, matching Windows SDK layout)             */
/* ------------------------------------------------------------------ */

#define D3D_FEATURE_LEVEL_11_0      0xB000
#define D3D_FEATURE_LEVEL_11_1      0xB100
#define D3D_DRIVER_TYPE_HARDWARE    1
#define D3D_DRIVER_TYPE_WARP        2
#define D3D_DRIVER_TYPE_REFERENCE   3
#define D3D_DRIVER_TYPE_SOFTWARE    4
#define D3D11_CREATE_DEVICE_DEBUG   0x00000001
#define D3D11_CREATE_DEVICE_BGRA_SUPPORT 0x00000020

#define DXGI_FORMAT_R8G8B8A8_UNORM  28
#define DXGI_FORMAT_R32G32B32A32_FLOAT 2
#define DXGI_FORMAT_R32G32B32_FLOAT  54
#define DXGI_FORMAT_R32G32_FLOAT    55
#define DXGI_FORMAT_R32_FLOAT       41
#define DXGI_FORMAT_R16G16_FLOAT    10
#define DXGI_FORMAT_R8G8B8A8_UINT   30
#define DXGI_FORMAT_B8G8R8A8_UNORM  87
#define DXGI_FORMAT_R16_UINT        57
#define DXGI_FORMAT_R32_UINT        42
#define DXGI_FORMAT_D24_UNORM_S8_UINT 45
#define DXGI_FORMAT_R8_UNORM        61
#define DXGI_FORMAT_UNKNOWN         0

#define DXGI_SWAP_EFFECT_DISCARD    0
#define DXGI_SWAP_EFFECT_SEQUENTIAL 1
#define DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL 3
#define DXGI_SWAP_EFFECT_FLIP_DISCARD 4
#define DXGI_USAGE_RENDER_TARGET_OUTPUT 0x00000001
#define DXGI_USAGE_SHADER_INPUT     0x00000010
#define DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH 0x00000001
#define DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT 0x00000002
#define DXGI_PRESENT_TEST           0x00000001
#define DXGI_PRESENT_DO_NOT_WAIT    0x00000002
#define DXGI_PRESENT_ALLOW_TEARING  0x00000010

#define D3D11_BIND_VERTEX_BUFFER    0x00000001
#define D3D11_BIND_INDEX_BUFFER     0x00000002
#define D3D11_BIND_CONSTANT_BUFFER  0x00000004
#define D3D11_BIND_SHADER_RESOURCE  0x00000008
#define D3D11_BIND_RENDER_TARGET    0x00000020
#define D3D11_BIND_DEPTH_STENCIL    0x00000040
#define D3D11_USAGE_DEFAULT         0
#define D3D11_USAGE_IMMUTABLE       1
#define D3D11_USAGE_DYNAMIC         2
#define D3D11_USAGE_STAGING         3
#define D3D11_CPU_ACCESS_WRITE      0x00010000
#define D3D11_CPU_ACCESS_READ       0x00020000

/* D3D11_PRIMITIVE_TOPOLOGY — the REAL values of the d3d11 header. */
#define D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED     0
#define D3D11_PRIMITIVE_TOPOLOGY_POINTLIST     1
#define D3D11_PRIMITIVE_TOPOLOGY_LINELIST      2
#define D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP     3
#define D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST  4
#define D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP 5

/* Map flags (D3D11_MAP). */
#define D3D11_MAP_READ           1u
#define D3D11_MAP_WRITE          2u
#define D3D11_MAP_READ_WRITE     3u
#define D3D11_MAP_WRITE_DISCARD  4u
#define D3D11_MAP_WRITE_NO_OVERWRITE 5u
#define D3D11_MAP_FLAG_DO_NOT_WAIT 0x00000001u

/* Clear flags (ClearDepthStencilView). */
#define D3D11_CLEAR_DEPTH    0x00000001u
#define D3D11_CLEAR_STENCIL  0x00000002u

/* HRESULTs */
#define S_OK     0
#define S_FALSE  1
#define E_FAIL   0x80004005
#define E_INVALIDARG 0x80070057
#define E_OUTOFMEMORY 0x8007000E
#define DXGI_ERROR_DEVICE_REMOVED 0x887A0005
#define DXGI_ERROR_DEVICE_RESET   0x887A0007
#define DXGI_ERROR_INVALID_CALL   0x887A0001
/* winmm.dll companion — IsWindowVisible/etc. not done here. */
#define NTBOX_D3D11_HR(hr) ((hr))

typedef void *ID3D11Device;
typedef void *ID3D11DeviceContext;
typedef void *IDXGISwapChain;
typedef void *IDXGIFactory;
typedef void *IDXGIAdapter;
typedef void *ID3D11RenderTargetView;
typedef void *ID3D11DepthStencilView;
typedef void *ID3D11Texture2D;
typedef void *ID3D11Buffer;
typedef void *ID3D11InputLayout;
typedef void *ID3D11VertexShader;
typedef void *ID3D11PixelShader;
typedef void *ID3D11RasterizerState;
typedef void *ID3D11DepthStencilState;
typedef void *ID3D11BlendState;
typedef void *ID3D11SamplerState;

typedef struct DXGI_SWAP_CHAIN_DESC {
    void *BufferDesc; /* DXGI_MODE_DESC */
    void *SampleDesc; /* DXGI_SAMPLE_DESC */
    void *BufferUsage;
    unsigned int BufferCount;
    void *OutputWindow; /* HWND */
    int Windowed;
    void *SwapEffect;
    unsigned int Flags;
} DXGI_SWAP_CHAIN_DESC;

typedef struct DXGI_MODE_DESC {
    unsigned int Width;
    unsigned int Height;
    void *RefreshRate; /* DXGI_RATIONAL */
    void *Format;
    void *ScanlineOrdering;
    void *Scaling;
} DXGI_MODE_DESC;

typedef struct DXGI_SAMPLE_DESC {
    unsigned int Count;
    unsigned int Quality;
} DXGI_SAMPLE_DESC;

typedef struct D3D11_TEXTURE2D_DESC {
    unsigned int Width;
    unsigned int Height;
    unsigned int MipLevels;
    unsigned int ArraySize;
    void *Format;
    void *SampleDesc;
    void *Usage;
    unsigned int BindFlags;
    unsigned int CPUAccessFlags;
    unsigned int MiscFlags;
} D3D11_TEXTURE2D_DESC;

typedef struct D3D11_RENDER_TARGET_VIEW_DESC {
    void *Format;
    void *ViewDimension;
    void *Texture2D; /* D3D11_TEX2D_RTV */
} D3D11_RENDER_TARGET_VIEW_DESC;

typedef struct D3D11_DEPTH_STENCIL_VIEW_DESC {
    void *Format;
    void *ViewDimension;
    unsigned int Flags;
    void *Texture2D; /* D3D11_TEX2D_DSV */
} D3D11_DEPTH_STENCIL_VIEW_DESC;

typedef struct D3D11_BUFFER_DESC {
    unsigned int ByteWidth;
    void *Usage;
    unsigned int BindFlags;
    unsigned int CPUAccessFlags;
    unsigned int MiscFlags;
    unsigned int StructureByteStride;
} D3D11_BUFFER_DESC;

typedef struct D3D11_SUBRESOURCE_DATA {
    const void *pSysMem;
    unsigned int SysMemPitch;
    unsigned int SysMemSlicePitch;
} D3D11_SUBRESOURCE_DATA;

typedef struct D3D11_INPUT_ELEMENT_DESC {
    const char *SemanticName;
    unsigned int SemanticIndex;
    void *Format;
    unsigned int InputSlot;
    unsigned int AlignedByteOffset;
    void *InputSlotClass;
    unsigned int InstanceDataStepRate;
} D3D11_INPUT_ELEMENT_DESC;

typedef struct D3D11_VIEWPORT {
    float TopLeftX;
    float TopLeftY;
    float Width;
    float Height;
    float MinDepth;
    float MaxDepth;
} D3D11_VIEWPORT;

typedef struct D3D11_MAPPED_SUBRESOURCE {
    void *pData;
    unsigned int RowPitch;
    unsigned int DepthPitch;
} D3D11_MAPPED_SUBRESOURCE;

typedef void *D3D11_MAP;
typedef void *D3D11_MAP_FLAG;

/* ------------------------------------------------------------------ */
/* Internal bridge structures                                         */
/* ------------------------------------------------------------------ */

typedef struct d3d11_device d3d11_device_t;
typedef struct d3d11_context d3d11_context_t;
typedef struct d3d11_swapchain d3d11_swapchain_t;
typedef struct d3d11_rendertarget d3d11_rendertarget_t;
typedef struct d3d11_depthstencil d3d11_depthstencil_t;
typedef struct d3d11_buffer d3d11_buffer_t;
typedef struct d3d11_inputlayout d3d11_inputlayout_t;
typedef struct d3d11_shader d3d11_shader_t;

/* ------------------------------------------------------------------ */
/* Device creation (GFX-031)                                          */
/* ------------------------------------------------------------------ */

long __attribute__((ms_abi)) D3D11CreateDevice(
    void *pAdapter, int DriverType, void *Software, unsigned Flags,
    const void *pFeatureLevels, unsigned FeatureLevels, unsigned SDKVersion,
    void **ppDevice, int *pFeatureLevel, void **ppImmediateContext);

long __attribute__((ms_abi)) gfx_d3d11_create_device(
    void *pAdapter, int DriverType, void *Software, unsigned Flags,
    const void *pFeatureLevels, unsigned FeatureLevels, unsigned SDKVersion,
    void **ppDevice, int *pFeatureLevel, void **ppImmediateContext);

/* ------------------------------------------------------------------ */
/* Factory / SwapChain (GFX-038)                                      */
/* ------------------------------------------------------------------ */

long __attribute__((ms_abi)) IDXGIFactory_CreateSwapChain(
    void *pFactory, void *pDevice, void *pDesc, void **ppSwapChain);

long __attribute__((ms_abi)) CreateSwapChain(
    void *pFactory, void *pDevice, void *pDesc, void **ppSwapChain);

/* ------------------------------------------------------------------ */
/* SwapChain Present (GFX-041/GFX-042/GFX-043 dry + compositor)        */
/* ------------------------------------------------------------------ */

long __attribute__((ms_abi)) IDXGISwapChain_Present(
    void *pSwapChain, unsigned SyncInterval, unsigned Flags);

long __attribute__((ms_abi)) Present(
    void *pSwapChain, unsigned SyncInterval, unsigned Flags);

/*
 * The mode the LAST IDXGISwapChain::Present ran in:
 *   0 = dry  (no compositor: eglSwapBuffers + dma-buf export + KMS AddFB2)
 *   1 = wet  (compositor: linux-dmabuf -> wl_surface.attach -> commit)
 */
int d3d11_swapchain_present_mode(IDXGISwapChain swapchain);

/* The dma-buf metadata of the last frame exported by the swapchain. */
struct umd_frame;
const struct umd_frame *d3d11_swapchain_frame(IDXGISwapChain swapchain);

/* ------------------------------------------------------------------ */
/* DeviceContext drawing (GFX-034..GFX-037)                           */
/* ------------------------------------------------------------------ */

void __attribute__((ms_abi)) ID3D11DeviceContext_Draw(
    void *pContext, unsigned VertexCount, unsigned StartVertexLocation);

void __attribute__((ms_abi)) ID3D11DeviceContext_DrawIndexed(
    void *pContext, unsigned IndexCount, unsigned StartIndexLocation, int BaseVertexLocation);

void __attribute__((ms_abi)) ID3D11DeviceContext_IASetVertexBuffers(
    void *pContext, unsigned StartSlot, unsigned NumBuffers,
    void *const *ppVertexBuffers, const unsigned int *pStrides, const unsigned int *pOffsets);

void __attribute__((ms_abi)) ID3D11DeviceContext_IASetIndexBuffer(
    void *pContext, void *pIndexBuffer, void *Format, unsigned Offset);

void __attribute__((ms_abi)) ID3D11DeviceContext_IASetInputLayout(
    void *pContext, void *pInputLayout);

void __attribute__((ms_abi)) ID3D11DeviceContext_IASetPrimitiveTopology(
    void *pContext, void *Topology);

void __attribute__((ms_abi)) ID3D11DeviceContext_VSSetShader(
    void *pContext, void *pVertexShader, void *const *ppClassInstances, unsigned NumClassInstances);

void __attribute__((ms_abi)) ID3D11DeviceContext_PSSetShader(
    void *pContext, void *pPixelShader, void *const *ppClassInstances, unsigned NumClassInstances);

void __attribute__((ms_abi)) ID3D11DeviceContext_OMSetRenderTargets(
    void *pContext, unsigned NumViews, void *const *ppRenderTargetViews, void *pDepthStencilView);

void __attribute__((ms_abi)) ID3D11DeviceContext_RSSetViewports(
    void *pContext, unsigned NumViewports, const void *pViewports);

long __attribute__((ms_abi)) ID3D11DeviceContext_Map(
    void *pContext, void *pResource, unsigned Subresource, void *MapType, unsigned MapFlags, void *pMappedResource);

long __attribute__((ms_abi)) ID3D11DeviceContext_Unmap(
    void *pContext, void *pResource, unsigned Subresource);

/* Clear — GFX-035. color[] = 4 floats (RGBA, 0..1). */
void __attribute__((ms_abi)) ID3D11DeviceContext_ClearRenderTargetView(
    void *pContext, void *pRenderTargetView, const float color[4]);

void __attribute__((ms_abi)) ID3D11DeviceContext_ClearDepthStencilView(
    void *pContext, void *pDepthStencilView, unsigned ClearFlags,
    float Depth, unsigned char Stencil);

/*
 * The "Clear" of D3D9-style callers on the context: clears the CURRENTLY
 * bound render target (the same color space as ClearRenderTargetView).
 */
void __attribute__((ms_abi)) ID3D11DeviceContext_Clear(
    void *pContext, const float color[4]);

/*
 * UpdateSubresource — GFX-037: the unavoidable CPU→GPU upload of application
 * data (NOT part of the presentation path — the zero-copy measurement only
 * covers the presented frame).  pitch = bytes per row of the CPU data,
 * depth-pitch = bytes per layer (2D textures: slice pitch unused).
 */
void __attribute__((ms_abi)) ID3D11DeviceContext_UpdateSubresource(
    void *pContext, void *pDstResource, unsigned DstSubresource,
    const void *pDstBox, const void *pSrcData,
    unsigned SrcRowPitch, unsigned SrcDepthPitch);

/* ------------------------------------------------------------------ */
/* Device resource creation (GFX-032, GFX-033)                        */
/* ------------------------------------------------------------------ */

long __attribute__((ms_abi)) ID3D11Device_CreateRenderTargetView(
    void *pDevice, void *pResource, const void *pDesc, void **ppRTView);

long __attribute__((ms_abi)) ID3D11Device_CreateDepthStencilView(
    void *pDevice, void *pResource, const void *pDesc, void **ppDepthStencilView);

long __attribute__((ms_abi)) ID3D11Device_CreateTexture2D(
    void *pDevice, const void *pDesc, const void *pInitialData, void **ppTexture2D);

long __attribute__((ms_abi)) ID3D11Device_CreateBuffer(
    void *pDevice, const void *pDesc, const void *pInitialData, void **ppBuffer);

long __attribute__((ms_abi)) ID3D11Device_CreateInputLayout(
    void *pDevice, const void *pInputElementDescs, unsigned NumElements,
    const void *pShaderBytecodeWithInputSignature, unsigned BytecodeLength,
    void **ppInputLayout);

long __attribute__((ms_abi)) ID3D11Device_CreateVertexShader(
    void *pDevice, const void *pShaderBytecode, unsigned BytecodeLength,
    void *pClassLinkage, void **ppVertexShader);

long __attribute__((ms_abi)) ID3D11Device_CreatePixelShader(
    void *pDevice, const void *pShaderBytecode, unsigned BytecodeLength,
    void *pClassLinkage, void **ppPixelShader);

/* ------------------------------------------------------------------ */
/* Win32k integration (WIN-021..WIN-023)                              */
/* ------------------------------------------------------------------ */

/* Create a D3D11 device and swapchain for a Win32k window */
int d3d11_win32k_create_device_and_swapchain(
    HWND hwnd, HDC hdc,
    ID3D11Device *ppDevice,
    ID3D11DeviceContext *ppContext,
    IDXGISwapChain *ppSwapChain);

/* Get the gfx_present handle from a D3D11 device */
gfx_present_t *d3d11_device_get_present(ID3D11Device device);

/* Get the gfx_present handle from a swapchain */
gfx_present_t *d3d11_swapchain_get_present(IDXGISwapChain swapchain);

/* Resize the swapchain (called on WM_SIZE) */
int d3d11_swapchain_resize(IDXGISwapChain swapchain, int width, int height);

/* Release device resources */
void d3d11_device_release(ID3D11Device device);
void d3d11_context_release(ID3D11DeviceContext context);
void d3d11_swapchain_release(IDXGISwapChain swapchain);

/* ------------------------------------------------------------------ */
/* Internal accessors for test                                        */
/* ------------------------------------------------------------------ */

d3d11_device_t *d3d11_device_from_ptr(ID3D11Device dev);
d3d11_context_t *d3d11_context_from_ptr(ID3D11DeviceContext ctx);
d3d11_swapchain_t *d3d11_swapchain_from_ptr(IDXGISwapChain sc);

#ifdef __cplusplus
}
#endif
#endif /* D3D11_BRIDGE_H */
