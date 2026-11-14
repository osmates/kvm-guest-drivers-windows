/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * D3D12 DDI command queues + fence operations: create, execute and the
 * fence entries all forward to the inner device's queue.  Ordering
 * against the app's fences is not established here but by the
 * monitored-fence gate below.
 */

#include "triton12.h"
#include "triton_log.h"

static VOID APIENTRY t12DestroyCommandQueue(D3D12DDI_HDEVICE hDevice,
                                            D3D12DDI_HCOMMANDQUEUE hQueue);

static D3D12_COMMAND_LIST_TYPE
t12QueueFlagsToApiType(D3D12DDI_COMMAND_QUEUE_FLAGS Flags)
{
    /* A DIRECT queue carries 3D|COMPUTE|COPY (0x7) -- test 3D FIRST.
     * Checking COMPUTE first made the app's direct queue a compute
     * queue on the inner device, and the recorded clear/draw work then
     * hits the host's compute ring and hangs the GPU.  Copies are legal
     * on a compute queue, so a copy-only workload hides the mistake. */
    if (Flags & D3D12DDI_COMMAND_QUEUE_FLAG_3D)
        return D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (Flags & D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE)
        return D3D12_COMMAND_LIST_TYPE_COMPUTE;
    if (Flags & D3D12DDI_COMMAND_QUEUE_FLAG_COPY)
        return D3D12_COMMAND_LIST_TYPE_COPY;
    return D3D12_COMMAND_LIST_TYPE_DIRECT;
}

static SIZE_T APIENTRY
t12CalcPrivateCommandQueueSize(D3D12DDI_HDEVICE hDevice,
                               const D3D12DDIARG_CREATECOMMANDQUEUE_0001 *pArgs)
{
    (void)hDevice; (void)pArgs;
    return sizeof(TRITON12_QUEUE);
}

static HRESULT APIENTRY
t12CreateCommandQueue(D3D12DDI_HDEVICE hDevice,
                      const D3D12DDIARG_CREATECOMMANDQUEUE_0001 *pArgs);

