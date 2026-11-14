/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Resource lifecycle, Map/Unmap, UpdateSubresource, copies, hazards,
 * constant-buffer binds, and the various
 * unimplementable WDDM 2.0+ stubs.
 */

#include "triton.h"
#include "triton_log.h"
#include "tritonPresent.h"


/* tritonQuery.c; the DDI table in tritonDDI.c carries the same prototype. */
void APIENTRY tritonCheckFormatSupport(D3D10DDI_HDEVICE, DXGI_FORMAT, UINT *);

TR_CALC_PRIVATE_SIZE(tritonCalcPrivateResourceSize, TRITON_RESOURCE,
                     const D3D11DDIARG_CREATERESOURCE *pArgs)

TR_CALC_PRIVATE_SIZE(tritonCalcPrivateOpenedResourceSize, TRITON_RESOURCE,
                     const D3D10DDIARG_OPENRESOURCE *pArgs)

/* Translate the DDI's combined Usage/CpuAccess/Bind flag salad into the
 * corresponding D3D11 enums.
 *
 * BindFlags bit layout is NOT 1:1 between the DDI and D3D11:
 *   - DDI 0x080 = D3D10_DDI_BIND_PRESENT (no D3D11 BindFlag equivalent)
 *   - DDI 0x100 = D3D11_DDI_BIND_UNORDERED_ACCESS, but D3D11_BIND_UNORDERED_ACCESS
 *     is at 0x080. A verbatim copy would (a) drop the UAV bit and (b)
 *     spuriously enable UAV for any present-bound texture.
 *
 * MiscFlags has its own DDI-only bits (DISCARD_ON_PRESENT 0x08, REMOTE
 * 0x400, ...) that have no D3D11 meaning; we drop them. TEXTURECUBE
 * (0x4) is signaled via ResourceDimension, not MiscFlags, so it is
 * re-added in the cube create path. */
static void tritonTranslateUsage(const D3D11DDIARG_CREATERESOURCE *a,
                                 D3D11_USAGE *pUsage,
                                 UINT *pCPUAccessFlags,
                                 UINT *pBindFlags,
                                 UINT *pMiscFlags)
{
    switch (a->Usage) {
    case D3D10_DDI_USAGE_DEFAULT:    *pUsage = D3D11_USAGE_DEFAULT;   break;
    case D3D10_DDI_USAGE_IMMUTABLE:  *pUsage = D3D11_USAGE_IMMUTABLE; break;
    case D3D10_DDI_USAGE_DYNAMIC:    *pUsage = D3D11_USAGE_DYNAMIC;   break;
    case D3D10_DDI_USAGE_STAGING:    *pUsage = D3D11_USAGE_STAGING;   break;
    default:                          *pUsage = D3D11_USAGE_DEFAULT;   break;
    }

    *pCPUAccessFlags = 0;
    if (a->MapFlags & D3D10_DDI_CPU_ACCESS_READ)  *pCPUAccessFlags |= D3D11_CPU_ACCESS_READ;
    if (a->MapFlags & D3D10_DDI_CPU_ACCESS_WRITE) *pCPUAccessFlags |= D3D11_CPU_ACCESS_WRITE;

    UINT bf = 0;
    const UINT dbf = a->BindFlags;
    if (dbf & D3D10_DDI_BIND_VERTEX_BUFFER)        bf |= D3D11_BIND_VERTEX_BUFFER;
    if (dbf & D3D10_DDI_BIND_INDEX_BUFFER)         bf |= D3D11_BIND_INDEX_BUFFER;
    if (dbf & D3D10_DDI_BIND_CONSTANT_BUFFER)      bf |= D3D11_BIND_CONSTANT_BUFFER;
    if (dbf & D3D10_DDI_BIND_SHADER_RESOURCE)      bf |= D3D11_BIND_SHADER_RESOURCE;
    if (dbf & D3D10_DDI_BIND_STREAM_OUTPUT)        bf |= D3D11_BIND_STREAM_OUTPUT;
    if (dbf & D3D10_DDI_BIND_RENDER_TARGET)        bf |= D3D11_BIND_RENDER_TARGET;
    if (dbf & D3D10_DDI_BIND_DEPTH_STENCIL)        bf |= D3D11_BIND_DEPTH_STENCIL;
    if (dbf & D3D11_DDI_BIND_UNORDERED_ACCESS)     bf |= D3D11_BIND_UNORDERED_ACCESS;
    if (dbf & D3D11_DDI_BIND_DECODER)              bf |= D3D11_BIND_DECODER;
    if (dbf & D3D11_DDI_BIND_VIDEO_ENCODER)        bf |= D3D11_BIND_VIDEO_ENCODER;
    /* BIND_PRESENT (0x80) for swap-chain back buffers has no D3D11
     * equivalent; RENDER_TARGET covers the only access we need. */
    *pBindFlags = bf;

    UINT mf = 0;
    const UINT dmf = a->MiscFlags;
    if (dmf & D3D10_DDI_RESOURCE_AUTO_GEN_MIP_MAP)         mf |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (dmf & D3D10_DDI_RESOURCE_MISC_SHARED)              mf |= D3D11_RESOURCE_MISC_SHARED;
    if (dmf & D3D11_DDI_RESOURCE_MISC_DRAWINDIRECT_ARGS)   mf |= D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
    if (dmf & D3D11_DDI_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS) mf |= D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (dmf & D3D11_DDI_RESOURCE_MISC_BUFFER_STRUCTURED)   mf |= D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    if (dmf & D3D11_DDI_RESOURCE_MISC_RESOURCE_CLAMP)      mf |= D3D11_RESOURCE_MISC_RESOURCE_CLAMP;
    *pMiscFlags = mf;
}

