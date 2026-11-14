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

#include "driver.h"
#include "viogpu3d.h"
#include "vidpn.h"
#include "baseobj.h"
#if !DBG
#include "driver.tmh"
#endif

int nDebugLevel;
int virtioDebugLevel;
int bDebugPrint;
int bBreakAlways;

tDebugPrintFunc VirtioDebugPrintProc;

#if DBG
void InitializeDebugPrints(IN PDRIVER_OBJECT DriverObject, IN PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);
    bDebugPrint = 1;
    virtioDebugLevel = 0x5;
    bBreakAlways = 0;
    nDebugLevel = TRACE_LEVEL_WARNING;
#if defined(COM_DEBUG)
    VirtioDebugPrintProc = DebugPrintFuncSerial;
#elif defined(PRINT_DEBUG)
    VirtioDebugPrintProc = DebugPrintFuncKdPrint;
#endif
}
#endif

static VioGpu3dAdapter *Adapter(HANDLE h)
{
    return reinterpret_cast<VioGpu3dAdapter *>(h);
}

#pragma code_seg(push)
#pragma code_seg("INIT")
extern "C" NTSTATUS DriverEntry(_In_ DRIVER_OBJECT *pDriverObject, _In_ UNICODE_STRING *pRegistryPath)
{
    WPP_INIT_TRACING(pDriverObject, pRegistryPath);

    DRIVER_INITIALIZATION_DATA InitialData = {};
    // The DDI version, not the WDDMVersion capability, sets the contract dxgkrnl holds the driver to.
    InitialData.Version = DXGKDDI_INTERFACE_VERSION;

    InitialData.DxgkDdiAddDevice = VioGpu3dAddDevice;
    InitialData.DxgkDdiStartDevice = VioGpu3dStartDevice;
    InitialData.DxgkDdiStopDevice = VioGpu3dStopDevice;
    InitialData.DxgkDdiRemoveDevice = VioGpu3dRemoveDevice;
    InitialData.DxgkDdiDispatchIoRequest = VioGpu3dDispatchIoRequest;
    InitialData.DxgkDdiInterruptRoutine = VioGpu3dInterruptRoutine;
    InitialData.DxgkDdiDpcRoutine = VioGpu3dDpcRoutine;
    InitialData.DxgkDdiQueryChildRelations = VioGpu3dQueryChildRelations;
    InitialData.DxgkDdiQueryChildStatus = VioGpu3dQueryChildStatus;
    InitialData.DxgkDdiQueryDeviceDescriptor = VioGpu3dQueryDeviceDescriptor;
    InitialData.DxgkDdiSetPowerState = VioGpu3dSetPowerState;
    InitialData.DxgkDdiResetDevice = VioGpu3dResetDevice;
    InitialData.DxgkDdiUnload = VioGpu3dUnload;

    InitialData.DxgkDdiQueryAdapterInfo = VioGpu3dQueryAdapterInfo;
    InitialData.DxgkDdiCreateDevice = VioGpu3dCreateDevice;
    InitialData.DxgkDdiCreateAllocation = VioGpu3dCreateAllocation;
    InitialData.DxgkDdiDestroyAllocation = VioGpu3dDestroyAllocation;
    InitialData.DxgkDdiDescribeAllocation = VioGpu3dDescribeAllocation;
    InitialData.DxgkDdiGetStandardAllocationDriverData = VioGpu3dGetStandardAllocationDriverData;
    InitialData.DxgkDdiPatch = VioGpu3dPatch;
    InitialData.DxgkDdiSubmitCommand = VioGpu3dSubmitCommand;
    InitialData.DxgkDdiPreemptCommand = VioGpu3dPreemptCommand;
    InitialData.DxgkDdiBuildPagingBuffer = VioGpu3dBuildPagingBuffer;
    InitialData.DxgkDdiSetPointerPosition = VioGpu3dSetPointerPosition;
    InitialData.DxgkDdiSetPointerShape = VioGpu3dSetPointerShape;
    InitialData.DxgkDdiResetFromTimeout = VioGpu3dResetFromTimeout;
    InitialData.DxgkDdiRestartFromTimeout = VioGpu3dRestartFromTimeout;
    InitialData.DxgkDdiEscape = VioGpu3dEscape;
    InitialData.DxgkDdiCollectDbgInfo = VioGpu3dCollectDbgInfo;
    InitialData.DxgkDdiQueryCurrentFence = VioGpu3dQueryCurrentFence;
    InitialData.DxgkDdiIsSupportedVidPn = VioGpu3dIsSupportedVidPn;
    InitialData.DxgkDdiRecommendFunctionalVidPn = VioGpu3dRecommendFunctionalVidPn;
    InitialData.DxgkDdiEnumVidPnCofuncModality = VioGpu3dEnumVidPnCofuncModality;
    InitialData.DxgkDdiSetVidPnSourceAddress = VioGpu3dSetVidPnSourceAddress;
    InitialData.DxgkDdiSetVidPnSourceVisibility = VioGpu3dSetVidPnSourceVisibility;
    InitialData.DxgkDdiCommitVidPn = VioGpu3dCommitVidPn;
    InitialData.DxgkDdiUpdateActiveVidPnPresentPath = VioGpu3dUpdateActiveVidPnPresentPath;
    InitialData.DxgkDdiRecommendMonitorModes = VioGpu3dRecommendMonitorModes;
    InitialData.DxgkDdiRecommendVidPnTopology = VioGpu3dRecommendVidPnTopology;
    InitialData.DxgkDdiGetScanLine = VioGpu3dGetScanLine;
    InitialData.DxgkDdiControlInterrupt = VioGpu3dControlInterrupt;

    InitialData.DxgkDdiDestroyDevice = VioGpu3dDestroyDevice;
    InitialData.DxgkDdiOpenAllocation = VioGpu3dOpenAllocation;
    InitialData.DxgkDdiCloseAllocation = VioGpu3dCloseAllocation;
    InitialData.DxgkDdiRender = VioGpu3dRender;
    InitialData.DxgkDdiPresent = VioGpu3dPresent;
    InitialData.DxgkDdiCreateContext = VioGpu3dCreateContext;
    InitialData.DxgkDdiDestroyContext = VioGpu3dDestroyContext;

    InitialData.DxgkDdiQueryVidPnHWCapability = VioGpu3dQueryVidPnHWCapability;
    InitialData.DxgkDdiQueryEngineStatus = VioGpu3dQueryEngineStatus;
    InitialData.DxgkDdiResetEngine = VioGpu3dResetEngine;
    InitialData.DxgkDdiStopDeviceAndReleasePostDisplayOwnership = VioGpu3dStopDeviceAndReleasePostDisplayOwnership;
    InitialData.DxgkDdiSystemDisplayEnable = VioGpu3dSystemDisplayEnable;
    InitialData.DxgkDdiSystemDisplayWrite = VioGpu3dSystemDisplayWrite;
    InitialData.DxgkDdiCancelCommand = VioGpu3dCancelCommand;
    InitialData.DxgkDdiGetNodeMetadata = VioGpu3dGetNodeMetadata;
    InitialData.DxgkDdiCalibrateGpuClock = VioGpu3dCalibrateGpuClock;

    InitialData.DxgkDdiRenderGdi = VioGpu3dRenderGdi;
    InitialData.DxgkDdiSubmitCommandVirtual = VioGpu3dSubmitCommandVirtual;
    InitialData.DxgkDdiSetRootPageTable = VioGpu3dSetRootPageTable;
    InitialData.DxgkDdiGetRootPageTableSize = VioGpu3dGetRootPageTableSize;
    InitialData.DxgkDdiMapCpuHostAperture = VioGpu3dMapCpuHostAperture;
    InitialData.DxgkDdiUnmapCpuHostAperture = VioGpu3dUnmapCpuHostAperture;
    InitialData.DxgkDdiCreateProcess = VioGpu3dCreateProcess;
    InitialData.DxgkDdiDestroyProcess = VioGpu3dDestroyProcess;
    InitialData.DxgkDdiSetStablePowerState = VioGpu3dSetStablePowerState;

    NTSTATUS Status = DxgkInitialize(pDriverObject, pRegistryPath, &InitialData);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("DxgkInitialize failed with Status: 0x%X\n", Status));
        WPP_CLEANUP(pDriverObject);
    }
    return Status;
}
#pragma code_seg(pop)

