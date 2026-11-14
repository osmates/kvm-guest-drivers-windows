/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * D3D12 present path.  The runtime performs the kernel
 * present itself; our list-table pfnPresent fills D3D12DDI_PRESENT_0003
 * with the backbuffer's shared KM allocation and the presenting
 * queue's kernel context.  Presentable committed resources (PRIMARY
 * heap flag) and present-copy companions are the backend's shared
 * surfaces (triton12_backend_create_shared_surface), whose memory is the
 * KM allocation.
 */

#include "triton12.h"
#include "triton_log.h"

#include "tritonPresentTiming.h"

void
triton12DeallocateKM(PTRITON12_DEVICE p, D3DKMT_HANDLE *phAllocation)
{
    if (!p || !phAllocation || !*phAllocation)
        return;
    if (p->pUMCallbacks && p->pUMCallbacks->pfnDeallocateCb) {
        D3D12DDICB_DEALLOCATE_0022 da;
        memset(&da, 0, sizeof(da));
        da.NumAllocations = 1;
        da.HandleList     = phAllocation;
        HRESULT hr = p->pUMCallbacks->pfnDeallocateCb(p->hRTDevice, &da);
        if (FAILED(hr))
            TR_LOG("12.dealloc: pfnDeallocateCb(alloc=0x%x) failed 0x%08lx",
                   *phAllocation, (unsigned long)hr);
    }
    *phAllocation = 0;
}

/* The backend's shared surface stands in for r->pResource (primaries and
 * the direct-share fallback) or for its companion; either way its KM
 * allocation replaces a residency-only placeholder. */
static HRESULT
t12MakeSharedSurface(PTRITON12_DEVICE p, PTRITON12_RESOURCE r,
                     const D3D12_RESOURCE_DESC *desc, BOOL primary,
                     ID3D12Resource **ppSurface)
{
    D3DKMT_HANDLE alloc = 0;
    HRESULT hr = triton12_backend_create_shared_surface(
        p, r, desc, primary ? TRITON12_SHARED_SURFACE_PRIMARY : 0, ppSurface,
        &alloc);
    if (FAILED(hr)) {
        TR_LOG("12.shared: backend create FAILED 0x%08lx %llux%u fmt=%d "
               "primary=%d", (unsigned long)hr,
               (unsigned long long)desc->Width, desc->Height,
               (int)desc->Format, primary);
        return hr;
    }
    if (r->ResidencyOnlyAlloc) {
        triton12DeallocateKM(p, &r->hKMAllocation);
        r->ResidencyOnlyAlloc = FALSE;
    }
    r->hKMAllocation = alloc;
    TR_LOG_HOT("12.shared: alloc=0x%x %llux%u primary=%d", alloc,
               (unsigned long long)desc->Width, desc->Height, primary);
    return S_OK;
}

HRESULT
triton12CreateSharedResource(PTRITON12_DEVICE p, PTRITON12_RESOURCE r,
                             const D3D12_RESOURCE_DESC *desc, BOOL primary)
{
    if (!p || !r || !desc || r->pResource)
        return E_INVALIDARG;
    return t12MakeSharedSurface(p, r, desc, primary, &r->pResource);
}

/* Present-copy companion for a native simple-2D texture (a render target
 * at create, a plain texture at its first present): a second texture with
 * the same desc, single-sample and laid out as the backend shares it,
 * whose memory backs the KM allocation registered against the GAME
 * resource's runtime handle.  Its only writer is the present's copy
 * (t12Present), which relies on implicit state promotion: the first copy
 * promotes it to COPY_DEST, and it stays there, since a texture written
 * on a direct queue does not decay back to COMMON. */