/* Assemble D3D11_SUBRESOURCE_DATA[] from pInitialDataUP for the
 * CreateBuffer/CreateTexture* initial-data forms. The caller owns the
 * returned heap buffer. NULL when there is no initial data, and also when
 * the allocation failed, which *pOom reports. */
static D3D11_SUBRESOURCE_DATA *
tritonBuildInitData(const D3D11DDIARG_CREATERESOURCE *a, BOOL *pOom)
{
    *pOom = FALSE;
    if (!a->pInitialDataUP) return NULL;
    /* Subresource count is MipLevels * ArraySize. A cube may arrive with
     * ArraySize==0 meaning one 6-face cube; the create path below
     * substitutes 6 for the desc, so mirror that here or the faces would
     * be created without their supplied initial data. */
    UINT arraySize = a->ArraySize;
    if (a->ResourceDimension == D3D10DDIRESOURCE_TEXTURECUBE && arraySize == 0)
        arraySize = 6;
    const UINT count = a->MipLevels * arraySize;
    if (count == 0) return NULL;
    D3D11_SUBRESOURCE_DATA *p = (D3D11_SUBRESOURCE_DATA *)(
        HeapAlloc(GetProcessHeap(), 0, count * sizeof(D3D11_SUBRESOURCE_DATA)));
    if (!p) { *pOom = TRUE; return NULL; }
    for (UINT i = 0; i < count; ++i) {
        p[i].pSysMem          = a->pInitialDataUP[i].pSysMem;
        p[i].SysMemPitch      = a->pInitialDataUP[i].SysMemPitch;
        p[i].SysMemSlicePitch = a->pInitialDataUP[i].SysMemSlicePitch;
    }
    return p;
}