static HRESULT
t12CreateCommandQueueWithPriority(D3D12DDI_HDEVICE hDevice,
                                  const D3D12DDIARG_CREATECOMMANDQUEUE_0001 *pArgs,
                                  D3D12_COMMAND_QUEUE_PRIORITY priority)
{
    PTRITON12_DEVICE p = triton12Device(hDevice);
    PTRITON12_QUEUE q = pArgs ? (PTRITON12_QUEUE)pArgs->hDrvCommandQueue.pDrvPrivate : NULL;
    if (!p || !p->pDev || !q)
        return E_INVALIDARG;
    memset(q, 0, sizeof(*q));
    q->pDev = p;

    D3D12_COMMAND_QUEUE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.Type = t12QueueFlagsToApiType(pArgs->QueueFlags);
    desc.Priority = priority;
    HRESULT hr = ID3D12Device_CreateCommandQueue(
        p->pDev, &desc, &IID_ID3D12CommandQueue, (void **)&q->pQueue);
    TR_LOG_HR(hr, "12.CreateCommandQueue: flags=0x%x priority=%d -> 0x%08lx",
           (unsigned)pArgs->QueueFlags, (int)priority, (unsigned long)hr);
    if (FAILED(hr))
        return hr;

    /* Per-queue kernel context: the runtime binds the context to
     * hRTCommandQueue and runs its kernel fence ops against it.  The
     * KMD accepts a zero-private-data DX12 virtual context (kmprobe). */
    q->hRTCommandQueue = pArgs->hRTCommandQueue;
    if (p->pUMCallbacks && p->pUMCallbacks->pfnCreateContextVirtualCb) {
        D3DDDICB_CREATECONTEXTVIRTUAL ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.NodeOrdinal = 0;
        ctx.EngineAffinity = 1;
        HRESULT chr = p->pUMCallbacks->pfnCreateContextVirtualCb(
            pArgs->hRTCommandQueue, &ctx);
        TR_LOG("12.CreateCommandQueue: CreateContextVirtualCb -> 0x%08lx "
               "hCtx=%p", (unsigned long)chr, ctx.hContext);
        if (FAILED(chr)) {
            ID3D12CommandQueue_Release(q->pQueue);
            q->pQueue = NULL;
            return chr;
        }
        q->hKMContext = ctx.hContext;
    }

    /* Per-queue drain fence for the monitored-fence gates (see
     * t12ExecuteCommandLists).  A queue whose fences can't be gated would
     * complete every app fence at CPU speed -- fail the create loudly
     * instead. */
    hr = ID3D12Device_CreateFence(p->pDev, 0, D3D12_FENCE_FLAG_NONE,
                                  &IID_ID3D12Fence, (void **)&q->pDrainFence);
    if (FAILED(hr)) {
        TR_LOG("12.CreateCommandQueue: drain-fence create FAILED 0x%08lx",
               (unsigned long)hr);
        t12DestroyCommandQueue(hDevice, pArgs->hDrvCommandQueue);
        return hr;
    }
    q->DrainValue = 0;
    /* The backend's half of the gate.  Same rule as the drain fence: a
     * queue whose kernel context nothing holds back would complete every
     * app fence at CPU speed. */
    hr = triton12_backend_create_queue(p, q);
    if (FAILED(hr)) {
        TR_LOG("12.CreateCommandQueue: backend gate create FAILED 0x%08lx",
               (unsigned long)hr);
        t12DestroyCommandQueue(hDevice, pArgs->hDrvCommandQueue);
        return hr;
    }
    /* 1 ms slice timer for the CPU waits on the drain fence (see
     * triton12.h).  Not fatal: without it the wait falls back to Sleep(1)
     * at the process timer resolution. */
    q->hPresentPaceTimer = CreateWaitableTimerExW(NULL, NULL,
                                                  CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                  TIMER_ALL_ACCESS);
    if (!q->hPresentPaceTimer)
        TR_LOG("12.CreateCommandQueue: high-res pace timer create FAILED %lu "
               "(present wait at event/timer-resolution granularity)",
               (unsigned long)GetLastError());

    /* Join the device's queue registry so later ECLs on OTHER queues can order
     * themselves behind this one (t12OrderAgainstSiblings).  Not fatal if the
     * registry is full: the queue simply is not ordered against, which is
     * today's behaviour. */
    q->Slot = TRITON12_MAX_QUEUES;
    if (!p->QueueLockInit)
        return S_OK;   /* device create failed to init the lock; stay unordered */
    EnterCriticalSection(&p->QueueLock);
    if (p->QueueCount < TRITON12_MAX_QUEUES) {
        q->Slot = p->QueueCount;
        /* Siblings may hold a high-water mark for this slot from a queue
         * that left it; this queue's drain values restart at 1. */
        for (UINT j = 0; j < p->QueueCount; j++) {
            PTRITON12_QUEUE s = (PTRITON12_QUEUE)p->Queues[j];
            if (s)
                s->XQueueSeen[q->Slot] = 0;
        }
        p->Queues[p->QueueCount++] = (struct TRITON12_QUEUE *)q;
    }
    LeaveCriticalSection(&p->QueueLock);
    if (q->Slot >= TRITON12_MAX_QUEUES)
        TR_LOG("12.CreateCommandQueue: queue registry full (%u); this queue is "
               "NOT cross-queue ordered", TRITON12_MAX_QUEUES);
    return S_OK;
}

