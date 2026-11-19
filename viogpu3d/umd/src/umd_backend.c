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
#include "umd_shared.h"

#include <d3d11_umd.h>

#include <stdlib.h>
#include <string.h>

C_ASSERT(UMD_FEATURE_LEVEL_10_0 == D3D_FEATURE_LEVEL_10_0);
C_ASSERT(UMD_FEATURE_LEVEL_10_1 == D3D_FEATURE_LEVEL_10_1);
C_ASSERT(UMD_FEATURE_LEVEL_11_0 == D3D_FEATURE_LEVEL_11_0);
C_ASSERT(UMD_FEATURE_LEVEL_11_1 == D3D_FEATURE_LEVEL_11_1);
C_ASSERT(UMD_FEATURE_LEVEL_12_0 == D3D_FEATURE_LEVEL_12_0);
C_ASSERT(UMD_FEATURE_LEVEL_12_1 == D3D_FEATURE_LEVEL_12_1);
C_ASSERT(UMD_PIPELINE_LEVEL_10_0 == D3D11DDI_3DPIPELINELEVEL_10_0);
C_ASSERT(UMD_PIPELINE_LEVEL_10_1 == D3D11DDI_3DPIPELINELEVEL_10_1);
C_ASSERT(UMD_PIPELINE_LEVEL_11_0 == D3D11DDI_3DPIPELINELEVEL_11_0);
C_ASSERT(UMD_PIPELINE_LEVEL_11_1 == D3D11_1DDI_3DPIPELINELEVEL_11_1);
C_ASSERT(UMD_PIPELINE_LEVEL_12_0 == D3DWDDM2_0DDI_3DPIPELINELEVEL_12_0);
C_ASSERT(UMD_PIPELINE_LEVEL_12_1 == D3DWDDM2_0DDI_3DPIPELINELEVEL_12_1);
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

// VIOGPU3D_UMD_DEBUG=present_timing,d3d12_list_migration,no_d3d12_xqueue_order,no_flip_gate,frame_trace,
// no_companion_wait,no_buffer_wait (any of them); VIOGPU3D_UMD_PRESENT_GATE_TRACE=<ms>.
static INIT_ONCE umd_env_once = INIT_ONCE_STATIC_INIT;
static UINT umd_debug_flags;
static int umd_gate_trace_ms;
// no_flip_gate: flips of scanout primaries reach the kernel unordered, as before the flip gate, to measure it.
static BOOL umd_no_flip_gate;