void APIENTRY
tritonCreateResource(D3D10DDI_HDEVICE hDevice,
                     const D3D11DDIARG_CREATERESOURCE *pArgs,
                     D3D10DDI_HRESOURCE hResource,
                     D3D10DDI_HRTRESOURCE hRTResource)
{
    PTRITON_DEVICE   pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RESOURCE r  = (PTRITON_RESOURCE)(hResource.pDrvPrivate);
    if (!pD || !r) {
        TR_LOG("CreateResource: missing pDrvPrivate");
        return;
    }

    r->hRTResource = hRTResource;
    r->Format      = pArgs->Format;
    r->MipLevels   = pArgs->MipLevels;
    r->ArraySize   = pArgs->ArraySize;
    r->BindFlags   = pArgs->BindFlags;
    r->MiscFlags   = pArgs->MiscFlags;
    r->MapFlags    = pArgs->MapFlags;
    r->Usage       = pArgs->Usage;
    r->SampleDesc  = pArgs->SampleDesc;
    r->ByteStride  = pArgs->ByteStride;
    r->pResource        = NULL;
    r->pPresentCompanion = NULL;
    r->pViewList        = NULL;
    /* A resource is a display primary only when the runtime supplies
     * pPrimaryDesc (d3d10umddi.h: "If pPrimaryDesc absent, blt/copy style
     * is implied when used with Present").  BIND_PRESENT alone marks any
     * swapchain back buffer, including windowed ones that DWM composites
     * as ordinary shared textures and that must never claim the scanout. */
    r->IsPresentable    = !!(pArgs->BindFlags & D3D10_DDI_BIND_PRESENT) &&
                          pArgs->pPrimaryDesc != NULL;
    r->hKMAllocation    = 0;
    r->IsShared         = FALSE;
    r->hImportAlloc     = 0;
    r->hImportResKmt    = 0;

    /* Non-primary TEXTURE2D with MISC_SHARED: emulate the shared handle.
     * Display primaries take the host-swapchain route below.
     *
     * BIND_PRESENT joins them even without MISC_SHARED.  A windowed
     * blt-model swapchain (DXGI_SWAP_EFFECT_DISCARD) creates a plain back
     * buffer and presents it with Flags.Blt and hDstResource 0, so the only
     * handle dxgkrnl ever receives is the source allocation; with no KM
     * allocation tritonDxgiPresent has nothing to submit and the window is
     * never composed.  Blob-backed shared storage supplies that allocation;
     * primary=FALSE keeps it off the scanout, exactly like a flip-model back
     * buffer.
     *
     * The host exports only single-sample, single-mip, single-slice
     * textures.  A back buffer of any other shape keeps its own storage and
     * presents through a shared companion of that shape instead, filled at
     * present time (tritonResourceSyncPresentCompanion). */
    const BOOL is2D = !r->IsPresentable &&
                      pArgs->ResourceDimension == D3D10DDIRESOURCE_TEXTURE2D;
    const BOOL presentOnly = is2D &&
        (pArgs->MiscFlags & D3D10_DDI_RESOURCE_MISC_SHARED) == 0 &&
        (pArgs->BindFlags & D3D10_DDI_BIND_PRESENT) != 0;
    const BOOL exportableShape = pArgs->MipLevels <= 1 &&
                                 pArgs->ArraySize <= 1 &&
                                 pArgs->SampleDesc.Count <= 1;
    BOOL isShared = is2D &&
                    ((pArgs->MiscFlags & D3D10_DDI_RESOURCE_MISC_SHARED) != 0 ||
                     (presentOnly && exportableShape));
    const BOOL needCompanion = presentOnly && !exportableShape;


    /* Top-level dimensions come from the mip-0 entry of pMipInfoList. */
    if (pArgs->pMipInfoList) {
        r->Width  = pArgs->pMipInfoList[0].TexelWidth;
        r->Height = pArgs->pMipInfoList[0].TexelHeight;
        r->Depth  = pArgs->pMipInfoList[0].TexelDepth;
    } else {
        r->Width = r->Height = r->Depth = 0;
    }

    /* A host create is asynchronous and reports nothing back, so a texture
     * whose format the host cannot make must be refused here: forwarded, the
     * app holds a texture that never existed, its first Map comes back
     * DEVICE_REMOVED, and a test then recreates its device per case until
     * the host GPU runs out of memory. The video, palettized and R1 formats
     * have no Metal pixel format and report no support bits at all.
     * E_OUTOFMEMORY is the create error the runtime hands back to the app. */
    if (pArgs->ResourceDimension != D3D10DDIRESOURCE_BUFFER &&
        pArgs->Format != DXGI_FORMAT_UNKNOWN) {
        UINT ddiSupport = 0;
        tritonCheckFormatSupport(hDevice, pArgs->Format, &ddiSupport);
        if (ddiSupport == 0) {
            TR_LOG("CreateResource: format %u has no host support; refused",
                   (UINT)pArgs->Format);
            tritonSetError(pD, E_OUTOFMEMORY);
            return;
        }
    }

    /* Display primaries are ordinary host textures with a linear dmabuf
     * export, bound to a virtio-gpu blob the KMD scans out directly. */
    if (r->IsPresentable && pArgs->ResourceDimension == D3D10DDIRESOURCE_TEXTURE2D) {
        if (r->Width == 0 || r->Height == 0) {
            tritonSetCreateError(pD, E_INVALIDARG);
            return;
        }
        tritonPresentEnsureKernelContext(pD);

        /* Shared textures reject *_SRGB and TYPELESS formats; primaries
         * for SRGB backbuffers must be created with the UNORM base
         * (gamma is applied through SRGB views, which dxvk permits on
         * UNORM textures via its mutable view-format list). */
        DXGI_FORMAT scFmt = triton_backend_shared_format(r->Format);

        /* MISC_SHARED gives the texture exportable dedicated storage;
         * the backend makes a primary's storage linear for the scanout.
         * SHADER_RESOURCE lets copy/composite paths sample it. */
        D3D11_TEXTURE2D_DESC d = {};
        d.Width            = r->Width;
        d.Height           = r->Height;
        d.MipLevels        = 1;
        d.ArraySize        = 1;
        d.Format           = scFmt;
        d.SampleDesc.Count = 1;
        d.Usage            = D3D11_USAGE_DEFAULT;
        d.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags        = D3D11_RESOURCE_MISC_SHARED;
        ID3D11Texture2D *tex = NULL;
        HRESULT phr = triton_backend_create_shared_texture(
            pD, r, pArgs, &d, NULL, TRITON_SHARED_TEXTURE_PRIMARY, &tex);
        if (FAILED(phr) || !tex) {
            TR_LOG("present: primary create failed hr=0x%08lx", phr);
            tritonSetCreateError(pD, FAILED(phr) ? phr : E_FAIL);
            return;
        }
        r->pResource = (ID3D11Resource *)tex;
        // A primary the kernel driver cannot scan out is no primary.
        if (!r->hKMAllocation) {
            ID3D11Resource_Release(r->pResource);
            r->pResource = NULL;
            triton_backend_destroy_resource(pD, r);
            tritonSetCreateError(pD, E_FAIL);
            return;
        }
        r->IsShared = TRUE;
        return;
    }

    D3D11_USAGE usage;
    UINT cpuAccess, bind, misc;
    tritonTranslateUsage(pArgs, &usage, &cpuAccess, &bind, &misc);

    /* Cross-process sharing is carried by the virtio res_id bound to the KM
     * allocation, not by Win32 shared handles: keep MISC_SHARED so the host
     * backs the texture with exportable memory, and guarantee consumers (DWM
     * samples every composited surface) can create SRVs against it. */
    if (isShared) {
        if (usage != D3D11_USAGE_STAGING)
            bind |= D3D11_BIND_SHADER_RESOURCE;
        /* A back buffer that reaches here only for BIND_PRESENT never asked
         * for MISC_SHARED, so its host storage would not be exportable and
         * the export below would fail; force it on as the primary path
         * above does. */
        misc |= D3D11_RESOURCE_MISC_SHARED;
    }

    BOOL initOom;
    D3D11_SUBRESOURCE_DATA *initData = tritonBuildInitData(pArgs, &initOom);
    if (initOom) {
        tritonSetCreateError(pD, E_OUTOFMEMORY);
        return;
    }
    HRESULT hr = E_NOTIMPL;

    switch (pArgs->ResourceDimension) {
    case D3D10DDIRESOURCE_TEXTURE2D: {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width            = r->Width;
        d.Height           = r->Height;
        d.MipLevels        = r->MipLevels;
        d.ArraySize        = r->ArraySize;
        d.Format           = r->Format;
        d.SampleDesc       = r->SampleDesc;
        d.Usage            = usage;
        d.BindFlags        = bind;
        d.CPUAccessFlags   = cpuAccess;
        d.MiscFlags        = misc;
        ID3D11Texture2D *tex = NULL;
        if (isShared)
            hr = triton_backend_create_shared_texture(pD, r, pArgs, &d, initData, 0, &tex);
        else
            hr = ID3D11Device1_CreateTexture2D(pD->pDev1, &d, initData, &tex);
        if (SUCCEEDED(hr) && tex)
            r->pResource = (ID3D11Resource *)tex;
        break;
    }
    case D3D10DDIRESOURCE_BUFFER:
    case D3D11DDIRESOURCE_BUFFEREX: {
        D3D11_BUFFER_DESC d = {};
        /* Buffer size: pMipInfoList[0].TexelWidth is bytes for buffers. */
        d.ByteWidth           = r->Width;
        d.Usage               = usage;
        d.BindFlags           = bind;
        d.CPUAccessFlags      = cpuAccess;
        d.MiscFlags           = misc;
        d.StructureByteStride = r->ByteStride;
        ID3D11Buffer *buf = NULL;
        hr = ID3D11Device1_CreateBuffer(pD->pDev1, &d, initData, &buf);
        if (SUCCEEDED(hr) && buf)
            r->pResource = (ID3D11Resource *)buf;
        break;
    }
    case D3D10DDIRESOURCE_TEXTURE1D: {
        D3D11_TEXTURE1D_DESC d = {};
        d.Width          = r->Width;
        d.MipLevels      = r->MipLevels;
        d.ArraySize      = r->ArraySize;
        d.Format         = r->Format;
        d.Usage          = usage;
        d.BindFlags      = bind;
        d.CPUAccessFlags = cpuAccess;
        d.MiscFlags      = misc;
        ID3D11Texture1D *tex = NULL;
        hr = ID3D11Device1_CreateTexture1D(pD->pDev1, &d, initData, &tex);
        if (SUCCEEDED(hr) && tex) r->pResource = (ID3D11Resource *)tex;
        break;
    }
    case D3D10DDIRESOURCE_TEXTURE3D: {
        D3D11_TEXTURE3D_DESC d = {};
        d.Width          = r->Width;
        d.Height         = r->Height;
        d.Depth          = r->Depth;
        d.MipLevels      = r->MipLevels;
        d.Format         = r->Format;
        d.Usage          = usage;
        d.BindFlags      = bind;
        d.CPUAccessFlags = cpuAccess;
        d.MiscFlags      = misc;
        ID3D11Texture3D *tex = NULL;
        hr = ID3D11Device1_CreateTexture3D(pD->pDev1, &d, initData, &tex);
        if (SUCCEEDED(hr) && tex) r->pResource = (ID3D11Resource *)tex;
        break;
    }
    case D3D10DDIRESOURCE_TEXTURECUBE: {
        /* Cubes are tex2d arrays with MiscFlags |= TEXTURECUBE. */
        D3D11_TEXTURE2D_DESC d = {};
        d.Width            = r->Width;
        d.Height           = r->Height;
        d.MipLevels        = r->MipLevels;
        d.ArraySize        = r->ArraySize ? r->ArraySize : 6;
        d.Format           = r->Format;
        d.SampleDesc       = r->SampleDesc;
        d.Usage            = usage;
        d.BindFlags        = bind;
        d.CPUAccessFlags   = cpuAccess;
        d.MiscFlags        = misc | D3D11_RESOURCE_MISC_TEXTURECUBE;
        ID3D11Texture2D *tex = NULL;
        hr = ID3D11Device1_CreateTexture2D(pD->pDev1, &d, initData, &tex);
        if (SUCCEEDED(hr) && tex) r->pResource = (ID3D11Resource *)tex;
        break;
    }
    default:
        TR_LOG("CreateResource: unsupported resource dimension");
        hr = E_INVALIDARG;
        break;
    }

    if (initData) HeapFree(GetProcessHeap(), 0, initData);

    if (FAILED(hr) || !r->pResource) {
        TR_LOG("CreateResource: failed dim=%d hr=0x%08lx", pArgs->ResourceDimension, hr);
        if (isShared)
            triton_backend_destroy_resource(pD, r);
        tritonSetCreateError(pD, FAILED(hr) ? hr : E_OUTOFMEMORY);
        return;
    }

    if (isShared) {
        r->IsShared = r->hKMAllocation != 0;
        if (!r->hKMAllocation)
            TR_LOG("shared: registration failed; texture stays process-local");
    } else if (needCompanion) {
        D3D11_TEXTURE2D_DESC cd = {};
        cd.Width            = r->Width;
        cd.Height           = r->Height;
        cd.MipLevels        = 1;
        cd.ArraySize        = 1;
        cd.Format           = r->Format;
        cd.SampleDesc.Count = 1;
        cd.Usage            = D3D11_USAGE_DEFAULT;
        cd.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        cd.MiscFlags        = D3D11_RESOURCE_MISC_SHARED;
        ID3D11Texture2D *comp = NULL;
        if (SUCCEEDED(triton_backend_create_shared_texture(
                pD, r, pArgs, &cd, NULL, TRITON_SHARED_TEXTURE_COMPANION, &comp)) &&
            comp) {
            r->pPresentCompanion = (ID3D11Resource *)comp;
            if (!r->hKMAllocation) {
                ID3D11Resource_Release(r->pPresentCompanion);
                r->pPresentCompanion = NULL;
                triton_backend_destroy_resource(pD, r);
            }
        }
        if (!r->pPresentCompanion)
            TR_LOG("shared: present companion failed; back buffer cannot present");
    }
}