static VOID APIENTRY
t12DestroyCommandQueue(D3D12DDI_HDEVICE hDevice, D3D12DDI_HCOMMANDQUEUE hQueue)
{
    PTRITON12_DEVICE p = triton12Device(hDevice);
    PTRITON12_QUEUE q = (PTRITON12_QUEUE)hQueue.pDrvPrivate;
    if (!q)
        return;

    /* Leave the registry before anything is released: a concurrent ECL on a
     * sibling walks Queues[] and touches pDrainFence.  Compact by moving the
     * tail entry down and fixing its Slot, so slots stay dense and every
     * queue's XQueueSeen[] indices remain valid. */
    if (p && p->QueueLockInit) {
        EnterCriticalSection(&p->QueueLock);
        for (UINT i = 0; i < p->QueueCount; i++) {
            if (p->Queues[i] != (struct TRITON12_QUEUE *)q)
                continue;
            PTRITON12_QUEUE moved = (PTRITON12_QUEUE)p->Queues[p->QueueCount - 1];
            p->Queues[i] = (struct TRITON12_QUEUE *)moved;
            p->Queues[--p->QueueCount] = NULL;
            if (moved && moved != q) {
                moved->Slot = i;
                /* The slot now means a different queue, so every sibling's
                 * high-water mark for it is meaningless -- clear them or a
                 * stale mark would suppress real waits. */
                for (UINT j = 0; j < p->QueueCount; j++) {
                    PTRITON12_QUEUE s = (PTRITON12_QUEUE)p->Queues[j];
                    if (s)
                        s->XQueueSeen[i] = 0;
                }
            }
            break;
        }
        LeaveCriticalSection(&p->QueueLock);
    }

    /* The backend's gate names the inner queue and the kernel context;
     * it goes before either. */
    if (p)
        triton12_backend_destroy_queue(p, q);

    if (q->hKMContext && p && p->pUMCallbacks &&
        p->pUMCallbacks->pfnDestroyContextCb) {
        D3DDDICB_DESTROYCONTEXT dc;
        memset(&dc, 0, sizeof(dc));
        dc.hContext = q->hKMContext;
        p->pUMCallbacks->pfnDestroyContextCb(q->hRTCommandQueue, &dc);
        q->hKMContext = NULL;
    }
    if (q->pCopyList) {
        ID3D12GraphicsCommandList_Release(q->pCopyList);
        q->pCopyList = NULL;
    }
    for (UINT ci = 0; ci < TRITON12_COPY_RING; ci++) {
        if (q->pCopyAlloc[ci]) {
            ID3D12CommandAllocator_Release(q->pCopyAlloc[ci]);
            q->pCopyAlloc[ci] = NULL;
        }
    }
    if (q->pDrainFence) {
        ID3D12Fence_Release(q->pDrainFence);
        q->pDrainFence = NULL;
    }
    if (q->pQueue) {
        ID3D12CommandQueue_Release(q->pQueue);
        q->pQueue = NULL;
    }
    if (q->hPresentPaceTimer) {
        CloseHandle(q->hPresentPaceTimer);
        q->hPresentPaceTimer = NULL;
    }
}

/* ---------- queue table (D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001) ---------- */

/*
 * Monitored-fence gate, once per ECL batch.  The runtime never calls
 * pfnSignalFence/pfnWaitForFence on this driver: app fences are serviced
 * by dxgkrnl monitored-fence packets on the queue's kernel context,
 * which complete once the context's prior work completes.  A context
 * carrying no work therefore retires every fence at CPU speed while the
 * GPU work is still running on the inner queue, and the app reads
 * results that do not exist yet.
 *
 * The gate restores the WDDM contract: (1) Signal the per-queue drain
 * fence on the inner queue -- FIFO-after the ECL, so it completes after
 * the batch, and the companion allocators and sibling queues key on it;
 * (2) have the backend hold the queue's kernel context until the GPU has
 * finished everything submitted to the inner queue so far
 * (triton12_backend_gate_queue), so dxgkrnl's fence packets (and CPU
 * waiters/GetCompletedValue) only observe values the GPU truly reached.
 */
BOOL
triton12QueueGate(PTRITON12_QUEUE q)
{
    if (!q->pDrainFence || !q->hKMContext || !q->pDev) {
        static LONG miss;
        if (miss < 4) { InterlockedIncrement(&miss);
            TR_LOG("12.QueueGate: UNGATED queue %p (no fence/ctx)", (void *)q); }
        return FALSE;
    }

    UINT64 v;
    HRESULT hr = t12DrainSignal(q, &v);
    if (FAILED(hr)) {
        TR_LOG("12.QueueGate: drain Signal FAILED 0x%08lx v=%llu",
               (unsigned long)hr, (unsigned long long)v);
        return FALSE;
    }

    if (!triton12_backend_gate_queue(q)) {
        static LONG gateFail;
        if (gateFail < 8) { InterlockedIncrement(&gateFail);
            TR_LOG("12.QueueGate: backend gate FAILED v=%llu -- fences will "
                   "run EARLY for this batch", (unsigned long long)v); }
        return FALSE;
    }
    static LONG gateOk;
    if (gateOk < 8) { InterlockedIncrement(&gateOk);
        TR_LOG("12.QueueGate: GATED v=%llu", (unsigned long long)v); }
    return TRUE;
}

