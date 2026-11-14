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

#pragma once

#include "helper.h"
#include "viogpu.h"
#include "viogpu_queue.h"
#include "viogpu3d_uapi.h"

// Engines. The Venus ICD of Mesa's Windows work opens one virtual context a ring on the ring nodes, or, built for
// hardware queues, one on the Venus node.
#define VIOGPU3D_NODE_COUNT          64
#define VIOGPU3D_NODE_SYSTEM         0
#define VIOGPU3D_NODE_VENUS          1
#define VIOGPU3D_NODE_PAGING         2
#define VIOGPU3D_NODE_FIRST_RING     3
#define VIOGPU3D_RING_COUNT          64

// Segments, numbered from 1: system memory the device reaches through backing entries, and the page tables.
#define VIOGPU3D_SEGMENT_APERTURE    1
#define VIOGPU3D_SEGMENT_PAGE_TABLES 2
#define VIOGPU3D_APERTURE_SIZE       (1ull << 30)

// GPU virtual addresses: two levels of 1024 entries over 4 KiB pages, 32 bits in all, written by the CPU. A virtio
// GPU reads no GPU virtual address; the tables exist because VidMm keeps the WDDM 2.0 model only with them.
#define VIOGPU3D_VA_BITS             32
#define VIOGPU3D_PT_LEVELS           2
#define VIOGPU3D_PT_INDEX_BITS       10
#define VIOGPU3D_PT_ENTRIES          (1u << VIOGPU3D_PT_INDEX_BITS)
#define VIOGPU3D_PT_SEGMENT_SIZE     (16u << 20)

#define VIOGPU3D_PAGING_BUFFER_SIZE  (16u * PAGE_SIZE)
#define VIOGPU3D_DMA_BUFFER_SIZE     (64u * 1024)

// The largest display: 16,777,216 pixels, 64 MiB at 4 bytes, so 5120 x 2880 fits.
#define VIOGPU3D_MAX_DISPLAY_PIXELS  (16u * 1024 * 1024)
#define VIOGPU3D_MAX_DISPLAY_SIDE    8184

#define VIOGPU3D_TAG                 '3dGV'

class VioGpu3dAdapter;
class VioGpu3dDevice;
class VioGpu3dContext;
class VioGpu3dAllocation;
class VioGpuVidPN;

// A DMA buffer the scheduler gave a node, done once every fenced command it sent has signalled.
struct VIOGPU3D_DMA
{
    LIST_ENTRY Link;
    VioGpu3dAdapter *Adapter;
    UINT Node;
    UINT FenceId;
    // One for the submission itself, released once every command went out, and one for each fenced command.
    volatile LONG Outstanding;
    BOOLEAN Done;
};

struct VIOGPU3D_NODE_STATE
{
    LIST_ENTRY Pending;
    UINT LastSubmitted;
    UINT LastCompleted;
    BOOLEAN PreemptRequested;
    UINT PreemptionFenceId;
};

#define VIOGPU3D_FENCE_WAITERS 4

// A fence an EXEC_BUF escape sent, which a later FENCE command of a submission waits for.
struct VIOGPU3D_ESCAPE_FENCE
{
    LIST_ENTRY Link;
    VioGpu3dAdapter *Adapter;
    VioGpu3dDevice *Device;
    ULONGLONG FenceId;
    BOOLEAN Signaled;
    VIOGPU3D_DMA *Waiters[VIOGPU3D_FENCE_WAITERS];
};

struct VIOGPU3D_CAPSET
{
    UINT Id;
    UINT MaxVersion;
    UINT MaxSize;
};

struct VIOGPU3D_USER_MAPPING
{
    LIST_ENTRY Link;
    PEPROCESS Process;
    PVOID Address;
};

enum VIOGPU3D_ALLOCATION_KIND
{
    VIOGPU3D_ALLOCATION_BLOB,
    VIOGPU3D_ALLOCATION_3D,
};

// One WDDM allocation and the virtio resource behind it.
class VioGpu3dAllocation
{
  public:
    VioGpu3dAllocation(VioGpu3dAdapter *adapter);
    ~VioGpu3dAllocation();

