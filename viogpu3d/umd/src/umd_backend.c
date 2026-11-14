/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

// Triton's backend on DXVK and Venus: the D3D11 device is DXVK's, on a Venus instance that sends everything through
// the D3D runtime's callbacks to viogpu3d.

#include "umd.h"
#include "umd_format.h"
#include "umd_pipeline.h"
#include "umd_resource_record.h"

#include <d3d11_umd.h>

#include <stdlib.h>
#include <string.h>

C_ASSERT(UMD_FEATURE_LEVEL_10_0 == D3D_FEATURE_LEVEL_10_0);
C_ASSERT(UMD_FEATURE_LEVEL_10_1 == D3D_FEATURE_LEVEL_10_1);
C_ASSERT(UMD_FEATURE_LEVEL_11_0 == D3D_FEATURE_LEVEL_11_0);
C_ASSERT(UMD_FEATURE_LEVEL_11_1 == D3D_FEATURE_LEVEL_11_1);
C_ASSERT(UMD_PIPELINE_LEVEL_10_0 == D3D11DDI_3DPIPELINELEVEL_10_0);
C_ASSERT(UMD_PIPELINE_LEVEL_10_1 == D3D11DDI_3DPIPELINELEVEL_10_1);
C_ASSERT(UMD_PIPELINE_LEVEL_11_0 == D3D11DDI_3DPIPELINELEVEL_11_0);
C_ASSERT(UMD_PIPELINE_LEVEL_11_1 == D3D11_1DDI_3DPIPELINELEVEL_11_1);
C_ASSERT(UMD_DXGI_FORMAT_R10G10B10A2_TYPELESS == DXGI_FORMAT_R10G10B10A2_TYPELESS);
C_ASSERT(UMD_DXGI_FORMAT_R10G10B10A2_UNORM == DXGI_FORMAT_R10G10B10A2_UNORM);
C_ASSERT(UMD_DXGI_FORMAT_R8G8B8A8_TYPELESS == DXGI_FORMAT_R8G8B8A8_TYPELESS);
C_ASSERT(UMD_DXGI_FORMAT_R8G8B8A8_UNORM == DXGI_FORMAT_R8G8B8A8_UNORM);
C_ASSERT(UMD_DXGI_FORMAT_R8G8B8A8_UNORM_SRGB == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
C_ASSERT(UMD_DXGI_FORMAT_B8G8R8A8_UNORM == DXGI_FORMAT_B8G8R8A8_UNORM);
C_ASSERT(UMD_DXGI_FORMAT_B8G8R8X8_UNORM == DXGI_FORMAT_B8G8R8X8_UNORM);
C_ASSERT(UMD_DXGI_FORMAT_B8G8R8A8_TYPELESS == DXGI_FORMAT_B8G8R8A8_TYPELESS);
C_ASSERT(UMD_DXGI_FORMAT_B8G8R8A8_UNORM_SRGB == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
C_ASSERT(UMD_DXGI_FORMAT_B8G8R8X8_TYPELESS == DXGI_FORMAT_B8G8R8X8_TYPELESS);
C_ASSERT(UMD_DXGI_FORMAT_B8G8R8X8_UNORM_SRGB == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB);
C_ASSERT(sizeof(LUID) == VK_LUID_SIZE);
C_ASSERT(VIOGPU3D_DXGI_FORMAT_B8G8R8A8_UNORM == DXGI_FORMAT_B8G8R8A8_UNORM);
C_ASSERT(VIOGPU3D_DXGI_FORMAT_B8G8R8X8_UNORM == DXGI_FORMAT_B8G8R8X8_UNORM);
C_ASSERT(VIOGPU3D_D3D11_USAGE_DEFAULT == D3D11_USAGE_DEFAULT);
C_ASSERT(VIOGPU3D_D3D11_BIND_SHADER_RESOURCE == D3D11_BIND_SHADER_RESOURCE);
C_ASSERT(VIOGPU3D_D3D11_BIND_RENDER_TARGET == D3D11_BIND_RENDER_TARGET);
C_ASSERT(VIOGPU3D_D3D11_RESOURCE_MISC_SHARED == D3D11_RESOURCE_MISC_SHARED);

void triton_backend_log(const char *line)
{
    umd_log_line(line);
}

// VIOGPU3D_UMD_DEBUG=present_timing; VIOGPU3D_UMD_PRESENT_GATE_TRACE=<ms>.
static INIT_ONCE umd_env_once = INIT_ONCE_STATIC_INIT;
static UINT umd_debug_flags;
static int umd_gate_trace_ms;

static BOOL CALLBACK umd_env_read(PINIT_ONCE once, PVOID param, PVOID *context)
{
    char value[64];
    DWORD length;

    (void)once;
    (void)param;
    (void)context;
    length = GetEnvironmentVariableA("VIOGPU3D_UMD_DEBUG", value, sizeof(value));
    if (length > 0 && length < sizeof(value) && strstr(value, "present_timing") != NULL)
    {
        umd_debug_flags |= TRITON_DEBUG_PRESENT_TIMING;
    }
    length = GetEnvironmentVariableA("VIOGPU3D_UMD_PRESENT_GATE_TRACE", value, sizeof(value));
    if (length > 0 && length < sizeof(value))
    {
        umd_gate_trace_ms = atoi(value);
    }
    return TRUE;
}

BOOL triton_backend_debug(UINT flag)
{
    InitOnceExecuteOnce(&umd_env_once, umd_env_read, NULL, NULL);
    return (umd_debug_flags & flag) != 0;
}

int triton_backend_present_gate_trace_ms(void)
{
    InitOnceExecuteOnce(&umd_env_once, umd_env_read, NULL, NULL);
    return umd_gate_trace_ms;
}

// DXVK compiles DXBC itself; the rewrites are for another host compiler.
UINT triton_backend_workaround_flags(void)
{
    return 0;
}

void triton_backend_adapter_probe(PTRITON_ADAPTER pAdapter, struct triton_adapter_probe *out)
{
    memset(out, 0, sizeof(*out));
    out->viogpu = pAdapter != NULL && pAdapter->pBackend != NULL;
    out->wddm2 = TRUE;
    out->host_ok = out->viogpu;
    // viogpu3d is a WDDM 2.0 driver: dxgkrnl checks each higher DDI's features against it.
    out->wddm2_0_only = TRUE;
}

void triton_backend_close_adapter(PTRITON_ADAPTER pAdapter)
{
    struct umd_adapter *adapter = (struct umd_adapter *)pAdapter->pBackend;

    if (adapter == NULL)
    {
        return;
    }
    if (adapter->caps != NULL)
    {
        umd_vk_destroy(adapter->caps);
        HeapFree(GetProcessHeap(), 0, adapter->caps);
    }
    HeapFree(GetProcessHeap(), 0, adapter);
    pAdapter->pBackend = NULL;
}

static HRESULT umd_dxvk_device(struct umd_vk *vk,
                               LUID luid,
                               const D3D_FEATURE_LEVEL *levels,
                               UINT count,
                               ID3D11Device **ppDevice,
                               D3D_FEATURE_LEVEL *pLevel,
                               ID3D11DeviceContext **ppContext)
{
    IDXGIAdapter1 *adapter = NULL;
    HRESULT hr;

    hr = IDXGIFactory4_EnumAdapterByLuid(vk->factory, luid, &IID_IDXGIAdapter1, (void **)&adapter);
    if (FAILED(hr))
    {
        umd_log("DXVK has no adapter with LUID %08lx-%08lx: 0x%08lx",
                (unsigned long)luid.HighPart,
                (unsigned long)luid.LowPart,
                (unsigned long)hr);
        return hr;
    }
    hr = D3D11CreateDevice((IDXGIAdapter *)adapter,
                           D3D_DRIVER_TYPE_UNKNOWN,
                           NULL,
                           0,
                           levels,
                           count,
                           D3D11_SDK_VERSION,
                           ppDevice,
                           pLevel,
                           ppContext);
    IDXGIAdapter1_Release(adapter);
    if (FAILED(hr))
    {
        umd_log("DXVK made no D3D11 device: 0x%08lx", (unsigned long)hr);
    }
    return hr;
}

// GetCaps comes before any runtime device, so this instance is Venus's own, through D3DKMT; the runtime never sees its
// device, which therefore cannot become an app's.
HRESULT triton_backend_create_caps_device(PTRITON_ADAPTER pAdapter,
                                          D3D_FEATURE_LEVEL maxLevel,
                                          ID3D11Device **ppDevice,
                                          D3D_FEATURE_LEVEL *pLevel,
                                          ID3D11DeviceContext **ppContext,
                                          BOOL *pAdoptable)
{
    struct umd_adapter *adapter = (struct umd_adapter *)pAdapter->pBackend;
    D3D_FEATURE_LEVEL levels[4];
    UINT count;
    HRESULT hr;

    *ppDevice = NULL;
    *ppContext = NULL;
    *pLevel = (D3D_FEATURE_LEVEL)0;
    *pAdoptable = FALSE;
    if (adapter == NULL)
    {
        return E_INVALIDARG;
    }
    count = umd_feature_levels_upto((UINT)maxLevel, (UINT *)levels, 4);
    if (count == 0)
    {
        return E_INVALIDARG;
    }
    if (adapter->caps == NULL)
    {
        struct umd_vk *caps = (struct umd_vk *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*caps));
        if (caps == NULL)
        {
            return E_OUTOFMEMORY;
        }
        hr = umd_vk_create(NULL, caps);
        if (FAILED(hr))
        {
            HeapFree(GetProcessHeap(), 0, caps);
            return hr;
        }
        adapter->caps = caps;
    }
    hr = umd_dxvk_device(adapter->caps, adapter->luid, levels, count, ppDevice, pLevel, ppContext);
    if (SUCCEEDED(hr))
    {
        umd_log("caps: DXVK on Venus reaches feature level 0x%x", (unsigned)*pLevel);
    }
    return hr;
}

HRESULT triton_backend_create_device(PTRITON_DEVICE pD,
                                     const D3D10DDIARG_CREATEDEVICE *pArgs,
                                     D3D_FEATURE_LEVEL requested,
                                     ID3D11Device **ppDevice,
                                     D3D_FEATURE_LEVEL *pLevel,
                                     ID3D11DeviceContext **ppContext)
{
    struct umd_adapter *adapter = pD->pAdapter != NULL ? (struct umd_adapter *)pD->pAdapter->pBackend : NULL;
    struct umd_device *dev;
    UINT asked;
    HRESULT hr;

    *ppDevice = NULL;
    *ppContext = NULL;
    *pLevel = (D3D_FEATURE_LEVEL)0;
    if (adapter == NULL || pArgs->pKTCallbacks == NULL || pArgs->pKTCallbacks->pfnAllocateCb == NULL)
    {
        return E_INVALIDARG;
    }
    asked = umd_feature_level_from_pipeline(D3D11DDI_EXTRACT_3DPIPELINELEVEL_FROM_FLAGS(pArgs->Flags));
    if (asked != (UINT)requested)
    {
        umd_log("CreateDevice: Triton asks for level 0x%x, the runtime for 0x%x", (unsigned)requested, asked);
        return E_INVALIDARG;
    }

    dev = (struct umd_device *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*dev));
    if (dev == NULL)
    {
        return E_OUTOFMEMORY;
    }
    umd_device_callbacks_init(dev, pArgs->hRTDevice.handle, pArgs->pKTCallbacks);
    dev->callbacks.sType = UMD_VK_STRUCTURE_TYPE_D3DDDI_CALLBACKS_MESA;
    dev->callbacks.AdapterLuid = adapter->luid;
    dev->callbacks.hRTAdapter = adapter->hRTAdapter;
    dev->callbacks.hRTDevice = pArgs->hRTDevice.handle;
    dev->callbacks.pAdapterCallbacks = adapter->pAdapterCallbacks;
    dev->callbacks.pKTCallbacks = &dev->ktCallbacks;
    dev->callbacks.pDXGIBaseCallbacks = pArgs->DXGIBaseDDI.pDXGIBaseCallbacks;
    dev->callbacks.hRTCoreLayer = pArgs->hRTCoreLayer.handle;
    dev->callbacks.p11UMCallbacks = pArgs->p11UMCallbacks;
    pD->pBackend = dev;

    hr = umd_vk_create(&dev->callbacks, &dev->vk);
    if (SUCCEEDED(hr))
    {
        hr = umd_dxvk_device(&dev->vk, adapter->luid, &requested, 1, ppDevice, pLevel, ppContext);
    }
    if (FAILED(hr))
    {
        // Triton calls triton_backend_destroy_device after a failed create.
        return hr;
    }
    umd_log("CreateDevice: DXVK device at level 0x%x on Venus", (unsigned)*pLevel);
    return S_OK;
}