/*
 * Cross-queue submission ordering.
 *
 * An app's ID3D12CommandQueue::Wait(fence, v) never reaches this driver: the
 * runtime services D3D12 fences through dxgkrnl monitored-fence packets and
 * never calls pfnWaitForFence, so t12WaitForFence exists only in case some
 * runtime one day does.  On real hardware that costs nothing, because the GPU
 * work itself travels in kernel DMA packets that the scheduler holds until
 * the wait clears.  Here it does not: t12ExecuteCommandLists forwards the
 * batch to the host over the ring, in user mode, immediately, and dxgkrnl
 * parks only the kernel packets.  The host is left running the app's DIRECT
 * and COMPUTE queues on two independent host queues with no ordering at all,
 * so a consumer pass samples a surface its producer is still writing.
 *
 * Before forwarding a batch on queue Q, therefore, make Q's inner queue
 * Wait on every sibling queue's drain fence at that sibling's current value.
 * The drain fences already exist and are already signalled once per batch by
 * t12QueueGate, so this adds no objects: it turns "submitted earlier on
 * another queue" into a real ordering edge on the host timeline.  A per-queue
 * high-water mark per sibling keeps it to one wait per sibling per advance.
 *
 * This enforces SUBMISSION order, not the app's waits, since the waits are
 * not observable here at all.  It is correct only when the producer's
 * ExecuteCommandLists precedes the consumer's -- the ordinary pattern.  An
 * app that submits the consumer first and relies on a later Wait still races.
 * Closing that needs execution ordering routed through the kernel the way the
 * gate already routes completion: a DMA packet whose KMD handler releases a
 * host-side start fence, so host execution order follows dxgkrnl's scheduling
 * order.
 *
 * It also serialises async compute against graphics, so the overlap is lost.
 * The backend's TRITON_DEBUG_NO_D3D12_XQUEUE_ORDER switch turns it off. */
static BOOL
t12XQueueOrderEnabled(void)
{
    return !triton_backend_debug(TRITON_DEBUG_NO_D3D12_XQUEUE_ORDER);
}

static void
t12OrderAgainstSiblings(PTRITON12_QUEUE q)
{
    PTRITON12_DEVICE p = q->pDev;
    if (!p || !p->QueueLockInit || !t12XQueueOrderEnabled())
        return;

    /* Snapshot under the lock, then issue the waits outside it:
     * ID3D12CommandQueue_Wait goes to the host over the wire, and holding a
     * lock across that would serialise every queue in the process on it. */
    ID3D12Fence *fences[TRITON12_MAX_QUEUES];
    UINT64 values[TRITON12_MAX_QUEUES];
    UINT slots[TRITON12_MAX_QUEUES];
    UINT n = 0;

    EnterCriticalSection(&p->QueueLock);
    for (UINT i = 0; i < p->QueueCount; i++) {
        PTRITON12_QUEUE s = (PTRITON12_QUEUE)p->Queues[i];
        if (!s || s == q || !s->pDrainFence)
            continue;
        /* A stale (lower) value only means a weaker edge this time, never
         * a wrong one.  DrainVisible carries only values whose Signal is
         * already encoded into the wire ring (t12DrainSignal), so waiting
         * on it cannot deadlock. */
        const UINT64 v = t12Load64(&s->DrainVisible);
        if (v == 0 || s->Slot >= TRITON12_MAX_QUEUES ||
            v <= q->XQueueSeen[s->Slot])
            continue;
        q->XQueueSeen[s->Slot] = v;
        /* Held across the Wait below: the sibling may be destroyed once
         * the lock is dropped. */
        ID3D12Fence_AddRef(s->pDrainFence);
        fences[n] = s->pDrainFence;
        values[n] = v;
        slots[n]  = s->Slot;
        n++;
    }
    LeaveCriticalSection(&p->QueueLock);

    for (UINT i = 0; i < n; i++) {
        HRESULT hr = ID3D12CommandQueue_Wait(q->pQueue, fences[i], values[i]);
        static LONG s_n;
        LONG ln = InterlockedIncrement(&s_n);
        if (ln <= 8 || (ln & 1023) == 0)
            TR_LOG("12.XQueueOrder #%ld: queue %u waits on queue %u drain "
                   "v=%llu -> 0x%08lx", (long)ln, q->Slot, slots[i],
                   (unsigned long long)values[i], (unsigned long)hr);
        ID3D12Fence_Release(fences[i]);
    }
}