static BOOL CALLBACK umd_env_read(PINIT_ONCE once, PVOID param, PVOID *context)
{
    char value[128];
    DWORD length;

    (void)once;
    (void)param;
    (void)context;
    length = GetEnvironmentVariableA("VIOGPU3D_UMD_DEBUG", value, sizeof(value));
    if (length > 0 && length < sizeof(value))
    {
        umd_debug_flags |= strstr(value, "present_timing") != NULL ? TRITON_DEBUG_PRESENT_TIMING : 0;
        umd_debug_flags |= strstr(value, "d3d12_list_migration") != NULL ? TRITON_DEBUG_D3D12_LIST_MIGRATION : 0;
        umd_debug_flags |= strstr(value, "no_d3d12_xqueue_order") != NULL ? TRITON_DEBUG_NO_D3D12_XQUEUE_ORDER : 0;
        umd_debug_flags |= strstr(value, "frame_trace") != NULL ? TRITON_DEBUG_FRAME_TRACE : 0;
        umd_debug_flags |= strstr(value, "no_companion_wait") != NULL ? TRITON_DEBUG_NO_COMPANION_WAIT : 0;
        umd_debug_flags |= strstr(value, "no_buffer_wait") != NULL ? TRITON_DEBUG_NO_BUFFER_WAIT : 0;
        umd_no_flip_gate = strstr(value, "no_flip_gate") != NULL;
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
    // DXVK's own feature level and tiled resources tier, which Triton forwards.
    out->caps_flags = TRITON_HOSTCAP_HOST_FEATURE_LEVEL;
}

// DXVK's IDXGIVkInteropDevice (dxvk/src/dxgi/dxgi_interfaces.h), whose methods C reaches through this table.
struct umd_dxvk_interop;

struct umd_dxvk_interop_vtbl
{
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(struct umd_dxvk_interop *self, REFIID iid, void **out);
    ULONG(STDMETHODCALLTYPE *AddRef)(struct umd_dxvk_interop *self);
    ULONG(STDMETHODCALLTYPE *Release)(struct umd_dxvk_interop *self);
    void(STDMETHODCALLTYPE *GetVulkanHandles)(struct umd_dxvk_interop *self,
                                              VkInstance *instance,
                                              VkPhysicalDevice *physical,
                                              VkDevice *device);
    void(STDMETHODCALLTYPE *GetSubmissionQueue)(struct umd_dxvk_interop *self, VkQueue *queue, uint32_t *family);
    void(STDMETHODCALLTYPE *TransitionSurfaceLayout)(struct umd_dxvk_interop *self,
                                                     void *surface,
                                                     const VkImageSubresourceRange *range,
                                                     VkImageLayout from,
                                                     VkImageLayout to);
    void(STDMETHODCALLTYPE *FlushRenderingCommands)(struct umd_dxvk_interop *self);
    void(STDMETHODCALLTYPE *LockSubmissionQueue)(struct umd_dxvk_interop *self);
    void(STDMETHODCALLTYPE *ReleaseSubmissionQueue)(struct umd_dxvk_interop *self);
};

struct umd_dxvk_interop
{
    const struct umd_dxvk_interop_vtbl *lpVtbl;
};

static const GUID umd_iid_dxvk_interop = {0xe2ef5fa5, 0xdc21, 0x4af7, {0x90, 0xc4, 0xf6, 0x7e, 0xf6, 0xa0, 0x93, 0x23}};

// The flip gate: DXVK's queue signals the semaphore after a frame, and the present's context waits for it.
struct umd_flip_gate
{
    // Made at the device's first flip; failed once, never again.
    BOOL tried;
    struct umd_dxvk_interop *interop;
    VkDevice device;
    VkQueue queue;
    VkSemaphore semaphore;
    D3DKMT_HANDLE sync;
    UINT64 value;
    PFN_vkQueueSubmit queueSubmit;
    PFN_vkDestroySemaphore destroySemaphore;
    LONG flips;
    LONG running;
    // frame_trace: a thread logs when the semaphore reaches each value, the frames' completions.
    PFN_vkWaitSemaphores waitSemaphores;
    PFN_vkGetSemaphoreCounterValue getSemaphoreCounterValue;
    HANDLE traceThread;
    volatile LONG traceStop;
};

struct umd11_device
{
    struct umd_device base;
    struct umd_flip_gate gate;
    // frame_trace: the private data of the flip being handed over, under kmCtxLock.
    VIOGPU3D_FLIP_TRACE trace;
    LONG traceSeq;
    // The buffer wait's: the present's kernel context signals it, the CPU waits for it.
    struct umd_context_mark mark;
};

static void umd_flip_gate_fini(struct umd_flip_gate *gate)
{
    if (gate->traceThread != NULL)
    {
        InterlockedExchange(&gate->traceStop, 1);
        WaitForSingleObject(gate->traceThread, 2000);
        CloseHandle(gate->traceThread);
        gate->traceThread = NULL;
    }
    if (gate->semaphore != VK_NULL_HANDLE && gate->destroySemaphore != NULL)
    {
        gate->destroySemaphore(gate->device, gate->semaphore, NULL);
    }
    gate->semaphore = VK_NULL_HANDLE;
    gate->sync = 0;
    if (gate->interop != NULL)
    {
        gate->interop->lpVtbl->Release(gate->interop);
        gate->interop = NULL;
    }
}

// Logs each value the gate's semaphore reaches, with the time, until the device goes.
static DWORD WINAPI umd_flip_trace_thread(void *param)
{
    struct umd_flip_gate *gate = (struct umd_flip_gate *)param;
    VkSemaphoreWaitInfo wait;
    UINT64 done = 0;
    UINT64 next;
    UINT64 now;
    LARGE_INTEGER t;

    memset(&wait, 0, sizeof(wait));
    wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wait.semaphoreCount = 1;
    wait.pSemaphores = &gate->semaphore;
    wait.pValues = &next;
    while (!gate->traceStop)
    {
        next = done + 1;
        if (*(volatile UINT64 *)&gate->value < next)
        {
            Sleep(1);
            continue;
        }
        if (gate->waitSemaphores(gate->device, &wait, 50000000ull) != VK_SUCCESS)
        {
            continue;
        }
        QueryPerformanceCounter(&t);
        now = next;
        gate->getSemaphoreCounterValue(gate->device, gate->semaphore, &now);
        umd_log("trace: gate done %llu qpc %lld", (unsigned long long)now, (long long)t.QuadPart);
        done = now > next ? now : next;
    }
    return 0;
}

static void umd_flip_trace_start(struct umd_flip_gate *gate)
{
    PFN_vkVoidFunction counter = umd_vk_icd_device_proc(gate->device, "vkGetSemaphoreCounterValue");
    LARGE_INTEGER frequency;

    gate->waitSemaphores = (PFN_vkWaitSemaphores)umd_vk_icd_device_proc(gate->device, "vkWaitSemaphores");
    gate->getSemaphoreCounterValue = (PFN_vkGetSemaphoreCounterValue)counter;
    if (gate->waitSemaphores == NULL || gate->getSemaphoreCounterValue == NULL)
    {
        umd_log("trace: Venus has no vkWaitSemaphores; no gate completions");
        return;
    }
    gate->traceThread = CreateThread(NULL, 0, umd_flip_trace_thread, gate, 0, NULL);
    QueryPerformanceFrequency(&frequency);
    umd_log("trace: qpc frequency %lld", (long long)frequency.QuadPart);
}

static BOOL umd_flip_gate_init(struct umd_flip_gate *gate, PTRITON_DEVICE pD)
{
    PFN_vkCreateSemaphore createSemaphore;
    PFN_vkGetSemaphoreWin32HandleKHR getSemaphoreWin32Handle;
    VkSemaphoreTypeCreateInfo type;
    VkExportSemaphoreCreateInfo exportInfo;
    VkSemaphoreCreateInfo create;
    VkSemaphoreGetWin32HandleInfoKHR get;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t family = 0;
    HANDLE handle = NULL;
    VkResult result;

    gate->tried = TRUE;
    if (pD->pDev1 == NULL || pD->KTCallbacks.pfnWaitForSynchronizationObjectFromGpuCb == NULL ||
        FAILED(ID3D11Device1_QueryInterface(pD->pDev1, &umd_iid_dxvk_interop, (void **)&gate->interop)))
    {
        gate->interop = NULL;
        umd_log("flip gate: no DXVK interop or no GPU wait callback; flips are not ordered after their frames");
        return FALSE;
    }
    gate->interop->lpVtbl->GetVulkanHandles(gate->interop, &instance, &physical, &gate->device);
    gate->interop->lpVtbl->GetSubmissionQueue(gate->interop, &gate->queue, &family);
    umd_vk_note_instance(instance);
    createSemaphore = (PFN_vkCreateSemaphore)umd_vk_icd_device_proc(gate->device, "vkCreateSemaphore");
    getSemaphoreWin32Handle = (PFN_vkGetSemaphoreWin32HandleKHR)umd_vk_icd_device_proc(gate->device,
                                                                                       "vkGetSemaphoreWin32HandleKHR");
    gate->destroySemaphore = (PFN_vkDestroySemaphore)umd_vk_icd_device_proc(gate->device, "vkDestroySemaphore");
    gate->queueSubmit = (PFN_vkQueueSubmit)umd_vk_icd_device_proc(gate->device, "vkQueueSubmit");
    if (gate->queue == VK_NULL_HANDLE || createSemaphore == NULL || getSemaphoreWin32Handle == NULL ||
        gate->destroySemaphore == NULL || gate->queueSubmit == NULL)
    {
        umd_log("flip gate: DXVK's device has no exportable semaphores (VK_KHR_external_semaphore_win32)");
        umd_flip_gate_fini(gate);
        return FALSE;
    }
    memset(&type, 0, sizeof(type));
    type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    memset(&exportInfo, 0, sizeof(exportInfo));
    exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
    exportInfo.pNext = &type;
    // Venus exports only timeline semaphores, and as a D3D11 fence: the runtime's sync object behind it.
    exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT;
    memset(&create, 0, sizeof(create));
    create.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    create.pNext = &exportInfo;
    result = createSemaphore(gate->device, &create, NULL, &gate->semaphore);
    if (result != VK_SUCCESS)
    {
        gate->semaphore = VK_NULL_HANDLE;
        umd_log("flip gate: Venus made no exportable timeline semaphore: %d", (int)result);
        umd_flip_gate_fini(gate);
        return FALSE;
    }
    memset(&get, 0, sizeof(get));
    get.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR;
    get.semaphore = gate->semaphore;
    get.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT;
    result = getSemaphoreWin32Handle(gate->device, &get, &handle);
    gate->sync = (D3DKMT_HANDLE)(UINT_PTR)handle;
    if (result != VK_SUCCESS || gate->sync == 0)
    {
        umd_log("flip gate: Venus gave no sync object for the semaphore: %d", (int)result);
        umd_flip_gate_fini(gate);
        return FALSE;
    }
    umd_log("flip gate: on, queue family %u", (unsigned)family);
    if (triton_backend_debug(TRITON_DEBUG_FRAME_TRACE))
    {
        umd_flip_trace_start(gate);
    }
    return TRUE;
}

UINT64 triton_backend_flip_signal(PTRITON_DEVICE pD, BOOL frameDone)
{
    struct umd11_device *dev = (struct umd11_device *)pD->pBackend;
    struct umd_flip_gate *gate;
    VkTimelineSemaphoreSubmitInfo timeline;
    VkSubmitInfo submit;
    VkResult result;
    UINT64 value;
    LONG flips;
    LONG running;

    if (dev == NULL)
    {
        return 0;
    }
    gate = &dev->gate;
    InitOnceExecuteOnce(&umd_env_once, umd_env_read, NULL, NULL);
    flips = InterlockedIncrement(&gate->flips);
    running = frameDone ? gate->running : InterlockedIncrement(&gate->running);
    if (flips % 600 == 0)
    {
        umd_log("flip gate: %ld flips of scanout primaries, %ld of them with their frame still running when handed "
                "to the kernel, %s",
                flips,
                running,
                umd_no_flip_gate  ? "not ordered (no_flip_gate)"
                : gate->sync != 0 ? "ordered"
                                  : "not ordered");
    }
    // frame_trace signals the semaphore without the gate too, so that the trace has the frames' completions.
    if (umd_no_flip_gate && !triton_backend_debug(TRITON_DEBUG_FRAME_TRACE))
    {
        return 0;
    }
    if (!gate->tried)
    {
        umd_flip_gate_init(gate, pD);
    }
    if (gate->sync == 0)
    {
        return 0;
    }
    value = gate->value + 1;
    memset(&timeline, 0, sizeof(timeline));
    timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timeline.signalSemaphoreValueCount = 1;
    timeline.pSignalSemaphoreValues = &value;
    memset(&submit, 0, sizeof(submit));
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.pNext = &timeline;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &gate->semaphore;
    // The frame Triton flushed reaches the queue first; a signal then covers every earlier submission.
    gate->interop->lpVtbl->FlushRenderingCommands(gate->interop);
    gate->interop->lpVtbl->LockSubmissionQueue(gate->interop);
    result = gate->queueSubmit(gate->queue, 1, &submit, VK_NULL_HANDLE);
    gate->interop->lpVtbl->ReleaseSubmissionQueue(gate->interop);
    if (result != VK_SUCCESS)
    {
        umd_log("flip gate: the semaphore signal failed: %d", (int)result);
        return 0;
    }
    gate->value = value;
    return umd_no_flip_gate ? 0 : value;
}

void triton_backend_flip_trace(PTRITON_DEVICE pD, DXGIDDICB_PRESENT *cb, UINT64 frame, UINT64 frameDone, UINT64 gate)
{
    struct umd11_device *dev = (struct umd11_device *)pD->pBackend;
    LARGE_INTEGER t;

    if (dev == NULL)
    {
        return;
    }
    QueryPerformanceCounter(&t);
    memset(&dev->trace, 0, sizeof(dev->trace));
    dev->trace.Magic = VIOGPU3D_FLIP_TRACE_MAGIC;
    dev->trace.Seq = (UINT32)InterlockedIncrement(&dev->traceSeq);
    dev->trace.Frame = frame;
    dev->trace.FrameDone = frameDone;
    dev->trace.Gate = gate != 0 ? gate : dev->gate.value;
    dev->trace.PresentQpc = (UINT64)t.QuadPart;
    cb->pPrivateDriverData = &dev->trace;
    cb->PrivateDriverDataSize = sizeof(dev->trace);
    umd_log("trace: flip %u frame %llu done %llu gate %llu qpc %lld",
            (unsigned)dev->trace.Seq,
            (unsigned long long)frame,
            (unsigned long long)frameDone,
            (unsigned long long)dev->trace.Gate,
            (long long)t.QuadPart);
}

void triton_backend_flip_wait(PTRITON_DEVICE pD, UINT64 value)
{
    struct umd11_device *dev = (struct umd11_device *)pD->pBackend;
    D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU wait;
    HRESULT hr;

    if (dev == NULL || dev->gate.sync == 0 || pD->hKMContext == NULL)
    {
        return;
    }
    memset(&wait, 0, sizeof(wait));
    wait.hContext = pD->hKMContext;
    wait.ObjectCount = 1;
    wait.ObjectHandleArray = &dev->gate.sync;
    wait.MonitoredFenceValueArray = &value;
    hr = pD->KTCallbacks.pfnWaitForSynchronizationObjectFromGpuCb(pD->hRTDevice.handle, &wait);
    if (FAILED(hr))
    {
        static LONG failures;
        if (InterlockedIncrement(&failures) == 1)
        {
            umd_log("flip gate: the present context's wait for value %llu failed 0x%08lx",
                    (unsigned long long)value,
                    (unsigned long)hr);
        }
    }
}

// The system node runs in physical mode for CDD, and takes no virtual context.
UINT triton_backend_context_node(void)
{
    return VIOGPU3D_NODE_RUNTIME;
}

BOOL triton_backend_wait_present_context(PTRITON_DEVICE pD, DWORD timeoutMs)
{
    struct umd11_device *dev = (struct umd11_device *)pD->pBackend;
    UINT64 value;
    BOOL signalled;

    if (dev == NULL || pD->hKMContext == NULL || !pD->presentLockInit)
    {
        return FALSE;
    }
    EnterCriticalSection(&pD->kmCtxLock);
    signalled = umd_context_mark_signal(&dev->base, &dev->mark, pD->hKMContext, &value);
    LeaveCriticalSection(&pD->kmCtxLock);
    return signalled && umd_context_mark_wait(&dev->base, &dev->mark, value, timeoutMs, "buffer wait");
}

// One per process: each Venus instance has its own contexts on the host.
static struct umd_shared umd_caps_vk = {UMD_LOCK_INIT, NULL, 0};

static HRESULT umd_caps_vk_make(void **object)
{
    struct umd_vk *vk = (struct umd_vk *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*vk));
    HRESULT hr;

    if (vk == NULL)
    {
        return E_OUTOFMEMORY;
    }
    // GetCaps comes before any runtime device, so Venus reaches the kernel driver through D3DKMT itself.
    hr = umd_vk_create(NULL, vk);
    if (FAILED(hr))
    {
        HeapFree(GetProcessHeap(), 0, vk);
        return hr;
    }
    umd_log("caps: made this process's Venus instance");
    *object = vk;
    return S_OK;
}