void triton_backend_destroy_device(PTRITON_DEVICE pD)
{
    struct umd_device *dev = (struct umd_device *)pD->pBackend;

    if (dev == NULL)
    {
        return;
    }
    umd_vk_destroy(&dev->vk);
    umd_device_callbacks_fini(dev);
    HeapFree(GetProcessHeap(), 0, dev);
    pD->pBackend = NULL;
}

DXGI_FORMAT triton_backend_shared_format(DXGI_FORMAT format)
{
    return (DXGI_FORMAT)umd_shared_format((UINT)format);
}

static void umd_texture_desc_from(const D3D11_TEXTURE2D_DESC *d, struct umd_texture_desc *out)
{
    memset(out, 0, sizeof(*out));
    out->width = d->Width;
    out->height = d->Height;
    out->mip_levels = d->MipLevels;
    out->array_size = d->ArraySize;
    out->format = (UINT)d->Format;
    out->sample_count = d->SampleDesc.Count;
    out->usage = (UINT)d->Usage;
    out->bind_flags = d->BindFlags;
    out->cpu_access_flags = d->CPUAccessFlags;
    out->misc_flags = d->MiscFlags;
}

// Venus makes the blob through pfnAllocateCb for r->hRTResource and appends the record for the processes that open it.
HRESULT triton_backend_create_shared_texture(PTRITON_DEVICE pD,
                                             PTRITON_RESOURCE r,
                                             const D3D11DDIARG_CREATERESOURCE *pArgs,
                                             const D3D11_TEXTURE2D_DESC *pDesc,
                                             const D3D11_SUBRESOURCE_DATA *pInitialData,
                                             UINT flags,
                                             ID3D11Texture2D **ppTexture)
{
    const BOOL primary = (flags & TRITON_SHARED_TEXTURE_PRIMARY) != 0;
    struct umd_texture_desc desc;
    VIOGPU3D_SURFACE record;
    BOOL linear;
    VkD3DDDICreateResourceMESA create;
    struct umd_vk_pending pending;
    D3D11_TEXTURE2D_DESC1 desc1;
    ID3D11Texture2D1 *texture = NULL;
    D3DKMT_HANDLE allocation = 0;
    HRESULT hr;

    *ppTexture = NULL;
    r->hKMAllocation = 0;
    if (pD->pBackend == NULL || pArgs == NULL)
    {
        return E_INVALIDARG;
    }
    if (primary && umd_scanout_vk_format((UINT)pDesc->Format) == 0)
    {
        umd_log("primary: format %u is none the kernel driver scans out", (unsigned)pDesc->Format);
        return E_INVALIDARG;
    }

    umd_texture_desc_from(pDesc, &desc);
    // A host that shares Venus memory as host memory binds only linear images to it, here and in every opener.
    linear = umd_texture_can_be_linear(&desc);
    if (primary && !linear)
    {
        umd_log("primary: %u levels, %u layers, %u samples: a scanout is one of each",
                desc.mip_levels,
                desc.array_size,
                desc.sample_count);
        return E_INVALIDARG;
    }
    if (!linear)
    {
        umd_log("shared texture %ux%u: %u levels, %u layers, %u samples: not linear, so a host that shares memory as "
                "host memory cannot import it",
                desc.width,
                desc.height,
                desc.mip_levels,
                desc.array_size,
                desc.sample_count);
    }
    desc.flags = linear ? VIOGPU3D_SURFACE_FLAG_LINEAR : 0;
    umd_resource_record_fill(&record, &desc);
    memset(&create, 0, sizeof(create));
    create.sType = UMD_VK_STRUCTURE_TYPE_D3DDDI_CREATE_RESOURCE_MESA;
    create.hRTResource = r->hRTResource.handle;
    create.pCreateResource11 = pArgs;
    create.pExtraData = &record;
    create.ExtraDataSize = sizeof(record);

    memset(&desc1, 0, sizeof(desc1));
    desc1.Width = pDesc->Width;
    desc1.Height = pDesc->Height;
    desc1.MipLevels = pDesc->MipLevels;
    desc1.ArraySize = pDesc->ArraySize;
    desc1.Format = pDesc->Format;
    desc1.SampleDesc = pDesc->SampleDesc;
    desc1.Usage = pDesc->Usage;
    desc1.BindFlags = pDesc->BindFlags;
    desc1.CPUAccessFlags = pDesc->CPUAccessFlags;
    desc1.MiscFlags = pDesc->MiscFlags;
    desc1.TextureLayout = D3D11_TEXTURE_LAYOUT_UNDEFINED;

    memset(&pending, 0, sizeof(pending));
    pending.kind = UMD_VK_ALLOC_SHARED_IMAGE;
    pending.ext = (VkBaseOutStructure *)&create;
    pending.hRTDevice = pD->hRTDevice.handle;
    pending.hRTResource = r->hRTResource.handle;
    pending.pAllocation = &allocation;
    umd_vk_pending_begin(&pending);
    hr = DXVKUmdCreateTexture2D((ID3D11Device *)pD->pDev1,
                                &desc1,
                                pInitialData,
                                linear ? DXVK_UMD_TEXTURE_LINEAR : 0,
                                &texture);
    umd_vk_pending_end(&pending);
    if (FAILED(hr))
    {
        umd_log("shared texture %ux%u format %u: DXVK refused it: 0x%08lx",
                pDesc->Width,
                pDesc->Height,
                (unsigned)pDesc->Format,
                (unsigned long)hr);
        return hr;
    }
    if (!pending.chained || allocation == 0)
    {
        umd_log("shared texture %ux%u: no kernel allocation for the runtime's resource (%s)",
                pDesc->Width,
                pDesc->Height,
                pending.chained ? "the runtime made none" : "DXVK made no shared allocation");
    }
    r->hKMAllocation = allocation;
    *ppTexture = (ID3D11Texture2D *)texture;
    return S_OK;
}