static VOID APIENTRY
t12ExecuteCommandLists(D3D12DDI_HCOMMANDQUEUE hQueue, UINT Count,
                       const D3D12DDI_HCOMMANDLIST *pCommandLists)
{
    PTRITON12_QUEUE q = (PTRITON12_QUEUE)hQueue.pDrvPrivate;
    if (!q || !q->pQueue || !Count || !pCommandLists)
        return;

    /* Before the batch reaches the host, not after: the wait has to be on the
     * host queue's timeline ahead of this batch's work. */
    t12OrderAgainstSiblings(q);

    /* Forwarded in stack-sized chunks: a heap allocation that could fail
     * would drop the whole batch. */
    ID3D12CommandList *lists[64];
    UINT n = 0, total = 0;
    for (UINT i = 0; i < Count; i++) {
        PTRITON12_LIST l = (PTRITON12_LIST)pCommandLists[i].pDrvPrivate;
        if (l && l->pList)
            lists[n++] = (ID3D12CommandList *)l->pList;
        if (n == ARRAYSIZE(lists) || (i + 1 == Count && n)) {
            ID3D12CommandQueue_ExecuteCommandLists(q->pQueue, n, lists);
            total += n;
            n = 0;
        }
    }
    TR_LOG_HOT("12.ExecuteCommandLists: %u list(s)", total);
    if (total)
        triton12QueueGate(q);
}

static void APIENTRY
t12SignalFence(D3D12DDI_HCOMMANDQUEUE hQueue, D3D12DDIARG_FENCE_OPERATION *pOp)
{
    PTRITON12_QUEUE q = (PTRITON12_QUEUE)hQueue.pDrvPrivate;
    PTRITON12_FENCE f = pOp ? (PTRITON12_FENCE)pOp->Fence.pDrvPrivate : NULL;
    if (!q || !q->pQueue || !f || !f->pFence)
        return;

    /* Forward to the inner queue so the host GPU timeline carries the
     * signal.  No CPU wait here: the runtime services app fences through
     * dxgkrnl monitored-fence packets on the queue's kernel context,
     * which the per-ECL gate packets (t12QueueGate) hold to real GPU
     * completion.  (In practice the runtime never calls this DDI on this
     * driver -- kernel packets carry the fence model -- but the forward
     * keeps the host timeline correct if it ever does.) */
    HRESULT hr = ID3D12CommandQueue_Signal(q->pQueue, f->pFence, pOp->Value);
    TR_LOG_HOT("12.SignalFence: v=%llu -> 0x%08lx",
           (unsigned long long)pOp->Value, (unsigned long)hr);
    (void)hr;
}

static void APIENTRY
t12WaitForFence(D3D12DDI_HCOMMANDQUEUE hQueue, D3D12DDIARG_FENCE_OPERATION *pOp)
{
    PTRITON12_QUEUE q = (PTRITON12_QUEUE)hQueue.pDrvPrivate;
    PTRITON12_FENCE f = pOp ? (PTRITON12_FENCE)pOp->Fence.pDrvPrivate : NULL;
    if (!q || !q->pQueue || !f || !f->pFence)
        return;
    HRESULT hr = ID3D12CommandQueue_Wait(q->pQueue, f->pFence, pOp->Value);
    TR_LOG_HOT("12.WaitForFence: v=%llu -> 0x%08lx",
           (unsigned long long)pOp->Value, (unsigned long)hr);
    (void)hr; /* only consumed by TR_LOG_HOT, which compiles out by default */
}

static HRESULT APIENTRY
t12CreateCommandQueue(D3D12DDI_HDEVICE hDevice,
                      const D3D12DDIARG_CREATECOMMANDQUEUE_0001 *pArgs)
{
    return t12CreateCommandQueueWithPriority(
        hDevice, pArgs, D3D12_COMMAND_QUEUE_PRIORITY_NORMAL);
}

/* _0023 added QueueCreationFlags: GLOBAL_REALTIME_PRIORITY is only
 * requested for the queue types GetCaps(UMD_BASED_COMMAND_QUEUE_PRIORITY)
 * admitted, and becomes the host queue's priority. */
static D3D12_COMMAND_QUEUE_PRIORITY
t12QueueCreationPriority(D3D12DDI_COMMAND_QUEUE_CREATION_FLAGS flags)
{
    return (flags & D3D12DDI_COMMAND_QUEUE_CREATION_FLAG_GLOBAL_REALTIME_PRIORITY)
               ? D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME
               : D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
}