PAGED_CODE_SEG_BEGIN

VOID VioGpu3dUnload(VOID)
{
    PAGED_CODE();
    WPP_CLEANUP(NULL);
}

NTSTATUS VioGpu3dAddDevice(_In_ DEVICE_OBJECT *pPhysicalDeviceObject, _Outptr_ PVOID *ppDeviceContext)
{
    PAGED_CODE();

    if (pPhysicalDeviceObject == NULL || ppDeviceContext == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    VioGpu3dAdapter *pAdapter = new (NonPagedPoolNx) VioGpu3dAdapter(pPhysicalDeviceObject);
    if (pAdapter == NULL || pAdapter->vidpn == NULL)
    {
        delete pAdapter;
        *ppDeviceContext = NULL;
        return STATUS_NO_MEMORY;
    }
    *ppDeviceContext = pAdapter;
    return STATUS_SUCCESS;
}

NTSTATUS VioGpu3dRemoveDevice(_In_ VOID *pDeviceContext)
{
    PAGED_CODE();
    delete Adapter(pDeviceContext);
    return STATUS_SUCCESS;
}

NTSTATUS VioGpu3dStartDevice(_In_ VOID *pDeviceContext,
                             _In_ DXGK_START_INFO *pDxgkStartInfo,
                             _In_ DXGKRNL_INTERFACE *pDxgkInterface,
                             _Out_ ULONG *pNumberOfViews,
                             _Out_ ULONG *pNumberOfChildren)
{
    PAGED_CODE();
    return Adapter(pDeviceContext)->StartDevice(pDxgkStartInfo, pDxgkInterface, pNumberOfViews, pNumberOfChildren);
}

NTSTATUS VioGpu3dStopDevice(_In_ VOID *pDeviceContext)
{
    PAGED_CODE();
    return Adapter(pDeviceContext)->StopDevice();
}

NTSTATUS VioGpu3dDispatchIoRequest(_In_ VOID *pDeviceContext,
                                   _In_ ULONG VidPnSourceId,
                                   _In_ VIDEO_REQUEST_PACKET *pVideoRequestPacket)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(pDeviceContext);
    UNREFERENCED_PARAMETER(VidPnSourceId);
    UNREFERENCED_PARAMETER(pVideoRequestPacket);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS VioGpu3dSetPowerState(_In_ VOID *pDeviceContext,
                               _In_ ULONG HardwareUid,
                               _In_ DEVICE_POWER_STATE DevicePowerState,
                               _In_ POWER_ACTION ActionType)
{
    PAGED_CODE();
    VioGpu3dAdapter *pAdapter = Adapter(pDeviceContext);
    if (!pAdapter->IsDriverActive())
    {
        return STATUS_SUCCESS;
    }
    return pAdapter->SetPowerState(HardwareUid, DevicePowerState, ActionType);
}

NTSTATUS VioGpu3dQueryChildRelations(_In_ VOID *pDeviceContext,
                                     _Out_writes_bytes_(ChildRelationsSize) DXGK_CHILD_DESCRIPTOR *pChildRelations,
                                     _In_ ULONG ChildRelationsSize)
{
    PAGED_CODE();
    return Adapter(pDeviceContext)->QueryChildRelations(pChildRelations, ChildRelationsSize);
}

NTSTATUS VioGpu3dQueryChildStatus(_In_ VOID *pDeviceContext,
                                  _Inout_ DXGK_CHILD_STATUS *pChildStatus,
                                  _In_ BOOLEAN NonDestructiveOnly)
{
    PAGED_CODE();
    return Adapter(pDeviceContext)->QueryChildStatus(pChildStatus, NonDestructiveOnly);
}

NTSTATUS VioGpu3dQueryDeviceDescriptor(_In_ VOID *pDeviceContext,
                                       _In_ ULONG ChildUid,
                                       _Inout_ DXGK_DEVICE_DESCRIPTOR *pDeviceDescriptor)
{
    PAGED_CODE();
    VioGpu3dAdapter *pAdapter = Adapter(pDeviceContext);
    if (!pAdapter->IsDriverActive())
    {
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->QueryDeviceDescriptor(ChildUid, pDeviceDescriptor);
}

NTSTATUS APIENTRY VioGpu3dQueryAdapterInfo(_In_ CONST HANDLE hAdapter,
                                           _In_ CONST DXGKARG_QUERYADAPTERINFO *pQueryAdapterInfo)
{
    PAGED_CODE();
    return Adapter(hAdapter)->QueryAdapterInfo(pQueryAdapterInfo);
}

NTSTATUS APIENTRY VioGpu3dCreateDevice(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_CREATEDEVICE *pCreateDevice)
{
    PAGED_CODE();
    VioGpu3dDevice *pDevice = new (NonPagedPoolNx) VioGpu3dDevice(Adapter(hAdapter));
    if (pDevice == NULL)
    {
        return STATUS_NO_MEMORY;
    }
    pCreateDevice->hDevice = pDevice;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY VioGpu3dDestroyDevice(_In_ CONST HANDLE hDevice)
{
    PAGED_CODE();
    delete reinterpret_cast<VioGpu3dDevice *>(hDevice);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY VioGpu3dCreateAllocation(_In_ CONST HANDLE hAdapter,
                                           _Inout_ DXGKARG_CREATEALLOCATION *pCreateAllocation)
{
    PAGED_CODE();
    return Adapter(hAdapter)->CreateAllocation(pCreateAllocation);
}

NTSTATUS APIENTRY VioGpu3dDestroyAllocation(_In_ CONST HANDLE hAdapter,
                                            _In_ CONST DXGKARG_DESTROYALLOCATION *pDestroyAllocation)
{
    PAGED_CODE();
    return Adapter(hAdapter)->DestroyAllocation(pDestroyAllocation);
}

NTSTATUS APIENTRY VioGpu3dDescribeAllocation(_In_ CONST HANDLE hAdapter,
                                             _Inout_ DXGKARG_DESCRIBEALLOCATION *pDescribeAllocation)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    reinterpret_cast<VioGpu3dAllocation *>(pDescribeAllocation->hAllocation)->DescribeAllocation(pDescribeAllocation);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY VioGpu3dGetStandardAllocationDriverData(_In_ CONST HANDLE hAdapter,
                                                          _Inout_ DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *pStandard)
{
    PAGED_CODE();
    return Adapter(hAdapter)->GetStandardAllocationDriverData(pStandard);
}

NTSTATUS APIENTRY VioGpu3dOpenAllocation(_In_ CONST HANDLE hDevice, _In_ CONST DXGKARG_OPENALLOCATION *pOpenAllocation)
{
    PAGED_CODE();
    VioGpu3dDevice *pDevice = reinterpret_cast<VioGpu3dDevice *>(hDevice);
    VioGpu3dAdapter *pAdapter = pDevice->GetAdapter();
    for (UINT i = 0; i < pOpenAllocation->NumAllocations; i++)
    {
        DXGK_OPENALLOCATIONINFO *pInfo = &pOpenAllocation->pOpenAllocation[i];
        DXGKARGCB_GETHANDLEDATA getHandleData = {};
        getHandleData.hObject = pInfo->hAllocation;
        getHandleData.Type = DXGK_HANDLE_ALLOCATION;
        VioGpu3dAllocation *pAllocation = reinterpret_cast<VioGpu3dAllocation *>(pAdapter->GetDxgkInterface()->DxgkCbGetHandleData(
                                                                                                            &getHandleData));
        if (pAllocation == NULL)
        {
            return STATUS_INVALID_PARAMETER;
        }
        // The creating device makes the host resource in its context, after any command its resource carried.
        NTSTATUS status = pOpenAllocation->Flags.Create ? pAllocation->CreateOnHost(pDevice)
                                                        : pAllocation->Open(pDevice);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
        if (pInfo->pPrivateDriverData != NULL && pInfo->PrivateDriverDataSize >= sizeof(VIRTIO_WDDM_CREATE_ALLOCATION))
        {
            *(VIRTIO_WDDM_CREATE_ALLOCATION *)pInfo->pPrivateDriverData = pAllocation->GetInfo();
        }
        pInfo->hDeviceSpecificAllocation = pAllocation;
    }
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY VioGpu3dCloseAllocation(_In_ CONST HANDLE hDevice,
                                          _In_ CONST DXGKARG_CLOSEALLOCATION *pCloseAllocation)
{
    PAGED_CODE();
    VioGpu3dDevice *pDevice = reinterpret_cast<VioGpu3dDevice *>(hDevice);
    for (UINT i = 0; i < pCloseAllocation->NumAllocations; i++)
    {
        VioGpu3dAllocation *pAllocation = reinterpret_cast<VioGpu3dAllocation *>(pCloseAllocation->pOpenHandleList[i]);
        if (pAllocation != NULL)
        {
            pAllocation->Close(pDevice);
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY VioGpu3dCreateContext(_In_ CONST HANDLE hDevice, _Inout_ DXGKARG_CREATECONTEXT *pCreateContext)
{
    PAGED_CODE();
    if (pCreateContext->NodeOrdinal >= VIOGPU3D_NODE_COUNT)
    {
        return STATUS_INVALID_PARAMETER;
    }
    VioGpu3dContext *pContext = new (NonPagedPoolNx) VioGpu3dContext(reinterpret_cast<VioGpu3dDevice *>(hDevice),
                                                                     pCreateContext->NodeOrdinal,
                                                                     pCreateContext->Flags);
    if (pContext == NULL)
    {
        return STATUS_NO_MEMORY;
    }
    pCreateContext->hContext = pContext;
    DXGK_CONTEXTINFO *pInfo = &pCreateContext->ContextInfo;
    RtlZeroMemory(pInfo, sizeof(*pInfo));
    if (pCreateContext->Flags.VirtualAddressing)
    {
        pInfo->DmaBufferPrivateDataSize = VIRTIO_WDDM_MAX_SUBMIT_COMMAND_VIRTUAL_SIZE;
    }
    else
    {
        pInfo->DmaBufferSize = VIOGPU3D_DMA_BUFFER_SIZE;
        // The blts and fills of Present.
        pInfo->DmaBufferPrivateDataSize = VIOGPU3D_PRESENT_PRIVATE_SIZE;
        pInfo->DmaBufferSegmentSet = 0;
        pInfo->AllocationListSize = pCreateContext->Flags.GdiContext ? DXGK_ALLOCATION_LIST_SIZE_GDICONTEXT : 64;
        pInfo->PatchLocationListSize = 64;
    }
    pInfo->PagingCompanionNodeId = VIOGPU3D_NODE_PAGING;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY VioGpu3dDestroyContext(_In_ CONST HANDLE hContext)
{
    PAGED_CODE();
    delete reinterpret_cast<VioGpu3dContext *>(hContext);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY VioGpu3dSubmitCommandVirtual(_In_ CONST HANDLE hAdapter,
                                               _In_ CONST DXGKARG_SUBMITCOMMANDVIRTUAL *pSubmitCommand)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    VioGpu3dContext *pContext = reinterpret_cast<VioGpu3dContext *>(pSubmitCommand->hContext);
    if (pContext == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    return pContext->SubmitVirtual(pSubmitCommand);
}

// Commands go in the private data of virtual contexts; a legacy DMA buffer has nothing to patch.
NTSTATUS APIENTRY VioGpu3dPatch(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_PATCH *pPatch)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pPatch);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY VioGpu3dRender(_In_ CONST HANDLE hContext, _Inout_ DXGKARG_RENDER *pRender)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hContext);
    UNREFERENCED_PARAMETER(pRender);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS APIENTRY VioGpu3dRenderGdi(_In_ CONST HANDLE hContext, _Inout_ DXGKARG_RENDERGDI *pRenderGdi)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hContext);
    UNREFERENCED_PARAMETER(pRenderGdi);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS APIENTRY VioGpu3dPresent(_In_ CONST HANDLE hContext, _Inout_ DXGKARG_PRESENT *pPresent)
{
    PAGED_CODE();
    VioGpu3dContext *pContext = reinterpret_cast<VioGpu3dContext *>(hContext);
    if (pContext == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    return pContext->GetDevice()->GetAdapter()->Present(pContext, pPresent);
}

NTSTATUS APIENTRY VioGpu3dBuildPagingBuffer(_In_ CONST HANDLE hAdapter,
                                            _In_ DXGKARG_BUILDPAGINGBUFFER *pBuildPagingBuffer)
{
    PAGED_CODE();
    return Adapter(hAdapter)->BuildPagingBuffer(pBuildPagingBuffer);
}

NTSTATUS APIENTRY VioGpu3dSetPointerPosition(_In_ CONST HANDLE hAdapter,
                                             _In_ CONST DXGKARG_SETPOINTERPOSITION *pSetPointerPosition)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pSetPointerPosition);
    return STATUS_SUCCESS;
}

// No hardware cursor yet: MaxPointerWidth is 0, and Windows draws the pointer.
NTSTATUS APIENTRY VioGpu3dSetPointerShape(_In_ CONST HANDLE hAdapter,
                                          _In_ CONST DXGKARG_SETPOINTERSHAPE *pSetPointerShape)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pSetPointerShape);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS APIENTRY VioGpu3dResetFromTimeout(_In_ CONST HANDLE hAdapter)
{
    PAGED_CODE();
    return Adapter(hAdapter)->ResetFromTimeout();
}

NTSTATUS APIENTRY VioGpu3dRestartFromTimeout(_In_ CONST HANDLE hAdapter)
{
    PAGED_CODE();
    return Adapter(hAdapter)->RestartFromTimeout();
}

NTSTATUS APIENTRY VioGpu3dEscape(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_ESCAPE *pEscape)
{
    PAGED_CODE();
    return Adapter(hAdapter)->Escape(pEscape);
}

NTSTATUS APIENTRY VioGpu3dCollectDbgInfo(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_COLLECTDBGINFO *pCollectDbgInfo)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    if (pCollectDbgInfo->pBuffer != NULL && pCollectDbgInfo->BufferSize > 0)
    {
        RtlZeroMemory(pCollectDbgInfo->pBuffer, pCollectDbgInfo->BufferSize);
    }
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY VioGpu3dQueryCurrentFence(_In_ CONST HANDLE hAdapter,
                                            _Inout_ DXGKARG_QUERYCURRENTFENCE *pCurrentFence)
{
    PAGED_CODE();
    return Adapter(hAdapter)->QueryCurrentFence(pCurrentFence);
}

NTSTATUS APIENTRY VioGpu3dIsSupportedVidPn(_In_ CONST HANDLE hAdapter,
                                           _Inout_ DXGKARG_ISSUPPORTEDVIDPN *pIsSupportedVidPn)
{
    PAGED_CODE();
    VioGpu3dAdapter *pAdapter = Adapter(hAdapter);
    if (!pAdapter->IsDriverActive())
    {
        pIsSupportedVidPn->IsVidPnSupported = TRUE;
        return STATUS_SUCCESS;
    }
    return pAdapter->vidpn->IsSupportedVidPn(pIsSupportedVidPn);
}

NTSTATUS APIENTRY VioGpu3dRecommendFunctionalVidPn(_In_ CONST HANDLE hAdapter,
                                                   _In_ CONST DXGKARG_RECOMMENDFUNCTIONALVIDPN *CONST pRecommend)
{
    PAGED_CODE();
    return Adapter(hAdapter)->vidpn->RecommendFunctionalVidPn(pRecommend);
}

NTSTATUS APIENTRY VioGpu3dRecommendVidPnTopology(_In_ CONST HANDLE hAdapter,
                                                 _In_ CONST DXGKARG_RECOMMENDVIDPNTOPOLOGY *CONST pRecommend)
{
    PAGED_CODE();
    return Adapter(hAdapter)->vidpn->RecommendVidPnTopology(pRecommend);
}

NTSTATUS APIENTRY VioGpu3dRecommendMonitorModes(_In_ CONST HANDLE hAdapter,
                                                _In_ CONST DXGKARG_RECOMMENDMONITORMODES *CONST pRecommend)
{
    PAGED_CODE();
    return Adapter(hAdapter)->vidpn->RecommendMonitorModes(pRecommend);
}

NTSTATUS APIENTRY VioGpu3dEnumVidPnCofuncModality(_In_ CONST HANDLE hAdapter,
                                                  _In_ CONST DXGKARG_ENUMVIDPNCOFUNCMODALITY *CONST pEnumCofuncModality)
{
    PAGED_CODE();
    return Adapter(hAdapter)->vidpn->EnumVidPnCofuncModality(pEnumCofuncModality);
}

NTSTATUS APIENTRY VioGpu3dSetVidPnSourceVisibility(_In_ CONST HANDLE hAdapter,
                                                   _In_ CONST DXGKARG_SETVIDPNSOURCEVISIBILITY *pVisibility)
{
    PAGED_CODE();
    return Adapter(hAdapter)->vidpn->SetVidPnSourceVisibility(pVisibility);
}

NTSTATUS APIENTRY VioGpu3dCommitVidPn(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_COMMITVIDPN *CONST pCommitVidPn)
{
    PAGED_CODE();
    return Adapter(hAdapter)->vidpn->CommitVidPn(pCommitVidPn);
}

NTSTATUS APIENTRY VioGpu3dUpdateActiveVidPnPresentPath(_In_ CONST HANDLE hAdapter,
                                                       _In_ CONST DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *CONST pUpdate)
{
    PAGED_CODE();
    return Adapter(hAdapter)->vidpn->UpdateActiveVidPnPresentPath(pUpdate);
}

NTSTATUS APIENTRY VioGpu3dQueryVidPnHWCapability(_In_ CONST HANDLE hAdapter,
                                                 _Inout_ DXGKARG_QUERYVIDPNHWCAPABILITY *pVidPnHWCaps)
{
    PAGED_CODE();
    return Adapter(hAdapter)->vidpn->QueryVidPnHWCapability(pVidPnHWCaps);
}

NTSTATUS APIENTRY VioGpu3dControlInterrupt(_In_ CONST HANDLE hAdapter,
                                           _In_ CONST DXGK_INTERRUPT_TYPE InterruptType,
                                           _In_ BOOLEAN EnableInterrupt)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(InterruptType);
    UNREFERENCED_PARAMETER(EnableInterrupt);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY VioGpu3dGetScanLine(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_GETSCANLINE *pGetScanLine)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    pGetScanLine->InVerticalBlank = FALSE;
    pGetScanLine->ScanLine = 0;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY VioGpu3dStopDeviceAndReleasePostDisplayOwnership(_In_ VOID *pDeviceContext,
                                                                   _In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                                                   _Out_ DXGK_DISPLAY_INFORMATION *pDisplayInfo)
{
    PAGED_CODE();
    return Adapter(pDeviceContext)->StopDeviceAndReleasePostDisplayOwnership(TargetId, pDisplayInfo);
}

NTSTATUS APIENTRY VioGpu3dQueryEngineStatus(_In_ CONST HANDLE hAdapter,
                                            _Inout_ DXGKARG_QUERYENGINESTATUS *pQueryEngineStatus)
{
    PAGED_CODE();
    return Adapter(hAdapter)->QueryEngineStatus(pQueryEngineStatus);
}

NTSTATUS APIENTRY VioGpu3dResetEngine(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_RESETENGINE *pResetEngine)
{
    PAGED_CODE();
    return Adapter(hAdapter)->ResetEngine(pResetEngine);
}

NTSTATUS APIENTRY VioGpu3dCancelCommand(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_CANCELCOMMAND *pCancelCommand)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pCancelCommand);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY VioGpu3dGetNodeMetadata(_In_ CONST HANDLE hAdapter,
                                          UINT NodeOrdinal,
                                          _Out_ DXGKARG_GETNODEMETADATA *pGetNodeMetadata)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    if (NodeOrdinal >= VIOGPU3D_NODE_COUNT)
    {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(pGetNodeMetadata, sizeof(*pGetNodeMetadata));
    const WCHAR *name;
    switch (NodeOrdinal)
    {
        case VIOGPU3D_NODE_SYSTEM:
            pGetNodeMetadata->EngineType = DXGK_ENGINE_TYPE_3D;
            name = L"VirtIO GPU";
            break;
        case VIOGPU3D_NODE_PAGING:
            pGetNodeMetadata->EngineType = DXGK_ENGINE_TYPE_COPY;
            name = L"VirtIO GPU paging";
            break;
        default:
            pGetNodeMetadata->EngineType = DXGK_ENGINE_TYPE_OTHER;
            name = L"Venus";
            break;
    }
    RtlStringCchCopyW(pGetNodeMetadata->FriendlyName, ARRAYSIZE(pGetNodeMetadata->FriendlyName), name);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY VioGpu3dMapCpuHostAperture(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_MAPCPUHOSTAPERTURE *pArgs)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pArgs);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS APIENTRY VioGpu3dUnmapCpuHostAperture(_In_ CONST HANDLE hAdapter,
                                               _In_ CONST DXGKARG_UNMAPCPUHOSTAPERTURE *pArgs)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pArgs);
    return STATUS_NOT_SUPPORTED;
}

// Nothing of a process lives in the driver: page tables are VidMm's and the device never walks them.
NTSTATUS APIENTRY VioGpu3dCreateProcess(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_CREATEPROCESS *pArgs)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    pArgs->hKmdProcess = pArgs->hDxgkProcess;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY VioGpu3dDestroyProcess(_In_ CONST HANDLE hAdapter, _In_ CONST HANDLE hKmdProcess)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(hKmdProcess);
    return STATUS_SUCCESS;
}

// The root table is a whole number of pages of the page table segment.
SIZE_T APIENTRY VioGpu3dGetRootPageTableSize(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_GETROOTPAGETABLESIZE *pArgs)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    const UINT ptesPerPage = PAGE_SIZE / sizeof(DXGK_PTE);
    UINT pages = (pArgs->NumberOfPte + ptesPerPage - 1) / ptesPerPage;
    pages = max(pages, 1u);
    pArgs->NumberOfPte = pages * ptesPerPage;
    return (SIZE_T)pages * PAGE_SIZE;
}

