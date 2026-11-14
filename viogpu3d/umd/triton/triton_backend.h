/*
 * Copyright 2026 Turing Software LLC
 * Copyright 2026 OSMates contributors
 * SPDX-License-Identifier: MIT
 */

// What Triton needs from the stack below it: the D3D11 device it forwards to, and shared and scanout textures.
// triton_backend_null.c links and makes nothing. Include through triton.h.

#ifndef TRITON_BACKEND_H_INCLUDED
#define TRITON_BACKEND_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

void triton_backend_log(const char *line);

#define TRITON_DEBUG_PRESENT_TIMING (1u << 0)

BOOL triton_backend_debug(UINT flag);
int triton_backend_present_gate_trace_ms(void);

// Shader rewrites a host compiler needs.
#define TRITON_WA_WIDEN_SCALAR_VS_INPUT_MASK       (1u << 0)
#define TRITON_WA_TYPE_VS_INPUT_FROM_VERTEX_FORMAT (1u << 1)
#define TRITON_WA_LINEARIZE_NOPERSPECTIVE_PS_INPUT (1u << 2)

UINT triton_backend_workaround_flags(void);

#define TRITON_HOSTCAP_D3D12                     (1u << 0)
#define TRITON_HOSTCAP_TBDR                      (1u << 1)
#define TRITON_HOSTCAP_MSAA_RTV_FORCED_SC1       (1u << 2)
#define TRITON_HOSTCAP_EXTENDED_RESOURCE_SHARING (1u << 3)
#define TRITON_HOSTCAP_MAP_DEFAULT_BUFFERS       (1u << 4)
#define TRITON_HOSTCAP_SHADER_CACHE              (1u << 5)
#define TRITON_HOSTCAP_DXIL                      (1u << 6)

struct triton_adapter_probe {
   BOOL viogpu;
   BOOL wddm2;
   BOOL host_ok;
   UINT caps_flags;
   BOOL no_wddm2_ddi;
   BOOL wddm2_0_only;
};

void triton_backend_adapter_probe(PTRITON_ADAPTER pAdapter, struct triton_adapter_probe *out);
void triton_backend_close_adapter(PTRITON_ADAPTER pAdapter);

// GetCaps runs before the runtime makes a device. When *pAdoptable is FALSE, Triton releases the device after the
// snapshot instead of handing it to the first CreateDevice.
HRESULT triton_backend_create_caps_device(PTRITON_ADAPTER pAdapter, D3D_FEATURE_LEVEL maxLevel,
                                          ID3D11Device **ppDevice, D3D_FEATURE_LEVEL *pLevel,
                                          ID3D11DeviceContext **ppContext, BOOL *pAdoptable);

HRESULT triton_backend_create_device(PTRITON_DEVICE pD, const D3D10DDIARG_CREATEDEVICE *pArgs,
                                     D3D_FEATURE_LEVEL requested, ID3D11Device **ppDevice,
                                     D3D_FEATURE_LEVEL *pLevel, ID3D11DeviceContext **ppContext);

// Also called when CreateDevice fails after the backend made its device. Triton has released its D3D11 objects.
void triton_backend_destroy_device(PTRITON_DEVICE pD);

DXGI_FORMAT triton_backend_shared_format(DXGI_FORMAT format);

#define TRITON_SHARED_TEXTURE_PRIMARY   (1u << 0)
// r->hKMAllocation then names the companion, not r->pResource.
#define TRITON_SHARED_TEXTURE_COMPANION (1u << 1)

// pArgs is the runtime's CreateResource. On success r->hKMAllocation is the texture's kernel allocation, or 0 when it
// could not be shared.
HRESULT triton_backend_create_shared_texture(PTRITON_DEVICE pD, PTRITON_RESOURCE r,
                                             const D3D11DDIARG_CREATERESOURCE *pArgs,
                                             const D3D11_TEXTURE2D_DESC *pDesc,
                                             const D3D11_SUBRESOURCE_DATA *pInitialData, UINT flags,
                                             ID3D11Texture2D **ppTexture);

struct triton_texture_desc {
   UINT        width;
   UINT        height;
   UINT        mip_levels;
   UINT        array_size;
   DXGI_FORMAT format;
   UINT        sample_count;
   UINT        usage;
   UINT        bind_flags;
   UINT        cpu_access_flags;
   UINT        misc_flags;
};

// r is zeroed with hRTResource set; the backend may keep its handles in hImportAlloc and hImportResKmt.
HRESULT triton_backend_open_shared_texture(PTRITON_DEVICE pD, PTRITON_RESOURCE r,
                                           const D3D10DDIARG_OPENRESOURCE *pArgs,
                                           struct triton_texture_desc *pDesc, ID3D11Resource **ppResource);

// After Triton released r's D3D11 objects.
void triton_backend_destroy_resource(PTRITON_DEVICE pD, PTRITON_RESOURCE r);

// The driver's OpenAdapter10_2 calls this once its backend accepted the adapter.
HRESULT tritonOpenAdapter10_2(D3D10DDIARG_OPENADAPTER *pOpenData, void *pBackendAdapter);

#ifdef __cplusplus
}
#endif

#endif