/* Fill a back buffer's present companion from its first subresource: a
 * resolve for a multisampled back buffer, a copy of mip 0 / slice 0
 * otherwise.  Caller holds presentLock (the immediate context is shared with
 * the present threads). */
void
tritonResourceSyncPresentCompanion(PTRITON_DEVICE pD, PTRITON_RESOURCE r)
{
    if (!r->pPresentCompanion || !r->pResource)
        return;
    if (r->SampleDesc.Count > 1)
        ID3D11DeviceContext1_ResolveSubresource(pD->pCtx1, r->pPresentCompanion, 0,
                                                r->pResource, 0, r->Format);
    else
        ID3D11DeviceContext1_CopySubresourceRegion(pD->pCtx1, r->pPresentCompanion,
                                                   0, 0, 0, 0, r->pResource, 0,
                                                   NULL);
}

void APIENTRY
tritonDestroyResource(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hResource)
{
    PTRITON_RESOURCE r = (PTRITON_RESOURCE)(hResource.pDrvPrivate);
    if (!r) return;
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);

    if (r->pResource) {
        ID3D11Resource_Release(r->pResource);
        r->pResource = NULL;
    }
    if (r->pPresentCompanion) {
        ID3D11Resource_Release(r->pPresentCompanion);
        r->pPresentCompanion = NULL;
    }

    if (pD)
        triton_backend_destroy_resource(pD, r);
}

