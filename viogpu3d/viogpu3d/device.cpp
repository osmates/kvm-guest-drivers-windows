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
#include "device.tmh"
#endif

PAGED_CODE_SEG_BEGIN

VioGpu3dDevice::VioGpu3dDevice(VioGpu3dAdapter *adapter)
{
    PAGED_CODE();

    m_pAdapter = adapter;
    m_Process = PsGetCurrentProcess();
    m_CtxId = 0;
    m_CapsetId = 0;
    m_NumRings = 0;
}

VioGpu3dDevice::~VioGpu3dDevice()
{
    PAGED_CODE();

    m_pAdapter->ForgetDevice(this);
    if (m_CtxId != 0)
    {
        m_pAdapter->ctrlQueue.DestroyContext(m_CtxId);
        m_pAdapter->ctxIdr.PutId(m_CtxId);
        m_CtxId = 0;
    }
}

NTSTATUS VioGpu3dDevice::InitContext(_In_ const VIRTIO_WDDM_CONTEXT_INIT *pInit)
{
    PAGED_CODE();

    if (m_CtxId != 0)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (m_pAdapter->FindCapset(pInit->CapsetId) == NULL ||
        (pInit->CapsetId == VIRTIO_WDDM_CAPSET_ID_VENUS && !m_pAdapter->HasVenus()))
    {
        return STATUS_NOT_SUPPORTED;
    }

    UINT ctxId = m_pAdapter->ctxIdr.GetId();
    if (ctxId == 0)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    UINT nameLength = 0;
    while (nameLength < sizeof(pInit->DebugName) && pInit->DebugName[nameLength] != 0)
    {
        nameLength++;
    }
    UINT reply = m_pAdapter->ctrlQueue.CreateContext(ctxId,
                                                     pInit->CapsetId,
                                                     (const CHAR *)pInit->DebugName,
                                                     nameLength);
    if (reply != VIRTIO_GPU_RESP_OK_NODATA)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s context of capset %u refused with %x\n", __FUNCTION__, pInit->CapsetId, reply));
        m_pAdapter->ctxIdr.PutId(ctxId);
        return STATUS_UNSUCCESSFUL;
    }
    m_CtxId = ctxId;
    m_CapsetId = pInit->CapsetId;
    m_NumRings = (pInit->NumRings == 0 || pInit->NumRings > VIOGPU3D_RING_COUNT) ? VIOGPU3D_RING_COUNT
                                                                                 : pInit->NumRings;
    return STATUS_SUCCESS;
}

VioGpu3dContext::VioGpu3dContext(VioGpu3dDevice *device, UINT node, DXGK_CREATECONTEXTFLAGS flags)
{
    PAGED_CODE();

    m_pDevice = device;
    m_Node = node;
    m_Flags = flags;
}