HRESULT
triton12CreateCompanion(PTRITON12_DEVICE p, PTRITON12_RESOURCE r,
                        const D3D12_HEAP_PROPERTIES *props,
                        const D3D12_RESOURCE_DESC *desc)
{
    (void)props;
    D3D12_RESOURCE_DESC cd = *desc;
    cd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    cd.SampleDesc.Count = 1;
    cd.SampleDesc.Quality = 0;
    if (r->pCompanion)
        return S_OK;
    HRESULT hr = t12MakeSharedSurface(p, r, &cd, FALSE, &r->pCompanion);
    if (FAILED(hr)) {
        r->pCompanion = NULL;
        return hr;
    }
    TR_LOG_HOT("12.CreateCompanion: %llux%u fmt=%d alloc=0x%x",
           (unsigned long long)desc->Width, desc->Height, (int)desc->Format,
           r->hKMAllocation);
    return S_OK;
}

/* ---------- residency-only KM allocation ---------- */

D3DKMT_HANDLE
triton12AllocResidencyOnly(PTRITON12_DEVICE p, HANDLE hRTResource)
{
    if (!p || !hRTResource)
        return 0;
    D3DKMT_HANDLE alloc = triton12_backend_allocate_empty(p, hRTResource);
    if (!alloc)
        TR_LOG("12.residency: the backend made no memory-less allocation");
    return alloc;
}

BOOL
triton12RegisterResidencyAlloc(PTRITON12_DEVICE p, PTRITON12_RESOURCE r)
{
    if (!p || !r || r->hKMAllocation)
        return FALSE;
    r->hKMAllocation = triton12AllocResidencyOnly(p, r->hRTResource.handle);
    if (!r->hKMAllocation)
        return FALSE;
    r->ResidencyOnlyAlloc = TRUE;
    return TRUE;
}

/* ---------- companions made on demand ---------- */

/* Give src a companion where it has no shared memory of its own: the
 * backend cannot share a texture's memory after the fact, so what a
 * presenter or the DWM bind consumes is a companion the present copies
 * into.  The companion takes the same shape rule as the create path's --
 * one plane -- and is single-sample: a multisampled source (a D3D9 MSAA
 * back buffer) is resolved into it, so its format must support the
 * resolve.  A ROW_MAJOR source is left alone: CopyResource needs both
 * resources the same size, which a linear source and an opaque companion
 * need not be.  A shape the rule rejects, or a create the backend
 * refuses, is latched so it costs one attempt rather than a create per
 * frame; any other failure is retried at the next call. */
static void
t12EnsureCompanion(PTRITON12_DEVICE p, PTRITON12_RESOURCE src)
{
    if (!p || !src || !src->pResource || src->pCompanion ||
        src->CompanionFailed ||
        (src->hKMAllocation && !src->ResidencyOnlyAlloc))
        return;
    D3D12_RESOURCE_DESC desc;
    src->pResource->lpVtbl->GetDesc(src->pResource, &desc);
    HRESULT hr = E_INVALIDARG;
    BOOL resolvable = desc.SampleDesc.Count <= 1;
    if (!resolvable) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT fs;
        memset(&fs, 0, sizeof(fs));
        fs.Format = desc.Format;
        resolvable = SUCCEEDED(ID3D12Device_CheckFeatureSupport(
                         p->pDev, D3D12_FEATURE_FORMAT_SUPPORT, &fs,
                         sizeof(fs))) &&
                     (fs.Support1 & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE);
    }
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        desc.MipLevels <= 1 && desc.DepthOrArraySize <= 1 &&
        resolvable && desc.Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR) {
        D3D12_HEAP_PROPERTIES props;
        memset(&props, 0, sizeof(props));
        props.Type = D3D12_HEAP_TYPE_DEFAULT;
        hr = triton12CreateCompanion(p, src, &props, &desc);
    }
    if (hr == E_INVALIDARG || hr == E_NOTIMPL) {
        src->CompanionFailed = TRUE;
        TR_LOG("12.companion: none for %llux%u fmt=%d dim=%d mips=%u "
               "array=%u samples=%u layout=%d; it presents nothing",
               (unsigned long long)desc.Width, desc.Height, (int)desc.Format,
               (int)desc.Dimension, (unsigned)desc.MipLevels,
               (unsigned)desc.DepthOrArraySize,
               (unsigned)desc.SampleDesc.Count, (int)desc.Layout);
    }
}