/* OpenResource imports a cross-process shared resource: the backend reads
 * the exporter's description from the allocations the runtime hands over
 * and opens the texture on this device.  PFND3D10DDI_OPENRESOURCE is VOID,
 * so failure MUST be signalled via pfnSetErrorCb or the runtime leaves a
 * NULL resource that later no-ops. */
void APIENTRY
tritonOpenResource(D3D10DDI_HDEVICE hDevice, const D3D10DDIARG_OPENRESOURCE *pArgs,
                   D3D10DDI_HRESOURCE hResource, D3D10DDI_HRTRESOURCE hRTResource)
{
    PTRITON_DEVICE   pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RESOURCE r  = (PTRITON_RESOURCE)(hResource.pDrvPrivate);
    if (!pD || !r) {
        tritonSetCreateError(pD, E_INVALIDARG);
        return;
    }

    if (pArgs->NumAllocations < 1 || !pArgs->pOpenAllocationInfo) {
        TR_STUB("OpenResource: no allocation info");
        tritonSetCreateError(pD, E_NOTIMPL);
        return;
    }
    memset(r, 0, sizeof(*r));
    r->hRTResource = hRTResource;

    struct triton_texture_desc desc;
    memset(&desc, 0, sizeof(desc));
    ID3D11Resource *opened = NULL;
    HRESULT hr = triton_backend_open_shared_texture(pD, r, pArgs, &desc, &opened);
    if (FAILED(hr) || !opened) {
        TR_LOG("OpenResource: backend open failed hr=0x%08lx", hr);
        if (opened)
            ID3D11Resource_Release(opened);
        triton_backend_destroy_resource(pD, r);
        tritonSetCreateError(pD, FAILED(hr) ? hr : E_FAIL);
        return;
    }

    r->Format             = desc.format;
    r->Width              = desc.width;
    r->Height             = desc.height;
    r->Depth              = 1;
    r->MipLevels          = desc.mip_levels ? desc.mip_levels : 1;
    r->ArraySize          = desc.array_size ? desc.array_size : 1;
    r->BindFlags          = desc.bind_flags;
    r->MiscFlags          = desc.misc_flags;
    r->SampleDesc.Count   = desc.sample_count ? desc.sample_count : 1;
    r->SampleDesc.Quality = 0;
    r->IsShared           = TRUE;
    /* Consumer: the runtime owns the opened KM allocation, so leave
     * hKMAllocation 0 and DestroyResource won't try to free it. */
    r->pResource          = opened;
    TR_LOG_HOT("shared: consumer opened %ux%u", r->Width, r->Height);
}

/* D3D10_DDI_MAP enum values match D3D11_MAP (READ=1, WRITE=2,
 * READWRITE=3, WRITE_DISCARD=4, WRITE_NOOVERWRITE=5) and
 * D3D10DDI_MAPPED_SUBRESOURCE has the same layout as
 * D3D11_MAPPED_SUBRESOURCE -- so we forward directly. */

