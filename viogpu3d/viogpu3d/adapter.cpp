/*
 * Copyright (C) 2019-2020 Red Hat, Inc.
 * Copyright (C) 2023 Max R (viogpu3d, virtio-win pull request 943)
 * Copyright (C) 2026 OSMates contributors
 *
 * Written By: Vadim Rozenfeld <vrozenfe@redhat.com>
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
#include "vidpn.h"
#include "baseobj.h"
#include "bitops.h"
#if !DBG
#include "adapter.tmh"
#endif

struct NOTIFY_CONTEXT
{
    DXGKRNL_INTERFACE *pDxgkInterface;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA *interrupt;
    BOOL triggerDpc;
};

static BOOLEAN NotifyRoutine(PVOID ctx_void)
{
    NOTIFY_CONTEXT *ctx = (NOTIFY_CONTEXT *)ctx_void;
    DXGKRNL_INTERFACE *pDxgkInterface = ctx->pDxgkInterface;
    pDxgkInterface->DxgkCbNotifyInterrupt(pDxgkInterface->DeviceHandle, ctx->interrupt);
    if (ctx->triggerDpc)
    {
        pDxgkInterface->DxgkCbQueueDpc(pDxgkInterface->DeviceHandle);
    }
    return TRUE;
}

NTSTATUS VioGpu3dAdapter::NotifyInterrupt(DXGKARGCB_NOTIFY_INTERRUPT_DATA *interruptData, BOOL triggerDpc)
{
    NOTIFY_CONTEXT notify;
    notify.pDxgkInterface = &m_DxgkInterface;
    notify.interrupt = interruptData;
    notify.triggerDpc = triggerDpc;
    BOOLEAN bRet;
    return m_DxgkInterface.DxgkCbSynchronizeExecution(m_DxgkInterface.DeviceHandle, NotifyRoutine, &notify, 0, &bRet);
}

virtio_gpu_formats ColorFormat(UINT format)
{
    switch (format)
    {
        case D3DDDIFMT_A8R8G8B8:
            return VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
        case D3DDDIFMT_X8R8G8B8:
            return VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
        case D3DDDIFMT_A8B8G8R8:
            return VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM;
        case D3DDDIFMT_X8B8G8R8:
            return VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM;
    }
    DbgPrint(TRACE_LEVEL_ERROR, ("---> %s Unsupported color format %d\n", __FUNCTION__, format));
    return VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
}

PAGED_CODE_SEG_BEGIN

VioGpu3dAdapter::VioGpu3dAdapter(_In_ DEVICE_OBJECT *pPhysicalDeviceObject)
    : m_pPhysicalDevice(pPhysicalDeviceObject), m_AdapterPowerState(PowerDeviceD0)
{
    PAGED_CODE();

    *((UINT *)&m_Flags) = 0;
    RtlZeroMemory(&m_DxgkInterface, sizeof(m_DxgkInterface));
    RtlZeroMemory(&m_DeviceInfo, sizeof(m_DeviceInfo));
    RtlZeroMemory(&m_VioDev, sizeof(m_VioDev));
    RtlZeroMemory(&m_AdapterLuid, sizeof(m_AdapterLuid));
    RtlZeroMemory(m_Capsets, sizeof(m_Capsets));
    m_CapsetMask = 0;
    m_PendingWorks = 0;
    KeInitializeEvent(&m_ConfigUpdateEvent, SynchronizationEvent, FALSE);
    m_bStopWorkThread = FALSE;
    m_pWorkThread = NULL;
    m_u64HostFeatures = 0;
    m_u64GuestFeatures = 0;
    m_u32NumCapsets = 0;
    m_u32NumScanouts = 0;
    m_HasVenus = FALSE;
    m_BlobAlignment = PAGE_SIZE;
    m_pPageTables = NULL;
    m_PageTablesPA.QuadPart = 0;
    m_NextFence = 0;
    KeInitializeSpinLock(&m_NodeLock);
    for (UINT node = 0; node < VIOGPU3D_NODE_COUNT; node++)
    {
        InitializeListHead(&m_Nodes[node].Pending);
        m_Nodes[node].LastSubmitted = 0;
        m_Nodes[node].LastCompleted = 0;
        m_Nodes[node].PreemptRequested = FALSE;
        m_Nodes[node].PreemptionFenceId = 0;
    }
    InitializeListHead(&m_EscapeFences);
    m_EscapeFenceCount = 0;
    vidpn = new (NonPagedPoolNx) VioGpuVidPN(this);
}

VioGpu3dAdapter::~VioGpu3dAdapter(void)
{
    PAGED_CODE();

    // The display lets go of its framebuffer while the queue still runs, and once nothing else calls it.
    StopWorkThread();
    delete vidpn;
    vidpn = NULL;
    VioGpuAdapterClose();
    HWClose();
}

BOOLEAN VioGpu3dAdapter::CheckHardware()
{
    PAGED_CODE();

    PCI_COMMON_HEADER Header = {};
    ULONG BytesRead;

    NTSTATUS Status = m_DxgkInterface.DxgkCbReadDeviceSpace(m_DxgkInterface.DeviceHandle,
                                                            DXGK_WHICHSPACE_CONFIG,
                                                            &Header,
                                                            0,
                                                            sizeof(Header),
                                                            &BytesRead);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("DxgkCbReadDeviceSpace failed with status 0x%X\n", Status));
        return FALSE;
    }
    if (Header.VendorID == REDHAT_PCI_VENDOR_ID && Header.DeviceID == 0x1050)
    {
        m_Flags.VgaDevice = Header.SubClass == PCI_SUBCLASS_VID_VGA_CTLR;
        return TRUE;
    }
    return FALSE;
}

NTSTATUS VioGpu3dAdapter::ReadRegistryDWORD(_In_ HANDLE hKey, _In_ PCWSTR pszwValueName, _Inout_ PDWORD pdwValue)
{
    PAGED_CODE();

    UNICODE_STRING name;
    RtlInitUnicodeString(&name, pszwValueName);
    UCHAR buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(DWORD)] = {};
    ULONG length = 0;
    NTSTATUS status = ZwQueryValueKey(hKey, &name, KeyValuePartialInformation, buffer, sizeof(buffer), &length);
    PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buffer;
    if (NT_SUCCESS(status) && info->Type == REG_DWORD && info->DataLength == sizeof(DWORD))
    {
        *pdwValue = *(PDWORD)info->Data;
    }
    return status;
}

void VioGpu3dAdapter::ReadSettings(void)
{
    PAGED_CODE();

    m_Flags.FlexResolution = 1;
    HANDLE hKey = NULL;
    NTSTATUS status = IoOpenDeviceRegistryKey(m_DeviceInfo.PhysicalDeviceObject,
                                              PLUGPLAY_REGKEY_DRIVER,
                                              KEY_READ,
                                              &hKey);
    if (!NT_SUCCESS(status))
    {
        return;
    }
    DWORD value = 1;
    if (NT_SUCCESS(ReadRegistryDWORD(hKey, L"FlexResolution", &value)))
    {
        m_Flags.FlexResolution = value != 0;
    }
    ZwClose(hKey);
}

NTSTATUS VioGpu3dAdapter::StartDevice(_In_ DXGK_START_INFO *pDxgkStartInfo,
                                      _In_ DXGKRNL_INTERFACE *pDxgkInterface,
                                      _Out_ ULONG *pNumberOfViews,
                                      _Out_ ULONG *pNumberOfChildren)
{
    PAGED_CODE();

    RtlCopyMemory(&m_DxgkInterface, pDxgkInterface, sizeof(m_DxgkInterface));
    m_AdapterLuid = pDxgkStartInfo->AdapterLuid;

    NTSTATUS Status = m_DxgkInterface.DxgkCbGetDeviceInformation(m_DxgkInterface.DeviceHandle, &m_DeviceInfo);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("DxgkCbGetDeviceInformation failed with status 0x%X\n", Status));
        return Status;
    }

    if (!CheckHardware())
    {
        return STATUS_GRAPHICS_DRIVER_MISMATCH;
    }

    ReadSettings();

    Status = HWInit(m_DeviceInfo.TranslatedResourceList);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("HWInit failed with status 0x%X\n", Status));
        return Status;
    }

    // Without virgl the device has no contexts and no 3D: Windows takes Basic Display instead of this driver.
    if (!virtio_is_feature_enabled(m_u64GuestFeatures, VIRTIO_GPU_F_VIRGL))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("%s the GPU offers no 3D (VIRTIO_GPU_F_VIRGL)\n", __FUNCTION__));
        return STATUS_UNSUCCESSFUL;
    }

    Status = vidpn->Start(pNumberOfViews, pNumberOfChildren);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("VioGpuVidPN::Start failed with status 0x%X\n", Status));
        return STATUS_UNSUCCESSFUL;
    }

    // Memory for the system display before DWM's primaries exist, for the largest mode the monitor lists, as
    // viogpudo backs its framebuffers.
    UINT fbSize = max((UINT)vidpn->LargestModeBytes(), 0x1000000u);
    fbSize = min(fbSize, (UINT)(VIOGPU3D_MAX_DISPLAY_PIXELS * 4));
    PHYSICAL_ADDRESS noPa = {};
    if (!frameSegment.Init(fbSize, &noPa))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("%s failed to allocate %u bytes for the framebuffer\n", __FUNCTION__, fbSize));
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    m_Flags.DriverStarted = TRUE;
    DbgPrint(TRACE_LEVEL_FATAL,
             ("%s started: %u scanouts, %u capsets, Venus %s, blob alignment %u\n",
              __FUNCTION__,
              m_u32NumScanouts,
              m_u32NumCapsets,
              m_HasVenus ? "on" : "off",
              m_BlobAlignment));
    return STATUS_SUCCESS;
}

NTSTATUS VioGpu3dAdapter::StopDevice(VOID)
{
    PAGED_CODE();

    m_Flags.DriverStarted = FALSE;
    vidpn->StopFlipThread();
    return STATUS_SUCCESS;
}

NTSTATUS VioGpu3dAdapter::SetPowerState(_In_ ULONG HardwareUid,
                                        _In_ DEVICE_POWER_STATE DevicePowerState,
                                        _In_ POWER_ACTION ActionType)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(ActionType);

    if (HardwareUid != DISPLAY_ADAPTER_HW_ID)
    {
        return STATUS_SUCCESS;
    }

    if (DevicePowerState == PowerDeviceD0)
    {
        vidpn->AcquirePostDisplayOwnership();
        if (m_AdapterPowerState == PowerDeviceD3)
        {
            DXGKARG_SETVIDPNSOURCEVISIBILITY Visibility;
            Visibility.VidPnSourceId = D3DDDI_ID_ALL;
            Visibility.Visible = FALSE;
            vidpn->SetVidPnSourceVisibility(&Visibility);
        }
    }
    m_AdapterPowerState = DevicePowerState;

    switch (DevicePowerState)
    {
        case PowerDeviceUnspecified:
        case PowerDeviceD0:
            VioGpuAdapterInit();
            break;
        default:
            vidpn->Powerdown();
            VioGpuAdapterClose();
            break;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
VioGpu3dAdapter::QueryChildRelations(_Out_writes_bytes_(ChildRelationsSize) DXGK_CHILD_DESCRIPTOR *pChildRelations,
                                     _In_ ULONG ChildRelationsSize)
{
    PAGED_CODE();

    ULONG ChildRelationsCount = (ChildRelationsSize / sizeof(DXGK_CHILD_DESCRIPTOR)) - 1;
    for (UINT ChildIndex = 0; ChildIndex < ChildRelationsCount && ChildIndex < MAX_CHILDREN; ++ChildIndex)
    {
        pChildRelations[ChildIndex].ChildDeviceType = TypeVideoOutput;
        pChildRelations[ChildIndex].ChildCapabilities.HpdAwareness = IsVgaDevice() ? HpdAwarenessAlwaysConnected
                                                                                   : HpdAwarenessInterruptible;
        pChildRelations[ChildIndex].ChildCapabilities.Type.VideoOutput.InterfaceTechnology = IsVgaDevice() ? D3DKMDT_VOT_INTERNAL
                                                                                                           : D3DKMDT_VOT_HD15;
        pChildRelations[ChildIndex].ChildCapabilities.Type.VideoOutput.MonitorOrientationAwareness = D3DKMDT_MOA_NONE;
        pChildRelations[ChildIndex].ChildCapabilities.Type.VideoOutput.SupportsSdtvModes = FALSE;
        pChildRelations[ChildIndex].AcpiUid = 0;
        pChildRelations[ChildIndex].ChildUid = ChildIndex;
    }
    return STATUS_SUCCESS;
}

NTSTATUS VioGpu3dAdapter::QueryChildStatus(_Inout_ DXGK_CHILD_STATUS *pChildStatus, _In_ BOOLEAN NonDestructiveOnly)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(NonDestructiveOnly);

    switch (pChildStatus->Type)
    {
        case StatusConnection:
            // Scanout 0 is the one monitor this driver drives so far.
            pChildStatus->HotPlug.Connected = IsDriverActive() && pChildStatus->ChildUid == 0;
            return STATUS_SUCCESS;
        case StatusRotation:
            return STATUS_INVALID_PARAMETER;
        default:
            return STATUS_NOT_SUPPORTED;
    }
}

NTSTATUS VioGpu3dAdapter::QueryDeviceDescriptor(_In_ ULONG ChildUid, _Inout_ DXGK_DEVICE_DESCRIPTOR *pDeviceDescriptor)
{
    PAGED_CODE();

    if (ChildUid >= MAX_CHILDREN)
    {
        return STATUS_GRAPHICS_CHILD_DESCRIPTOR_NOT_SUPPORTED;
    }
    PBYTE edid = vidpn->GetEdidData(ChildUid);
    if (!edid)
    {
        return STATUS_GRAPHICS_CHILD_DESCRIPTOR_NOT_SUPPORTED;
    }
    if (pDeviceDescriptor->DescriptorOffset < EDID_RAW_BLOCK_SIZE)
    {
        ULONG len = min(pDeviceDescriptor->DescriptorLength,
                        (EDID_RAW_BLOCK_SIZE - pDeviceDescriptor->DescriptorOffset));
        RtlCopyMemory(pDeviceDescriptor->DescriptorBuffer, (edid + pDeviceDescriptor->DescriptorOffset), len);
        pDeviceDescriptor->DescriptorLength = len;
        return STATUS_SUCCESS;
    }
    return STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA;
}

NTSTATUS VioGpu3dAdapter::QuerySegment4(_In_ CONST DXGKARG_QUERYADAPTERINFO *pQuery)
{
    PAGED_CODE();

    if (pQuery->OutputDataSize < sizeof(DXGK_QUERYSEGMENTOUT4))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }
    DXGK_QUERYSEGMENTOUT4 *pOut = (DXGK_QUERYSEGMENTOUT4 *)pQuery->pOutputData;
    const UINT count = 2;
    // The counting pass gives only NbSegment.
    if (pOut->NbSegment == 0)
    {
        pOut->NbSegment = count;
        return STATUS_SUCCESS;
    }
    if (pOut->NbSegment != count || pOut->pSegmentDescriptor == NULL ||
        pOut->SegmentDescriptorStride < sizeof(DXGK_SEGMENTDESCRIPTOR4))
    {
        return STATUS_INVALID_PARAMETER;
    }

    SIZE_T stride = pOut->SegmentDescriptorStride;
    RtlZeroMemory(pOut->pSegmentDescriptor, stride * count);
    // Paging buffers in the aperture segment: VidMm maps them there.
    pOut->PagingBufferSegmentId = VIOGPU3D_SEGMENT_APERTURE;
    pOut->PagingBufferSize = VIOGPU3D_PAGING_BUFFER_SIZE;
    pOut->PagingBufferPrivateDataSize = 0;

    DXGK_SEGMENTDESCRIPTOR4 *pAperture = (DXGK_SEGMENTDESCRIPTOR4 *)(pOut->pSegmentDescriptor);
    pAperture->Flags.Aperture = TRUE;
    pAperture->Flags.CacheCoherent = TRUE;
    pAperture->Flags.DirectFlip = TRUE;
    // Apart from the page tables' range, as viogpu3d of pull request 943 placed it.
    pAperture->BaseAddress.QuadPart = 0xC0000000;
    pAperture->Size = (SIZE_T)VIOGPU3D_APERTURE_SIZE;
    pAperture->CommitLimit = (SIZE_T)VIOGPU3D_APERTURE_SIZE;

    DXGK_SEGMENTDESCRIPTOR4 *pTables = (DXGK_SEGMENTDESCRIPTOR4 *)(pOut->pSegmentDescriptor + stride);
    pTables->Flags.Aperture = FALSE;
    pTables->Flags.PopulatedFromSystemMemory = TRUE;
    pTables->Flags.CpuVisible = TRUE;
    pTables->Flags.CacheCoherent = TRUE;
    pTables->BaseAddress.QuadPart = 0;
    pTables->CpuTranslatedAddress = m_PageTablesPA;
    pTables->Size = VIOGPU3D_PT_SEGMENT_SIZE;
    pTables->SystemMemoryEndAddress = VIOGPU3D_PT_SEGMENT_SIZE;
    return STATUS_SUCCESS;
}

NTSTATUS VioGpu3dAdapter::QueryAdapterInfo(_In_ CONST DXGKARG_QUERYADAPTERINFO *pQueryAdapterInfo)
{
    PAGED_CODE();

    switch (pQueryAdapterInfo->Type)
    {
        case DXGKQAITYPE_UMDRIVERPRIVATE:
            {
                if (pQueryAdapterInfo->OutputDataSize < sizeof(VIRTIO_WDDM_ADAPTER_INFO))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }
                VIRTIO_WDDM_ADAPTER_INFO *info = (VIRTIO_WDDM_ADAPTER_INFO *)pQueryAdapterInfo->pOutputData;
                RtlZeroMemory(info, sizeof(*info));
                info->Tag = VIRTIO_WDDM_ADAPTER_INFO_TAG;
                info->Luid = ((UINT64)(UINT32)m_AdapterLuid.HighPart << 32) | m_AdapterLuid.LowPart;
                info->CapsetMask = m_CapsetMask;
                info->Supports3d = virtio_is_feature_enabled(m_u64GuestFeatures, VIRTIO_GPU_F_VIRGL) ? 1 : 0;
                info->HasShmem = m_HasVenus ? 1 : 0;
                return STATUS_SUCCESS;
            }
        case DXGKQAITYPE_DRIVERCAPS:
            {
                if (pQueryAdapterInfo->OutputDataSize < sizeof(DXGK_DRIVERCAPS))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }
                DXGK_DRIVERCAPS *pCaps = (DXGK_DRIVERCAPS *)pQueryAdapterInfo->pOutputData;
                RtlZeroMemory(pCaps, sizeof(DXGK_DRIVERCAPS));
                pCaps->WDDMVersion = DXGKDDI_WDDMv2;
                pCaps->HighestAcceptableAddress.QuadPart = (ULONG64)-1;
                pCaps->MaxAllocationListSlotId = 0x1000;
                pCaps->ApertureSegmentCommitLimit = (SIZE_T)VIOGPU3D_APERTURE_SIZE;
                pCaps->MemoryManagementCaps.VirtualAddressingSupported = 1;
                pCaps->MemoryManagementCaps.GpuMmuSupported = 1;
                pCaps->MemoryManagementCaps.SectionBackedPrimary = 1;
                pCaps->SchedulingCaps.MultiEngineAware = 1;
                pCaps->SchedulingCaps.PreemptionAware = 1;
                pCaps->PreemptionCaps.GraphicsPreemptionGranularity = D3DKMDT_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY;
                pCaps->PreemptionCaps.ComputePreemptionGranularity = D3DKMDT_COMPUTE_PREEMPTION_DMA_BUFFER_BOUNDARY;
                pCaps->GpuEngineTopology.NbAsymetricProcessingNodes = VIOGPU3D_NODE_COUNT;
                pCaps->FlipCaps.FlipOnVSyncMmIo = 1;
                pCaps->FlipCaps.FlipIndependent = 1;
                pCaps->MaxQueuedFlipOnVSync = 1;
                // The blt surface pitch alignment, 1 << AlignmentShift bytes; the DDI wants at least 2.
                pCaps->PresentationCaps.AlignmentShift = 2;
                pCaps->SupportNonVGA = IsVgaDevice();
                pCaps->SupportSmoothRotation = FALSE;
                return STATUS_SUCCESS;
            }
        case DXGKQAITYPE_QUERYSEGMENT4:
            return QuerySegment4(pQueryAdapterInfo);
        case DXGKQAITYPE_GPUMMUCAPS:
            {
                if (pQueryAdapterInfo->OutputDataSize < sizeof(DXGK_GPUMMUCAPS))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }
                DXGK_GPUMMUCAPS *pMmu = (DXGK_GPUMMUCAPS *)pQueryAdapterInfo->pOutputData;
                RtlZeroMemory(pMmu, sizeof(*pMmu));
                pMmu->CacheCoherentMemorySupported = 1;
                pMmu->PageTableUpdateMode = DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;
                // VidMm sizes its tracking from the address space: 32 bits keep that at a few MiB.
                pMmu->VirtualAddressBitCount = VIOGPU3D_VA_BITS;
                pMmu->PageTableLevelCount = VIOGPU3D_PT_LEVELS;
                return STATUS_SUCCESS;
            }
        case DXGKQAITYPE_PAGETABLELEVELDESC:
            {
                if (pQueryAdapterInfo->OutputDataSize < sizeof(DXGK_PAGE_TABLE_LEVEL_DESC))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }
                DXGK_PAGE_TABLE_LEVEL_DESC *pLevel = (DXGK_PAGE_TABLE_LEVEL_DESC *)pQueryAdapterInfo->pOutputData;
                RtlZeroMemory(pLevel, sizeof(*pLevel));
                pLevel->PageTableIndexBitCount = VIOGPU3D_PT_INDEX_BITS;
                pLevel->PageTableSegmentId = VIOGPU3D_SEGMENT_PAGE_TABLES;
                pLevel->PagingProcessPageTableSegmentId = VIOGPU3D_SEGMENT_PAGE_TABLES;
                pLevel->PageTableSizeInBytes = VIOGPU3D_PT_ENTRIES * sizeof(DXGK_PTE);
                pLevel->PageTableAlignmentInBytes = 0;
                return STATUS_SUCCESS;
            }
        case DXGKQAITYPE_PHYSICALADAPTERCAPS:
            {
                if (pQueryAdapterInfo->OutputDataSize < sizeof(DXGK_PHYSICALADAPTERCAPS))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }
                DXGK_PHYSICALADAPTERCAPS *pCaps = (DXGK_PHYSICALADAPTERCAPS *)pQueryAdapterInfo->pOutputData;
                RtlZeroMemory(pCaps, sizeof(*pCaps));
                pCaps->NumExecutionNodes = VIOGPU3D_NODE_COUNT;
                pCaps->PagingNodeIndex = VIOGPU3D_NODE_PAGING;
                // dxgkrnl's own handle for this physical adapter, the one it gave at start.
                pCaps->DxgkPhysicalAdapterHandle = m_DxgkInterface.DeviceHandle;
                pCaps->Flags.GpuMmuSupported = 1;
                return STATUS_SUCCESS;
            }
        case DXGKQAITYPE_HISTORYBUFFERPRECISION:
            {
                if (pQueryAdapterInfo->OutputDataSize < sizeof(DXGKARG_HISTORYBUFFERPRECISION))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }
                DXGKARG_HISTORYBUFFERPRECISION *pPrecision = (DXGKARG_HISTORYBUFFERPRECISION *)pQueryAdapterInfo->pOutputData;
                UINT entries = pQueryAdapterInfo->OutputDataSize / sizeof(DXGKARG_HISTORYBUFFERPRECISION);
                for (UINT i = 0; i < entries; i++)
                {
                    pPrecision[i].PrecisionBits = 64;
                }
                return STATUS_SUCCESS;
            }
        case DXGKQAITYPE_DISPLAY_DRIVERCAPS_EXTENSION:
            {
                if (pQueryAdapterInfo->OutputDataSize < sizeof(DXGK_DISPLAY_DRIVERCAPS_EXTENSION))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }
                RtlZeroMemory(pQueryAdapterInfo->pOutputData, sizeof(DXGK_DISPLAY_DRIVERCAPS_EXTENSION));
                return STATUS_SUCCESS;
            }
        default:
            DbgPrint(TRACE_LEVEL_WARNING,
                     ("<--- %s unanswered query type %d\n", __FUNCTION__, pQueryAdapterInfo->Type));
            return STATUS_NOT_SUPPORTED;
    }
}

const VIOGPU3D_CAPSET *VioGpu3dAdapter::FindCapset(UINT id) const
{
    PAGED_CODE();

    if (id == 0 || id > VIRTIO_GPU_MAX_CAPSET_ID || !(m_CapsetMask & VIRTIO_WDDM_CAPSET_MASK(id)))
    {
        return NULL;
    }
    return &m_Capsets[id];
}

NTSTATUS VioGpu3dAdapter::EscapeCapset(_Inout_ VIRTIO_WDDM_CAPSET *pCapset, UINT size)
{
    PAGED_CODE();

    const VIOGPU3D_CAPSET *pInfo = FindCapset(pCapset->CapsetId);
    if (pInfo == NULL || pCapset->Version > pInfo->MaxVersion)
    {
        return STATUS_INVALID_PARAMETER;
    }
    UINT room = size - sizeof(VIRTIO_WDDM_CAPSET);
    UINT bytes = min(room, pInfo->MaxSize);
    if (bytes == 0)
    {
        return STATUS_SUCCESS;
    }
    PUCHAR data = new (NonPagedPoolNx) UCHAR[bytes];
    if (data == NULL)
    {
        return STATUS_NO_MEMORY;
    }
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    if (ctrlQueue.AskCapset(pCapset->CapsetId, pCapset->Version, bytes, data))
    {
        // The escape's buffer is the runtime's kernel copy of the caller's.
        RtlCopyMemory(pCapset + 1, data, bytes);
        status = STATUS_SUCCESS;
    }
    delete[] data;
    return status;
}

NTSTATUS VioGpu3dAdapter::EscapeResourceInfo(_Inout_ VIRTIO_WDDM_RESOURCE_INFO *pInfo)
{
    PAGED_CODE();

    VioGpu3dAllocation *pAllocation = AllocationFromHandle(pInfo->Handle);
    if (pAllocation == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    pInfo->Info = pAllocation->GetInfo();
    return STATUS_SUCCESS;
}

NTSTATUS VioGpu3dAdapter::EscapeBlobMap(_Inout_ VIRTIO_WDDM_BLOB_MAP *pMap)
{
    PAGED_CODE();

    VioGpu3dAllocation *pAllocation = AllocationFromHandle(pMap->Handle);
    if (pAllocation == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (pMap->Flags & VIRTIO_WDDM_BLOB_MAP_FLAGS_UNMAP)
    {
        return pAllocation->UnmapFromProcess((PVOID)(ULONG_PTR)pMap->Ptr);
    }
    PVOID address = NULL;
    NTSTATUS status = pAllocation->MapToProcess(&address);
    pMap->Ptr = (UINT64)(ULONG_PTR)address;
    return status;
}

NTSTATUS VioGpu3dAdapter::Escape(_In_ CONST DXGKARG_ESCAPE *pEscape)
{
    PAGED_CODE();

    UINT size = pEscape->PrivateDriverDataSize;
    if (pEscape->pPrivateDriverData == NULL || size < sizeof(UINT64))
    {
        return STATUS_INVALID_BUFFER_SIZE;
    }
    UINT64 tag = *(UINT64 *)pEscape->pPrivateDriverData;
    VioGpu3dDevice *pDevice = reinterpret_cast<VioGpu3dDevice *>(pEscape->hDevice);
    VioGpu3dContext *pContext = reinterpret_cast<VioGpu3dContext *>(pEscape->hContext);

    switch (tag)
    {
        case VIRTIO_WDDM_ESCAPE_CAPSET_TAG:
            if (size < sizeof(VIRTIO_WDDM_CAPSET))
            {
                return STATUS_INVALID_BUFFER_SIZE;
            }
            return EscapeCapset((VIRTIO_WDDM_CAPSET *)pEscape->pPrivateDriverData, size);
        case VIRTIO_WDDM_ESCAPE_CONTEXT_INIT_TAG:
            if (size < sizeof(VIRTIO_WDDM_CONTEXT_INIT) || pDevice == NULL)
            {
                return STATUS_INVALID_PARAMETER;
            }
            return pDevice->InitContext((VIRTIO_WDDM_CONTEXT_INIT *)pEscape->pPrivateDriverData);
        case VIRTIO_WDDM_ESCAPE_RESOURCE_INFO_TAG:
            if (size < sizeof(VIRTIO_WDDM_RESOURCE_INFO))
            {
                return STATUS_INVALID_BUFFER_SIZE;
            }
            return EscapeResourceInfo((VIRTIO_WDDM_RESOURCE_INFO *)pEscape->pPrivateDriverData);
        case VIRTIO_WDDM_ESCAPE_BLOB_MAP_TAG:
            if (size < sizeof(VIRTIO_WDDM_BLOB_MAP))
            {
                return STATUS_INVALID_BUFFER_SIZE;
            }
            return EscapeBlobMap((VIRTIO_WDDM_BLOB_MAP *)pEscape->pPrivateDriverData);
        case VIRTIO_WDDM_ESCAPE_EXEC_BUF_TAG:
            if (size < sizeof(VIRTIO_WDDM_EXEC_BUFFER) || pContext == NULL || pContext->GetDevice() != pDevice)
            {
                return STATUS_INVALID_PARAMETER;
            }
            return pContext->ExecBuffer((VIRTIO_WDDM_EXEC_BUFFER *)pEscape->pPrivateDriverData, size);
        case VIRTIO_WDDM_ESCAPE_RESOURCE_BUSY_TAG:
        case VIRTIO_WDDM_ESCAPE_BLIT_INIT_TAG:
        default:
            DbgPrint(TRACE_LEVEL_ERROR, ("%s: escape %I64x is not supported\n", __FUNCTION__, tag));
            return STATUS_NOT_SUPPORTED;
    }
}

NTSTATUS VioGpu3dAdapter::StopDeviceAndReleasePostDisplayOwnership(_In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                                                   _Out_ DXGK_DISPLAY_INFORMATION *pDisplayInfo)
{
    PAGED_CODE();

    vidpn->ReleasePostDisplayOwnership(TargetId, pDisplayInfo);
    return StopDevice();
}

NTSTATUS VioGpu3dAdapter::VioGpuAdapterInit()
{
    PAGED_CODE();

    if (IsHardwareInit())
    {
        return STATUS_SUCCESS;
    }
    NTSTATUS status = VirtIoDeviceInit();
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("Failed to initialize virtio device, error %x\n", status));
        return status;
    }

    m_u64HostFeatures = virtio_get_features(&m_VioDev);
    m_u64GuestFeatures = 0;
    do
    {
        struct virtqueue *vqs[2];
        if (!AckFeature(VIRTIO_F_VERSION_1))
        {
            status = STATUS_UNSUCCESSFUL;
            break;
        }
        AckFeature(VIRTIO_F_ACCESS_PLATFORM);
        AckFeature(VIRTIO_GPU_F_VIRGL);
        AckFeature(VIRTIO_GPU_F_EDID);
        AckFeature(VIRTIO_GPU_F_RESOURCE_BLOB);
        AckFeature(VIRTIO_GPU_F_CONTEXT_INIT);
        AckFeature(VIRTIO_GPU_F_BLOB_ALIGNMENT);

        status = virtio_set_features(&m_VioDev, m_u64GuestFeatures);
        if (!NT_SUCCESS(status))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("%s virtio_set_features failed with %x\n", __FUNCTION__, status));
            break;
        }

        status = virtio_find_queues(&m_VioDev, 2, vqs);
        if (!NT_SUCCESS(status))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("virtio_find_queues failed with error %x\n", status));
            break;
        }

        if (!ctrlQueue.Init(&m_VioDev, vqs[0], 0) || !m_CursorQueue.Init(&m_VioDev, vqs[1], 1))
        {
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        virtio_get_config(&m_VioDev,
                          FIELD_OFFSET(GPU_CONFIG, num_scanouts),
                          &m_u32NumScanouts,
                          sizeof(m_u32NumScanouts));
        virtio_get_config(&m_VioDev, FIELD_OFFSET(GPU_CONFIG, num_capsets), &m_u32NumCapsets, sizeof(m_u32NumCapsets));
        m_BlobAlignment = PAGE_SIZE;
        if (virtio_is_feature_enabled(m_u64GuestFeatures, VIRTIO_GPU_F_BLOB_ALIGNMENT))
        {
            UINT32 alignment = 0;
            virtio_get_config(&m_VioDev, FIELD_OFFSET(GPU_CONFIG, blob_alignment), &alignment, sizeof(alignment));
            // A power of two from a page up; the device's own page is 16 KiB on a Mac.
            if (alignment > PAGE_SIZE && (alignment & (alignment - 1)) == 0)
            {
                m_BlobAlignment = alignment;
            }
        }
    } while (0);

    if (status == STATUS_SUCCESS)
    {
        virtio_device_ready(&m_VioDev);
        SetHardwareInit(TRUE);
    }
    else
    {
        virtio_add_status(&m_VioDev, VIRTIO_CONFIG_S_FAILED);
    }
    return status;
}

void VioGpu3dAdapter::VioGpuAdapterClose()
{
    PAGED_CODE();

    if (IsHardwareInit())
    {
        SetHardwareInit(FALSE);
        ctrlQueue.DisableInterrupt();
        m_CursorQueue.DisableInterrupt();
        virtio_device_reset(&m_VioDev);
        virtio_delete_queues(&m_VioDev);
        ctrlQueue.Close();
        m_CursorQueue.Close();
        virtio_device_shutdown(&m_VioDev);
    }
}

BOOLEAN VioGpu3dAdapter::AckFeature(UINT64 Feature)
{
    PAGED_CODE();

    if (virtio_is_feature_enabled(m_u64HostFeatures, Feature))
    {
        virtio_feature_enable(m_u64GuestFeatures, Feature);
        return TRUE;
    }
    return FALSE;
}

NTSTATUS VioGpu3dAdapter::VirtIoDeviceInit()
{
    PAGED_CODE();

    return virtio_device_initialize(&m_VioDev,
                                    &VioGpuSystemOps,
                                    reinterpret_cast<IVioGpuPCI *>(this),
                                    m_PciResources.IsMSIEnabled());
}

void VioGpu3dAdapter::QueryCapsets(void)
{
    PAGED_CODE();

    m_CapsetMask = 0;
    for (UINT32 i = 0; i < m_u32NumCapsets && i <= VIRTIO_GPU_MAX_CAPSET_ID; i++)
    {
        GPU_RESP_CAPSET_INFO info;
        if (!ctrlQueue.AskCapsetInfo(i, &info))
        {
            continue;
        }
        // Capability set ids run from 1 to 63; a set larger than 64 KiB is not one of the known ones.
        if (info.capset_id == 0 || info.capset_id > VIRTIO_GPU_MAX_CAPSET_ID || info.capset_max_size > 0x10000)
        {
            continue;
        }
        m_Capsets[info.capset_id].Id = info.capset_id;
        m_Capsets[info.capset_id].MaxVersion = info.capset_max_version;
        m_Capsets[info.capset_id].MaxSize = info.capset_max_size;
        m_CapsetMask |= VIRTIO_WDDM_CAPSET_MASK(info.capset_id);
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("capset %u: id %u, version %u, size %u\n",
                  i,
                  info.capset_id,
                  info.capset_max_version,
                  info.capset_max_size));
    }
}

NTSTATUS VioGpu3dAdapter::HWInit(PCM_RESOURCE_LIST pResList)
{
    PAGED_CODE();

    if (!m_PciResources.Init(GetDxgkInterface(), pResList))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("Incomplete resources\n"));
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    NTSTATUS status = VioGpuAdapterInit();
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    UINT size = ctrlQueue.QueryAllocation() + m_CursorQueue.QueryAllocation();
    if (!m_GpuBuf.Init(size))
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    ctrlQueue.SetGpuBuf(&m_GpuBuf);
    m_CursorQueue.SetGpuBuf(&m_GpuBuf);

    if (!resourceIdr.Init(1) || !ctxIdr.Init(1))
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    HANDLE threadHandle = 0;
    status = PsCreateSystemThread(&threadHandle,
                                  (ACCESS_MASK)0,
                                  NULL,
                                  (HANDLE)0,
                                  NULL,
                                  VioGpu3dAdapter::ThreadWork,
                                  this);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    ObReferenceObjectByHandle(threadHandle, THREAD_ALL_ACCESS, NULL, KernelMode, (PVOID *)(&m_pWorkThread), NULL);
    ZwClose(threadHandle);

    QueryCapsets();

    // The page tables live here: VidMm writes them through the CPU, and a virtio GPU never walks them.
    PHYSICAL_ADDRESS low = {}, high, boundary = {};
    high.QuadPart = (LONGLONG)-1;
    m_pPageTables = MmAllocateContiguousMemorySpecifyCache(VIOGPU3D_PT_SEGMENT_SIZE, low, high, boundary, MmCached);
    if (m_pPageTables == NULL)
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("%s no contiguous memory for the page tables\n", __FUNCTION__));
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(m_pPageTables, VIOGPU3D_PT_SEGMENT_SIZE);
    m_PageTablesPA = MmGetPhysicalAddress(m_pPageTables);

    // Venus needs blobs, contexts of a capability set, and the host-visible region where blobs are mapped.
    UINT shmBar = 0;
    ULONGLONG shmOffset = 0, shmLength = 0;
    if (virtio_is_feature_enabled(m_u64GuestFeatures, VIRTIO_GPU_F_RESOURCE_BLOB) &&
        virtio_is_feature_enabled(m_u64GuestFeatures, VIRTIO_GPU_F_CONTEXT_INIT) &&
        m_PciResources.FindSharedMemory(VIRTIO_GPU_SHM_ID_HOST_VISIBLE, &shmBar, &shmOffset, &shmLength))
    {
        PHYSICAL_ADDRESS base = m_PciResources.GetPciBar(shmBar)->GetPA();
        base.QuadPart += shmOffset;
        if (NT_SUCCESS(m_HostVisible.Init(base, shmLength, m_BlobAlignment)))
        {
            m_HasVenus = FindCapset(VIRTIO_WDDM_CAPSET_ID_VENUS) != NULL;
        }
    }
    if (!m_HasVenus)
    {
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("%s no Venus: blobs, context init, the host-visible region or the capset is missing\n",
                  __FUNCTION__));
    }
    return STATUS_SUCCESS;
}

void VioGpu3dAdapter::StopWorkThread(void)
{
    PAGED_CODE();

    if (m_pWorkThread != NULL)
    {
        LARGE_INTEGER timeout;
        timeout.QuadPart = Int32x32To64(1000, -10000);
        m_bStopWorkThread = TRUE;
        KeSetEvent(&m_ConfigUpdateEvent, IO_NO_INCREMENT, FALSE);
        if (KeWaitForSingleObject(m_pWorkThread, Executive, KernelMode, FALSE, &timeout) == STATUS_TIMEOUT)
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("---> Failed to exit the worker thread\n"));
        }
        ObDereferenceObject(m_pWorkThread);
        m_pWorkThread = NULL;
    }
}

NTSTATUS VioGpu3dAdapter::HWClose(void)
{
    PAGED_CODE();

    SetHardwareInit(FALSE);
    StopWorkThread();
    frameSegment.Close();
    m_HostVisible.Close();
    if (m_pPageTables != NULL)
    {
        MmFreeContiguousMemory(m_pPageTables);
        m_pPageTables = NULL;
    }
    return STATUS_SUCCESS;
}

NTSTATUS VioGpu3dAdapter::UpdateChildStatus(BOOLEAN connect)
{
    PAGED_CODE();

    DXGK_CHILD_STATUS ChildStatus;
    RtlZeroMemory(&ChildStatus, sizeof(ChildStatus));
    ChildStatus.Type = StatusConnection;
    ChildStatus.ChildUid = 0;
    ChildStatus.HotPlug.Connected = connect;
    return m_DxgkInterface.DxgkCbIndicateChildStatus(m_DxgkInterface.DeviceHandle, &ChildStatus);
}

void VioGpu3dAdapter::ThreadWork(_In_ PVOID Context)
{
    PAGED_CODE();

    VioGpu3dAdapter *pdev = reinterpret_cast<VioGpu3dAdapter *>(Context);
    pdev->ThreadWorkRoutine();
}

void VioGpu3dAdapter::ThreadWorkRoutine(void)
{
    PAGED_CODE();

    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);
    for (;;)
    {
        KeWaitForSingleObject(&m_ConfigUpdateEvent, Executive, KernelMode, FALSE, NULL);
        if (m_bStopWorkThread)
        {
            PsTerminateSystemThread(STATUS_SUCCESS);
            break;
        }
        ConfigChanged();
    }
}

void VioGpu3dAdapter::ConfigChanged(void)
{
    PAGED_CODE();

    UINT32 events_read = 0, events_clear = 0;
    virtio_get_config(&m_VioDev, FIELD_OFFSET(GPU_CONFIG, events_read), &events_read, sizeof(events_read));
    if (events_read & VIRTIO_GPU_EVENT_DISPLAY)
    {
        vidpn->GetDisplayInfo();
        events_clear |= VIRTIO_GPU_EVENT_DISPLAY;
        virtio_set_config(&m_VioDev, FIELD_OFFSET(GPU_CONFIG, events_clear), &events_clear, sizeof(events_clear));
        UpdateChildStatus(TRUE);
    }
}

NTSTATUS VioGpu3dAdapter::QueryCurrentFence(_Inout_ DXGKARG_QUERYCURRENTFENCE *pCurrentFence)
{
    PAGED_CODE();

    if (pCurrentFence->NodeOrdinal >= VIOGPU3D_NODE_COUNT)
    {
        return STATUS_INVALID_PARAMETER;
    }
    KIRQL irql;
    KeAcquireSpinLock(&m_NodeLock, &irql);
    pCurrentFence->CurrentFence = m_Nodes[pCurrentFence->NodeOrdinal].LastCompleted;
    KeReleaseSpinLock(&m_NodeLock, irql);
    return STATUS_SUCCESS;
}

NTSTATUS VioGpu3dAdapter::QueryEngineStatus(_Inout_ DXGKARG_QUERYENGINESTATUS *pQueryEngineStatus)
{
    PAGED_CODE();

    pQueryEngineStatus->EngineStatus.Responsive = 1;
    return STATUS_SUCCESS;
}

NTSTATUS VioGpu3dAdapter::ResetEngine(_Inout_ DXGKARG_RESETENGINE *pResetEngine)
{
    PAGED_CODE();

    if (pResetEngine->NodeOrdinal >= VIOGPU3D_NODE_COUNT)
    {
        return STATUS_INVALID_PARAMETER;
    }
    // The host runs what it was given; this engine's waits are given up, and their fences reported done.
    KIRQL irql;
    KeAcquireSpinLock(&m_NodeLock, &irql);
    VIOGPU3D_NODE_STATE *pNode = &m_Nodes[pResetEngine->NodeOrdinal];
    while (!IsListEmpty(&pNode->Pending))
    {
        VIOGPU3D_DMA *pDma = CONTAINING_RECORD(RemoveHeadList(&pNode->Pending), VIOGPU3D_DMA, Link);
        pNode->LastCompleted = pDma->FenceId;
        if (pDma->Outstanding == 0)
        {
            delete pDma;
        }
        else
        {
            // Its last release frees it.
            pDma->Done = TRUE;
        }
    }
    pResetEngine->LastAbortedFenceId = pNode->LastCompleted;
    KeReleaseSpinLock(&m_NodeLock, irql);
    return STATUS_SUCCESS;
}

NTSTATUS VioGpu3dAdapter::ResetFromTimeout(VOID)
{
    PAGED_CODE();

    KIRQL irql;
    KeAcquireSpinLock(&m_NodeLock, &irql);
    for (UINT node = 0; node < VIOGPU3D_NODE_COUNT; node++)
    {
        VIOGPU3D_NODE_STATE *pNode = &m_Nodes[node];
        while (!IsListEmpty(&pNode->Pending))
        {
            VIOGPU3D_DMA *pDma = CONTAINING_RECORD(RemoveHeadList(&pNode->Pending), VIOGPU3D_DMA, Link);
            if (pDma->Outstanding == 0)
            {
                delete pDma;
            }
            else
            {
                // A fence the host still owes keeps the entry; its signal frees it.
                pDma->Done = TRUE;
            }
        }
        pNode->LastSubmitted = 0;
        pNode->LastCompleted = 0;
        pNode->PreemptRequested = FALSE;
    }
    KeReleaseSpinLock(&m_NodeLock, irql);
    return STATUS_SUCCESS;
}

NTSTATUS VioGpu3dAdapter::RestartFromTimeout(VOID)
{
    PAGED_CODE();

    return STATUS_SUCCESS;
}

PAGED_CODE_SEG_END

//
// Non-paged code: interrupts, DPCs and what they call.
//

VOID VioGpu3dAdapter::ResetDevice(VOID)
{
}

BOOLEAN VioGpu3dAdapter::InterruptRoutine(_In_ ULONG MessageNumber)
{
    if (!IsHardwareInit())
    {
        return FALSE;
    }

    BOOLEAN serviced = TRUE;
    ULONG intReason = 0;
    if (m_PciResources.IsMSIEnabled())
    {
        switch (MessageNumber)
        {
            case 0:
                intReason = ISR_REASON_CHANGE;
                break;
            case 1:
                intReason = ISR_REASON_DISPLAY;
                break;
            case 2:
                intReason = ISR_REASON_CURSOR;
                break;
            default:
                serviced = FALSE;
        }
    }
    else
    {
        UCHAR isrstat = virtio_read_isr_status(&m_VioDev);
        if (isrstat & 1)
        {
            intReason |= ISR_REASON_DISPLAY | ISR_REASON_CURSOR;
        }
        if (isrstat & 2)
        {
            intReason |= ISR_REASON_CHANGE;
        }
        serviced = intReason != 0;
    }

    if (serviced)
    {
        InterlockedOr((PLONG)&m_PendingWorks, intReason);
        m_DxgkInterface.DxgkCbQueueDpc(m_DxgkInterface.DeviceHandle);
    }
    return serviced;
}

VOID VioGpu3dAdapter::DpcRoutine(VOID)
{
    PGPU_VBUFFER pvbuf = NULL;
    UINT len = 0;
    ULONG reason;
    while ((reason = InterlockedExchange((PLONG)&m_PendingWorks, 0)) != 0)
    {
        if (reason & ISR_REASON_DISPLAY)
        {
            while ((pvbuf = ctrlQueue.DequeueBuffer(&len)) != NULL)
            {
                PGPU_CTRL_HDR pcmd = (PGPU_CTRL_HDR)pvbuf->buf;
                PGPU_CTRL_HDR resp = (PGPU_CTRL_HDR)pvbuf->resp_buf;
                if (resp->type >= VIRTIO_GPU_RESP_ERR_UNSPEC)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s command %x of context %u failed with %x\n",
                              __FUNCTION__,
                              pcmd->type,
                              pcmd->ctx_id,
                              resp->type));
                }
                if (pvbuf->complete_cb != NULL)
                {
                    pvbuf->complete_cb(pvbuf->complete_ctx);
                }
                if (pvbuf->auto_release)
                {
                    ctrlQueue.ReleaseBuffer(pvbuf);
                }
            }
        }
        if (reason & ISR_REASON_CURSOR)
        {
            while ((pvbuf = m_CursorQueue.DequeueCursor(&len)) != NULL)
            {
                m_CursorQueue.ReleaseBuffer(pvbuf);
            }
        }
        if (reason & ISR_REASON_CHANGE)
        {
            KeSetEvent(&m_ConfigUpdateEvent, IO_NO_INCREMENT, FALSE);
        }
    }
    m_DxgkInterface.DxgkCbNotifyDpc((HANDLE)m_DxgkInterface.DeviceHandle);
}

VIOGPU3D_DMA *VioGpu3dAdapter::BeginDma(UINT node, UINT fenceId)
{
    VIOGPU3D_DMA *pDma = new (NonPagedPoolNx) VIOGPU3D_DMA;
    if (pDma == NULL)
    {
        return NULL;
    }
    pDma->Adapter = this;
    pDma->Node = node;
    pDma->FenceId = fenceId;
    pDma->Outstanding = 1;
    pDma->Done = FALSE;
    KIRQL irql;
    KeAcquireSpinLock(&m_NodeLock, &irql);
    InsertTailList(&m_Nodes[node].Pending, &pDma->Link);
    m_Nodes[node].LastSubmitted = fenceId;
    KeReleaseSpinLock(&m_NodeLock, irql);
    return pDma;
}

// Every release of a reference happens under the node lock, so that a reset and the last release agree on who
// frees the entry.
void VioGpu3dAdapter::EndDma(VIOGPU3D_DMA *pDma)
{
    KIRQL irql;
    KeAcquireSpinLock(&m_NodeLock, &irql);
    if (InterlockedDecrement(&pDma->Outstanding) != 0)
    {
        KeReleaseSpinLock(&m_NodeLock, irql);
        return;
    }
    if (pDma->Done)
    {
        // A reset took it off its node already.
        KeReleaseSpinLock(&m_NodeLock, irql);
        delete pDma;
        return;
    }
    UINT node = pDma->Node;
    KeReleaseSpinLock(&m_NodeLock, irql);
    CompleteNode(node);
}

void VioGpu3dAdapter::DmaFenceSignaled(void *ctx)
{
    VIOGPU3D_DMA *pDma = (VIOGPU3D_DMA *)ctx;
    pDma->Adapter->EndDma(pDma);
}

// Reports the DMA buffers of a node that are done, in the order the scheduler gave them.
void VioGpu3dAdapter::CompleteNode(UINT node)
{
    KIRQL irql;
    KeAcquireSpinLock(&m_NodeLock, &irql);
    VIOGPU3D_NODE_STATE *pNode = &m_Nodes[node];
    BOOLEAN completed = FALSE;
    UINT last = pNode->LastCompleted;
    while (!IsListEmpty(&pNode->Pending))
    {
        VIOGPU3D_DMA *pDma = CONTAINING_RECORD(pNode->Pending.Flink, VIOGPU3D_DMA, Link);
        if (pDma->Outstanding != 0)
        {
            break;
        }
        RemoveEntryList(&pDma->Link);
        last = pDma->FenceId;
        completed = TRUE;
        delete pDma;
    }
    if (completed)
    {
        pNode->LastCompleted = last;
        ReportCompletion(node, last);
    }
    if (pNode->PreemptRequested && IsListEmpty(&pNode->Pending))
    {
        pNode->PreemptRequested = FALSE;
        ReportPreemption(node, pNode->PreemptionFenceId, pNode->LastCompleted);
    }
    KeReleaseSpinLock(&m_NodeLock, irql);
}

void VioGpu3dAdapter::ReportCompletion(UINT node, UINT fenceId)
{
    DXGKARGCB_NOTIFY_INTERRUPT_DATA interrupt;
    RtlZeroMemory(&interrupt, sizeof(interrupt));
    interrupt.InterruptType = DXGK_INTERRUPT_DMA_COMPLETED;
    interrupt.DmaCompleted.SubmissionFenceId = fenceId;
    interrupt.DmaCompleted.NodeOrdinal = node;
    interrupt.DmaCompleted.EngineOrdinal = 0;
    NotifyInterrupt(&interrupt, TRUE);
}

void VioGpu3dAdapter::ReportPreemption(UINT node, UINT preemptionFenceId, UINT lastCompleted)
{
    DXGKARGCB_NOTIFY_INTERRUPT_DATA interrupt;
    RtlZeroMemory(&interrupt, sizeof(interrupt));
    interrupt.InterruptType = DXGK_INTERRUPT_DMA_PREEMPTED;
    interrupt.DmaPreempted.PreemptionFenceId = preemptionFenceId;
    interrupt.DmaPreempted.LastCompletedFenceId = lastCompleted;
    interrupt.DmaPreempted.NodeOrdinal = node;
    interrupt.DmaPreempted.EngineOrdinal = 0;
    NotifyInterrupt(&interrupt, TRUE);
}

// A DMA buffer of the paging node, or of a context that sends nothing to the host, is done once submitted.
NTSTATUS VioGpu3dAdapter::SubmitCommand(_In_ CONST DXGKARG_SUBMITCOMMAND *pSubmitCommand)
{
    UINT node = pSubmitCommand->NodeOrdinal;
    VioGpu3dContext *pContext = reinterpret_cast<VioGpu3dContext *>(pSubmitCommand->hContext);
    if (pContext != NULL)
    {
        node = pContext->GetNode();
    }
    if (node >= VIOGPU3D_NODE_COUNT)
    {
        return STATUS_INVALID_PARAMETER;
    }
    VIOGPU3D_DMA *pDma = BeginDma(node, pSubmitCommand->SubmissionFenceId);
    if (pDma == NULL)
    {
        return STATUS_NO_MEMORY;
    }
    EndDma(pDma);
    return STATUS_SUCCESS;
}

// Only between DMA buffers: what the host was given runs to its end, and the preemption is reported once the node
// has nothing left.
NTSTATUS VioGpu3dAdapter::PreemptCommand(_In_ CONST DXGKARG_PREEMPTCOMMAND *pPreemptCommand)
{
    UINT node = pPreemptCommand->NodeOrdinal;
    if (node >= VIOGPU3D_NODE_COUNT)
    {
        return STATUS_INVALID_PARAMETER;
    }
    KIRQL irql;
    KeAcquireSpinLock(&m_NodeLock, &irql);
    VIOGPU3D_NODE_STATE *pNode = &m_Nodes[node];
    if (IsListEmpty(&pNode->Pending))
    {
        ReportPreemption(node, pPreemptCommand->PreemptionFenceId, pNode->LastCompleted);
    }
    else
    {
        pNode->PreemptRequested = TRUE;
        pNode->PreemptionFenceId = pPreemptCommand->PreemptionFenceId;
    }
    KeReleaseSpinLock(&m_NodeLock, irql);
    return STATUS_SUCCESS;
}

VIOGPU3D_ESCAPE_FENCE *VioGpu3dAdapter::TrackEscapeFence(VioGpu3dDevice *pDevice, ULONGLONG fenceId)
{
    VIOGPU3D_ESCAPE_FENCE *pFence = new (NonPagedPoolNx) VIOGPU3D_ESCAPE_FENCE;
    if (pFence == NULL)
    {
        return NULL;
    }
    RtlZeroMemory(pFence, sizeof(*pFence));
    pFence->Adapter = this;
    pFence->Device = pDevice;
    pFence->FenceId = fenceId;
    KIRQL irql;
    KeAcquireSpinLock(&m_NodeLock, &irql);
    // A fence no FENCE command ever waits for is forgotten once many newer ones exist.
    if (m_EscapeFenceCount >= 4096)
    {
        for (PLIST_ENTRY p = m_EscapeFences.Flink; p != &m_EscapeFences; p = p->Flink)
        {
            VIOGPU3D_ESCAPE_FENCE *pOld = CONTAINING_RECORD(p, VIOGPU3D_ESCAPE_FENCE, Link);
            if (pOld->Signaled)
            {
                RemoveEntryList(&pOld->Link);
                m_EscapeFenceCount--;
                delete pOld;
                break;
            }
        }
    }
    InsertTailList(&m_EscapeFences, &pFence->Link);
    m_EscapeFenceCount++;
    KeReleaseSpinLock(&m_NodeLock, irql);
    return pFence;
}

void VioGpu3dAdapter::EscapeFenceSignaled(void *ctx)
{
    VIOGPU3D_ESCAPE_FENCE *pFence = (VIOGPU3D_ESCAPE_FENCE *)ctx;
    VioGpu3dAdapter *pAdapter = pFence->Adapter;
    VIOGPU3D_DMA *waiters[VIOGPU3D_FENCE_WAITERS] = {};
    KIRQL irql;
    KeAcquireSpinLock(&pAdapter->m_NodeLock, &irql);
    pFence->Signaled = TRUE;
    BOOLEAN waited = FALSE;
    for (UINT i = 0; i < VIOGPU3D_FENCE_WAITERS; i++)
    {
        waiters[i] = pFence->Waiters[i];
        waited = waited || waiters[i] != NULL;
    }
    if (waited || pFence->Device == NULL)
    {
        RemoveEntryList(&pFence->Link);
        pAdapter->m_EscapeFenceCount--;
        delete pFence;
    }
    KeReleaseSpinLock(&pAdapter->m_NodeLock, irql);
    for (UINT i = 0; i < VIOGPU3D_FENCE_WAITERS; i++)
    {
        if (waiters[i] != NULL)
        {
            pAdapter->EndDma(waiters[i]);
        }
    }
}

void VioGpu3dAdapter::WaitForEscapeFence(VioGpu3dDevice *pDevice, ULONGLONG fenceId, VIOGPU3D_DMA *pDma)
{
    KIRQL irql;
    KeAcquireSpinLock(&m_NodeLock, &irql);
    for (PLIST_ENTRY p = m_EscapeFences.Flink; p != &m_EscapeFences; p = p->Flink)
    {
        VIOGPU3D_ESCAPE_FENCE *pFence = CONTAINING_RECORD(p, VIOGPU3D_ESCAPE_FENCE, Link);
        if (pFence->FenceId != fenceId || pFence->Device != pDevice)
        {
            continue;
        }
        if (pFence->Signaled)
        {
            RemoveEntryList(&pFence->Link);
            m_EscapeFenceCount--;
            delete pFence;
            break;
        }
        for (UINT i = 0; i < VIOGPU3D_FENCE_WAITERS; i++)
        {
            if (pFence->Waiters[i] == NULL)
            {
                InterlockedIncrement(&pDma->Outstanding);
                pFence->Waiters[i] = pDma;
                break;
            }
        }
        break;
    }
    // A fence this device never sent, or one long done and forgotten, holds nothing up.
    KeReleaseSpinLock(&m_NodeLock, irql);
}

void VioGpu3dAdapter::ForgetDevice(VioGpu3dDevice *pDevice)
{
    KIRQL irql;
    KeAcquireSpinLock(&m_NodeLock, &irql);
    PLIST_ENTRY p = m_EscapeFences.Flink;
    while (p != &m_EscapeFences)
    {
        VIOGPU3D_ESCAPE_FENCE *pFence = CONTAINING_RECORD(p, VIOGPU3D_ESCAPE_FENCE, Link);
        p = p->Flink;
        if (pFence->Device != pDevice)
        {
            continue;
        }
        if (pFence->Signaled)
        {
            RemoveEntryList(&pFence->Link);
            m_EscapeFenceCount--;
            delete pFence;
        }
        else
        {
            // The host still owes it; its signal frees it.
            pFence->Device = NULL;
        }
    }
    KeReleaseSpinLock(&m_NodeLock, irql);
}

VioGpu3dAllocation *VioGpu3dAdapter::AllocationFromHandle(D3DKMT_HANDLE handle)
{
    DXGKARGCB_GETHANDLEDATA getHandleData;
    getHandleData.hObject = handle;
    getHandleData.Type = DXGK_HANDLE_ALLOCATION;
    getHandleData.Flags.DeviceSpecific = 0;
    return reinterpret_cast<VioGpu3dAllocation *>(m_DxgkInterface.DxgkCbGetHandleData(&getHandleData));
}