VOID APIENTRY VioGpu3dSetRootPageTable(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_SETROOTPAGETABLE *pSetPageTable)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pSetPageTable);
}

VOID APIENTRY VioGpu3dSetStablePowerState(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_SETSTABLEPOWERSTATE *pArgs)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pArgs);
}

PAGED_CODE_SEG_END

//
// Non-paged code
//

BOOLEAN VioGpu3dInterruptRoutine(_In_ VOID *pDeviceContext, _In_ ULONG MessageNumber)
{
    return Adapter(pDeviceContext)->InterruptRoutine(MessageNumber);
}

VOID VioGpu3dDpcRoutine(_In_ VOID *pDeviceContext)
{
    Adapter(pDeviceContext)->DpcRoutine();
}

VOID VioGpu3dResetDevice(_In_ VOID *pDeviceContext)
{
    Adapter(pDeviceContext)->ResetDevice();
}

NTSTATUS APIENTRY VioGpu3dSubmitCommand(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_SUBMITCOMMAND *pSubmitCommand)
{
    return Adapter(hAdapter)->SubmitCommand(pSubmitCommand);
}

NTSTATUS APIENTRY VioGpu3dPreemptCommand(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_PREEMPTCOMMAND *pPreemptCommand)
{
    return Adapter(hAdapter)->PreemptCommand(pPreemptCommand);
}