/* No DDI flag marks swapchain backbuffers at create time (they arrive
 * as plain RT textures, heapFlags=0x20 resFlags=0x11).  The runtime
 * asks the driver for a resource's kernel allocation through
 * pfnCheckResourceAllocationHandle when it needs one (CreateSharedHandle
 * for the DWM bind) -- create the companion lazily there, so ordinary
 * resources stay unshared. */
static D3DKMT_HANDLE APIENTRY
t12CheckResourceAllocationHandle(D3D12DDI_HDEVICE hDevice,
                                 D3D10DDI_HRESOURCE hResource)
{
    PTRITON12_DEVICE p = triton12Device(hDevice);
    PTRITON12_RESOURCE r = (PTRITON12_RESOURCE)hResource.pDrvPrivate;
    if (!p || !r)
        return 0;
    t12EnsureCompanion(p, r);
    /* A placeholder names no memory: report no allocation, so a swapchain
     * create fails cleanly instead of presenting it. */
    if (r->ResidencyOnlyAlloc)
        return 0;
    TR_LOG_HOT("12.CheckResourceAllocationHandle: r=%p -> 0x%x", (void *)r,
           r->hKMAllocation);
    return r->hKMAllocation;
}

static UINT APIENTRY
t12GetPresentPrivateDriverDataSize(D3D12DDI_HDEVICE hDevice,
                                   const D3D12DDIARG_PRESENT_0001 *pArgs)
{
    /* No per-present private driver data; the KMD present path reads
     * everything it needs from the allocation. */
    (void)hDevice; (void)pArgs;
    return 0;
}

void
triton12InstallPresentDeviceFuncs(D3D12DDI_DEVICE_FUNCS_CORE_0022 *t)
{
    t->pfnCheckResourceAllocationHandle = t12CheckResourceAllocationHandle;
    t->pfnGetPresentPrivateDriverDataSize = t12GetPresentPrivateDriverDataSize;
}

/* ---------- list-table pfnPresent ---------- */

/* Present-stage timing (the backend's TRITON_DEBUG_PRESENT_TIMING),
 * reported every TRITON12_PT_REPORT presents: gate (drain Signal plus the
 * backend's gate), wait (the CPU poll where no gate held the context),
 * plus how many presents the gate covered, how many hit the deadline, and
 * the largest single wait.  The clock and the enable are shared with the
 * D3D11 path in tritonPresentTiming.h. */
#define TRITON12_PT_REPORT 256

static struct {
    LONG64 n, gated, timeouts, gate_us, wait_us, wait_max_us;
} g_pt12;

static void
t12PtAccount(double gate, double wait, BOOL gated, BOOL timedOut)
{
    if (!triton_backend_debug(TRITON_DEBUG_PRESENT_TIMING))
        return;
    InterlockedAdd64(&g_pt12.gate_us, (LONG64)gate);
    InterlockedAdd64(&g_pt12.wait_us, (LONG64)wait);
    if (gated)
        InterlockedIncrement64(&g_pt12.gated);
    if (timedOut)
        InterlockedIncrement64(&g_pt12.timeouts);
    triton_pt_max(&g_pt12.wait_max_us, (LONG64)wait);
    LONG64 n = InterlockedIncrement64(&g_pt12.n);
    if (n % TRITON12_PT_REPORT == 0) {
        LONG64 gateT = InterlockedExchange64(&g_pt12.gate_us, 0);
        LONG64 waitT = InterlockedExchange64(&g_pt12.wait_us, 0);
        LONG64 gat   = InterlockedExchange64(&g_pt12.gated, 0);
        LONG64 to    = InterlockedExchange64(&g_pt12.timeouts, 0);
        LONG64 wmax  = InterlockedExchange64(&g_pt12.wait_max_us, 0);
        TR_LOG("12.PT n=%lld avg_us gate=%lld wait=%lld wait_max=%lld "
               "gated=%lld timeouts=%lld /%d",
               n, gateT / TRITON12_PT_REPORT, waitT / TRITON12_PT_REPORT,
               wmax, gat, to, TRITON12_PT_REPORT);
    }
}