// One command to the device's context. A fenced one holds pDma until the device signals it.
NTSTATUS VioGpu3dContext::SendCommand(_In_ const VIRTIO_WDDM_COMMAND_HEADER *pHeader,
                                      _In_reads_bytes_(pHeader->Size) const UCHAR *pBody,
                                      _In_opt_ VIOGPU3D_DMA *pDma,
                                      _Out_opt_ ULONGLONG *pFenceId)
{
    PAGED_CODE();

    VioGpu3dAdapter *pAdapter = m_pDevice->GetAdapter();
    UINT ctxId = m_pDevice->GetCtxId();
    BOOLEAN ring = (pHeader->Flags & VIRTIO_WDDM_COMMAND_FLAG_RING_IDX) != 0;
    if (ctxId == 0 || (ring && pHeader->Ring >= m_pDevice->GetRingCount()))
    {
        return STATUS_INVALID_PARAMETER;
    }

    switch (pHeader->Id)
    {
        case VIRTIO_WDDM_COMMAND_ID_NOP:
            return STATUS_SUCCESS;
        case VIRTIO_WDDM_COMMAND_ID_SUBMIT:
            {
                if (pHeader->Size == 0)
                {
                    return STATUS_SUCCESS;
                }
                PUCHAR data = new (NonPagedPoolNx) UCHAR[pHeader->Size];
                if (data == NULL)
                {
                    return STATUS_NO_MEMORY;
                }
                RtlCopyMemory(data, pBody, pHeader->Size);
                ULONGLONG fenceId = pAdapter->NextFenceId();
                void (*complete)(void *) = NULL;
                void *completeCtx = NULL;
                VIOGPU3D_ESCAPE_FENCE *pEscapeFence = NULL;
                if (pDma != NULL)
                {
                    InterlockedIncrement(&pDma->Outstanding);
                    complete = VioGpu3dAdapter::DmaFenceSignaled;
                    completeCtx = pDma;
                }
                else
                {
                    pEscapeFence = pAdapter->TrackEscapeFence(m_pDevice, fenceId);
                    if (pEscapeFence == NULL)
                    {
                        delete[] data;
                        return STATUS_NO_MEMORY;
                    }
                    complete = VioGpu3dAdapter::EscapeFenceSignaled;
                    completeCtx = pEscapeFence;
                }
                if (pAdapter->ctrlQueue.Submit3D(ctxId,
                                                 fenceId,
                                                 ring,
                                                 pHeader->Ring,
                                                 data,
                                                 pHeader->Size,
                                                 complete,
                                                 completeCtx) != 0)
                {
                    // The queue let go of data; the reference the fence held goes, or the record is signalled.
                    complete(completeCtx);
                    return STATUS_UNSUCCESSFUL;
                }
                if (pFenceId != NULL)
                {
                    *pFenceId = fenceId;
                }
                return STATUS_SUCCESS;
            }
        case VIRTIO_WDDM_COMMAND_ID_FENCE:
            {
                if (pDma == NULL || pHeader->Size < sizeof(UINT64))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                pAdapter->WaitForEscapeFence(m_pDevice, *(const UINT64 UNALIGNED *)pBody, pDma);
                return STATUS_SUCCESS;
            }
        case VIRTIO_WDDM_COMMAND_ID_TRANSFER_TO_HOST:
        case VIRTIO_WDDM_COMMAND_ID_TRANSFER_FROM_HOST:
            {
                if (pDma == NULL || pHeader->Size % sizeof(VIRTIO_WDDM_COMMAND_TRANSFER) != 0)
                {
                    return STATUS_INVALID_PARAMETER;
                }
                const VIRTIO_WDDM_COMMAND_TRANSFER *pTransfer = (const VIRTIO_WDDM_COMMAND_TRANSFER *)pBody;
                UINT count = pHeader->Size / sizeof(VIRTIO_WDDM_COMMAND_TRANSFER);
                for (UINT i = 0; i < count; i++)
                {
                    GPU_CMD_TRANSFER_HOST_3D transfer = {};
                    transfer.resource_id = pTransfer[i].ResId;
                    transfer.stride = pTransfer[i].Stride;
                    transfer.offset = pTransfer[i].Offset;
                    transfer.level = pTransfer[i].Level;
                    transfer.layer_stride = pTransfer[i].LayerStride;
                    transfer.box.x = pTransfer[i].Box.X;
                    transfer.box.y = pTransfer[i].Box.Y;
                    transfer.box.z = pTransfer[i].Box.Z;
                    transfer.box.width = pTransfer[i].Box.Width;
                    transfer.box.height = pTransfer[i].Box.Height;
                    transfer.box.depth = pTransfer[i].Box.Depth;
                    InterlockedIncrement(&pDma->Outstanding);
                    if (pAdapter->ctrlQueue.Transfer3D(ctxId,
                                                       pHeader->Id == VIRTIO_WDDM_COMMAND_ID_TRANSFER_TO_HOST,
                                                       &transfer,
                                                       pAdapter->NextFenceId(),
                                                       ring,
                                                       pHeader->Ring,
                                                       VioGpu3dAdapter::DmaFenceSignaled,
                                                       pDma) != 0)
                    {
                        pAdapter->EndDma(pDma);
                        return STATUS_UNSUCCESSFUL;
                    }
                }
                return STATUS_SUCCESS;
            }
        default:
            // Aperture maps are the driver's own; anything else is not a command.
            return STATUS_INVALID_PARAMETER;
    }
}