/* _0023 and _0050 move the two queue handles out of the arg struct;
 * _0050 adds a scheduling group, which HARDWARE_SCHEDULING_CAPS reports as
 * never used (ComputeQueuesPer3DQueue = 0), so it is always null. */
#define T12_QUEUE_CREATE_VARIANT(ver)                                          \
static SIZE_T APIENTRY                                                         \
t12CalcPrivateCommandQueueSize##ver(D3D12DDI_HDEVICE hDevice,                  \
                                    const D3D12DDIARG_CREATECOMMANDQUEUE_##ver *pArgs) \
{                                                                              \
    (void)hDevice; (void)pArgs;                                                \
    return sizeof(TRITON12_QUEUE);                                             \
}                                                                              \
static HRESULT APIENTRY                                                        \
t12CreateCommandQueue##ver(D3D12DDI_HDEVICE hDevice,                           \
                           const D3D12DDIARG_CREATECOMMANDQUEUE_##ver *pArgs,  \
                           D3D12DDI_HCOMMANDQUEUE hDrvCommandQueue,            \
                           D3D12DDI_HRTCOMMANDQUEUE hRTCommandQueue)           \
{                                                                              \
    if (!pArgs)                                                                \
        return E_INVALIDARG;                                                   \
    D3D12DDIARG_CREATECOMMANDQUEUE_0001 a;                                     \
    memset(&a, 0, sizeof(a));                                                  \
    a.hDrvCommandQueue = hDrvCommandQueue;                                     \
    a.hRTCommandQueue  = hRTCommandQueue;                                      \
    a.QueueFlags       = pArgs->QueueFlags;                                    \
    a.NodeMask         = pArgs->NodeMask;                                      \
    return t12CreateCommandQueueWithPriority(                                  \
        hDevice, &a, t12QueueCreationPriority(pArgs->QueueCreationFlags));     \
}

T12_QUEUE_CREATE_VARIANT(0023)
T12_QUEUE_CREATE_VARIANT(0050)

void
triton12InstallQueueDeviceFuncs0062(D3D12DDI_DEVICE_FUNCS_CORE_0062 *t)
{
    t->pfnCalcPrivateCommandQueueSize = t12CalcPrivateCommandQueueSize0050;
    t->pfnCreateCommandQueue          = t12CreateCommandQueue0050;
}

void
triton12InstallQueueDeviceFuncs0033(D3D12DDI_DEVICE_FUNCS_CORE_0033 *t)
{
    t->pfnCalcPrivateCommandQueueSize = t12CalcPrivateCommandQueueSize0023;
    t->pfnCreateCommandQueue          = t12CreateCommandQueue0023;
}

void
triton12InstallQueueDeviceFuncs(D3D12DDI_DEVICE_FUNCS_CORE_0022 *t)
{
    t->pfnCalcPrivateCommandQueueSize = t12CalcPrivateCommandQueueSize;
    t->pfnCreateCommandQueue          = t12CreateCommandQueue;
    t->pfnDestroyCommandQueue         = t12DestroyCommandQueue;
}

/* Tile mappings.  A reserved resource the host backs sparsely
 * (r->TiledHost, see tritonResource12.c) gets UpdateTileMappings and
 * CopyTileMappings forwarded: the host maps sparse-heap tiles for the
 * named regions and unmapped tiles read zero.  A resource on the
 * committed-backing shim is fully backed at create, so for it these are
 * deliberate no-ops.
 *
 * The DDI structs are layout-identical to the API ones (the SDK's public
 * {X,Y,Z,Subresource} / {NumTiles,UseBox,Width,Height,Depth} PODs and a
 * UINT flag enum), so the arrays pass through by cast. */
C_ASSERT(sizeof(D3D12DDI_TILED_RESOURCE_COORDINATE) == sizeof(D3D12_TILED_RESOURCE_COORDINATE));
C_ASSERT(sizeof(D3D12DDI_TILE_REGION_SIZE) == sizeof(D3D12_TILE_REGION_SIZE));
C_ASSERT(sizeof(D3D12DDI_TILE_RANGE_FLAGS) == sizeof(D3D12_TILE_RANGE_FLAGS));