/* Wait until the drain fence reaches v, polling its completed value on the
 * queue's high-resolution 1 ms slices (Sleep(1) quantizes to the process
 * timer resolution).  Returns FALSE when the deadline passes first. */
static BOOL
t12PollDrain(PTRITON12_QUEUE q, UINT64 v, DWORD timeoutMs)
{
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (ID3D12Fence_GetCompletedValue(q->pDrainFence) < v) {
        if (GetTickCount64() >= deadline)
            return FALSE;
        LARGE_INTEGER due;
        due.QuadPart = -10000;   /* 1 ms, relative */
        if (q->hPresentPaceTimer &&
            SetWaitableTimer(q->hPresentPaceTimer, &due, 0, NULL, NULL, FALSE))
            WaitForSingleObject(q->hPresentPaceTimer, 5);
        else
            Sleep(1);
    }
    return TRUE;
}

/* Fill src's companion from its frame.  A single-sample source is copied
 * under implicit promotion (see t12PresentCompanionCopy).  A multisampled
 * source is resolved: the companion is always single-sample, and the
 * resolve states are not promotable, so both resources are transitioned
 * explicitly and returned to PRESENT (COMMON), where the copy path also
 * leaves the source. */
static void
t12RecordCompanionFill(ID3D12GraphicsCommandList *list,
                       PTRITON12_RESOURCE src)
{
    if (src->Desc.SampleDesc.Count <= 1) {
        ID3D12GraphicsCommandList_CopyResource(list, src->pCompanion,
                                               src->pResource);
        return;
    }
    D3D12_RESOURCE_BARRIER b[2];
    memset(b, 0, sizeof(b));
    b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.pResource = src->pResource;
    b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    b[1] = b[0];
    b[1].Transition.pResource = src->pCompanion;
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_DEST;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 2, b);
    ID3D12GraphicsCommandList_ResolveSubresource(
        list, src->pCompanion, 0, src->pResource, 0,
        (DXGI_FORMAT)src->Desc.Format);
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_DEST;
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 2, b);
}

/* ---------- companion present copy ----------
 *
 * Companion-backed back buffers (tritonResource12.c) render into a NATIVE
 * texture; the KM allocation the runtime's kernel present names is the
 * shared linear companion.  Copy the finished frame into it here, on the
 * presenting queue, BEFORE the present's gate, which then holds the
 * kernel context until the copy is done too: the compositor must never
 * sample a mid-copy companion.
 *
 * A flip present cannot use the DDI's own AddedGpuWork/hCommandList
 * mechanism for this: the runtime executes that list only after
 * pfnPresent returns, which is after the gate -- the copy would land
 * outside it.  A blt present does use it (t12Present); its kernel
 * packet is ordered by the queue gate the list's ExecuteCommandLists
 * submits instead.
 *
 * The copy relies on implicit state promotion with no barriers: the source
 * sits in PRESENT (== COMMON), is promoted to COPY_SOURCE and decays back
 * after the copy; the companion is promoted to COPY_DEST by its first copy
 * and stays there.
 *
 * Failure policy: any failure skips the copy for this frame -- the
 * compositor shows the previous frame's companion contents, which beats
 * tearing down the present.  A rig build failure latches CopyRigDead. */