void APIENTRY
tritonResourceMap(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hResource,
                  UINT Subresource, D3D10_DDI_MAP DDIMap, UINT Flags,
                  D3D10DDI_MAPPED_SUBRESOURCE *pMapped)
{
    PTRITON_DEVICE   pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RESOURCE r  = (PTRITON_RESOURCE)(hResource.pDrvPrivate);
    if (!pD || !r || !r->pResource || !pMapped) return;

    UINT mapFlags = 0;
    if (Flags & D3D10_DDI_MAP_FLAG_DONOTWAIT) mapFlags |= D3D11_MAP_FLAG_DO_NOT_WAIT;

    D3D11_MAPPED_SUBRESOURCE m = {};
    HRESULT hr = ID3D11DeviceContext1_Map(pD->pCtx1, r->pResource, Subresource,
                                          (D3D11_MAP)DDIMap, mapFlags, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        /* DO_NOT_WAIT + GPU busy: propagate to runtime so the app gets the
         * DXGI status from the next API call instead of reading garbage. */
        tritonSetError(pD, DXGI_DDI_ERR_WASSTILLDRAWING);
        pMapped->pData = NULL;
        pMapped->RowPitch = pMapped->DepthPitch = 0;
        return;
    }
    if (FAILED(hr)) {
        /* Map may report only WASSTILLDRAWING and DEVICEREMOVED; any other
         * code is taken as a driver bug.  A failed map leaves the app no
         * usable pointer either way, so report the device as removed and
         * record what failed. */
        TR_LOG("Map: sub=%u type=%u flags=0x%x hr=0x%08lx", Subresource,
               (unsigned)DDIMap, Flags, hr);
        tritonSetError(pD, D3DDDIERR_DEVICEREMOVED);
        pMapped->pData = NULL;
        pMapped->RowPitch = pMapped->DepthPitch = 0;
        return;
    }
    pMapped->pData      = m.pData;
    pMapped->RowPitch   = m.RowPitch;
    pMapped->DepthPitch = m.DepthPitch;
}

void APIENTRY
tritonResourceUnmap(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hResource, UINT Subresource)
{
    PTRITON_DEVICE   pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RESOURCE r  = (PTRITON_RESOURCE)(hResource.pDrvPrivate);
    if (!pD || !r || !r->pResource) return;
    ID3D11DeviceContext1_Unmap(pD->pCtx1, r->pResource, Subresource);
}

/* No side-effect-free busy query reaches the host: probing with a Map/Unmap
 * of a subresource would share the sync map region with the app's own
 * mappings (the DDI allows this call while other subresources are mapped)
 * and replace its map record, and costs a full readback.  "Not busy" is
 * always safe -- the Map that follows still honours DONOTWAIT and reports
 * WASSTILLDRAWING from the host. */
BOOL APIENTRY
tritonResourceIsStagingBusy(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hResource)
{
    (void)hDevice;
    (void)hResource;
    return FALSE;
}

void APIENTRY
tritonResourceUpdateSubresourceUP(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hResource,
                                  UINT DstSubresource, const D3D10_DDI_BOX *pDstBox,
                                  const VOID *pSrc, UINT SrcRowPitch, UINT SrcDepthPitch)
{
    PTRITON_DEVICE   pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RESOURCE r  = (PTRITON_RESOURCE)(hResource.pDrvPrivate);
    if (!pD || !r || !r->pResource) return;
    ID3D11DeviceContext1_UpdateSubresource(
        pD->pCtx1, r->pResource, DstSubresource,
        (const D3D11_BOX *)(pDstBox), pSrc, SrcRowPitch, SrcDepthPitch);
}

void APIENTRY
tritonResourceUpdateSubresourceUP_11_1(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hResource,
                                       UINT DstSubresource, const D3D10_DDI_BOX *pDstBox,
                                       const VOID *pSrc, UINT SrcRowPitch, UINT SrcDepthPitch,
                                       UINT CopyFlags)
{
    PTRITON_DEVICE   pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RESOURCE r  = (PTRITON_RESOURCE)(hResource.pDrvPrivate);
    if (!pD || !r || !r->pResource) return;
    ID3D11DeviceContext1_UpdateSubresource1(
        pD->pCtx1, r->pResource, DstSubresource,
        (const D3D11_BOX *)(pDstBox), pSrc, SrcRowPitch, SrcDepthPitch, CopyFlags);
}

void APIENTRY
tritonResourceCopy(D3D10DDI_HDEVICE hDevice,
                   D3D10DDI_HRESOURCE hDst, D3D10DDI_HRESOURCE hSrc)
{
    PTRITON_DEVICE   pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RESOURCE d  = (PTRITON_RESOURCE)(hDst.pDrvPrivate);
    PTRITON_RESOURCE s  = (PTRITON_RESOURCE)(hSrc.pDrvPrivate);
    if (!pD || !d || !s || !d->pResource || !s->pResource) return;
    ID3D11DeviceContext1_CopyResource(pD->pCtx1, d->pResource, s->pResource);
}