    NTSTATUS Init(_Inout_ DXGK_ALLOCATIONINFO *pInfo,
                  _In_opt_ const VIRTIO_WDDM_CREATE_RESOURCE *pResource,
                  UINT resourceSize);
    // The host resource, made in the creating device's context once the runtime binds the allocation to it.
    NTSTATUS CreateOnHost(_In_ VioGpu3dDevice *pDevice);
    NTSTATUS Open(_In_ VioGpu3dDevice *pDevice);
    void Close(_In_ VioGpu3dDevice *pDevice);

    NTSTATUS MapToProcess(_Out_ PVOID *pAddress);
    NTSTATUS UnmapFromProcess(PVOID address);

    NTSTATUS AttachBacking(_In_ PMDL pMdl, SIZE_T offset, SIZE_T pages);
    void DetachBacking();

    void DescribeAllocation(_Inout_ DXGKARG_DESCRIBEALLOCATION *pDescribe) const;

    UINT GetResId() const
    {
        return m_ResId;
    }
    BOOLEAN IsBlob() const
    {
        return m_Kind == VIOGPU3D_ALLOCATION_BLOB;
    }
    const VIRTIO_WDDM_CREATE_ALLOCATION &GetInfo() const
    {
        return m_Info;
    }
    BOOLEAN IsPrimary() const
    {
        return m_Primary;
    }
    void SetPrimary(D3DDDI_VIDEO_PRESENT_SOURCE_ID sourceId, UINT width, UINT height, UINT pitch, D3DDDIFORMAT format);
    UINT GetWidth() const
    {
        return m_Width;
    }
    UINT GetHeight() const
    {
        return m_Height;
    }
    UINT GetPitch() const
    {
        return m_Pitch;
    }
    D3DDDIFORMAT GetFormat() const
    {
        return m_Format;
    }

  private:
    NTSTATUS SendPrelude(_In_ VioGpu3dDevice *pDevice);
    void ReleaseHostMapping();

    VioGpu3dAdapter *m_pAdapter;
    VIOGPU3D_ALLOCATION_KIND m_Kind;
    VIRTIO_WDDM_CREATE_ALLOCATION m_Info;
    UINT m_ResId;
    BOOLEAN m_Created;
    UINT m_CreatorCtxId;
    PUCHAR m_pPrelude;
    UINT m_PreludeSize;

    BOOLEAN m_Primary;
    UINT m_Width;
    UINT m_Height;
    UINT m_Pitch;
    D3DDDIFORMAT m_Format;

    BOOLEAN m_Backed;

    // The blob's place in the host-visible region while the host maps it there, and the processes it is mapped in.
    KEVENT m_MapLock;
    BOOLEAN m_HostMapped;
    ULONGLONG m_RegionOffset;
    ULONGLONG m_RegionSize;
    MEMORY_CACHING_TYPE m_CacheType;
    LIST_ENTRY m_UserMappings;
};

// A device of one process, and the virtio context its user-mode driver makes for it.
class VioGpu3dDevice
{
  public:
    VioGpu3dDevice(VioGpu3dAdapter *adapter);
    ~VioGpu3dDevice();

    NTSTATUS InitContext(_In_ const VIRTIO_WDDM_CONTEXT_INIT *pInit);
    UINT GetCtxId() const
    {
        return m_CtxId;
    }
    UINT GetRingCount() const
    {
        return m_NumRings;
    }
    PEPROCESS GetProcess() const
    {
        return m_Process;
    }
    VioGpu3dAdapter *GetAdapter() const
    {
        return m_pAdapter;
    }

  private:
    VioGpu3dAdapter *m_pAdapter;
    PEPROCESS m_Process;
    UINT m_CtxId;
    UINT m_CapsetId;
    UINT m_NumRings;
};

class VioGpu3dContext
{
  public:
    VioGpu3dContext(VioGpu3dDevice *device, UINT node, DXGK_CREATECONTEXTFLAGS flags);