static void
t12PresentCompanionCopy(PTRITON12_QUEUE q, PTRITON12_RESOURCE src)
{
    PTRITON12_DEVICE p = q->pDev;
    UINT i;
    if (q->CopyRigDead || !p || !p->pDev)
        return;

    if (!q->pCopyList) {
        for (i = 0; i < TRITON12_COPY_RING; i++) {
            if (FAILED(ID3D12Device_CreateCommandAllocator(
                    p->pDev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    &IID_ID3D12CommandAllocator,
                    (void **)&q->pCopyAlloc[i])))
                break;
        }
        if (i == TRITON12_COPY_RING &&
            SUCCEEDED(ID3D12Device_CreateCommandList(
                p->pDev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                q->pCopyAlloc[0], NULL, &IID_ID3D12GraphicsCommandList,
                (void **)&q->pCopyList)))
            ID3D12GraphicsCommandList_Close(q->pCopyList);
        if (!q->pCopyList) {
            for (i = 0; i < TRITON12_COPY_RING; i++) {
                if (q->pCopyAlloc[i]) {
                    ID3D12CommandAllocator_Release(q->pCopyAlloc[i]);
                    q->pCopyAlloc[i] = NULL;
                }
            }
            q->CopyRigDead = TRUE;
            TR_LOG("12.CompanionCopy: rig build FAILED (queue %p); presents "
                   "on this queue will show stale companions", (void *)q);
            return;
        }
        TR_LOG("12.CompanionCopy: rig built (ring=%d)", TRITON12_COPY_RING);
    }

    /* Reuse gate: the drain value stamped at this allocator's last use
     * must have completed, or its list is still in flight and Reset would
     * pull the commands out from under the GPU.  With a ring of 4 and the
     * present pacing depth this is virtually never waited on. */
    const UINT idx = q->CopyIdx++ % TRITON12_COPY_RING;
    if (q->pDrainFence && q->CopyAllocValue[idx] &&
        !t12PollDrain(q, q->CopyAllocValue[idx], 1000)) {
        TR_LOG("12.CompanionCopy: allocator %u still in flight after 1s "
               "(v=%llu done=%llu); skipping copy this frame", idx,
               (unsigned long long)q->CopyAllocValue[idx],
               (unsigned long long)
                   ID3D12Fence_GetCompletedValue(q->pDrainFence));
        return;
    }

    if (FAILED(ID3D12CommandAllocator_Reset(q->pCopyAlloc[idx])) ||
        FAILED(ID3D12GraphicsCommandList_Reset(q->pCopyList,
                                               q->pCopyAlloc[idx], NULL))) {
        TR_LOG("12.CompanionCopy: list reset FAILED; skipping copy");
        return;
    }
    t12RecordCompanionFill(q->pCopyList, src);
    if (FAILED(ID3D12GraphicsCommandList_Close(q->pCopyList)))
        return;
    {
        ID3D12CommandList *lists[1] = { (ID3D12CommandList *)q->pCopyList };
        ID3D12CommandQueue_ExecuteCommandLists(q->pQueue, 1, lists);
    }
    /* Stamp the allocator with its own drain value so the reuse gate above
     * has something to key on. */
    if (q->pDrainFence) {
        UINT64 v;
        if (SUCCEEDED(t12DrainSignal(q, &v)))
            q->CopyAllocValue[idx] = v;
    }
}

