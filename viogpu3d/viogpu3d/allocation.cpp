/*
 * Copyright (C) 2019-2020 Red Hat, Inc.
 * Copyright (C) 2023 Max R (viogpu3d, virtio-win pull request 943)
 * Copyright (C) 2026 OSMates contributors
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met :
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and / or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of their contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "viogpu3d.h"
#include "baseobj.h"
#if !DBG
#include "allocation.tmh"
#endif

// virgl's bind flags, which Venus ignores: what the kernel driver's blobs are for.
#define VIRGL_BIND_RENDER_TARGET  (1 << 1)
#define VIRGL_BIND_SAMPLER_VIEW   (1 << 3)
#define VIRGL_BIND_DISPLAY_TARGET (1 << 7)
#define VIRGL_BIND_SCANOUT        (1 << 18)

static ULONGLONG AlignUp(ULONGLONG value, ULONGLONG alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

// An event as the lock: mapping into a process needs PASSIVE_LEVEL, which a fast mutex would not leave.
static void LockMap(PKEVENT lock)
{
    KeWaitForSingleObject(lock, Executive, KernelMode, FALSE, NULL);
}

static void UnlockMap(PKEVENT lock)
{
    KeSetEvent(lock, IO_NO_INCREMENT, FALSE);
}

PAGED_CODE_SEG_BEGIN

VioGpu3dHostVisible::VioGpu3dHostVisible()
{
    PAGED_CODE();

    m_Base.QuadPart = 0;
    m_Size = 0;
    m_Alignment = PAGE_SIZE;
    m_pBits = NULL;
    RtlZeroMemory(&m_Bitmap, sizeof(m_Bitmap));
    ExInitializeFastMutex(&m_Lock);
}

VioGpu3dHostVisible::~VioGpu3dHostVisible()
{
    PAGED_CODE();

    Close();
}

NTSTATUS VioGpu3dHostVisible::Init(PHYSICAL_ADDRESS base, ULONGLONG size, ULONG alignment)
{
    PAGED_CODE();

    ULONGLONG units = size / alignment;
    if (units == 0 || units > MAXULONG / 2 || (base.QuadPart & (alignment - 1)) != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }
    SIZE_T bytes = (SIZE_T)(AlignUp(units, 32) / 8);
    m_pBits = new (NonPagedPoolNx) ULONG[bytes / sizeof(ULONG)];
    if (m_pBits == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlInitializeBitMap(&m_Bitmap, m_pBits, (ULONG)units);
    RtlClearAllBits(&m_Bitmap);
    m_Base = base;
    m_Size = units * alignment;
    m_Alignment = alignment;
    return STATUS_SUCCESS;
}

void VioGpu3dHostVisible::Close()
{
    PAGED_CODE();

    if (m_pBits != NULL)
    {
        delete[] m_pBits;
        m_pBits = NULL;
    }
    m_Size = 0;
}

NTSTATUS VioGpu3dHostVisible::Allocate(ULONGLONG size, _Out_ ULONGLONG *pOffset)
{
    PAGED_CODE();

    *pOffset = 0;
    if (m_Size == 0 || size == 0 || size > m_Size)
    {
        return STATUS_INVALID_PARAMETER;
    }
    ULONG units = (ULONG)(AlignUp(size, m_Alignment) / m_Alignment);
    ExAcquireFastMutex(&m_Lock);
    ULONG index = RtlFindClearBitsAndSet(&m_Bitmap, units, 0);
    ExReleaseFastMutex(&m_Lock);
    if (index == MAXULONG)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    *pOffset = (ULONGLONG)index * m_Alignment;
    return STATUS_SUCCESS;
}

void VioGpu3dHostVisible::Free(ULONGLONG offset, ULONGLONG size)
{
    PAGED_CODE();

    ULONG units = (ULONG)(AlignUp(size, m_Alignment) / m_Alignment);
    ExAcquireFastMutex(&m_Lock);
    RtlClearBits(&m_Bitmap, (ULONG)(offset / m_Alignment), units);
    ExReleaseFastMutex(&m_Lock);
}

VioGpu3dAllocation::VioGpu3dAllocation(VioGpu3dAdapter *adapter)
{
    PAGED_CODE();

    m_pAdapter = adapter;
    m_Kind = VIOGPU3D_ALLOCATION_3D;
    RtlZeroMemory(&m_Info, sizeof(m_Info));
    m_ResId = 0;
    m_Created = FALSE;
    m_CreatorCtxId = 0;
    m_pPrelude = NULL;
    m_PreludeSize = 0;
    m_KernelSurface = FALSE;
    m_StandardType = 0;
    m_Primary = FALSE;
    m_Width = 0;
    m_Height = 0;
    m_Pitch = 0;
    m_Format = D3DDDIFMT_A8R8G8B8;
    m_Backed = FALSE;
    m_pApertureMdl = NULL;
    m_ApertureMdlOffset = 0;
    KeInitializeEvent(&m_MapLock, SynchronizationEvent, TRUE);
    m_HostMapped = FALSE;
    m_RegionOffset = 0;
    m_RegionSize = 0;
    m_CacheType = MmCached;
    m_pKernelVa = NULL;
    m_pRegionMdl = NULL;
    InitializeListHead(&m_UserMappings);
}

VioGpu3dAllocation::~VioGpu3dAllocation()
{
    PAGED_CODE();

    ReleaseHostMapping();
    if (m_Backed)
    {
        DetachBacking();
    }
    if (m_ResId != 0)
    {
        if (m_Created)
        {
            m_pAdapter->ctrlQueue.DestroyResource(m_ResId);
        }
        m_pAdapter->resourceIdr.PutId(m_ResId);
    }
    delete[] m_pPrelude;
}

NTSTATUS VioGpu3dAllocation::Init(_Inout_ DXGK_ALLOCATIONINFO *pInfo,
                                  _In_opt_ const VIRTIO_WDDM_CREATE_RESOURCE *pResource,
                                  UINT resourceSize)
{
    PAGED_CODE();

    if (pInfo->pPrivateDriverData == NULL || pInfo->PrivateDriverDataSize < sizeof(VIRTIO_WDDM_CREATE_ALLOCATION))
    {
        return STATUS_INVALID_PARAMETER;
    }
    VIRTIO_WDDM_CREATE_ALLOCATION *pCreate = (VIRTIO_WDDM_CREATE_ALLOCATION *)pInfo->pPrivateDriverData;
    m_Info = *pCreate;

    RtlZeroMemory(&pInfo->FlagsWddm2, sizeof(pInfo->FlagsWddm2));
    pInfo->Alignment = 0;
    pInfo->PreferredSegment.Value = 0;
    pInfo->PreferredSegment.SegmentId0 = VIOGPU3D_SEGMENT_APERTURE;
    pInfo->SupportedReadSegmentSet = 1 << (VIOGPU3D_SEGMENT_APERTURE - 1);
    pInfo->SupportedWriteSegmentSet = 1 << (VIOGPU3D_SEGMENT_APERTURE - 1);
    pInfo->EvictionSegmentSet = 0;
    pInfo->PhysicalAdapterIndex = 0;
    pInfo->pAllocationUsageHint = NULL;
    pInfo->AllocationPriority = D3DDDI_ALLOCATIONPRIORITY_NORMAL;
    pInfo->hAllocation = this;

    // Only GetStandardAllocationDriverData knows the mark, so no process can claim a kernel surface.
    if (pInfo->PrivateDriverDataSize >= sizeof(VIRTIO_WDDM_CREATE_ALLOCATION) + sizeof(VIOGPU3D_SURFACE))
    {
        VIOGPU3D_SURFACE *pSurface = (VIOGPU3D_SURFACE *)(pCreate + 1);
        if (pSurface->Magic == VIOGPU3D_SURFACE_MAGIC && pSurface->Size == sizeof(VIOGPU3D_SURFACE) &&
            (pSurface->Kind == VIOGPU3D_SURFACE_KERNEL_BLOB || pSurface->Kind == VIOGPU3D_SURFACE_KERNEL_CPU) &&
            pSurface->Cookie == m_pAdapter->GetSurfaceCookie())
        {
            // Processes that open the allocation read this record.
            pSurface->Cookie = 0;
            return InitKernelSurface(pInfo, pCreate, pSurface);
        }
    }

    switch (m_Info.Base.Tag)
    {
        case VIRTIO_WDDM_ALLOCATE_BLOB_TAG:
            {
                VIRTIO_WDDM_ALLOCATE_BLOB *pBlob = &m_Info.Base.Blob;
                if (!m_pAdapter->HasVenus() || pBlob->Mem != VIRTIO_WDDM_BLOB_MEM_HOST3D || pBlob->Size == 0 ||
                    (pBlob->Flags & ~(VIRTIO_WDDM_BLOB_FLAG_MAPPABLE | VIRTIO_WDDM_BLOB_FLAG_SHAREABLE |
                                      VIRTIO_WDDM_BLOB_FLAG_CROSS_DEVICE)) != 0)
                {
                    return STATUS_INVALID_PARAMETER;
                }
                // The host maps a blob only in whole pages of its own, 16 KiB on a Mac; the real size goes back.
                ULONGLONG alignment = m_pAdapter->GetBlobAlignment();
                if (pBlob->Size > MAXULONG - alignment)
                {
                    return STATUS_INVALID_PARAMETER;
                }
                pBlob->Size = AlignUp(pBlob->Size, alignment);
                m_Kind = VIOGPU3D_ALLOCATION_BLOB;
                m_Width = pBlob->Info.Width;
                m_Height = pBlob->Info.Height;
                m_Pitch = m_Info.NumPlanes > 0 ? (UINT)m_Info.Planes[0].Stride : m_Width * 4;
                // The blob lives in the host; VidMm keeps a page of the aperture for it.
                pInfo->Size = PAGE_SIZE;

                if (pResource != NULL && resourceSize > sizeof(VIRTIO_WDDM_CREATE_RESOURCE) &&
                    pResource->Tag == VIRTIO_WDDM_CREATE_RESOURCE_TAG)
                {
                    UINT preludeSize = resourceSize - sizeof(VIRTIO_WDDM_CREATE_RESOURCE);
                    if (preludeSize > VIRTIO_WDDM_MAX_SUBMIT_COMMAND_VIRTUAL_SIZE)
                    {
                        return STATUS_INVALID_PARAMETER;
                    }
                    m_pPrelude = new (NonPagedPoolNx) UCHAR[preludeSize];
                    if (m_pPrelude == NULL)
                    {
                        return STATUS_NO_MEMORY;
                    }
                    RtlCopyMemory(m_pPrelude, pResource + 1, preludeSize);
                    m_PreludeSize = preludeSize;
                }
                break;
            }
        case VIRTIO_WDDM_ALLOCATE_3D_TAG:
            {
                VIRTIO_WDDM_ALLOCATE_3D *p3d = &m_Info.Base.Resource3d;
                if (p3d->Size == 0 || p3d->Size > VIOGPU3D_APERTURE_SIZE || p3d->Width == 0 || p3d->Height == 0)
                {
                    return STATUS_INVALID_PARAMETER;
                }
                m_Kind = VIOGPU3D_ALLOCATION_3D;
                m_Width = p3d->Width;
                m_Height = p3d->Height;
                m_Pitch = m_Width * 4;
                pInfo->Size = (SIZE_T)AlignUp(p3d->Size, PAGE_SIZE);
                break;
            }
        default:
            return STATUS_INVALID_PARAMETER;
    }

    m_ResId = m_pAdapter->resourceIdr.GetId();
    if (m_ResId == 0)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    m_Info.ResId = m_ResId;
    pCreate->ResId = m_ResId;
    pCreate->Base = m_Info.Base;

    if (m_Kind == VIOGPU3D_ALLOCATION_3D)
    {
        // A 3D resource needs no context; the devices that open it attach it to theirs.
        VIRTIO_WDDM_ALLOCATE_3D *p3d = &m_Info.Base.Resource3d;
        GPU_CMD_RES_CREATE_3D create = {};
        create.resource_id = m_ResId;
        create.target = p3d->Target;
        create.format = p3d->Format;
        create.bind = p3d->Bind;
        create.width = p3d->Width;
        create.height = p3d->Height;
        create.depth = p3d->Depth;
        create.array_size = p3d->ArraySize;
        create.last_level = p3d->LastLevel;
        create.nr_samples = p3d->NrSamples;
        create.flags = p3d->Flags;
        if (m_pAdapter->ctrlQueue.CreateResource3D(0, &create) != VIRTIO_GPU_RESP_OK_NODATA)
        {
            return STATUS_UNSUCCESSFUL;
        }
        m_Created = TRUE;
    }
    return STATUS_SUCCESS;
}

NTSTATUS VioGpu3dAllocation::InitKernelSurface(_Inout_ DXGK_ALLOCATIONINFO *pInfo,
                                               _Inout_ VIRTIO_WDDM_CREATE_ALLOCATION *pCreate,
                                               _In_ const VIOGPU3D_SURFACE *pSurface)
{
    PAGED_CODE();

    if (pSurface->Width == 0 || pSurface->Height == 0 || pSurface->Width > VIOGPU3D_MAX_DISPLAY_SIDE ||
        pSurface->Height > VIOGPU3D_MAX_DISPLAY_SIDE || pSurface->Pitch < pSurface->Width * 4 ||
        pSurface->Pitch % 4 != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }
    m_KernelSurface = TRUE;
    m_StandardType = pSurface->StandardType;
    m_Primary = pSurface->StandardType == D3DKMDT_STANDARDALLOCATION_SHAREDPRIMARYSURFACE;
    m_Width = pSurface->Width;
    m_Height = pSurface->Height;
    m_Pitch = pSurface->Pitch;
    m_Format = (D3DDDIFORMAT)pSurface->D3dFormat;
    ULONGLONG bytes = (ULONGLONG)m_Pitch * m_Height;

    if (pSurface->Kind == VIOGPU3D_SURFACE_KERNEL_CPU)
    {
        m_Kind = VIOGPU3D_ALLOCATION_CPU;
        pInfo->FlagsWddm2.CpuVisible = 1;
        pInfo->Size = (SIZE_T)AlignUp(bytes, PAGE_SIZE);
        return STATUS_SUCCESS;
    }

    VIRTIO_WDDM_ALLOCATE_BLOB *pBlob = &m_Info.Base.Blob;
    // No process's device makes these, and every process imports them.
    UINT ctxId = m_pAdapter->GetOwnCtxId();
    if (ctxId == 0 || m_Info.Base.Tag != VIRTIO_WDDM_ALLOCATE_BLOB_TAG || pBlob->Size < bytes ||
        pBlob->Size % m_pAdapter->GetBlobAlignment() != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }
    m_Kind = VIOGPU3D_ALLOCATION_BLOB;
    pInfo->Size = PAGE_SIZE;
    m_ResId = m_pAdapter->resourceIdr.GetId();
    if (m_ResId == 0)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    m_Info.ResId = m_ResId;
    pCreate->ResId = m_ResId;

    // Blob id 0 and only MAPPABLE: Venus' renderer makes plain shared memory, with no Vulkan object in the context.
    UINT reply = m_pAdapter->ctrlQueue.CreateBlob(ctxId,
                                                  m_ResId,
                                                  VIRTIO_GPU_BLOB_MEM_HOST3D,
                                                  VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE,
                                                  0,
                                                  pBlob->Size,
                                                  NULL,
                                                  0);
    if (reply != VIRTIO_GPU_RESP_OK_NODATA)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s surface of type %u refused with %x\n", __FUNCTION__, pSurface->StandardType, reply));
        return STATUS_UNSUCCESSFUL;
    }
    m_Created = TRUE;
    m_CreatorCtxId = ctxId;

    // A full region fails the creation here, not a present later.
    LockMap(&m_MapLock);
    NTSTATUS status = HostMapLocked();
    UnlockMap(&m_MapLock);
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("%s surface blob %u not mapped: %x\n", __FUNCTION__, m_ResId, status));
    }
    return status;
}

// The command a resource's private data carried, which the host runs before it exports the blob.
NTSTATUS VioGpu3dAllocation::SendPrelude(_In_ VioGpu3dDevice *pDevice)
{
    PAGED_CODE();

    if (m_PreludeSize == 0)
    {
        return STATUS_SUCCESS;
    }
    if (m_PreludeSize < sizeof(VIRTIO_WDDM_COMMAND_HEADER))
    {
        return STATUS_INVALID_PARAMETER;
    }
    VIRTIO_WDDM_COMMAND_HEADER *pHeader = (VIRTIO_WDDM_COMMAND_HEADER *)m_pPrelude;
    if (pHeader->Id != VIRTIO_WDDM_COMMAND_ID_SUBMIT || pHeader->Size == 0 ||
        pHeader->Size > m_PreludeSize - sizeof(*pHeader))
    {
        return STATUS_INVALID_PARAMETER;
    }
    PUCHAR data = new (NonPagedPoolNx) UCHAR[pHeader->Size];
    if (data == NULL)
    {
        return STATUS_NO_MEMORY;
    }
    RtlCopyMemory(data, pHeader + 1, pHeader->Size);
    // No fence: the queue keeps the order, so the blob is made after this ran.
    if (m_pAdapter->ctrlQueue.Submit3D(pDevice->GetCtxId(), 0, FALSE, 0, data, pHeader->Size, NULL, NULL) != 0)
    {
        return STATUS_UNSUCCESSFUL;
    }
    return STATUS_SUCCESS;
}

NTSTATUS VioGpu3dAllocation::CreateOnHost(_In_ VioGpu3dDevice *pDevice)
{
    PAGED_CODE();

    if (m_Kind != VIOGPU3D_ALLOCATION_BLOB || m_KernelSurface)
    {
        return Open(pDevice);
    }
    if (m_Created)
    {
        return STATUS_SUCCESS;
    }
    if (pDevice->GetCtxId() == 0)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("%s blob %u of a device without a context\n", __FUNCTION__, m_ResId));
        return STATUS_INVALID_PARAMETER;
    }
    NTSTATUS status = SendPrelude(pDevice);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    VIRTIO_WDDM_ALLOCATE_BLOB *pBlob = &m_Info.Base.Blob;
    UINT flags = 0;
    flags |= (pBlob->Flags & VIRTIO_WDDM_BLOB_FLAG_MAPPABLE) ? VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE : 0;
    flags |= (pBlob->Flags & VIRTIO_WDDM_BLOB_FLAG_SHAREABLE) ? VIRTIO_GPU_BLOB_FLAG_USE_SHAREABLE : 0;
    flags |= (pBlob->Flags & VIRTIO_WDDM_BLOB_FLAG_CROSS_DEVICE) ? VIRTIO_GPU_BLOB_FLAG_USE_CROSS_DEVICE : 0;
    UINT reply = m_pAdapter->ctrlQueue.CreateBlob(pDevice->GetCtxId(),
                                                  m_ResId,
                                                  VIRTIO_GPU_BLOB_MEM_HOST3D,
                                                  flags,
                                                  pBlob->Id,
                                                  pBlob->Size,
                                                  NULL,
                                                  0);
    if (reply != VIRTIO_GPU_RESP_OK_NODATA)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("%s blob %u refused with %x\n", __FUNCTION__, m_ResId, reply));
        return STATUS_UNSUCCESSFUL;
    }
    m_Created = TRUE;
    m_CreatorCtxId = pDevice->GetCtxId();
    return STATUS_SUCCESS;
}

NTSTATUS VioGpu3dAllocation::Open(_In_ VioGpu3dDevice *pDevice)
{
    PAGED_CODE();

    if (m_Created && pDevice->GetCtxId() != 0)
    {
        m_pAdapter->ctrlQueue.AttachResourceToContext(pDevice->GetCtxId(), m_ResId, TRUE);
    }
    return STATUS_SUCCESS;
}

void VioGpu3dAllocation::Close(_In_ VioGpu3dDevice *pDevice)
{
    PAGED_CODE();

    // Views in the closing process go now, while its address space still holds them.
    LockMap(&m_MapLock);
    PLIST_ENTRY p = m_UserMappings.Flink;
    while (p != &m_UserMappings)
    {
        VIOGPU3D_USER_MAPPING *pMapping = CONTAINING_RECORD(p, VIOGPU3D_USER_MAPPING, Link);
        p = p->Flink;
        if (pMapping->Process == pDevice->GetProcess() && pMapping->Process == PsGetCurrentProcess())
        {
            UnmapViewLocked(pMapping);
            RemoveEntryList(&pMapping->Link);
            delete pMapping;
        }
    }
    UnlockMap(&m_MapLock);

    if (m_Created && pDevice->GetCtxId() != 0 && pDevice->GetCtxId() != m_CreatorCtxId)
    {
        m_pAdapter->ctrlQueue.AttachResourceToContext(pDevice->GetCtxId(), m_ResId, FALSE);
    }
}

NTSTATUS VioGpu3dAllocation::HostMapLocked()
{
    PAGED_CODE();

    if (m_HostMapped)
    {
        return STATUS_SUCCESS;
    }
    VioGpu3dHostVisible *pRegion = m_pAdapter->GetHostVisible();
    ULONGLONG size = m_Info.Base.Blob.Size;
    ULONGLONG offset = 0;
    NTSTATUS status = pRegion->Allocate(size, &offset);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    switch (m_pAdapter->ctrlQueue.MapBlob(m_CreatorCtxId, m_ResId, offset))
    {
        case VIRTIO_GPU_MAP_CACHE_CACHED:
            m_CacheType = MmCached;
            break;
        case VIRTIO_GPU_MAP_CACHE_WC:
            m_CacheType = MmWriteCombined;
            break;
        case VIRTIO_GPU_MAP_CACHE_UNCACHED:
            m_CacheType = MmNonCached;
            break;
        default:
            pRegion->Free(offset, size);
            return STATUS_UNSUCCESSFUL;
    }
    m_HostMapped = TRUE;
    m_RegionOffset = offset;
    m_RegionSize = size;
    return STATUS_SUCCESS;
}

NTSTATUS VioGpu3dAllocation::SystemViewLocked()
{
    PAGED_CODE();

    if (m_pKernelVa != NULL)
    {
        return STATUS_SUCCESS;
    }
    ULONG protect = PAGE_READWRITE;
    if (m_CacheType == MmWriteCombined)
    {
        protect |= PAGE_WRITECOMBINE;
    }
    else if (m_CacheType == MmNonCached)
    {
        protect |= PAGE_NOCACHE;
    }
    PHYSICAL_ADDRESS pa = m_pAdapter->GetHostVisible()->GetBase();
    pa.QuadPart += m_RegionOffset;
    m_pKernelVa = MmMapIoSpaceEx(pa, (SIZE_T)m_RegionSize, protect);
    return m_pKernelVa != NULL ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

NTSTATUS VioGpu3dAllocation::RegionMdlLocked()
{
    PAGED_CODE();

    if (m_pRegionMdl != NULL)
    {
        return STATUS_SUCCESS;
    }
    // IoAllocateMdl takes less than 4 GiB.
    if (m_RegionSize > MAXULONG - PAGE_SIZE)
    {
        return STATUS_INVALID_PARAMETER;
    }
    NTSTATUS status = SystemViewLocked();
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    PMDL pMdl = IoAllocateMdl(m_pKernelVa, (ULONG)m_RegionSize, FALSE, FALSE, NULL);
    if (pMdl == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    MmBuildMdlForNonPagedPool(pMdl);
    m_pRegionMdl = pMdl;
    return STATUS_SUCCESS;
}

// A WOW64 process reads only the low half of the pointer the escape returns.
NTSTATUS VioGpu3dAllocation::MapBelow4GLocked(_Out_ PVOID *pAddress)
{
    PAGED_CODE();

    *pAddress = NULL;
    NTSTATUS status = RegionMdlLocked();
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    PVOID address = NULL;
    // Documented to land in a 32-bit process's 32-bit range; DxgkCbMapMemory promises nothing of the kind.
    __try
    {
        address = MmMapLockedPagesSpecifyCache(m_pRegionMdl, UserMode, m_CacheType, NULL, FALSE, NormalPagePriority);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        address = NULL;
    }
    if (address == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if ((ULONGLONG)(ULONG_PTR)address + m_RegionSize > 0x100000000ull)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s blob %u mapped at %p, past a 32-bit process's 4 GiB\n", __FUNCTION__, m_ResId, address));
        MmUnmapLockedPages(address, m_pRegionMdl);
        return STATUS_CONFLICTING_ADDRESSES;
    }
    *pAddress = address;
    return STATUS_SUCCESS;
}

NTSTATUS VioGpu3dAllocation::UnmapViewLocked(_In_ const VIOGPU3D_USER_MAPPING *pMapping)
{
    PAGED_CODE();

    if (pMapping->Below4G)
    {
        MmUnmapLockedPages(pMapping->Address, m_pRegionMdl);
        return STATUS_SUCCESS;
    }
    return m_pAdapter->GetDxgkInterface()->DxgkCbUnmapMemory(m_pAdapter->GetDxgkInterface()->DeviceHandle,
                                                             pMapping->Address);
}

NTSTATUS VioGpu3dAllocation::MapToProcess(_Out_ PVOID *pAddress)
{
    PAGED_CODE();

    *pAddress = NULL;
    if (m_Kind != VIOGPU3D_ALLOCATION_BLOB || !m_Created || !(m_Info.Base.Blob.Flags & VIRTIO_WDDM_BLOB_FLAG_MAPPABLE))
    {
        return STATUS_INVALID_PARAMETER;
    }

    LockMap(&m_MapLock);
    NTSTATUS status = HostMapLocked();
    VIOGPU3D_USER_MAPPING *pMapping = NULL;
    if (NT_SUCCESS(status))
    {
        pMapping = new (NonPagedPoolNx) VIOGPU3D_USER_MAPPING;
        status = pMapping ? STATUS_SUCCESS : STATUS_NO_MEMORY;
    }
    if (NT_SUCCESS(status))
    {
        PVOID address = NULL;
        BOOLEAN below4G = IoIs32bitProcess(NULL);
        if (below4G)
        {
            status = MapBelow4GLocked(&address);
        }
        else
        {
            PHYSICAL_ADDRESS pa = m_pAdapter->GetHostVisible()->GetBase();
            pa.QuadPart += m_RegionOffset;
            // The documented way for a display miniport to map device memory into the calling process.
            status = m_pAdapter->GetDxgkInterface()->DxgkCbMapMemory(m_pAdapter->GetDxgkInterface()->DeviceHandle,
                                                                     pa,
                                                                     (ULONG)m_RegionSize,
                                                                     FALSE,
                                                                     TRUE,
                                                                     m_CacheType,
                                                                     &address);
        }
        if (NT_SUCCESS(status))
        {
            pMapping->Process = PsGetCurrentProcess();
            pMapping->Address = address;
            pMapping->Below4G = below4G;
            InsertTailList(&m_UserMappings, &pMapping->Link);
            *pAddress = address;
            pMapping = NULL;
        }
    }
    delete pMapping;
    UnlockMap(&m_MapLock);
    return status;
}

NTSTATUS VioGpu3dAllocation::UnmapFromProcess(PVOID address)
{
    PAGED_CODE();

    NTSTATUS status = STATUS_INVALID_PARAMETER;
    LockMap(&m_MapLock);
    for (PLIST_ENTRY p = m_UserMappings.Flink; p != &m_UserMappings; p = p->Flink)
    {
        VIOGPU3D_USER_MAPPING *pMapping = CONTAINING_RECORD(p, VIOGPU3D_USER_MAPPING, Link);
        if (pMapping->Address == address && pMapping->Process == PsGetCurrentProcess())
        {
            status = UnmapViewLocked(pMapping);
            RemoveEntryList(&pMapping->Link);
            delete pMapping;
            break;
        }
    }
    UnlockMap(&m_MapLock);
    return status;
}

void VioGpu3dAllocation::ReleaseHostMapping()
{
    PAGED_CODE();

    BOOLEAN stale = FALSE;
    LockMap(&m_MapLock);
    while (!IsListEmpty(&m_UserMappings))
    {
        VIOGPU3D_USER_MAPPING *pMapping = CONTAINING_RECORD(RemoveHeadList(&m_UserMappings),
                                                            VIOGPU3D_USER_MAPPING,
                                                            Link);
        if (pMapping->Process == PsGetCurrentProcess())
        {
            UnmapViewLocked(pMapping);
        }
        else
        {
            stale = TRUE;
        }
        delete pMapping;
    }
    if (m_HostMapped)
    {
        if (stale)
        {
            // Another process may still see this range: it stays out of use rather than show it another blob.
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("%s blob %u kept mapped for a view of another process\n", __FUNCTION__, m_ResId));
        }
        else
        {
            if (m_pRegionMdl != NULL)
            {
                IoFreeMdl(m_pRegionMdl);
            }
            if (m_pKernelVa != NULL)
            {
                MmUnmapIoSpace(m_pKernelVa, (SIZE_T)m_RegionSize);
            }
            m_pAdapter->ctrlQueue.UnmapBlob(m_CreatorCtxId, m_ResId);
            m_pAdapter->GetHostVisible()->Free(m_RegionOffset, m_RegionSize);
        }
        m_HostMapped = FALSE;
        m_pRegionMdl = NULL;
        m_pKernelVa = NULL;
    }
    UnlockMap(&m_MapLock);
}

NTSTATUS VioGpu3dAllocation::AttachBacking(_In_ PMDL pMdl, SIZE_T offset, SIZE_T pages)
{
    PAGED_CODE();

    if (pMdl == NULL || pages == 0)
    {
        return STATUS_SUCCESS;
    }
    if (m_Kind == VIOGPU3D_ALLOCATION_CPU)
    {
        // The present reaches the pages through VidMm's MDL while a DMA buffer names the allocation.
        if (pages * PAGE_SIZE >= (ULONGLONG)m_Pitch * m_Height && offset <= MAXULONG)
        {
            m_pApertureMdl = pMdl;
            m_ApertureMdlOffset = (ULONG)offset;
        }
        return STATUS_SUCCESS;
    }
    if (m_Kind != VIOGPU3D_ALLOCATION_3D)
    {
        return STATUS_SUCCESS;
    }
    if (m_Backed)
    {
        DetachBacking();
    }
    PPFN_NUMBER pfns = MmGetMdlPfnArray(pMdl) + offset;
    // One entry for each run of physically adjacent pages.
    UINT runs = 1;
    for (SIZE_T i = 1; i < pages; i++)
    {
        if (pfns[i] != pfns[i - 1] + 1)
        {
            runs++;
        }
    }
    PGPU_MEM_ENTRY ents = reinterpret_cast<PGPU_MEM_ENTRY>(new (NonPagedPoolNx) BYTE[sizeof(GPU_MEM_ENTRY) * runs]);
    if (ents == NULL)
    {
        return STATUS_NO_MEMORY;
    }
    UINT entry = 0;
    ents[0].addr = (ULONGLONG)pfns[0] << PAGE_SHIFT;
    ents[0].length = PAGE_SIZE;
    ents[0].padding = 0;
    for (SIZE_T i = 1; i < pages; i++)
    {
        if (pfns[i] == pfns[i - 1] + 1)
        {
            ents[entry].length += PAGE_SIZE;
            continue;
        }
        entry++;
        ents[entry].addr = (ULONGLONG)pfns[i] << PAGE_SHIFT;
        ents[entry].length = PAGE_SIZE;
        ents[entry].padding = 0;
    }
    m_pAdapter->ctrlQueue.AttachBacking(m_ResId, ents, runs);
    m_Backed = TRUE;
    return STATUS_SUCCESS;
}

void VioGpu3dAllocation::DetachBacking()
{
    PAGED_CODE();

    m_pApertureMdl = NULL;
    m_ApertureMdlOffset = 0;
    if (m_Backed)
    {
        m_pAdapter->ctrlQueue.DetachBacking(m_ResId);
        m_Backed = FALSE;
    }
}

BOOLEAN VioGpu3dAllocation::IsPresentable() const
{
    PAGED_CODE();

    if (m_Width == 0 || m_Height == 0 || m_Pitch < m_Width * 4)
    {
        return FALSE;
    }
    if (m_Kind == VIOGPU3D_ALLOCATION_CPU)
    {
        return TRUE;
    }
    if (m_Kind != VIOGPU3D_ALLOCATION_BLOB || !m_Created ||
        !(m_Info.Base.Blob.Flags & VIRTIO_WDDM_BLOB_FLAG_MAPPABLE) ||
        (ULONGLONG)m_Pitch * m_Height > m_Info.Base.Blob.Size)
    {
        return FALSE;
    }
    if (m_KernelSurface)
    {
        return TRUE;
    }
    // Venus described its rows, in the byte order of the kernel driver's surfaces.
    return m_Info.NumPlanes == 1 && (m_Info.Base.Blob.Info.Format == VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM ||
                                     m_Info.Base.Blob.Info.Format == VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM);
}

BOOLEAN VioGpu3dAllocation::GetCpuView(_Out_ VIOGPU3D_CPU_VIEW *pView)
{
    PAGED_CODE();

    RtlZeroMemory(pView, sizeof(*pView));
    if (!IsPresentable())
    {
        return FALSE;
    }
    pView->Width = m_Width;
    pView->Height = m_Height;
    pView->Pitch = m_Pitch;
    if (m_Kind == VIOGPU3D_ALLOCATION_CPU)
    {
        PMDL pMdl = m_pApertureMdl;
        if (pMdl == NULL)
        {
            return FALSE;
        }
        PUCHAR base = (PUCHAR)MmGetSystemAddressForMdlSafe(pMdl, NormalPagePriority | MdlMappingNoExecute);
        if (base == NULL)
        {
            return FALSE;
        }
        pView->Bits = base + (SIZE_T)m_ApertureMdlOffset * PAGE_SIZE;
        pView->Cached = TRUE;
        return TRUE;
    }
    LockMap(&m_MapLock);
    NTSTATUS status = HostMapLocked();
    if (NT_SUCCESS(status))
    {
        status = SystemViewLocked();
    }
    PVOID va = m_pKernelVa;
    MEMORY_CACHING_TYPE cacheType = m_CacheType;
    UnlockMap(&m_MapLock);
    if (!NT_SUCCESS(status) || va == NULL)
    {
        return FALSE;
    }
    pView->Bits = (PUCHAR)va;
    pView->Cached = cacheType == MmCached;
    return TRUE;
}

void VioGpu3dAllocation::DescribeAllocation(_Inout_ DXGKARG_DESCRIBEALLOCATION *pDescribe) const
{
    PAGED_CODE();

    pDescribe->Width = m_Width;
    pDescribe->Height = m_Height;
    pDescribe->Format = m_Format;
    pDescribe->MultisampleMethod.NumSamples = 0;
    pDescribe->MultisampleMethod.NumQualityLevels = 0;
    pDescribe->RefreshRate.Numerator = 60000;
    pDescribe->RefreshRate.Denominator = 1000;
    pDescribe->PrivateDriverFormatAttribute = 0;
}

void VioGpu3dAllocation::SetPrimary(D3DDDI_VIDEO_PRESENT_SOURCE_ID sourceId,
                                    UINT width,
                                    UINT height,
                                    UINT pitch,
                                    D3DDDIFORMAT format)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(sourceId);
    m_Primary = TRUE;
    m_Width = width;
    m_Height = height;
    m_Pitch = pitch;
    m_Format = format;
}

NTSTATUS VioGpu3dAdapter::CreateAllocation(_Inout_ DXGKARG_CREATEALLOCATION *pCreateAllocation)
{
    PAGED_CODE();

    const VIRTIO_WDDM_CREATE_RESOURCE *pResource = NULL;
    UINT resourceSize = pCreateAllocation->PrivateDriverDataSize;
    if (pCreateAllocation->pPrivateDriverData != NULL && resourceSize >= sizeof(VIRTIO_WDDM_CREATE_RESOURCE))
    {
        pResource = (const VIRTIO_WDDM_CREATE_RESOURCE *)pCreateAllocation->pPrivateDriverData;
    }

    for (UINT i = 0; i < pCreateAllocation->NumAllocations; i++)
    {
        DXGK_ALLOCATIONINFO *pInfo = &pCreateAllocation->pAllocationInfo[i];
        VioGpu3dAllocation *pAllocation = new (NonPagedPoolNx) VioGpu3dAllocation(this);
        NTSTATUS status = pAllocation ? pAllocation->Init(pInfo, pResource, resourceSize) : STATUS_NO_MEMORY;
        if (!NT_SUCCESS(status))
        {
            delete pAllocation;
            for (UINT j = 0; j < i; j++)
            {
                delete reinterpret_cast<VioGpu3dAllocation *>(pCreateAllocation->pAllocationInfo[j].hAllocation);
                pCreateAllocation->pAllocationInfo[j].hAllocation = NULL;
            }
            pInfo->hAllocation = NULL;
            DbgPrint(TRACE_LEVEL_ERROR, ("%s allocation %u refused with %x\n", __FUNCTION__, i, status));
            return status;
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS VioGpu3dAdapter::DestroyAllocation(_In_ CONST DXGKARG_DESTROYALLOCATION *pDestroyAllocation)
{
    PAGED_CODE();

    for (UINT i = 0; i < pDestroyAllocation->NumAllocations; i++)
    {
        delete reinterpret_cast<VioGpu3dAllocation *>(pDestroyAllocation->pAllocationList[i]);
    }
    return STATUS_SUCCESS;
}

// A shared texture's record on every surface, so that a process that opens one knows what it is.
NTSTATUS VioGpu3dAdapter::GetStandardAllocationDriverData(_Inout_ DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *pStandard)
{
    PAGED_CODE();

    UINT width = 0, height = 0, kind = VIOGPU3D_SURFACE_KERNEL_CPU;
    D3DDDIFORMAT format = D3DDDIFMT_X8R8G8B8;
    D3DDDI_VIDEO_PRESENT_SOURCE_ID sourceId = D3DDDI_ID_UNINITIALIZED;
    UINT *pPitch = NULL;
    switch (pStandard->StandardAllocationType)
    {
        case D3DKMDT_STANDARDALLOCATION_SHAREDPRIMARYSURFACE:
            width = pStandard->pCreateSharedPrimarySurfaceData->Width;
            height = pStandard->pCreateSharedPrimarySurfaceData->Height;
            format = pStandard->pCreateSharedPrimarySurfaceData->Format;
            sourceId = pStandard->pCreateSharedPrimarySurfaceData->VidPnSourceId;
            kind = VIOGPU3D_SURFACE_KERNEL_BLOB;
            break;
        case D3DKMDT_STANDARDALLOCATION_SHADOWSURFACE:
            width = pStandard->pCreateShadowSurfaceData->Width;
            height = pStandard->pCreateShadowSurfaceData->Height;
            format = pStandard->pCreateShadowSurfaceData->Format;
            pPitch = &pStandard->pCreateShadowSurfaceData->Pitch;
            break;
        case D3DKMDT_STANDARDALLOCATION_STAGINGSURFACE:
            width = pStandard->pCreateStagingSurfaceData->Width;
            height = pStandard->pCreateStagingSurfaceData->Height;
            pPitch = &pStandard->pCreateStagingSurfaceData->Pitch;
            break;
        case D3DKMDT_STANDARDALLOCATION_GDISURFACE:
            width = pStandard->pCreateGdiSurfaceData->Width;
            height = pStandard->pCreateGdiSurfaceData->Height;
            format = pStandard->pCreateGdiSurfaceData->Format;
            pPitch = &pStandard->pCreateGdiSurfaceData->Pitch;
            switch (pStandard->pCreateGdiSurfaceData->Type)
            {
                case D3DKMDT_GDISURFACE_TEXTURE:
                    kind = VIOGPU3D_SURFACE_KERNEL_BLOB;
                    break;
                case D3DKMDT_GDISURFACE_STAGING_CPUVISIBLE:
                    break;
                default:
                    DbgPrint(TRACE_LEVEL_WARNING,
                             ("%s GDI surface of type %d refused\n",
                              __FUNCTION__,
                              pStandard->pCreateGdiSurfaceData->Type));
                    return STATUS_NOT_SUPPORTED;
            }
            break;
        default:
            return STATUS_NOT_SUPPORTED;
    }
    if (format != D3DDDIFMT_A8R8G8B8 && format != D3DDDIFMT_X8R8G8B8)
    {
        format = D3DDDIFMT_X8R8G8B8;
    }
    if (width == 0 || height == 0 || width > VIOGPU3D_MAX_DISPLAY_SIDE || height > VIOGPU3D_MAX_DISPLAY_SIDE)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (kind == VIOGPU3D_SURFACE_KERNEL_BLOB && m_OwnCtxId == 0)
    {
        return STATUS_NOT_SUPPORTED;
    }
    UINT pitch = (UINT)AlignUp((ULONGLONG)width * 4, VIOGPU3D_SURFACE_PITCH_ALIGNMENT);
    if (pPitch != NULL)
    {
        *pPitch = pitch;
    }

    pStandard->AllocationPrivateDriverDataSize = sizeof(VIRTIO_WDDM_CREATE_ALLOCATION) + sizeof(VIOGPU3D_SURFACE);
    pStandard->ResourcePrivateDriverDataSize = 0;
    if (pStandard->pAllocationPrivateDriverData == NULL)
    {
        return STATUS_SUCCESS;
    }

    VIRTIO_WDDM_CREATE_ALLOCATION *pCreate = (VIRTIO_WDDM_CREATE_ALLOCATION *)pStandard->pAllocationPrivateDriverData;
    RtlZeroMemory(pCreate, pStandard->AllocationPrivateDriverDataSize);
    pCreate->Modifier = VIRTIO_WDDM_MODIFIER_INVALID;
    if (kind == VIOGPU3D_SURFACE_KERNEL_BLOB)
    {
        VIRTIO_WDDM_ALLOCATE_BLOB *pBlob = &pCreate->Base.Blob;
        pBlob->Tag = VIRTIO_WDDM_ALLOCATE_BLOB_TAG;
        pBlob->Mem = VIRTIO_WDDM_BLOB_MEM_HOST3D;
        pBlob->Flags = VIRTIO_WDDM_BLOB_FLAG_MAPPABLE | VIRTIO_WDDM_BLOB_FLAG_SHAREABLE;
        pBlob->Size = AlignUp((ULONGLONG)pitch * height, m_BlobAlignment);
        pBlob->Info.Width = width;
        pBlob->Info.Height = height;
        pBlob->Info.Format = ColorFormat(format);
        pBlob->Info.Bind = VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW | VIRGL_BIND_DISPLAY_TARGET |
                           VIRGL_BIND_SCANOUT;
        pCreate->NumPlanes = 1;
        pCreate->Planes[0].Offset = 0;
        pCreate->Planes[0].Size = (UINT64)pitch * height;
        pCreate->Planes[0].Stride = pitch;
    }

    VIOGPU3D_SURFACE *pSurface = (VIOGPU3D_SURFACE *)(pCreate + 1);
    pSurface->Magic = VIOGPU3D_SURFACE_MAGIC;
    pSurface->Size = sizeof(VIOGPU3D_SURFACE);
    pSurface->Kind = kind;
    pSurface->Flags = VIOGPU3D_SURFACE_FLAG_LINEAR;
    pSurface->Width = width;
    pSurface->Height = height;
    pSurface->MipLevels = 1;
    pSurface->ArraySize = 1;
    pSurface->Format = format == D3DDDIFMT_A8R8G8B8 ? VIOGPU3D_DXGI_FORMAT_B8G8R8A8_UNORM
                                                    : VIOGPU3D_DXGI_FORMAT_B8G8R8X8_UNORM;
    pSurface->SampleCount = 1;
    pSurface->Usage = VIOGPU3D_D3D11_USAGE_DEFAULT;
    pSurface->BindFlags = VIOGPU3D_D3D11_BIND_RENDER_TARGET | VIOGPU3D_D3D11_BIND_SHADER_RESOURCE;
    pSurface->CpuAccessFlags = 0;
    pSurface->MiscFlags = VIOGPU3D_D3D11_RESOURCE_MISC_SHARED;
    pSurface->Pitch = pitch;
    pSurface->StandardType = pStandard->StandardAllocationType;
    pSurface->VidPnSourceId = sourceId;
    pSurface->D3dFormat = format;
    pSurface->Cookie = m_SurfaceCookie;
    return STATUS_SUCCESS;
}

NTSTATUS VioGpu3dAdapter::BuildPagingBuffer(_Inout_ DXGKARG_BUILDPAGINGBUFFER *pBuildPagingBuffer)
{
    PAGED_CODE();

    switch (pBuildPagingBuffer->Operation)
    {
        case DXGK_OPERATION_MAP_APERTURE_SEGMENT:
            {
                VioGpu3dAllocation *pAllocation = reinterpret_cast<VioGpu3dAllocation *>(pBuildPagingBuffer->MapApertureSegment.hAllocation);
                if (pAllocation == NULL)
                {
                    return STATUS_SUCCESS;
                }
                return pAllocation->AttachBacking(pBuildPagingBuffer->MapApertureSegment.pMdl,
                                                  pBuildPagingBuffer->MapApertureSegment.MdlOffset,
                                                  pBuildPagingBuffer->MapApertureSegment.NumberOfPages);
            }
        case DXGK_OPERATION_UNMAP_APERTURE_SEGMENT:
            {
                VioGpu3dAllocation *pAllocation = reinterpret_cast<VioGpu3dAllocation *>(pBuildPagingBuffer->UnmapApertureSegment.hAllocation);
                if (pAllocation != NULL)
                {
                    pAllocation->DetachBacking();
                }
                return STATUS_SUCCESS;
            }
        case DXGK_OPERATION_UPDATE_PAGE_TABLE:
            {
                // VidMm hands over a CPU mapping of the table, in the update mode GPUMMUCAPS asked for.
                DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE *pUpdate = &pBuildPagingBuffer->UpdatePageTable;
                if (pUpdate->UpdateMode != DXGK_PAGETABLEUPDATE_CPU_VIRTUAL || pUpdate->Flags.Use64KBPages ||
                    pUpdate->PageTableLevel >= VIOGPU3D_PT_LEVELS || pUpdate->StartIndex > VIOGPU3D_PT_ENTRIES ||
                    pUpdate->NumPageTableEntries > VIOGPU3D_PT_ENTRIES - pUpdate->StartIndex)
                {
                    return STATUS_INVALID_PARAMETER;
                }
                if (pUpdate->NumPageTableEntries == 0)
                {
                    return STATUS_SUCCESS;
                }
                DXGK_PTE *pDest = (DXGK_PTE *)pUpdate->PageTableAddress.CpuVirtual;
                if (pDest == NULL || pUpdate->pPageTableEntries == NULL)
                {
                    return STATUS_INVALID_PARAMETER;
                }
                pDest += pUpdate->StartIndex;
                if (pUpdate->Flags.Repeat)
                {
                    for (UINT i = 0; i < pUpdate->NumPageTableEntries; i++)
                    {
                        pDest[i] = pUpdate->pPageTableEntries[0];
                    }
                }
                else
                {
                    RtlCopyMemory(pDest, pUpdate->pPageTableEntries, pUpdate->NumPageTableEntries * sizeof(DXGK_PTE));
                }
                return STATUS_SUCCESS;
            }
        case DXGK_OPERATION_FLUSH_TLB:
        case DXGK_OPERATION_NOTIFY_RESIDENCY:
        case DXGK_OPERATION_DISCARD_CONTENT:
        case DXGK_OPERATION_INIT_CONTEXT_RESOURCE:
            // Nothing caches translations or reads the tables: a virtio GPU addresses memory by resource.
            return STATUS_SUCCESS;
        default:
            // The two segments are system memory the device never walks by address, so content moves within them
            // need no copy the CPU has not made already.
            DbgPrint(TRACE_LEVEL_WARNING,
                     ("%s paging operation %d done as nothing\n", __FUNCTION__, pBuildPagingBuffer->Operation));
            return STATUS_SUCCESS;
    }
}

PAGED_CODE_SEG_END