static void umd_caps_vk_free(void *object)
{
    umd_vk_destroy((struct umd_vk *)object);
    HeapFree(GetProcessHeap(), 0, object);
    umd_log("caps: released this process's Venus instance with its last adapter");
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
        umd_shared_release(&umd_caps_vk, umd_caps_vk_free);
        adapter->caps = NULL;
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

// The runtime never sees this device, which therefore cannot become an app's.
HRESULT triton_backend_create_caps_device(PTRITON_ADAPTER pAdapter,
                                          D3D_FEATURE_LEVEL maxLevel,
                                          ID3D11Device **ppDevice,
                                          D3D_FEATURE_LEVEL *pLevel,
                                          ID3D11DeviceContext **ppContext,
                                          BOOL *pAdoptable)
{
    struct umd_adapter *adapter = (struct umd_adapter *)pAdapter->pBackend;
    D3D_FEATURE_LEVEL levels[UMD_FEATURE_LEVELS];
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
    count = umd_feature_levels_upto((UINT)maxLevel, (UINT *)levels, UMD_FEATURE_LEVELS);
    if (count == 0)
    {
        return E_INVALIDARG;
    }
    if (adapter->caps == NULL)
    {
        void *caps;

        hr = umd_shared_acquire(&umd_caps_vk, umd_caps_vk_make, &caps);
        if (FAILED(hr))
        {
            return hr;
        }
        adapter->caps = (struct umd_vk *)caps;
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

    dev = (struct umd_device *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(struct umd11_device));
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
    umd_flip_gate_fini(&((struct umd11_device *)dev)->gate);
    umd_context_mark_fini(dev, &((struct umd11_device *)dev)->mark);
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

    if (triton_backend_debug(TRITON_DEBUG_FRAME_TRACE))
    {
        LARGE_INTEGER t;

        QueryPerformanceCounter(&t);
        r->TraceResId = ((const VIRTIO_WDDM_CREATE_ALLOCATION *)opened->pPrivateDriverData)->ResId;
        umd_log("trace: open res %u %ux%u qpc %lld", r->TraceResId, desc.width, desc.height, (long long)t.QuadPart);
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