    NTSTATUS SubmitVirtual(_In_ const DXGKARG_SUBMITCOMMANDVIRTUAL *pSubmit);
    NTSTATUS ExecBuffer(_Inout_ VIRTIO_WDDM_EXEC_BUFFER *pExec, UINT size);

    VioGpu3dDevice *GetDevice() const
    {
        return m_pDevice;
    }
    UINT GetNode() const
    {
        return m_Node;
    }
    BOOLEAN IsVirtual() const
    {
        return m_Flags.VirtualAddressing;
    }

  private:
    NTSTATUS SendCommand(_In_ const VIRTIO_WDDM_COMMAND_HEADER *pHeader,
                         _In_reads_bytes_(pHeader->Size) const UCHAR *pBody,
                         _In_opt_ VIOGPU3D_DMA *pDma,
                         _Out_opt_ ULONGLONG *pFenceId);

    VioGpu3dDevice *m_pDevice;
    UINT m_Node;
    DXGK_CREATECONTEXTFLAGS m_Flags;
};

// The host-visible region: shared memory region 1, where the host maps blobs, at blob alignment.
class VioGpu3dHostVisible
{
  public:
    VioGpu3dHostVisible();
    ~VioGpu3dHostVisible();

    NTSTATUS Init(PHYSICAL_ADDRESS base, ULONGLONG size, ULONG alignment);
    void Close();
    BOOLEAN IsPresent() const
    {
        return m_Size != 0;
    }
    ULONG GetAlignment() const
    {
        return m_Alignment;
    }
    PHYSICAL_ADDRESS GetBase() const
    {
        return m_Base;
    }
    NTSTATUS Allocate(ULONGLONG size, _Out_ ULONGLONG *pOffset);
    void Free(ULONGLONG offset, ULONGLONG size);

  private:
    PHYSICAL_ADDRESS m_Base;
    ULONGLONG m_Size;
    ULONG m_Alignment;
    RTL_BITMAP m_Bitmap;
    PULONG m_pBits;
    FAST_MUTEX m_Lock;
};

#pragma pack(push, 1)
typedef struct
{
    UINT DriverStarted : 1;
    UINT HardwareInit : 1;
    UINT PointerEnabled : 1;
    UINT VgaDevice : 1;
    UINT FlexResolution : 1;
    UINT UsePhysicalMemory : 1;
    UINT Unused : 26;
} DRIVER_STATUS_FLAG;
#pragma pack(pop)

virtio_gpu_formats ColorFormat(UINT format);

class VioGpu3dAdapter final : IVioGpuPCI
{
  public:
    VioGpu3dAdapter(_In_ DEVICE_OBJECT *pPhysicalDeviceObject);
    ~VioGpu3dAdapter(void);

    // Shared with the display code taken from viogpu3d of pull request 943.
    VioGpuVidPN *vidpn;
    VioGpuIdr resourceIdr;
    VioGpuIdr ctxIdr;
    CtrlQueue ctrlQueue;
    VioGpuMemSegment frameSegment;
    UINT64 m_u64HostFeatures;
    UINT64 m_u64GuestFeatures;
    UINT32 m_u32NumCapsets;
    UINT32 m_u32NumScanouts;

#pragma code_seg(push)
#pragma code_seg()
    BOOLEAN IsDriverActive() const
    {
        return m_Flags.DriverStarted;
    }
    BOOLEAN IsHardwareInit() const
    {
        return m_Flags.HardwareInit;
    }
    void SetHardwareInit(BOOLEAN init)
    {
        m_Flags.HardwareInit = init;
    }
    BOOLEAN IsPointerEnabled() const
    {
        return m_Flags.PointerEnabled;
    }
    BOOLEAN IsVgaDevice(void) const
    {
        return m_Flags.VgaDevice;
    }
    BOOLEAN IsFlexResolution(void) const
    {
        return m_Flags.FlexResolution;
    }
    BOOLEAN IsUsePhysicalMemory() const
    {
        return m_Flags.UsePhysicalMemory;
    }
#pragma code_seg(pop)

