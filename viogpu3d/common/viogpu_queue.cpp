/*
 * Copyright (C) 2019-2020 Red Hat, Inc.
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

#include "viogpu_queue.h"
#include "baseobj.h"
#if !DBG
#include "viogpu_queue.tmh"
#endif

static BOOLEAN BuildSGElement(VirtIOBufferDescriptor *sg, PVOID buf, ULONG size)
{
    if (size != 0 && MmIsAddressValid(buf))
    {
        sg->length = min(size, PAGE_SIZE);
        sg->physAddr = MmGetPhysicalAddress(buf);
        return TRUE;
    }
    return FALSE;
}

// Splits a buffer of nonpaged memory at its page boundaries, as each page may lie anywhere in physical memory.
static BOOLEAN AppendSGElements(VirtIOBufferDescriptor *sg, UINT *count, UINT max, PVOID buf, ULONG size)
{
    PUCHAR p = (PUCHAR)buf;
    while (size != 0)
    {
        if (*count >= max || !MmIsAddressValid(p))
        {
            return FALSE;
        }
        ULONG chunk = min(size, (ULONG)(PAGE_SIZE - BYTE_OFFSET(p)));
        sg[*count].physAddr = MmGetPhysicalAddress(p);
        sg[*count].length = chunk;
        (*count)++;
        p += chunk;
        size -= chunk;
    }
    return TRUE;
}

static void NotifyEventCompleteCB(void *ctx)
{
    KeSetEvent((PKEVENT)ctx, IO_NO_INCREMENT, FALSE);
}

VioGpuQueue::VioGpuQueue()
{
    m_pBuf = NULL;
    m_Index = (UINT)-1;
    m_pVIODevice = NULL;
    m_pVirtQueue = NULL;
    KeInitializeSpinLock(&m_SpinLock);
}

VioGpuQueue::~VioGpuQueue()
{
    Close();
}

void VioGpuQueue::Close(void)
{
    KIRQL SavedIrql;
    Lock(&SavedIrql);
    m_pVirtQueue = NULL;
    Unlock(SavedIrql);
}

BOOLEAN VioGpuQueue::Init(_In_ VirtIODevice *pVIODevice, _In_ struct virtqueue *pVirtQueue, _In_ UINT index)
{
    if ((pVIODevice == NULL) || (pVirtQueue == NULL))
    {
        return FALSE;
    }
    m_pVIODevice = pVIODevice;
    m_pVirtQueue = pVirtQueue;
    m_Index = index;
    EnableInterrupt();
    return TRUE;
}

_IRQL_requires_max_(DISPATCH_LEVEL) _IRQL_saves_global_(OldIrql, Irql) _IRQL_raises_(DISPATCH_LEVEL) void VioGpuQueue::
                                                                                                    Lock(KIRQL *Irql)
{
    KIRQL SavedIrql = KeGetCurrentIrql();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s at IRQL %d\n", __FUNCTION__, SavedIrql));

    if (SavedIrql < DISPATCH_LEVEL)
    {
        KeAcquireSpinLock(&m_SpinLock, &SavedIrql);
    }
    else if (SavedIrql == DISPATCH_LEVEL)
    {
        KeAcquireSpinLockAtDpcLevel(&m_SpinLock);
    }
    else
    {
        // This is possible situation in case of bugcheck.
        // DxgkDdiSystemDisplayEnable can be called at any IRQL.
        // We need to allocate buffer for several command during this proccess.
        // VioGpuDbgBreak();
    }
    *Irql = SavedIrql;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

_IRQL_requires_(DISPATCH_LEVEL) _IRQL_restores_global_(OldIrql, Irql) void VioGpuQueue::Unlock(KIRQL Irql)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s at IRQL %d\n", __FUNCTION__, Irql));

    if (Irql < DISPATCH_LEVEL)
    {
        KeReleaseSpinLock(&m_SpinLock, Irql);
    }
    else if (Irql == DISPATCH_LEVEL)
    {
        KeReleaseSpinLockFromDpcLevel(&m_SpinLock);
    }
    else
    {
        // This is possible situation in case of bugcheck.
        // DxgkDdiSystemDisplayEnable can be called at any IRQL.
        // We need to allocate buffer for several command during this proccess.
        // VioGpuDbgBreak();
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

PAGED_CODE_SEG_BEGIN

UINT VioGpuQueue::QueryAllocation()
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    USHORT NumEntries;
    ULONG RingSize, HeapSize;

    NTSTATUS status = virtio_query_queue_allocation(m_pVIODevice, m_Index, &NumEntries, &RingSize, &HeapSize);
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("[%s] virtio_query_queue_allocation(%d) failed with error %x\n", __FUNCTION__, m_Index, status));
        return 0;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return NumEntries;
}
PAGED_CODE_SEG_END

PAGED_CODE_SEG_BEGIN

BOOLEAN CtrlQueue::GetDisplayInfo(PGPU_VBUFFER buf, UINT id, PULONG xres, PULONG yres)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_RESP_DISP_INFO resp = (PGPU_RESP_DISP_INFO)buf->resp_buf;
    if (resp->hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO)
    {
        DbgPrint(TRACE_LEVEL_VERBOSE, (" %s type = %x: disabled\n", __FUNCTION__, resp->hdr.type));
        return FALSE;
    }
    if (resp->pmodes[id].enabled)
    {
        DbgPrint(TRACE_LEVEL_VERBOSE,
                 ("output %d: %dx%d+%d+%d\n",
                  id,
                  resp->pmodes[id].r.width,
                  resp->pmodes[id].r.height,
                  resp->pmodes[id].r.x,
                  resp->pmodes[id].r.y));
        *xres = resp->pmodes[id].r.width;
        *yres = resp->pmodes[id].r.height;
    }
    else
    {
        DbgPrint(TRACE_LEVEL_VERBOSE, ("output %d: disabled\n", id));
        return FALSE;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return TRUE;
}

BOOLEAN CtrlQueue::AskDisplayInfo(PGPU_VBUFFER *buf)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_CTRL_HDR cmd;
    PGPU_VBUFFER vbuf;
    PGPU_RESP_DISP_INFO resp_buf;
    KEVENT event;
    NTSTATUS status;

    resp_buf = reinterpret_cast<PGPU_RESP_DISP_INFO>(new (NonPagedPoolNx) BYTE[sizeof(GPU_RESP_DISP_INFO)]);

    if (!resp_buf)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("---> %s Failed allocate %d bytes\n", __FUNCTION__, sizeof(GPU_RESP_DISP_INFO)));
        return FALSE;
    }

    cmd = (PGPU_CTRL_HDR)AllocCmdResp(&vbuf, sizeof(GPU_CTRL_HDR), resp_buf, sizeof(GPU_RESP_DISP_INFO));
    RtlZeroMemory(cmd, sizeof(GPU_CTRL_HDR));

    cmd->type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    vbuf->complete_cb = NotifyEventCompleteCB;
    vbuf->complete_ctx = &event;
    vbuf->auto_release = false;

    LARGE_INTEGER timeout = {0};
    timeout.QuadPart = Int32x32To64(1000, -10000);

    QueueBuffer(vbuf);
    status = KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, &timeout);

    if (status == STATUS_TIMEOUT)
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("---> Failed to ask display info\n"));
        VioGpuDbgBreak();
    }
    *buf = vbuf;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return TRUE;
}

BOOLEAN CtrlQueue::AskEdidInfo(PGPU_VBUFFER *buf, UINT id)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_CMD_GET_EDID cmd;
    PGPU_VBUFFER vbuf;
    PGPU_RESP_EDID resp_buf;
    KEVENT event;
    NTSTATUS status;

    resp_buf = reinterpret_cast<PGPU_RESP_EDID>(new (NonPagedPoolNx) BYTE[sizeof(GPU_RESP_EDID)]);

    if (!resp_buf)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("---> %s Failed allocate %d bytes\n", __FUNCTION__, sizeof(GPU_RESP_EDID)));
        return FALSE;
    }
    cmd = (PGPU_CMD_GET_EDID)AllocCmdResp(&vbuf, sizeof(GPU_CMD_GET_EDID), resp_buf, sizeof(GPU_RESP_EDID));
    RtlZeroMemory(cmd, sizeof(GPU_CMD_GET_EDID));

    cmd->hdr.type = VIRTIO_GPU_CMD_GET_EDID;
    cmd->scanout = id;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    vbuf->complete_cb = NotifyEventCompleteCB;
    vbuf->complete_ctx = &event;
    vbuf->auto_release = false;

    LARGE_INTEGER timeout = {0};
    timeout.QuadPart = Int32x32To64(1000, -10000);

    QueueBuffer(vbuf);

    status = KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, &timeout);

    if (status == STATUS_TIMEOUT)
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("---> Failed to get edid info\n"));
        VioGpuDbgBreak();
    }

    *buf = vbuf;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return TRUE;
}

BOOLEAN CtrlQueue::GetEdidInfo(PGPU_VBUFFER buf, UINT id, PBYTE edid)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    PGPU_CMD_GET_EDID cmd = (PGPU_CMD_GET_EDID)buf->buf;
    PGPU_RESP_EDID resp = (PGPU_RESP_EDID)buf->resp_buf;
    if (resp->hdr.type != VIRTIO_GPU_RESP_OK_EDID)
    {
        DbgPrint(TRACE_LEVEL_VERBOSE, (" %s type = %x: disabled\n", __FUNCTION__, resp->hdr.type));
        return FALSE;
    }
    if (cmd->scanout != id)
    {
        DbgPrint(TRACE_LEVEL_VERBOSE, (" %s invalid scaout = %x\n", __FUNCTION__, cmd->scanout));
        return FALSE;
    }

    // Each reply holds its own scanout's EDID from edid[0].
    ULONG size = min(resp->size, (ULONG)EDID_RAW_BLOCK_SIZE);
    if (size < EDID_V1_BLOCK_SIZE)
    {
        DbgPrint(TRACE_LEVEL_VERBOSE, (" %s short edid = %d bytes\n", __FUNCTION__, resp->size));
        return FALSE;
    }

    RtlZeroMemory(edid, EDID_RAW_BLOCK_SIZE);
    RtlCopyMemory(edid, resp->edid, size);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    return TRUE;
}

void CtrlQueue::CreateResource(UINT res_id, UINT format, UINT width, UINT height)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_RES_CREATE_2D cmd;
    PGPU_VBUFFER vbuf;
    cmd = (PGPU_RES_CREATE_2D)AllocCmd(&vbuf, sizeof(*cmd));
    RtlZeroMemory(cmd, sizeof(*cmd));

    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    cmd->resource_id = res_id;
    cmd->format = format;
    cmd->width = width;
    cmd->height = height;

    // FIXME!!! if
    QueueBuffer(vbuf);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

void CtrlQueue::ResFlush(UINT res_id, UINT width, UINT height, UINT x, UINT y)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    PGPU_RES_FLUSH cmd;
    PGPU_VBUFFER vbuf;
    cmd = (PGPU_RES_FLUSH)AllocCmd(&vbuf, sizeof(*cmd));
    RtlZeroMemory(cmd, sizeof(*cmd));

    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    cmd->resource_id = res_id;
    cmd->r.width = width;
    cmd->r.height = height;
    cmd->r.x = x;
    cmd->r.y = y;

    QueueBuffer(vbuf);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

void CtrlQueue::TransferToHost2D(UINT res_id, ULONG offset, UINT width, UINT height, UINT x, UINT y)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    PGPU_RES_TRANSF_TO_HOST_2D cmd;
    PGPU_VBUFFER vbuf;
    cmd = (PGPU_RES_TRANSF_TO_HOST_2D)AllocCmd(&vbuf, sizeof(*cmd));
    RtlZeroMemory(cmd, sizeof(*cmd));

    cmd->hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    cmd->resource_id = res_id;
    cmd->offset = offset;
    cmd->r.width = width;
    cmd->r.height = height;
    cmd->r.x = x;
    cmd->r.y = y;

    QueueBuffer(vbuf);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

void CtrlQueue::AttachBacking(UINT res_id, PGPU_MEM_ENTRY ents, UINT nents)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_RES_ATTACH_BACKING cmd;
    PGPU_VBUFFER vbuf;
    cmd = (PGPU_RES_ATTACH_BACKING)AllocCmd(&vbuf, sizeof(*cmd));
    RtlZeroMemory(cmd, sizeof(*cmd));

    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    cmd->resource_id = res_id;
    cmd->nr_entries = nents;

    vbuf->data_buf = ents;
    vbuf->data_size = sizeof(*ents) * nents;

    QueueBuffer(vbuf);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

PAGED_CODE_SEG_END

void CtrlQueue::DestroyResource(UINT res_id)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_RES_UNREF cmd;
    PGPU_VBUFFER vbuf;
    cmd = (PGPU_RES_UNREF)AllocCmd(&vbuf, sizeof(*cmd));
    RtlZeroMemory(cmd, sizeof(*cmd));

    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    cmd->resource_id = res_id;

    QueueBuffer(vbuf);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

void CtrlQueue::DetachBacking(UINT res_id)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_RES_DETACH_BACKING cmd;
    PGPU_VBUFFER vbuf;
    cmd = (PGPU_RES_DETACH_BACKING)AllocCmd(&vbuf, sizeof(*cmd));
    RtlZeroMemory(cmd, sizeof(*cmd));

    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING;
    cmd->resource_id = res_id;

    QueueBuffer(vbuf);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

PVOID CtrlQueue::AllocCmdResp(PGPU_VBUFFER *buf, int cmd_sz, PVOID resp_buf, int resp_sz)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_VBUFFER vbuf;
    vbuf = m_pBuf->GetBuf(cmd_sz, resp_sz, resp_buf);
    ASSERT(vbuf);
    *buf = vbuf;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return vbuf ? vbuf->buf : NULL;
}

PVOID CtrlQueue::AllocCmd(PGPU_VBUFFER *buf, int sz)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    if (buf == NULL || sz == 0)
    {
        return NULL;
    }

    PGPU_VBUFFER vbuf = m_pBuf->GetBuf(sz, sizeof(GPU_CTRL_HDR), NULL);
    ASSERT(vbuf);
    *buf = vbuf;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s  vbuf = %p\n", __FUNCTION__, vbuf));

    return vbuf ? vbuf->buf : NULL;
}

void CtrlQueue::SetScanout(UINT scan_id, UINT res_id, UINT width, UINT height, UINT x, UINT y)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_SET_SCANOUT cmd;
    PGPU_VBUFFER vbuf;
    cmd = (PGPU_SET_SCANOUT)AllocCmd(&vbuf, sizeof(*cmd));
    RtlZeroMemory(cmd, sizeof(*cmd));

    cmd->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    cmd->resource_id = res_id;
    cmd->scanout_id = scan_id;
    cmd->r.width = width;
    cmd->r.height = height;
    cmd->r.x = x;
    cmd->r.y = y;

    // FIXME if
    QueueBuffer(vbuf);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

#define SGLIST_SIZE 256
UINT CtrlQueue::QueueBuffer(PGPU_VBUFFER buf)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    VirtIOBufferDescriptor sg[SGLIST_SIZE];
    UINT outcnt = 0, incnt = 0;
    int ret = -1;
    KIRQL SavedIrql;

    if (!AppendSGElements(sg, &outcnt, SGLIST_SIZE, buf->buf, buf->size) ||
        !AppendSGElements(sg, &outcnt, SGLIST_SIZE, buf->data_buf, buf->data_size))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("<--> %s command of %d + %u bytes does not fit\n", __FUNCTION__, buf->size, buf->data_size));
        return (UINT)-1;
    }
    UINT total = outcnt;
    if (!AppendSGElements(sg, &total, SGLIST_SIZE, buf->resp_buf, buf->resp_size))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("<--> %s reply of %d bytes does not fit\n", __FUNCTION__, buf->resp_size));
        return (UINT)-1;
    }
    incnt = total - outcnt;

    // The device frees descriptors as it answers; at PASSIVE_LEVEL wait for that rather than drop the command.
    for (UINT attempt = 0; attempt < 1000; attempt++)
    {
        Lock(&SavedIrql);
        ret = AddBuf(&sg[0], outcnt, incnt, buf, NULL, 0);
        Kick();
        Unlock(SavedIrql);
        if (ret >= 0 || KeGetCurrentIrql() > PASSIVE_LEVEL)
        {
            break;
        }
        LARGE_INTEGER delay;
        delay.QuadPart = -10000LL; // 1 ms
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }

    if (ret < 0)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("<--> %s the ring stayed full\n", __FUNCTION__));
        return (UINT)-1;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s ret = %d\n", __FUNCTION__, ret));

    return 0;
}

PGPU_VBUFFER CtrlQueue::DequeueBuffer(_Out_ UINT *len)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_VBUFFER buf = NULL;
    KIRQL SavedIrql;
    Lock(&SavedIrql);
    buf = (PGPU_VBUFFER)GetBuf(len);
    Unlock(SavedIrql);
    if (buf == NULL)
    {
        *len = 0;
    }
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return buf;
}

void VioGpuQueue::ReleaseBuffer(PGPU_VBUFFER buf)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    m_pBuf->FreeBuf(buf);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

BOOLEAN VioGpuBuf::Init(_In_ UINT cnt)
{
    KIRQL OldIrql;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    m_uCountMin = cnt;

    for (UINT i = 0; i < cnt; ++i)
    {
        PGPU_VBUFFER pvbuf = reinterpret_cast<PGPU_VBUFFER>(new (NonPagedPoolNx) BYTE[VBUFFER_SIZE]);
        // FIXME
        RtlZeroMemory(pvbuf, VBUFFER_SIZE);
        if (pvbuf)
        {
            KeAcquireSpinLock(&m_SpinLock, &OldIrql);
            InsertTailList(&m_FreeBufs, &pvbuf->list_entry);
            ++m_uCount;
            KeReleaseSpinLock(&m_SpinLock, OldIrql);
        }
    }
    ASSERT(m_uCount == cnt);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return (m_uCount > 0);
}

void VioGpuBuf::Close(void)
{
    KIRQL OldIrql;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    KeAcquireSpinLock(&m_SpinLock, &OldIrql);
    while (!IsListEmpty(&m_InUseBufs))
    {
        LIST_ENTRY *pListItem = RemoveHeadList(&m_InUseBufs);
        if (pListItem)
        {
            PGPU_VBUFFER pvbuf = CONTAINING_RECORD(pListItem, GPU_VBUFFER, list_entry);
            ASSERT(pvbuf);
            ASSERT(pvbuf->resp_size <= MAX_INLINE_RESP_SIZE);

            delete[] reinterpret_cast<PBYTE>(pvbuf);
            --m_uCount;
        }
    }

    while (!IsListEmpty(&m_FreeBufs))
    {
        LIST_ENTRY *pListItem = RemoveHeadList(&m_FreeBufs);
        if (pListItem)
        {
            PGPU_VBUFFER pbuf = CONTAINING_RECORD(pListItem, GPU_VBUFFER, list_entry);
            ASSERT(pbuf);

            if (pbuf->resp_buf && pbuf->resp_size > MAX_INLINE_RESP_SIZE)
            {
                delete[] reinterpret_cast<PBYTE>(pbuf->resp_buf);
                pbuf->resp_buf = NULL;
                pbuf->resp_size = 0;
            }

            if (pbuf->data_buf && pbuf->data_size)
            {
                delete[] reinterpret_cast<PBYTE>(pbuf->data_buf);
                pbuf->data_buf = NULL;
                pbuf->data_size = 0;
            }

            delete[] reinterpret_cast<PBYTE>(pbuf);
            --m_uCount;
        }
    }
    KeReleaseSpinLock(&m_SpinLock, OldIrql);

    ASSERT(m_uCount == 0);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

PGPU_VBUFFER VioGpuBuf::GetBuf(_In_ int size, _In_ int resp_size, _In_opt_ void *resp_buf)
{

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_VBUFFER pbuf = NULL;
    PLIST_ENTRY pListItem = NULL;
    KIRQL SavedIrql = KeGetCurrentIrql();

    if (SavedIrql < DISPATCH_LEVEL)
    {
        KeAcquireSpinLock(&m_SpinLock, &SavedIrql);
    }
    else if (SavedIrql == DISPATCH_LEVEL)
    {
        KeAcquireSpinLockAtDpcLevel(&m_SpinLock);
    }
    else
    {
        // This is possible situation in case of bugcheck.
        // DxgkDdiSystemDisplayEnable can be called at any IRQL.
        // We need to allocate buffer for several command during this proccess.
        // VioGpuDbgBreak();
    }

    if (IsListEmpty(&m_FreeBufs))
    {
        pbuf = reinterpret_cast<PGPU_VBUFFER>(new (NonPagedPoolNx) BYTE[VBUFFER_SIZE]);
        ++m_uCount;
    }
    else
    {
        pListItem = RemoveHeadList(&m_FreeBufs);
        pbuf = CONTAINING_RECORD(pListItem, GPU_VBUFFER, list_entry);
    }

    ASSERT(pbuf);
    memset(pbuf, 0, VBUFFER_SIZE);
    ASSERT(size <= MAX_INLINE_CMD_SIZE);

    pbuf->buf = (char *)((ULONG_PTR)pbuf + sizeof(*pbuf));
    pbuf->size = size;
    pbuf->auto_release = true;

    pbuf->resp_size = resp_size;
    if (resp_size <= MAX_INLINE_RESP_SIZE)
    {
        pbuf->resp_buf = (char *)((ULONG_PTR)pbuf->buf + size);
    }
    else
    {
        pbuf->resp_buf = (char *)resp_buf;
    }
    ASSERT(pbuf->resp_buf);
    InsertTailList(&m_InUseBufs, &pbuf->list_entry);

    if (SavedIrql < DISPATCH_LEVEL)
    {
        KeReleaseSpinLock(&m_SpinLock, SavedIrql);
    }
    else if (SavedIrql == DISPATCH_LEVEL)
    {
        KeReleaseSpinLockFromDpcLevel(&m_SpinLock);
    }
    else
    {
        // This is possible situation in case of bugcheck.
        // DxgkDdiSystemDisplayEnable can be called at any IRQL.
        // We need to allocate buffer for several command during this proccess.
        // VioGpuDbgBreak();
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s buf = %p\n", __FUNCTION__, pbuf));

    return pbuf;
}

void VioGpuBuf::FreeBuf(_In_ PGPU_VBUFFER pbuf)
{
    KIRQL OldIrql;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s buf = %p\n", __FUNCTION__, pbuf));
    KeAcquireSpinLock(&m_SpinLock, &OldIrql);

    if (!IsListEmpty(&m_InUseBufs))
    {
        PLIST_ENTRY leCurrent = m_InUseBufs.Flink;
        PGPU_VBUFFER pvbuf = CONTAINING_RECORD(leCurrent, GPU_VBUFFER, list_entry);
        while (leCurrent && pvbuf)
        {
            if (pvbuf == pbuf)
            {
                RemoveEntryList(leCurrent);
                pvbuf = NULL;
                break;
            }

            leCurrent = leCurrent->Flink;
            if (leCurrent)
            {
                pvbuf = CONTAINING_RECORD(leCurrent, GPU_VBUFFER, list_entry);
            }
        }
    }
    if (pbuf->resp_buf && pbuf->resp_size > MAX_INLINE_RESP_SIZE)
    {
        delete[] reinterpret_cast<PBYTE>(pbuf->resp_buf);
        pbuf->resp_buf = NULL;
        pbuf->resp_size = 0;
    }

    if (pbuf->data_buf && pbuf->data_size)
    {
        delete[] reinterpret_cast<PBYTE>(pbuf->data_buf);
        pbuf->data_buf = NULL;
        pbuf->data_size = 0;
    }

    if (m_uCount > m_uCountMin)
    {
        delete[] reinterpret_cast<PBYTE>(pbuf);
        --m_uCount;
    }
    else
    {
        InsertTailList(&m_FreeBufs, &pbuf->list_entry);
    }

    KeReleaseSpinLock(&m_SpinLock, OldIrql);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

PAGED_CODE_SEG_BEGIN
VioGpuBuf::VioGpuBuf()
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    InitializeListHead(&m_FreeBufs);
    InitializeListHead(&m_InUseBufs);
    KeInitializeSpinLock(&m_SpinLock);
    m_uCount = 0;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

VioGpuBuf::~VioGpuBuf()
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_FATAL, ("---> %s 0x%p\n", __FUNCTION__, this));

    Close();

    DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s\n", __FUNCTION__));
}

VioGpuMemSegment::VioGpuMemSegment(void)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    m_pSGList = NULL;
    m_pVAddr = NULL;
    m_pMdl = NULL;
    m_bSystemMemory = FALSE;
    m_bMapped = FALSE;
    m_Size = 0;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

VioGpuMemSegment::~VioGpuMemSegment(void)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    Close();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

BOOLEAN VioGpuMemSegment::Init(_In_ UINT size, _In_opt_ PPHYSICAL_ADDRESS pPAddr)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    ASSERT(size);
    PVOID buf = NULL;
    UINT pages = BYTES_TO_PAGES(size);
    UINT sglsize = sizeof(SCATTER_GATHER_LIST) + (sizeof(SCATTER_GATHER_ELEMENT) * pages);
    size = pages * PAGE_SIZE;

    if ((pPAddr == NULL) || pPAddr->QuadPart == 0LL)
    {
        m_pVAddr = new (NonPagedPoolNx) BYTE[size];

        if (!m_pVAddr)
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("%s insufficient resources to allocate %x bytes\n", __FUNCTION__, size));
            return FALSE;
        }
        RtlZeroMemory(m_pVAddr, size);
        m_bSystemMemory = TRUE;
    }
    else
    {
        NTSTATUS Status = MapFrameBuffer(*pPAddr, size, &m_pVAddr);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s MapFrameBuffer failed with Status: 0x%X\n", __FUNCTION__, Status));
            return FALSE;
        }
        m_bMapped = TRUE;
    }

    m_pMdl = IoAllocateMdl(m_pVAddr, size, FALSE, FALSE, NULL);
    if (!m_pMdl)
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("%s insufficient resources to allocate MDLs\n", __FUNCTION__));
        return FALSE;
    }
    if (m_bSystemMemory == TRUE)
    {
        __try
        {
            MmProbeAndLockPages(m_pMdl, KernelMode, IoWriteAccess);
        }
        // clang-format off
#pragma prefast(suppress : __WARNING_EXCEPTIONEXECUTEHANDLER, "try/except is only able to protect against user-mode errors and these are the only errors we try to catch here");
        // clang-format on
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("%s Failed to lock pages with error %x\n", __FUNCTION__, GetExceptionCode()));
            IoFreeMdl(m_pMdl);
            return FALSE;
        }
    }
    m_pSGList = reinterpret_cast<PSCATTER_GATHER_LIST>(new (NonPagedPoolNx) BYTE[sglsize]);
    m_pSGList->NumberOfElements = 0;
    m_pSGList->Reserved = 0;
    //       m_pSAddr = reinterpret_cast<BYTE*>
    //    (MmGetSystemAddressForMdlSafe(m_pMdl, NormalPagePriority | MdlMappingNoExecute));

    RtlZeroMemory(m_pSGList, sglsize);
    buf = PAGE_ALIGN(m_pVAddr);

    for (UINT i = 0; i < pages; ++i)
    {
        PHYSICAL_ADDRESS pa = {0};
        ASSERT(MmIsAddressValid(buf));
        pa = MmGetPhysicalAddress(buf);
        if (pa.QuadPart == 0LL)
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("%s Invalid PA buf = %p element %d\n", __FUNCTION__, buf, i));
            break;
        }
        m_pSGList->Elements[i].Address = pa;
        m_pSGList->Elements[i].Length = PAGE_SIZE;
        buf = (PVOID)((LONG_PTR)(buf) + PAGE_SIZE);
        m_pSGList->NumberOfElements++;
    }
    m_Size = size;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return TRUE;
}

PHYSICAL_ADDRESS VioGpuMemSegment::GetPhysicalAddress(void)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PHYSICAL_ADDRESS pa = {0};
    if (m_pVAddr && MmIsAddressValid(m_pVAddr))
    {
        pa = MmGetPhysicalAddress(m_pVAddr);
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return pa;
}

void VioGpuMemSegment::Close(void)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    if (m_pMdl)
    {
        if (m_bSystemMemory)
        {
            MmUnlockPages(m_pMdl);
        }
        IoFreeMdl(m_pMdl);
        m_pMdl = NULL;
    }

    if (m_bSystemMemory)
    {
        delete[] reinterpret_cast<PBYTE>(m_pVAddr);
    }
    else
    {
        UnmapFrameBuffer(m_pVAddr, (ULONG)m_Size);
        m_bMapped = FALSE;
    }
    m_pVAddr = NULL;

    delete[] reinterpret_cast<PBYTE>(m_pSGList);
    m_pSGList = NULL;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

VioGpuObj::VioGpuObj(void)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    m_uiHwRes = 0;
    m_pSegment = NULL;
    m_Size = 0;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

VioGpuObj::~VioGpuObj(void)
{
    // Driver can destroy object in case of a bugcheck.
    // DxgkDdiSystemDisplayEnable can be called at any IRQL, so it must
    // be in nonpageable memory. DxgkDdiSystemDisplayEnable must not
    // call any code that is in pageable memory and must not manipulate
    // any data that is in pageable memory.
    // PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

BOOLEAN VioGpuObj::Init(_In_ UINT size, VioGpuMemSegment *pSegment)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s requested size = %d\n", __FUNCTION__, size));

    ASSERT(size);
    ASSERT(pSegment);
    UINT pages = BYTES_TO_PAGES(size);
    size = pages * PAGE_SIZE;
    if (size > pSegment->GetSize())
    {
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("<--- %s segment size too small = %Iu (%u)\n", __FUNCTION__, pSegment->GetSize(), size));
        return FALSE;
    }
    m_pSegment = pSegment;
    m_Size = size;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s size = %Iu\n", __FUNCTION__, m_Size));
    return TRUE;
}

PVOID CrsrQueue::AllocCursor(PGPU_VBUFFER *buf)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_VBUFFER vbuf;
    vbuf = m_pBuf->GetBuf(sizeof(GPU_UPDATE_CURSOR), 0, NULL);
    ASSERT(vbuf);
    *buf = vbuf;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s  vbuf = %p\n", __FUNCTION__, vbuf));

    return vbuf ? vbuf->buf : NULL;
}

PAGED_CODE_SEG_END

UINT CrsrQueue::QueueCursor(PGPU_VBUFFER buf)
{
    //    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    UINT res = 0;
    KIRQL SavedIrql;

    VirtIOBufferDescriptor sg[1];
    int outcnt = 0;
    UINT ret = 0;

    ASSERT(buf->size <= PAGE_SIZE);
    if (BuildSGElement(&sg[outcnt], (PVOID)buf->buf, buf->size))
    {
        outcnt++;
    }

    ASSERT(outcnt);
    Lock(&SavedIrql);
    ret = AddBuf(&sg[0], outcnt, 0, buf, NULL, 0);
    Kick();
    Unlock(SavedIrql);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s vbuf = %p outcnt = %d, ret = %d\n", __FUNCTION__, buf, outcnt, ret));
    return res;
}

PGPU_VBUFFER CrsrQueue::DequeueCursor(_Out_ UINT *len)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PGPU_VBUFFER buf = NULL;
    KIRQL SavedIrql;
    Lock(&SavedIrql);
    buf = (PGPU_VBUFFER)GetBuf(len);
    Unlock(SavedIrql);
    if (buf == NULL)
    {
        *len = 0;
    }
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s buf %p len = %u\n", __FUNCTION__, buf, *len));
    return buf;
}

static void WaitedBufferAnswered(void *ctx)
{
    PGPU_VBUFFER buf = (PGPU_VBUFFER)ctx;
    if (InterlockedCompareExchange(&buf->wait_state, 1, 0) == 2)
    {
        // The waiter is gone; the DPC releases the buffer after this returns.
        buf->auto_release = true;
        return;
    }
    KeSetEvent(&buf->wait_event, IO_NO_INCREMENT, FALSE);
}

BOOLEAN CtrlQueue::QueueBufferAndWait(PGPU_VBUFFER buf, ULONG timeoutMs)
{
    KeInitializeEvent(&buf->wait_event, NotificationEvent, FALSE);
    buf->wait_state = 0;
    buf->complete_cb = WaitedBufferAnswered;
    buf->complete_ctx = buf;
    buf->auto_release = false;

    if (QueueBuffer(buf) != 0)
    {
        ReleaseBuffer(buf);
        return FALSE;
    }

    LARGE_INTEGER timeout;
    timeout.QuadPart = Int32x32To64(timeoutMs, -10000);
    if (KeWaitForSingleObject(&buf->wait_event, Executive, KernelMode, FALSE, &timeout) == STATUS_TIMEOUT &&
        InterlockedCompareExchange(&buf->wait_state, 2, 0) == 0)
    {
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("<--> %s no reply to command %x\n", __FUNCTION__, ((PGPU_CTRL_HDR)buf->buf)->type));
        return FALSE;
    }
    // Answered, possibly just as the wait ended: the event is set or about to be.
    KeWaitForSingleObject(&buf->wait_event, Executive, KernelMode, FALSE, NULL);
    return TRUE;
}

static UINT ReplyType(PGPU_VBUFFER buf)
{
    return ((PGPU_CTRL_HDR)buf->resp_buf)->type;
}

BOOLEAN CtrlQueue::AskCapsetInfo(UINT index, _Out_ PGPU_RESP_CAPSET_INFO info)
{
    RtlZeroMemory(info, sizeof(*info));

    PGPU_VBUFFER vbuf;
    PGPU_RESP_CAPSET_INFO resp = reinterpret_cast<PGPU_RESP_CAPSET_INFO>(new (NonPagedPoolNx) BYTE[sizeof(
                                                                                                        GPU_RESP_CAPSET_INFO)]);
    if (!resp)
    {
        return FALSE;
    }
    RtlZeroMemory(resp, sizeof(*resp));
    PGPU_CMD_GET_CAPSET_INFO cmd = (PGPU_CMD_GET_CAPSET_INFO)AllocCmdResp(&vbuf, sizeof(*cmd), resp, sizeof(*resp));
    RtlZeroMemory(cmd, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_GET_CAPSET_INFO;
    cmd->capset_index = index;

    if (!QueueBufferAndWait(vbuf, 1000))
    {
        return FALSE;
    }
    BOOLEAN ok = ReplyType(vbuf) == VIRTIO_GPU_RESP_OK_CAPSET_INFO;
    if (ok)
    {
        *info = *resp;
    }
    ReleaseBuffer(vbuf);
    return ok;
}

BOOLEAN CtrlQueue::AskCapset(UINT capsetId, UINT version, UINT size, _Out_writes_bytes_(size) PVOID data)
{
    RtlZeroMemory(data, size);

    PGPU_VBUFFER vbuf;
    UINT respSize = sizeof(GPU_RESP_CAPSET) + size;
    PUCHAR resp = new (NonPagedPoolNx) BYTE[respSize];
    if (!resp)
    {
        return FALSE;
    }
    RtlZeroMemory(resp, respSize);
    PGPU_CMD_GET_CAPSET cmd = (PGPU_CMD_GET_CAPSET)AllocCmdResp(&vbuf, sizeof(*cmd), resp, respSize);
    RtlZeroMemory(cmd, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_GET_CAPSET;
    cmd->capset_id = capsetId;
    cmd->capset_version = version;

    if (!QueueBufferAndWait(vbuf, 1000))
    {
        return FALSE;
    }
    BOOLEAN ok = ReplyType(vbuf) == VIRTIO_GPU_RESP_OK_CAPSET;
    if (ok)
    {
        RtlCopyMemory(data, resp + sizeof(GPU_RESP_CAPSET), size);
    }
    ReleaseBuffer(vbuf);
    return ok;
}

UINT CtrlQueue::CreateContext(UINT ctxId, UINT capsetId, _In_reads_bytes_(nameLength) const CHAR *name, UINT nameLength)
{
    PGPU_VBUFFER vbuf;
    PGPU_CMD_CTX_CREATE cmd = (PGPU_CMD_CTX_CREATE)AllocCmd(&vbuf, sizeof(GPU_CMD_CTX_CREATE));
    RtlZeroMemory(cmd, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_CTX_CREATE;
    cmd->hdr.ctx_id = ctxId;
    cmd->context_init = capsetId & VIRTIO_GPU_CONTEXT_INIT_CAPSET_ID_MASK;
    cmd->nlen = min(nameLength, (UINT)VIRTIO_GPU_CONTEXT_NAME_SIZE);
    RtlCopyMemory(cmd->debug_name, name, cmd->nlen);

    if (!QueueBufferAndWait(vbuf, 1000))
    {
        return VIRTIO_GPU_RESP_ERR_UNSPEC;
    }
    UINT type = ReplyType(vbuf);
    ReleaseBuffer(vbuf);
    return type;
}

void CtrlQueue::DestroyContext(UINT ctxId)
{
    PGPU_VBUFFER vbuf;
    PGPU_CMD_CTX_DESTROY cmd = (PGPU_CMD_CTX_DESTROY)AllocCmd(&vbuf, sizeof(GPU_CMD_CTX_DESTROY));
    RtlZeroMemory(cmd, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_CTX_DESTROY;
    cmd->hdr.ctx_id = ctxId;
    if (QueueBuffer(vbuf) != 0)
    {
        ReleaseBuffer(vbuf);
    }
}

UINT CtrlQueue::AttachResourceToContext(UINT ctxId, UINT resId, BOOLEAN attach)
{
    PGPU_VBUFFER vbuf;
    PGPU_CMD_CTX_RESOURCE cmd = (PGPU_CMD_CTX_RESOURCE)AllocCmd(&vbuf, sizeof(GPU_CMD_CTX_RESOURCE));
    RtlZeroMemory(cmd, sizeof(*cmd));
    cmd->hdr.type = attach ? VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE : VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE;
    cmd->hdr.ctx_id = ctxId;
    cmd->resource_id = resId;
    UINT ret = QueueBuffer(vbuf);
    if (ret != 0)
    {
        ReleaseBuffer(vbuf);
    }
    return ret;
}

UINT CtrlQueue::CreateBlob(UINT ctxId,
                           UINT resId,
                           UINT blobMem,
                           UINT blobFlags,
                           ULONGLONG blobId,
                           ULONGLONG size,
                           _In_opt_ PGPU_MEM_ENTRY ents,
                           UINT nents)
{
    PGPU_VBUFFER vbuf;
    PGPU_CMD_RES_CREATE_BLOB cmd = (PGPU_CMD_RES_CREATE_BLOB)AllocCmd(&vbuf, sizeof(GPU_CMD_RES_CREATE_BLOB));
    RtlZeroMemory(cmd, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB;
    cmd->hdr.ctx_id = ctxId;
    cmd->resource_id = resId;
    cmd->blob_mem = blobMem;
    cmd->blob_flags = blobFlags;
    cmd->blob_id = blobId;
    cmd->size = size;
    cmd->nr_entries = ents ? nents : 0;
    if (ents && nents)
    {
        vbuf->data_buf = ents;
        vbuf->data_size = sizeof(*ents) * nents;
    }

    if (!QueueBufferAndWait(vbuf, 1000))
    {
        return VIRTIO_GPU_RESP_ERR_UNSPEC;
    }
    UINT type = ReplyType(vbuf);
    ReleaseBuffer(vbuf);
    return type;
}

UINT CtrlQueue::CreateResource3D(UINT ctxId, _In_ const GPU_CMD_RES_CREATE_3D *create)
{
    PGPU_VBUFFER vbuf;
    PGPU_CMD_RES_CREATE_3D cmd = (PGPU_CMD_RES_CREATE_3D)AllocCmd(&vbuf, sizeof(GPU_CMD_RES_CREATE_3D));
    *cmd = *create;
    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    cmd->hdr.flags = 0;
    cmd->hdr.fence_id = 0;
    cmd->hdr.ctx_id = ctxId;

    if (!QueueBufferAndWait(vbuf, 1000))
    {
        return VIRTIO_GPU_RESP_ERR_UNSPEC;
    }
    UINT type = ReplyType(vbuf);
    ReleaseBuffer(vbuf);
    return type;
}

UINT CtrlQueue::MapBlob(UINT ctxId, UINT resId, ULONGLONG offset)
{
    PGPU_VBUFFER vbuf;
    PGPU_RESP_MAP_INFO resp = reinterpret_cast<PGPU_RESP_MAP_INFO>(new (NonPagedPoolNx) BYTE[sizeof(GPU_RESP_MAP_INFO)]);
    if (!resp)
    {
        return 0;
    }
    RtlZeroMemory(resp, sizeof(*resp));
    PGPU_CMD_RES_MAP_BLOB cmd = (PGPU_CMD_RES_MAP_BLOB)AllocCmdResp(&vbuf, sizeof(*cmd), resp, sizeof(*resp));
    RtlZeroMemory(cmd, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB;
    cmd->hdr.ctx_id = ctxId;
    cmd->resource_id = resId;
    cmd->offset = offset;

    if (!QueueBufferAndWait(vbuf, 1000))
    {
        return 0;
    }
    UINT mapInfo = ReplyType(vbuf) == VIRTIO_GPU_RESP_OK_MAP_INFO ? (resp->map_info & VIRTIO_GPU_MAP_CACHE_MASK) : 0;
    ReleaseBuffer(vbuf);
    return mapInfo;
}

UINT CtrlQueue::UnmapBlob(UINT ctxId, UINT resId)
{
    PGPU_VBUFFER vbuf;
    PGPU_CMD_RES_UNMAP_BLOB cmd = (PGPU_CMD_RES_UNMAP_BLOB)AllocCmd(&vbuf, sizeof(GPU_CMD_RES_UNMAP_BLOB));
    RtlZeroMemory(cmd, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB;
    cmd->hdr.ctx_id = ctxId;
    cmd->resource_id = resId;

    // The host takes the memory out of the region before it answers; the range is free for another blob only then.
    if (!QueueBufferAndWait(vbuf, 1000))
    {
        return VIRTIO_GPU_RESP_ERR_UNSPEC;
    }
    UINT type = ReplyType(vbuf);
    ReleaseBuffer(vbuf);
    return type;
}

UINT CtrlQueue::Submit3D(UINT ctxId,
                         ULONGLONG fenceId,
                         BOOLEAN ring,
                         UCHAR ringIdx,
                         _In_ PVOID data,
                         UINT size,
                         void (*complete)(void *),
                         void *completeCtx)
{
    PGPU_VBUFFER vbuf;
    PGPU_CMD_SUBMIT cmd = (PGPU_CMD_SUBMIT)AllocCmd(&vbuf, sizeof(GPU_CMD_SUBMIT));
    RtlZeroMemory(cmd, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    cmd->hdr.ctx_id = ctxId;
    if (fenceId != 0)
    {
        cmd->hdr.flags = VIRTIO_GPU_FLAG_FENCE | (ring ? VIRTIO_GPU_FLAG_INFO_RING_IDX : 0);
        cmd->hdr.fence_id = fenceId;
        cmd->hdr.ring_idx = ring ? ringIdx : 0;
    }
    cmd->size = size;
    vbuf->data_buf = data;
    vbuf->data_size = size;
    vbuf->complete_cb = complete;
    vbuf->complete_ctx = completeCtx;
    UINT ret = QueueBuffer(vbuf);
    if (ret != 0)
    {
        ReleaseBuffer(vbuf);
    }
    return ret;
}

UINT CtrlQueue::Transfer3D(UINT ctxId,
                           BOOLEAN toHost,
                           _In_ const GPU_CMD_TRANSFER_HOST_3D *transfer,
                           ULONGLONG fenceId,
                           BOOLEAN ring,
                           UCHAR ringIdx,
                           void (*complete)(void *),
                           void *completeCtx)
{
    PGPU_VBUFFER vbuf;
    PGPU_CMD_TRANSFER_HOST_3D cmd = (PGPU_CMD_TRANSFER_HOST_3D)AllocCmd(&vbuf, sizeof(GPU_CMD_TRANSFER_HOST_3D));
    *cmd = *transfer;
    RtlZeroMemory(&cmd->hdr, sizeof(cmd->hdr));
    cmd->hdr.type = toHost ? VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D : VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
    cmd->hdr.ctx_id = ctxId;
    if (fenceId != 0)
    {
        cmd->hdr.flags = VIRTIO_GPU_FLAG_FENCE | (ring ? VIRTIO_GPU_FLAG_INFO_RING_IDX : 0);
        cmd->hdr.fence_id = fenceId;
        cmd->hdr.ring_idx = ring ? ringIdx : 0;
    }
    vbuf->complete_cb = complete;
    vbuf->complete_ctx = completeCtx;
    UINT ret = QueueBuffer(vbuf);
    if (ret != 0)
    {
        ReleaseBuffer(vbuf);
    }
    return ret;
}

UINT CtrlQueue::SetScanoutBlob(_In_ const GPU_CMD_SET_SCANOUT_BLOB *scanout)
{
    PGPU_VBUFFER vbuf;
    PGPU_CMD_SET_SCANOUT_BLOB cmd = (PGPU_CMD_SET_SCANOUT_BLOB)AllocCmd(&vbuf, sizeof(GPU_CMD_SET_SCANOUT_BLOB));
    *cmd = *scanout;
    RtlZeroMemory(&cmd->hdr, sizeof(cmd->hdr));
    cmd->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT_BLOB;
    UINT ret = QueueBuffer(vbuf);
    if (ret != 0)
    {
        ReleaseBuffer(vbuf);
    }
    return ret;
}