void APIENTRY
tritonResourceCopyRegion(D3D10DDI_HDEVICE hDevice,
                         D3D10DDI_HRESOURCE hDst, UINT DstSubresource,
                         UINT DstX, UINT DstY, UINT DstZ,
                         D3D10DDI_HRESOURCE hSrc, UINT SrcSubresource,
                         const D3D10_DDI_BOX *pSrcBox)
{
    PTRITON_DEVICE   pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RESOURCE d  = (PTRITON_RESOURCE)(hDst.pDrvPrivate);
    PTRITON_RESOURCE s  = (PTRITON_RESOURCE)(hSrc.pDrvPrivate);
    if (!pD || !d || !s || !d->pResource || !s->pResource) return;
    ID3D11DeviceContext1_CopySubresourceRegion(
        pD->pCtx1, d->pResource, DstSubresource, DstX, DstY, DstZ,
        s->pResource, SrcSubresource, (const D3D11_BOX *)(pSrcBox));
}

void APIENTRY
tritonResourceCopyRegion_11_1(D3D10DDI_HDEVICE hDevice,
                              D3D10DDI_HRESOURCE hDst, UINT DstSubresource,
                              UINT DstX, UINT DstY, UINT DstZ,
                              D3D10DDI_HRESOURCE hSrc, UINT SrcSubresource,
                              const D3D10_DDI_BOX *pSrcBox, UINT CopyFlags)
{
    PTRITON_DEVICE   pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RESOURCE d  = (PTRITON_RESOURCE)(hDst.pDrvPrivate);
    PTRITON_RESOURCE s  = (PTRITON_RESOURCE)(hSrc.pDrvPrivate);
    if (!pD || !d || !s || !d->pResource || !s->pResource) return;
    ID3D11DeviceContext1_CopySubresourceRegion1(
        pD->pCtx1, d->pResource, DstSubresource, DstX, DstY, DstZ,
        s->pResource, SrcSubresource, (const D3D11_BOX *)(pSrcBox), CopyFlags);
}

void APIENTRY
tritonResourceResolveSubresource(D3D10DDI_HDEVICE hDevice,
                                 D3D10DDI_HRESOURCE hDst, UINT DstSubresource,
                                 D3D10DDI_HRESOURCE hSrc, UINT SrcSubresource,
                                 DXGI_FORMAT Format)
{
    PTRITON_DEVICE   pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RESOURCE d  = (PTRITON_RESOURCE)(hDst.pDrvPrivate);
    PTRITON_RESOURCE s  = (PTRITON_RESOURCE)(hSrc.pDrvPrivate);
    if (!pD || !d || !s || !d->pResource || !s->pResource) return;
    ID3D11DeviceContext1_ResolveSubresource(
        pD->pCtx1, d->pResource, DstSubresource, s->pResource, SrcSubresource, Format);
}

/* D3D11 tracks hazards on the immediate context; nothing to do here. */

void APIENTRY
tritonResourceReadAfterWriteHazard(D3D10DDI_HDEVICE hDev, D3D10DDI_HRESOURCE hRes) {}

void APIENTRY
tritonShaderResourceViewReadAfterWriteHazard(D3D10DDI_HDEVICE hDev, D3D10DDI_HSHADERRESOURCEVIEW hSRV, D3D10DDI_HRESOURCE hRes) {}

/* Per-stage constant-buffer setters. D3D11_0 takes only handles
 * (whole-buffer bind); D3D11_1 adds FirstConstant + NumConstants for
 * partial bind. */

/* Fill `out` with at most `cap` entries. Returns FALSE if any slot-range
 * check fails (StartSlot < cap, NumBuffers <= cap, StartSlot + NumBuffers
 * <= cap); callers must NOT issue the D3D11 bind in that case (would
 * OOB-read out). */
static BOOL tritonFillConstantBuffers(UINT Start, UINT N, const D3D10DDI_HRESOURCE *phB,
                                      ID3D11Buffer **out, UINT cap)
{
    if (Start >= cap || N > cap || Start + N > cap) return FALSE;
    for (UINT i = 0; i < N; ++i) {
        PTRITON_RESOURCE r = phB
            ? (PTRITON_RESOURCE)(phB[i].pDrvPrivate)
            : NULL;
        out[i] = r ? (ID3D11Buffer *)r->pResource : NULL;
    }
    return TRUE;
}

#define TR_CB_SETTER_10(stage, ctx_method)                                                  \
void APIENTRY                                                                    \
tritonCs_ ## stage ## _Set10(D3D10DDI_HDEVICE hDevice, UINT Start, UINT N,                  \
                             const D3D10DDI_HRESOURCE *phB)                                 \
{                                                                                           \
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);              \
    if (!pD) return;                                                                        \
    ID3D11Buffer *aB[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};               \
    if (!tritonFillConstantBuffers(Start, N, phB, aB,                                       \
                       D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT)) {                \
        return;                                                                             \
    }                                                                                       \
    ID3D11DeviceContext1_##ctx_method(pD->pCtx1, Start, N, aB);                             \
}