// The private data is the runtime's kernel copy, at most what CreateContext said: a tag, then command headers, each
// followed by its body.
NTSTATUS VioGpu3dContext::SubmitVirtual(_In_ const DXGKARG_SUBMITCOMMANDVIRTUAL *pSubmit)
{
    PAGED_CODE();

    VioGpu3dAdapter *pAdapter = m_pDevice->GetAdapter();
    VIOGPU3D_DMA *pDma = pAdapter->BeginDma(m_Node, pSubmit->SubmissionFenceId);
    if (pDma == NULL)
    {
        return STATUS_NO_MEMORY;
    }

    const UCHAR *p = (const UCHAR *)pSubmit->pDmaBufferPrivateData;
    // The buffer is as large as CreateContext asked; the user-mode driver filled this much of it.
    UINT size = pSubmit->DmaBufferPrivateDataSize;
    if (pSubmit->DmaBufferUmdPrivateDataSize != 0)
    {
        size = min(size, pSubmit->DmaBufferUmdPrivateDataSize);
    }
    NTSTATUS status = STATUS_SUCCESS;
    if (p != NULL && size >= sizeof(VIRTIO_WDDM_SUBMIT_COMMAND) &&
        ((const VIRTIO_WDDM_SUBMIT_COMMAND *)p)->Tag == VIRTIO_WDDM_SUBMIT_COMMAND_VIRTUAL_TAG)
    {
        UINT offset = sizeof(VIRTIO_WDDM_SUBMIT_COMMAND);
        while (offset + sizeof(VIRTIO_WDDM_COMMAND_HEADER) <= size)
        {
            const VIRTIO_WDDM_COMMAND_HEADER *pHeader = (const VIRTIO_WDDM_COMMAND_HEADER *)(p + offset);
            offset += sizeof(VIRTIO_WDDM_COMMAND_HEADER);
            if (pHeader->Size > size - offset)
            {
                status = STATUS_INVALID_PARAMETER;
                break;
            }
            status = SendCommand(pHeader, p + offset, pDma, NULL);
            if (!NT_SUCCESS(status))
            {
                break;
            }
            offset += pHeader->Size;
        }
    }
    if (!NT_SUCCESS(status))
    {
        // The rest of a bad buffer is dropped; the buffer still completes, so that the context goes on.
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s submission %u on node %u: %x\n", __FUNCTION__, pSubmit->SubmissionFenceId, m_Node, status));
    }
    pAdapter->EndDma(pDma);
    return STATUS_SUCCESS;
}

// Sends a command stream at once, outside the scheduler; the fence that comes back is what a later FENCE command of a
// submission waits for.
NTSTATUS VioGpu3dContext::ExecBuffer(_Inout_ VIRTIO_WDDM_EXEC_BUFFER *pExec, UINT size)
{
    PAGED_CODE();

    if (size < sizeof(VIRTIO_WDDM_EXEC_BUFFER) + sizeof(VIRTIO_WDDM_COMMAND_HEADER))
    {
        return STATUS_INVALID_BUFFER_SIZE;
    }
    const VIRTIO_WDDM_COMMAND_HEADER *pHeader = (const VIRTIO_WDDM_COMMAND_HEADER *)(pExec + 1);
    UINT room = size - sizeof(VIRTIO_WDDM_EXEC_BUFFER) - sizeof(VIRTIO_WDDM_COMMAND_HEADER);
    if (pHeader->Id != VIRTIO_WDDM_COMMAND_ID_SUBMIT || pHeader->Size > room)
    {
        return STATUS_INVALID_PARAMETER;
    }
    ULONGLONG fenceId = 0;
    NTSTATUS status = SendCommand(pHeader, (const UCHAR *)(pHeader + 1), NULL, &fenceId);
    pExec->FenceId = fenceId;
    return status;
}

PAGED_CODE_SEG_END