static VOID APIENTRY
t12Present(D3D12DDI_HCOMMANDLIST hList, D3D12DDI_HCOMMANDQUEUE hQueue,
           const D3D12DDIARG_PRESENT_0001 *pArgs, D3D12DDI_PRESENT_0003 *pOut)
{
    PTRITON12_QUEUE q = (PTRITON12_QUEUE)hQueue.pDrvPrivate;
    PTRITON12_LIST l = (PTRITON12_LIST)hList.pDrvPrivate;
    if (!pOut)
        return;
    memset(pOut, 0, sizeof(*pOut));
    if (!pArgs || !q)
        return;

    PTRITON12_RESOURCE src = NULL;
    if (pArgs->SurfacesToPresent && pArgs->phSurfacesToPresent)
        src = (PTRITON12_RESOURCE)
            pArgs->phSurfacesToPresent[0].hSurface.pDrvPrivate;
    PTRITON12_RESOURCE dst =
        (PTRITON12_RESOURCE)pArgs->hDstResource.pDrvPrivate;

    /* A source still holding its residency-only placeholder names no memory
     * for the kernel present to copy from.  D3D9On12 presents plain
     * textures (no RENDER_TARGET flag, e.g. its A2R10G10B10 back buffer),
     * which the create path gives no companion; build one here, once. */
    if (src && q->pDev)
        t12EnsureCompanion(q->pDev, src);

    pOut->hSrcAllocation = src ? src->hKMAllocation : 0;
    pOut->hDstAllocation = dst ? dst->hKMAllocation : 0;
    pOut->hContext       = q->hKMContext;
    pOut->AddedGpuWork   = FALSE;
    pOut->BackBufferMultiplicity = 1;

    static LONG s_presentN;
    LONG n = InterlockedIncrement(&s_presentN);

    /* Companion-backed back buffer: copy the rendered native frame into
     * the shared companion the KM allocation names, before the gate below
     * so it covers the copy.  A blt present instead records
     * the copy at the end of the open list the runtime executes ahead of
     * the kernel present: a downlevel presenter (D3D9On12, D3D11On12)
     * records the frame's last writes there, so a copy issued now would
     * read the previous frame.  This KMD claims neither DirectFlip nor
     * independent flip, so the runtime delivers every D3D9On12, D3D11On12
     * and native D3D12 present as a blt, windowed or fullscreen, and no
     * presenter reaches the flip copy below.  A flip whose open list
     * carries the frame's writes (a D3D11On12 swapchain records its clear,
     * update and draw there) would show the previous frame. */
    const BOOL blt = pArgs->Flags.Blt ? TRUE : FALSE;
    if (src && src->pCompanion && src->pResource) {
        if (blt && l && l->pList && !l->AwaitingReset) {
            t12RecordCompanionFill(l->pList, src);
            pOut->AddedGpuWork = TRUE;
        } else if (q->pQueue) {
            t12PresentCompanionCopy(q, src);
            /* A blt present's copy went to the inner queue directly, past
             * the gate ExecuteCommandLists would have submitted behind it;
             * gate here so the kernel present waits for it.  Without a
             * gate nothing holds the kernel present, so wait on the CPU
             * for the drain value already signalled behind the copy
             * instead. */
            if (blt && !triton12QueueGate(q) && q->pDrainFence &&
                !t12PollDrain(q, t12Load64(&q->DrainVisible), 1000))
                TR_LOG("12.Present: blt copy not done after 1s (v=%llu "
                       "done=%llu); presenting anyway",
                       (unsigned long long)t12Load64(&q->DrainVisible),
                       (unsigned long long)
                           ID3D12Fence_GetCompletedValue(q->pDrainFence));
        }
    }

    /* A blt present is not gated here: the frame's last writes sit in the
     * open list the runtime executes between this DDI and the kernel
     * present, and that ExecuteCommandLists gates the context itself.
     *
     * A flip is gated here.  The runtime puts its present packet on
     * q->hKMContext once this DDI returns, so holding the context until the
     * GPU finished the frame keeps the frame-ready signal dxgkrnl and DWM
     * key on GPU-true while this thread goes on to the next frame.  A
     * windowed title is DWM-composited: the consumer of this backbuffer is
     * dwm.exe sampling it as a shared texture from another process, and
     * the frame-ready signal it acts on is the present itself, so no
     * scanout-side gate could order that handoff.  Without a gate, the
     * CPU waits for the frame instead, bounded so a lost completion
     * degrades to a late frame rather than a hung present. */
    if (!blt && q->pQueue) {
        const double t0 = TRITON_PT_NOW();
        const BOOL gated = triton12QueueGate(q);
        const double t1 = TRITON_PT_NOW();
        BOOL timedOut = FALSE;
        if (!gated && q->pDrainFence)
            timedOut = !t12PollDrain(q, t12Load64(&q->DrainVisible), 1000);
        t12PtAccount(t1 - t0, TRITON_PT_NOW() - t1, gated, timedOut);
        if (n <= 8 || (n & 255) == 0 || timedOut)
            TR_LOG("12.PresentGate #%ld: tid=%lu gated=%d%s", (long)n,
                   GetCurrentThreadId(), (int)gated,
                   timedOut ? " DEADLINE (presented anyway)" : "");
    }

    if (n <= 8 || (n & 255) == 0)
        TR_LOG("12.Present #%ld: src=0x%x dst=0x%x ctx=%p surfaces=%u "
               "flipInterval=%d", (long)n,
               src ? src->hKMAllocation : 0, dst ? dst->hKMAllocation : 0,
               q->hKMContext, pArgs->SurfacesToPresent,
               (int)pArgs->FlipInterval);
}