HRESULT triton_backend_open_shared_texture(PTRITON_DEVICE pD,
                                           PTRITON_RESOURCE r,
                                           const D3D10DDIARG_OPENRESOURCE *pArgs,
                                           struct triton_texture_desc *pDesc,
                                           ID3D11Resource **ppResource)
{
    const D3DDDI_OPENALLOCATIONINFO *opened = &pArgs->pOpenAllocationInfo[0];
    VIRTIO_WDDM_RESOURCE_INFO info;
    struct umd_texture_desc desc;
    enum umd_open_check check;
    VkD3DDDIOpenResourceMESA open;
    struct umd_vk_pending pending;
    D3D11_TEXTURE2D_DESC1 desc1;
    ID3D11Texture2D1 *texture = NULL;
    UINT importFlags;
    HRESULT hr;

    *ppResource = NULL;
    if (pD->pBackend == NULL)
    {
        return E_INVALIDARG;
    }
    // One allocation is all a shared texture of this driver has.
    if (pArgs->NumAllocations != 1)
    {
        umd_log("OpenResource: %u allocations; a shared texture has one", pArgs->NumAllocations);
        return E_INVALIDARG;
    }
    check = umd_resource_record_open(opened->pPrivateDriverData,
                                     opened->PrivateDriverDataSize,
                                     (UINT32)opened->hAllocation,
                                     &info,
                                     &desc);
    if (check != UMD_OPEN_OK)
    {
        umd_log("OpenResource: refused: %s", umd_open_check_text(check));
        return E_INVALIDARG;
    }

    memset(&desc1, 0, sizeof(desc1));
    desc1.Width = desc.width;
    desc1.Height = desc.height;
    desc1.MipLevels = desc.mip_levels;
    desc1.ArraySize = desc.array_size;
    desc1.Format = (DXGI_FORMAT)desc.format;
    desc1.SampleDesc.Count = desc.sample_count;
    desc1.Usage = (D3D11_USAGE)desc.usage;
    desc1.BindFlags = desc.bind_flags;
    desc1.CPUAccessFlags = desc.cpu_access_flags;
    desc1.MiscFlags = desc.misc_flags | D3D11_RESOURCE_MISC_SHARED;

    memset(&open, 0, sizeof(open));
    open.sType = UMD_VK_STRUCTURE_TYPE_D3DDDI_OPEN_RESOURCE_MESA;
    open.hRTResource = r->hRTResource.handle;
    open.pOpenResource = pArgs;
    open.pResourceInfo = &info;

    memset(&pending, 0, sizeof(pending));
    pending.kind = UMD_VK_ALLOC_IMPORT;
    pending.ext = (VkBaseOutStructure *)&open;
    pending.hRTDevice = pD->hRTDevice.handle;
    // An import takes its maker's layout.
    importFlags = DXVK_UMD_TEXTURE_IMPORT | ((desc.flags & VIOGPU3D_SURFACE_FLAG_LINEAR) ? DXVK_UMD_TEXTURE_LINEAR : 0);
    umd_vk_pending_begin(&pending);
    hr = DXVKUmdCreateTexture2D((ID3D11Device *)pD->pDev1, &desc1, NULL, importFlags, &texture);
    umd_vk_pending_end(&pending);
    if (FAILED(hr) || !pending.chained)
    {
        umd_log("OpenResource: %ux%u format %u: %s 0x%08lx",
                desc.width,
                desc.height,
                desc.format,
                pending.chained ? "DXVK refused the import" : "DXVK made no import",
                (unsigned long)hr);
        if (texture != NULL)
        {
            ID3D11Texture2D1_Release(texture);
        }
        return FAILED(hr) ? hr : E_FAIL;
    }

    if (desc.kind == VIOGPU3D_SURFACE_KERNEL_BLOB)
    {
        umd_log("OpenResource: the kernel driver's surface of type %u, %ux%u, rows of %u bytes",
                desc.standard_type,
                desc.width,
                desc.height,
                desc.pitch);
    }
    pDesc->width = desc.width;
    pDesc->height = desc.height;
    pDesc->mip_levels = desc.mip_levels;
    pDesc->array_size = desc.array_size;
    pDesc->format = (DXGI_FORMAT)desc.format;
    pDesc->sample_count = desc.sample_count;
    pDesc->usage = desc.usage;
    pDesc->bind_flags = desc.bind_flags;
    pDesc->cpu_access_flags = desc.cpu_access_flags;
    pDesc->misc_flags = desc.misc_flags;
    *ppResource = (ID3D11Resource *)texture;
    return S_OK;
}

// Venus owns the kernel allocation and frees it with the texture's memory.
void triton_backend_destroy_resource(PTRITON_DEVICE pD, PTRITON_RESOURCE r)
{
    (void)pD;
    r->hKMAllocation = 0;
}