static VOID APIENTRY
t12UpdateTileMappings(D3D12DDI_HCOMMANDQUEUE hQueue, D3D12DDI_HRESOURCE hRes,
                      UINT NumRegions,
                      const D3D12DDI_TILED_RESOURCE_COORDINATE *pCoords,
                      const D3D12DDI_TILE_REGION_SIZE *pSizes,
                      D3D12DDI_HHEAP hPool, UINT NumRanges,
                      const D3D12DDI_TILE_RANGE_FLAGS *pRangeFlags,
                      const UINT *pHeapStartOffsets,
                      const UINT *pRangeTileCounts,
                      D3D12DDI_TILE_MAPPING_FLAGS Flags)
{
    PTRITON12_RESOURCE r = (PTRITON12_RESOURCE)hRes.pDrvPrivate;
    PTRITON12_HEAP h = (PTRITON12_HEAP)hPool.pDrvPrivate;
    PTRITON12_QUEUE q = (PTRITON12_QUEUE)hQueue.pDrvPrivate;
    if (r && r->TiledHost && r->pResource && q && q->pQueue) {
        static LONG onceF;
        if (!InterlockedExchange(&onceF, 1))
            TR_LOG("12.UpdateTileMappings: forwarding to the host (sparse "
                   "backing; regions=%u ranges=%u)", NumRegions, NumRanges);
        ID3D12CommandQueue_UpdateTileMappings(
            q->pQueue, r->pResource, NumRegions,
            (const D3D12_TILED_RESOURCE_COORDINATE *)pCoords,
            (const D3D12_TILE_REGION_SIZE *)pSizes,
            (h && h->pHeap) ? h->pHeap : NULL, NumRanges,
            (const D3D12_TILE_RANGE_FLAGS *)pRangeFlags,
            pHeapStartOffsets, pRangeTileCounts,
            (D3D12_TILE_MAPPING_FLAGS)Flags);
        return;
    }
    static LONG once;
    if (!InterlockedExchange(&once, 1))
        TR_LOG("12.UpdateTileMappings: no-op (committed-backing shim; "
               "regions=%u ranges=%u)", NumRegions, NumRanges);
}

static VOID APIENTRY
t12CopyTileMappings(D3D12DDI_HCOMMANDQUEUE hQueue, D3D12DDI_HRESOURCE hDst,
                    const D3D12DDI_TILED_RESOURCE_COORDINATE *pDstCoord,
                    D3D12DDI_HRESOURCE hSrc,
                    const D3D12DDI_TILED_RESOURCE_COORDINATE *pSrcCoord,
                    const D3D12DDI_TILE_REGION_SIZE *pSize,
                    D3D12DDI_TILE_MAPPING_FLAGS Flags)
{
    PTRITON12_RESOURCE d = (PTRITON12_RESOURCE)hDst.pDrvPrivate;
    PTRITON12_RESOURCE s = (PTRITON12_RESOURCE)hSrc.pDrvPrivate;
    PTRITON12_QUEUE q = (PTRITON12_QUEUE)hQueue.pDrvPrivate;
    if (d && d->TiledHost && s && s->TiledHost && q && q->pQueue) {
        static LONG onceF;
        if (!InterlockedExchange(&onceF, 1))
            TR_LOG("12.CopyTileMappings: forwarding to the host (sparse backing)");
        ID3D12CommandQueue_CopyTileMappings(
            q->pQueue, d->pResource,
            (const D3D12_TILED_RESOURCE_COORDINATE *)pDstCoord, s->pResource,
            (const D3D12_TILED_RESOURCE_COORDINATE *)pSrcCoord,
            (const D3D12_TILE_REGION_SIZE *)pSize,
            (D3D12_TILE_MAPPING_FLAGS)Flags);
        return;
    }
    static LONG once;
    if (!InterlockedExchange(&once, 1))
        TR_LOG("12.CopyTileMappings: no-op (committed-backing shim)");
}

void
triton12InstallQueueFuncs(D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *t)
{
    t->pfnExecuteCommandLists = t12ExecuteCommandLists;
    t->pfnSignalFence         = t12SignalFence;
    t->pfnWaitForFence        = t12WaitForFence;
    t->pfnUpdateTileMappings  = t12UpdateTileMappings;
    t->pfnCopyTileMappings    = t12CopyTileMappings;
}