TR_CB_SETTER_10(VS, VSSetConstantBuffers)
TR_CB_SETTER_10(PS, PSSetConstantBuffers)
TR_CB_SETTER_10(GS, GSSetConstantBuffers)
TR_CB_SETTER_10(HS, HSSetConstantBuffers)
TR_CB_SETTER_10(DS, DSSetConstantBuffers)
TR_CB_SETTER_10(CS, CSSetConstantBuffers)

#undef TR_CB_SETTER_10

#define TR_CB_SETTER_11_1(stage, ctx_method)                                                \
void APIENTRY                                                                    \
tritonCs_ ## stage ## _Set11_1(D3D10DDI_HDEVICE hDevice, UINT Start, UINT N,                \
                               const D3D10DDI_HRESOURCE *phB,                               \
                               const UINT *pFirst, const UINT *pNum)                        \
{                                                                                           \
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);              \
    if (!pD) return;                                                                        \
    ID3D11Buffer *aB[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};               \
    if (!tritonFillConstantBuffers(Start, N, phB, aB,                                       \
                       D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT)) {                \
        return;                                                                             \
    }                                                                                       \
    ID3D11DeviceContext1_##ctx_method(pD->pCtx1, Start, N, aB, pFirst, pNum);               \
}

TR_CB_SETTER_11_1(VS, VSSetConstantBuffers1)
TR_CB_SETTER_11_1(PS, PSSetConstantBuffers1)
TR_CB_SETTER_11_1(GS, GSSetConstantBuffers1)
TR_CB_SETTER_11_1(HS, HSSetConstantBuffers1)
TR_CB_SETTER_11_1(DS, DSSetConstantBuffers1)
TR_CB_SETTER_11_1(CS, CSSetConstantBuffers1)

#undef TR_CB_SETTER_11_1

/* Tiled resources are not offered (D3DWDDM1_3DDICAPS_D3D11_OPTIONS1 reports
 * no tier), so the runtime never creates one and the tile DDIs are never
 * reached; GetMipPacking still answers "no packed mips". */
void APIENTRY
tritonGetMipPacking(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hTiledRes,
                    UINT *pNumPackedMips, UINT *pNumTilesForPackedMips)
{
    (void)hDevice; (void)hTiledRes;
    if (pNumPackedMips)         *pNumPackedMips = 0;
    if (pNumTilesForPackedMips) *pNumTilesForPackedMips = 0;
}

/* WDDM 2.0: GetResourceLayout returns the kernel allocation handle plus
 * driver-internal texture swizzle / row-major layout metadata. D3D11
 * exposes none of this at the UMD level. */

void APIENTRY
tritonGetResourceLayout(D3D10DDI_HDEVICE hDev, D3D10DDI_HRESOURCE hRes, UINT SubresourceCount,
                        D3DKMT_HANDLE *pAllocation,
                        D3DWDDM2_0DDI_TEXTURE_LAYOUT *pLayout,
                        UINT *pMipLevelSwizzleTransition,
                        D3DWDDM2_0DDI_SUBRESOURCE_LAYOUT *pSubresources)
{
    TR_STUB("GetResourceLayout (no D3D11 equivalent)");
    if (pAllocation)                *pAllocation = 0;
    if (pLayout)                    *pLayout = D3DWDDM2_0DDI_TL_UNDEFINED;
    if (pMipLevelSwizzleTransition) *pMipLevelSwizzleTransition = 0;
    if (pSubresources)
        ZeroMemory(pSubresources, SubresourceCount * sizeof(*pSubresources));
}

/* WDDM 2.0: hardware content protection. D3D11 exposes this only via
 * the video-DDI surfaces; the non-video pfnSetHardwareProtection{,State}
 * DDI entries have no public Device-level API equivalent. */

void APIENTRY
tritonSetHardwareProtection(D3D10DDI_HDEVICE hDev, D3D10DDI_HRESOURCE hRes, BOOL flag)
{
    TR_STUB("SetHardwareProtection (no D3D11 equivalent)");
}

void APIENTRY
tritonSetHardwareProtectionState(D3D10DDI_HDEVICE hDev, BOOL flag)
{
    TR_STUB("SetHardwareProtectionState (no D3D11 equivalent)");
}

/* WDDM 2.1: sync-token acquire/release for cross-process shared-resource
 * handoff.  From the 2.1 interface up the runtime stops flushing around
 * shared/redirection surfaces itself and delegates producer-side
 * publication to pfnReleaseResource, so Release must flush the device's
 * pending work: the surface contents have to be submitted (and
 * host-ordered) before the consumer's acquire, or DWM composes whatever
 * bytes the surface happens to hold.
 *
 * Acquire needs no wait of its own: submissions execute in order on the
 * host ring, and dxgkrnl serializes the cross-process handoff on the KM
 * sync token before scheduling the acquirer. */

void APIENTRY
tritonAcquireResource(D3D10DDI_HDEVICE hDev, D3D10DDI_HRESOURCE hRes, HANDLE h)
{
    (void)hDev;
    (void)hRes;
    (void)h;
}

void APIENTRY
tritonReleaseResource(D3D10DDI_HDEVICE hDev, D3D10DDI_HRESOURCE hRes, HANDLE h)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDev.pDrvPrivate);
    (void)hRes;
    (void)h;
    if (pD && pD->pCtx1)
        ID3D11DeviceContext1_Flush(pD->pCtx1);
}