    NTSTATUS StartDevice(_In_ DXGK_START_INFO *pDxgkStartInfo,
                         _In_ DXGKRNL_INTERFACE *pDxgkInterface,
                         _Out_ ULONG *pNumberOfViews,
                         _Out_ ULONG *pNumberOfChildren);
    NTSTATUS StopDevice(VOID);
    VOID ResetDevice(VOID);
    NTSTATUS SetPowerState(_In_ ULONG HardwareUid,
                           _In_ DEVICE_POWER_STATE DevicePowerState,
                           _In_ POWER_ACTION ActionType);
    NTSTATUS QueryChildRelations(_Out_writes_bytes_(ChildRelationsSize) DXGK_CHILD_DESCRIPTOR *pChildRelations,
                                 _In_ ULONG ChildRelationsSize);
    NTSTATUS QueryChildStatus(_Inout_ DXGK_CHILD_STATUS *pChildStatus, _In_ BOOLEAN NonDestructiveOnly);
    NTSTATUS QueryDeviceDescriptor(_In_ ULONG ChildUid, _Inout_ DXGK_DEVICE_DESCRIPTOR *pDeviceDescriptor);
    BOOLEAN InterruptRoutine(_In_ ULONG MessageNumber);
    VOID DpcRoutine(VOID);
    NTSTATUS QueryAdapterInfo(_In_ CONST DXGKARG_QUERYADAPTERINFO *pQueryAdapterInfo);
    NTSTATUS Escape(_In_ CONST DXGKARG_ESCAPE *pEscape);
    NTSTATUS StopDeviceAndReleasePostDisplayOwnership(_In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                                      _Out_ DXGK_DISPLAY_INFORMATION *pDisplayInfo);

    NTSTATUS CreateAllocation(_Inout_ DXGKARG_CREATEALLOCATION *pCreateAllocation);
    NTSTATUS DestroyAllocation(_In_ CONST DXGKARG_DESTROYALLOCATION *pDestroyAllocation);
    NTSTATUS GetStandardAllocationDriverData(_Inout_ DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *pStandard);
    NTSTATUS BuildPagingBuffer(_Inout_ DXGKARG_BUILDPAGINGBUFFER *pBuildPagingBuffer);

    NTSTATUS SubmitCommand(_In_ CONST DXGKARG_SUBMITCOMMAND *pSubmitCommand);
    NTSTATUS PreemptCommand(_In_ CONST DXGKARG_PREEMPTCOMMAND *pPreemptCommand);
    NTSTATUS QueryCurrentFence(_Inout_ DXGKARG_QUERYCURRENTFENCE *pCurrentFence);
    NTSTATUS QueryEngineStatus(_Inout_ DXGKARG_QUERYENGINESTATUS *pQueryEngineStatus);
    NTSTATUS ResetEngine(_Inout_ DXGKARG_RESETENGINE *pResetEngine);
    NTSTATUS ResetFromTimeout(VOID);
    NTSTATUS RestartFromTimeout(VOID);

    PDXGKRNL_INTERFACE GetDxgkInterface(void)
    {
        return &m_DxgkInterface;
    }
    CPciResources *GetPciResources(void)
    {
        return &m_PciResources;
    }
    BOOLEAN IsMSIEnabled()
    {
        return m_PciResources.IsMSIEnabled();
    }
    PHYSICAL_ADDRESS GetFrameBufferPA(void)
    {
        return m_PciResources.GetPciBar(0)->GetPA();
    }
    NTSTATUS NotifyInterrupt(DXGKARGCB_NOTIFY_INTERRUPT_DATA *interruptData, BOOL triggerDpc);

    BOOLEAN HasVenus() const
    {
        return m_HasVenus;
    }
    VioGpu3dHostVisible *GetHostVisible()
    {
        return &m_HostVisible;
    }
    ULONG GetBlobAlignment() const
    {
        return m_BlobAlignment;
    }
    const VIOGPU3D_CAPSET *FindCapset(UINT id) const;
    VioGpu3dAllocation *AllocationFromHandle(D3DKMT_HANDLE handle);