NTSTATUS APIENTRY VioGpu3dSetVidPnSourceAddress(_In_ CONST HANDLE hAdapter,
                                                _In_ CONST DXGKARG_SETVIDPNSOURCEADDRESS *pSetVidPnSourceAddress)
{
    return Adapter(hAdapter)->vidpn->SetVidPnSourceAddress(pSetVidPnSourceAddress);
}

NTSTATUS APIENTRY VioGpu3dSystemDisplayEnable(_In_ VOID *pDeviceContext,
                                              _In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                              _In_ PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS Flags,
                                              _Out_ UINT *pWidth,
                                              _Out_ UINT *pHeight,
                                              _Out_ D3DDDIFORMAT *pColorFormat)
{
    return Adapter(pDeviceContext)->vidpn->SystemDisplayEnable(TargetId, Flags, pWidth, pHeight, pColorFormat);
}

VOID APIENTRY VioGpu3dSystemDisplayWrite(_In_ VOID *pDeviceContext,
                                         _In_reads_bytes_(SourceHeight *SourceStride) VOID *pSource,
                                         _In_ UINT SourceWidth,
                                         _In_ UINT SourceHeight,
                                         _In_ UINT SourceStride,
                                         _In_ UINT PositionX,
                                         _In_ UINT PositionY)
{
    Adapter(pDeviceContext)->vidpn->SystemDisplayWrite(pSource, SourceWidth, SourceHeight, SourceStride, PositionX, PositionY);
}

