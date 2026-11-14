/*
 * Copyright 2026 OSMates contributors
 * SPDX-License-Identifier: MIT
 */

// A backend that makes nothing: Triton with it links into a driver whose adapter offers no device.

#include "triton.h"

void triton_backend_log(const char *line)
{
    OutputDebugStringA(line);
}

BOOL triton_backend_debug(UINT flag)
{
    (void)flag;
    return FALSE;
}

int triton_backend_present_gate_trace_ms(void)
{
    return 0;
}

UINT triton_backend_workaround_flags(void)
{
    return 0;
}

void triton_backend_adapter_probe(PTRITON_ADAPTER pAdapter, struct triton_adapter_probe *out)
{
    (void)pAdapter;
    memset(out, 0, sizeof(*out));
    out->no_wddm2_ddi = TRUE;
}

void triton_backend_close_adapter(PTRITON_ADAPTER pAdapter)
{
    (void)pAdapter;
}

HRESULT triton_backend_create_caps_device(PTRITON_ADAPTER pAdapter, D3D_FEATURE_LEVEL maxLevel,
                                          ID3D11Device **ppDevice, D3D_FEATURE_LEVEL *pLevel,
                                          ID3D11DeviceContext **ppContext, BOOL *pAdoptable)
{
    (void)pAdapter;
    (void)maxLevel;
    *ppDevice = NULL;
    *pLevel = (D3D_FEATURE_LEVEL)0;
    *ppContext = NULL;
    *pAdoptable = FALSE;
    return E_NOTIMPL;
}

HRESULT triton_backend_create_device(PTRITON_DEVICE pD, const D3D10DDIARG_CREATEDEVICE *pArgs,
                                     D3D_FEATURE_LEVEL requested, ID3D11Device **ppDevice,
                                     D3D_FEATURE_LEVEL *pLevel, ID3D11DeviceContext **ppContext)
{
    (void)pD;
    (void)pArgs;
    (void)requested;
    *ppDevice = NULL;
    *pLevel = (D3D_FEATURE_LEVEL)0;
    *ppContext = NULL;
    return E_NOTIMPL;
}

void triton_backend_destroy_device(PTRITON_DEVICE pD)
{
    (void)pD;
}

DXGI_FORMAT triton_backend_shared_format(DXGI_FORMAT format)
{
    return format;
}

HRESULT triton_backend_create_shared_texture(PTRITON_DEVICE pD, PTRITON_RESOURCE r,
                                             const D3D11DDIARG_CREATERESOURCE *pArgs,
                                             const D3D11_TEXTURE2D_DESC *pDesc,
                                             const D3D11_SUBRESOURCE_DATA *pInitialData, UINT flags,
                                             ID3D11Texture2D **ppTexture)
{
    (void)pD;
    (void)r;
    (void)pArgs;
    (void)pDesc;
    (void)pInitialData;
    (void)flags;
    *ppTexture = NULL;
    return E_NOTIMPL;
}

HRESULT triton_backend_open_shared_texture(PTRITON_DEVICE pD, PTRITON_RESOURCE r,
                                           const D3D10DDIARG_OPENRESOURCE *pArgs,
                                           struct triton_texture_desc *pDesc, ID3D11Resource **ppResource)
{
    (void)pD;
    (void)r;
    (void)pArgs;
    (void)pDesc;
    *ppResource = NULL;
    return E_NOTIMPL;
}

void triton_backend_destroy_resource(PTRITON_DEVICE pD, PTRITON_RESOURCE r)
{
    (void)pD;
    (void)r;
}

HRESULT APIENTRY OpenAdapter10_2(D3D10DDIARG_OPENADAPTER *pOpenData)
{
    return tritonOpenAdapter10_2(pOpenData, NULL);
}