    // Submission tracking, from the scheduler to the device's fences and back.
    VIOGPU3D_DMA *BeginDma(UINT node, UINT fenceId);
    void EndDma(VIOGPU3D_DMA *pDma);
    static void DmaFenceSignaled(void *ctx);
    ULONGLONG NextFenceId()
    {
        return (ULONGLONG)InterlockedIncrement64((volatile LONG64 *)&m_NextFence);
    }
    VIOGPU3D_ESCAPE_FENCE *TrackEscapeFence(VioGpu3dDevice *pDevice, ULONGLONG fenceId);
    static void EscapeFenceSignaled(void *ctx);
    void WaitForEscapeFence(VioGpu3dDevice *pDevice, ULONGLONG fenceId, VIOGPU3D_DMA *pDma);
    void ForgetDevice(VioGpu3dDevice *pDevice);

  private:
    BOOLEAN CheckHardware();
    NTSTATUS HWInit(PCM_RESOURCE_LIST pResList);
    NTSTATUS HWClose(void);
    void StopWorkThread(void);
    NTSTATUS VioGpuAdapterInit();
    void VioGpuAdapterClose(void);
    NTSTATUS VirtIoDeviceInit(void);
    BOOLEAN AckFeature(UINT64 Feature);
    void QueryCapsets(void);
    NTSTATUS ReadRegistryDWORD(_In_ HANDLE hKey, _In_ PCWSTR pszwValueName, _Inout_ PDWORD pdwValue);
    void ReadSettings(void);

    static void ThreadWork(_In_ PVOID Context);
    void ThreadWorkRoutine(void);
    void ConfigChanged(void);
    NTSTATUS UpdateChildStatus(BOOLEAN connect);

    void CompleteNode(UINT node);
    void ReportCompletion(UINT node, UINT fenceId);
    void ReportPreemption(UINT node, UINT preemptionFenceId, UINT lastCompleted);

    NTSTATUS EscapeCapset(_Inout_ VIRTIO_WDDM_CAPSET *pCapset, UINT size);
    NTSTATUS EscapeResourceInfo(_Inout_ VIRTIO_WDDM_RESOURCE_INFO *pInfo);
    NTSTATUS EscapeBlobMap(_Inout_ VIRTIO_WDDM_BLOB_MAP *pMap);

    NTSTATUS QuerySegment4(_In_ CONST DXGKARG_QUERYADAPTERINFO *pQuery);

    DEVICE_OBJECT *m_pPhysicalDevice;
    DXGKRNL_INTERFACE m_DxgkInterface;
    DXGK_DEVICE_INFO m_DeviceInfo;
    LUID m_AdapterLuid;

    DEVICE_POWER_STATE m_AdapterPowerState;
    DRIVER_STATUS_FLAG m_Flags;

    VirtIODevice m_VioDev;
    CPciResources m_PciResources;
    CrsrQueue m_CursorQueue;
    VioGpuBuf m_GpuBuf;
    volatile ULONG m_PendingWorks;
    KEVENT m_ConfigUpdateEvent;
    PETHREAD m_pWorkThread;
    BOOLEAN m_bStopWorkThread;

    BOOLEAN m_HasVenus;
    ULONG m_BlobAlignment;
    VioGpu3dHostVisible m_HostVisible;
    VIOGPU3D_CAPSET m_Capsets[VIRTIO_GPU_MAX_CAPSET_ID + 1];
    UINT64 m_CapsetMask;

    // The page tables' segment: contiguous memory, as VidMm writes page tables through the CPU.
    PVOID m_pPageTables;
    PHYSICAL_ADDRESS m_PageTablesPA;

    KSPIN_LOCK m_NodeLock;
    VIOGPU3D_NODE_STATE m_Nodes[VIOGPU3D_NODE_COUNT];
    volatile LONG64 m_NextFence;
    LIST_ENTRY m_EscapeFences;
    UINT m_EscapeFenceCount;
};