// The device has no clock of its own; the guest's performance counter stands for it on both sides.
NTSTATUS APIENTRY VioGpu3dCalibrateGpuClock(_In_ CONST HANDLE hAdapter,
                                            _In_ UINT32 NodeOrdinal,
                                            _In_ UINT32 EngineOrdinal,
                                            _Out_ DXGKARG_CALIBRATEGPUCLOCK *pClockCalibration)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(NodeOrdinal);
    UNREFERENCED_PARAMETER(EngineOrdinal);
    LARGE_INTEGER frequency;
    LARGE_INTEGER now = KeQueryPerformanceCounter(&frequency);
    pClockCalibration->GpuFrequency = (UINT64)frequency.QuadPart;
    pClockCalibration->GpuClockCounter = (UINT64)now.QuadPart;
    pClockCalibration->CpuClockCounter = (UINT64)now.QuadPart;
    return STATUS_SUCCESS;
}

#if DBG
void DebugPrintFuncSerial(CONST char *format, ...)
{
    UNREFERENCED_PARAMETER(format);
}

void DebugPrintFuncKdPrint(CONST char *format, ...)
{
    va_list list;
    va_start(list, format);
    vDbgPrintEx(DPFLTR_IHVVIDEO_ID, DPFLTR_ERROR_LEVEL, format, list);
    va_end(list);
}
#endif