/* _0028 appends SyncIntervalOverride{Valid,}: left clear, the runtime's
 * own interval stands. */
static VOID APIENTRY
t12Present0028(D3D12DDI_HCOMMANDLIST hList, D3D12DDI_HCOMMANDQUEUE hQueue,
               const D3D12DDIARG_PRESENT_0001 *pArgs, D3D12DDI_PRESENT_0028 *pOut)
{
    if (!pOut)
        return;
    pOut->SyncIntervalOverrideValid = FALSE;
    pOut->SyncIntervalOverride = (DXGI_DDI_FLIP_INTERVAL_TYPE)0;
    t12Present(hList, hQueue, pArgs, (D3D12DDI_PRESENT_0003 *)pOut);
}

/* _0051 drops hSrc/hDst/hContext from the out struct: the allocations go
 * to slot 0 of the broadcast arrays and the context to the optional
 * contexts struct (the hardware-queue struct stays empty: no HW queues). */
static VOID APIENTRY
t12Present0051(D3D12DDI_HCOMMANDLIST hList, D3D12DDI_HCOMMANDQUEUE hQueue,
               const D3D12DDIARG_PRESENT_0001 *pArgs, D3D12DDI_PRESENT_0051 *pOut,
               D3D12DDI_PRESENT_CONTEXTS_0051 *pContexts,
               D3D12DDI_PRESENT_HWQUEUES_0051 *pHwQueues)
{
    D3D12DDI_PRESENT_0003 legacy;
    if (!pOut)
        return;
    memset(pOut, 0, sizeof(*pOut));
    if (pContexts)
        memset(pContexts, 0, sizeof(*pContexts));
    if (pHwQueues)
        memset(pHwQueues, 0, sizeof(*pHwQueues));
    memset(&legacy, 0, sizeof(legacy));
    t12Present(hList, hQueue, pArgs, &legacy);
    pOut->BroadcastSrcAllocation[0] = legacy.hSrcAllocation;
    pOut->BroadcastDstAllocation[0] = legacy.hDstAllocation;
    pOut->AddedGpuWork          = legacy.AddedGpuWork;
    pOut->BackBufferMultiplicity = legacy.BackBufferMultiplicity;
    if (pContexts) {
        pContexts->hContext = legacy.hContext;
        pContexts->BroadcastContextCount = 0;
    }
}

void
triton12InstallPresentFuncs0062(D3D12DDI_COMMAND_LIST_FUNCS_3D_0062 *t)
{
    t->pfnPresent = t12Present0051;
}

void
triton12InstallPresentFuncs0033(D3D12DDI_COMMAND_LIST_FUNCS_3D_0033 *t)
{
    t->pfnPresent = t12Present0028;
}

void
triton12InstallPresentFuncs(D3D12DDI_COMMAND_LIST_FUNCS_3D_0022 *t)
{
    t->pfnPresent = t12Present;
}
