/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

// Triton's Direct3D 12 backend on vkd3d-proton and Venus: the ID3D12Device is vkd3d-proton's, on a Venus instance that
// sends everything through the Direct3D 12 runtime's callbacks to viogpu3d.

#include "umd12.h"
#include "umd_d3d12.h"
#include "umd_format.h"

#include <string.h>

C_ASSERT(UMD_D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET == D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
C_ASSERT(UMD_D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL == D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
C_ASSERT(UMD_D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS == D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
C_ASSERT(UMD_D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE == D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
C_ASSERT(UMD_D3D11_BIND_UNORDERED_ACCESS == D3D11_BIND_UNORDERED_ACCESS);

static const GUID umd_iid_device_ext1 = UMD_IID_ID3D12DEVICEEXT1_INIT;

#define UMD12_VK_INSTANCE(instance, name) ((PFN_##name)umd_vk_icd_instance_proc((instance), #name))

// The Vulkan entry points of one vkd3d-proton device that this driver calls itself, from the ICD.
struct umd12_vk
{
    VkDevice device;
    VkPhysicalDeviceMemoryProperties memory;
    PFN_vkCreateImage createImage;
    PFN_vkDestroyImage destroyImage;
    PFN_vkGetImageMemoryRequirements getImageMemoryRequirements;
    PFN_vkAllocateMemory allocateMemory;
    PFN_vkFreeMemory freeMemory;
    PFN_vkBindImageMemory bindImageMemory;
    PFN_vkCreateSemaphore createSemaphore;
    PFN_vkDestroySemaphore destroySemaphore;
    PFN_vkGetSemaphoreWin32HandleKHR getSemaphoreWin32Handle;
    PFN_vkQueueSubmit queueSubmit;
};

struct umd12_device
{
    // The runtime's callbacks as Venus takes them (umd_vk.c).
    struct umd_device base;
    struct umd12_vk vk;
    struct umd_vkd3d_device_ext1 *ext;
    // Borrowed images start undefined: this queue moves them to the layout vkd3d-proton expects.
    SRWLOCK initLock;
    ID3D12CommandQueue *initQueue;
    ID3D12Fence *initFence;
    UINT64 initValue;
};

// The queue gate: a timeline semaphore of Venus's, whose kernel sync object the queue's context waits for.
struct umd12_queue
{
    SRWLOCK lock;
    VkSemaphore semaphore;
    D3DKMT_HANDLE sync;
    UINT64 value;
};

struct umd12_surface
{
    VkImage image;
    VkDeviceMemory memory;
    D3DKMT_HANDLE allocation;
};

static struct umd12_device *umd12_device(PTRITON12_DEVICE p)
{
    return p != NULL ? (struct umd12_device *)p->pBackend : NULL;
}

UINT triton12_backend_host_caps(struct TRITON12_ADAPTER *pAdapter)
{
    if (pAdapter == NULL || pAdapter->pBackend == NULL)
    {
        return 0;
    }
    // vkd3d-proton takes DXIL through dxil-spirv; report its own feature level and tiled resources tier.
    return TRITON_HOSTCAP_DXIL | TRITON_HOSTCAP_HOST_FEATURE_LEVEL;
}

void triton12_backend_close_adapter(struct TRITON12_ADAPTER *pAdapter)
{
    if (pAdapter->pBackend != NULL)
    {
        HeapFree(GetProcessHeap(), 0, pAdapter->pBackend);
        pAdapter->pBackend = NULL;
    }
}

static HRESULT umd12_physical_device(VkInstance instance, LUID luid, VkPhysicalDevice *out)
{
    PFN_vkEnumeratePhysicalDevices enumerate;
    PFN_vkGetPhysicalDeviceProperties2 properties2;
    VkPhysicalDevice devices[8];
    VkPhysicalDeviceIDProperties ids[8];
    uint32_t count = 8;
    int pick;

    *out = VK_NULL_HANDLE;
    enumerate = UMD12_VK_INSTANCE(instance, vkEnumeratePhysicalDevices);
    properties2 = UMD12_VK_INSTANCE(instance, vkGetPhysicalDeviceProperties2);
    if (enumerate == NULL || properties2 == NULL)
    {
        return DXGI_ERROR_UNSUPPORTED;
    }
    if (enumerate(instance, &count, devices) < 0 || count == 0)
    {
        umd_log("Venus lists no Vulkan device");
        return DXGI_ERROR_UNSUPPORTED;
    }
    memset(ids, 0, sizeof(ids));
    for (uint32_t i = 0; i < count; i++)
    {
        VkPhysicalDeviceProperties2 props;

        memset(&props, 0, sizeof(props));
        props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props.pNext = &ids[i];
        ids[i].sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        properties2(devices[i], &props);
        ids[i].pNext = NULL;
    }
    pick = umd_pick_physical_device(ids, count, luid);
    if (pick < 0)
    {
        umd_log("none of Venus's %u Vulkan devices is the adapter with LUID %08lx-%08lx",
                (unsigned)count,
                (unsigned long)luid.HighPart,
                (unsigned long)luid.LowPart);
        return DXGI_ERROR_UNSUPPORTED;
    }
    *out = devices[pick];
    return S_OK;
}

// callbacks NULL: Venus opens the adapter itself through D3DKMT, for a device outside the runtime.
static HRESULT umd12_vkd3d_device(const struct umd_vkd3d_api *api,
                                  VkD3DDDICallbacksMESA *callbacks,
                                  LUID luid,
                                  ID3D12Device **ppDevice)
{
    PFN_vkGetInstanceProcAddr loader = umd_vk_loader();
    struct umd_vkd3d_instance_create_info instanceInfo;
    struct umd_vkd3d_device_create_info deviceInfo;
    struct vkd3d_instance *instance = NULL;
    VkInstance vkInstance;
    HRESULT hr;

    *ppDevice = NULL;
    if (loader == NULL)
    {
        return DXGI_ERROR_UNSUPPORTED;
    }
    memset(&instanceInfo, 0, sizeof(instanceInfo));
    instanceInfo.pfn_vkGetInstanceProcAddr = loader;
    umd_vk_instance_callbacks_begin(callbacks);
    hr = api->create_instance(&instanceInfo, &instance);
    umd_vk_instance_callbacks_end();
    if (FAILED(hr))
    {
        umd_log("vkd3d-proton made no instance on Venus: 0x%08lx", (unsigned long)hr);
        return hr;
    }
    vkInstance = api->instance_get_vk_instance(instance);
    umd_vk_note_instance(vkInstance);

    memset(&deviceInfo, 0, sizeof(deviceInfo));
    deviceInfo.minimum_feature_level = D3D_FEATURE_LEVEL_11_0;
    deviceInfo.instance = instance;
    deviceInfo.adapter_luid = luid;
    // vkd3d-proton otherwise hands out one device per LUID, while every runtime device needs one on its own callbacks.
    deviceInfo.independent = true;
    hr = umd12_physical_device(vkInstance, luid, &deviceInfo.vk_physical_device);
    if (SUCCEEDED(hr))
    {
        // No device extensions of this driver's: the runtime presents, not a Vulkan swapchain.
        hr = api->create_device(&deviceInfo, &IID_ID3D12Device, (void **)ppDevice);
        if (FAILED(hr))
        {
            umd_log("vkd3d-proton made no device on Venus: 0x%08lx", (unsigned long)hr);
        }
    }
    // The device holds the instance from here.
    api->instance_decref(instance);
    return hr;
}

HRESULT triton12_backend_create_caps_device(struct TRITON12_ADAPTER *pAdapter,
                                            ID3D12Device **ppDevice,
                                            BOOL *pAdoptable)
{
    struct umd_adapter *adapter = (struct umd_adapter *)pAdapter->pBackend;
    const struct umd_vkd3d_api *api = umd_vkd3d();
    HRESULT hr;

    *ppDevice = NULL;
    *pAdoptable = FALSE;
    if (adapter == NULL || api == NULL)
    {
        return DXGI_ERROR_UNSUPPORTED;
    }
    // GetCaps comes before any runtime device, so this device's Venus instance goes through D3DKMT, and the runtime
    // never sees the device, which therefore cannot become an app's.
    hr = umd12_vkd3d_device(api, NULL, adapter->luid, ppDevice);
    if (SUCCEEDED(hr))
    {
        umd_log("caps: vkd3d-proton made a device on Venus");
    }
    return hr;
}

// Which of the kernel-thunk entries Venus calls this runtime fills: the Direct3D 12 runtime documents none of them
// for a driver that makes its contexts per device.
static void umd12_log_callbacks(const D3DDDI_DEVICECALLBACKS *kt, const D3D12DDI_CORELAYER_DEVICECALLBACKS_0022 *um)
{
    umd_log("CreateDevice12: kernel-thunk callbacks: CreateContextVirtual %s, DestroyContext %s, "
            "CreateSynchronizationObject2 %s, DestroySynchronizationObject %s, SignalFromGpu2 %s, WaitFromGpu %s, "
            "SignalFromCpu %s, WaitFromCpu %s",
            kt->pfnCreateContextVirtualCb ? "set" : "NULL",
            kt->pfnDestroyContextCb ? "set" : "NULL",
            kt->pfnCreateSynchronizationObject2Cb ? "set" : "NULL",
            kt->pfnDestroySynchronizationObjectCb ? "set" : "NULL",
            kt->pfnSignalSynchronizationObjectFromGpu2Cb ? "set" : "NULL",
            kt->pfnWaitForSynchronizationObjectFromGpuCb ? "set" : "NULL",
            kt->pfnSignalSynchronizationObjectFromCpuCb ? "set" : "NULL",
            kt->pfnWaitForSynchronizationObjectFromCpuCb ? "set" : "NULL");
    umd_log("CreateDevice12: kernel-thunk callbacks: SubmitCommand %s, Escape %s, CreatePagingQueue %s, "
            "MakeResident %s, Allocate %s, Deallocate %s; core layer: Allocate %s, Deallocate %s, "
            "CreateContextVirtual %s",
            kt->pfnSubmitCommandCb ? "set" : "NULL",
            kt->pfnEscapeCb ? "set" : "NULL",
            kt->pfnCreatePagingQueueCb ? "set" : "NULL",
            kt->pfnMakeResidentCb ? "set" : "NULL",
            kt->pfnAllocateCb ? "set" : "NULL",
            kt->pfnDeallocateCb ? "set" : "NULL",
            um->pfnAllocateCb ? "set" : "NULL",
            um->pfnDeallocateCb ? "set" : "NULL",
            um->pfnCreateContextVirtualCb ? "set" : "NULL");
}

static BOOL umd12_vk_init(struct umd12_vk *vk, const struct umd_vkd3d_api *api, ID3D12Device *device)
{
    VkPhysicalDevice physical = api->get_vk_physical_device(device);
    VkInstance instance = api->instance_get_vk_instance(api->instance_from_device(device));
    PFN_vkGetPhysicalDeviceMemoryProperties memoryProperties;

    memoryProperties = UMD12_VK_INSTANCE(instance, vkGetPhysicalDeviceMemoryProperties);
    memset(vk, 0, sizeof(*vk));
    vk->device = api->get_vk_device(device);
#define UMD12_VK(field, name) vk->field = (PFN_##name)umd_vk_icd_device_proc(vk->device, #name)
    UMD12_VK(createImage, vkCreateImage);
    UMD12_VK(destroyImage, vkDestroyImage);
    UMD12_VK(getImageMemoryRequirements, vkGetImageMemoryRequirements);
    UMD12_VK(allocateMemory, vkAllocateMemory);
    UMD12_VK(freeMemory, vkFreeMemory);
    UMD12_VK(bindImageMemory, vkBindImageMemory);
    UMD12_VK(createSemaphore, vkCreateSemaphore);
    UMD12_VK(destroySemaphore, vkDestroySemaphore);
    UMD12_VK(queueSubmit, vkQueueSubmit);
    UMD12_VK(getSemaphoreWin32Handle, vkGetSemaphoreWin32HandleKHR);
#undef UMD12_VK
    if (memoryProperties != NULL)
    {
        memoryProperties(physical, &vk->memory);
    }
    if (vk->createSemaphore == NULL || vk->destroySemaphore == NULL || vk->queueSubmit == NULL ||
        vk->getSemaphoreWin32Handle == NULL)
    {
        umd_log("CreateDevice12: vkd3d-proton's device has no exportable semaphores "
                "(VK_KHR_external_semaphore_win32), which the queue gate needs");
        return FALSE;
    }
    if (vk->createImage == NULL || vk->destroyImage == NULL || vk->getImageMemoryRequirements == NULL ||
        vk->allocateMemory == NULL || vk->freeMemory == NULL || vk->bindImageMemory == NULL || memoryProperties == NULL)
    {
        umd_log("CreateDevice12: vkd3d-proton's device lacks the image entry points shared surfaces need");
        return FALSE;
    }
    return TRUE;
}

HRESULT triton12_backend_create_device(struct TRITON12_DEVICE *p,
                                       const D3D12DDIARG_CREATEDEVICE_0003 *pArgs,
                                       ID3D12Device **ppDevice)
{
    struct umd_adapter *adapter = p->pAdapter != NULL ? (struct umd_adapter *)p->pAdapter->pBackend : NULL;
    const D3D12DDI_CORELAYER_DEVICECALLBACKS_0022 *um = pArgs->p12UMCallbacks_0022;
    const D3DDDI_DEVICECALLBACKS *kt = pArgs->pKTCallbacks;
    const struct umd_vkd3d_api *api = umd_vkd3d();
    struct umd12_device *dev;
    HRESULT hr;

    *ppDevice = NULL;
    if (adapter == NULL || kt == NULL || um == NULL)
    {
        return E_INVALIDARG;
    }
    if (api == NULL)
    {
        return DXGI_ERROR_UNSUPPORTED;
    }
    umd12_log_callbacks(kt, um);
    if (kt->pfnCreateContextVirtualCb == NULL || um->pfnAllocateCb == NULL || um->pfnDeallocateCb == NULL ||
        kt->pfnWaitForSynchronizationObjectFromGpuCb == NULL)
    {
        umd_log("CreateDevice12: refused: Venus makes its ring contexts and the queue gate waits through callbacks "
                "this runtime leaves NULL");
        return DXGI_ERROR_UNSUPPORTED;
    }

    dev = (struct umd12_device *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*dev));
    if (dev == NULL)
    {
        return E_OUTOFMEMORY;
    }
    InitializeSRWLock(&dev->initLock);
    umd_device_callbacks_init12(&dev->base, pArgs->hRTDevice.handle, kt, um);
    dev->base.callbacks.sType = UMD_VK_STRUCTURE_TYPE_D3DDDI_CALLBACKS_MESA;
    dev->base.callbacks.AdapterLuid = adapter->luid;
    dev->base.callbacks.hRTAdapter = adapter->hRTAdapter;
    dev->base.callbacks.hRTDevice = pArgs->hRTDevice.handle;
    dev->base.callbacks.pAdapterCallbacks = adapter->pAdapterCallbacks;
    dev->base.callbacks.pKTCallbacks = &dev->base.ktCallbacks;
    p->pBackend = dev;

    hr = umd12_vkd3d_device(api, &dev->base.callbacks, adapter->luid, ppDevice);
    if (FAILED(hr))
    {
        // Triton calls triton12_backend_destroy_device after a failed create.
        return hr;
    }
    if (!umd12_vk_init(&dev->vk, api, *ppDevice))
    {
        ID3D12Device_Release(*ppDevice);
        *ppDevice = NULL;
        return DXGI_ERROR_UNSUPPORTED;
    }
    if (FAILED(ID3D12Device_QueryInterface(*ppDevice, &umd_iid_device_ext1, (void **)&dev->ext)))
    {
        dev->ext = NULL;
        umd_log("CreateDevice12: vkd3d-proton has no ID3D12DeviceExt1, so no shared surface: swapchains will fail");
    }
    umd_log("CreateDevice12: vkd3d-proton device on Venus");
    return S_OK;
}

void triton12_backend_destroy_device(struct TRITON12_DEVICE *p)
{
    struct umd12_device *dev = umd12_device(p);

    if (dev == NULL)
    {
        return;
    }
    // The last of these takes vkd3d-proton's device with it, and Venus frees through the callbacks while it goes.
    if (dev->initFence != NULL)
    {
        ID3D12Fence_Release(dev->initFence);
    }
    if (dev->initQueue != NULL)
    {
        ID3D12CommandQueue_Release(dev->initQueue);
    }
    // The extension shares the device's count, so what is left of it says whether the device went.
    if (dev->ext != NULL && dev->ext->lpVtbl->Release(dev->ext) != 0)
    {
        // Venus keeps dev->base.callbacks for as long as the device lives: freeing them now would leave it a dangling
        // pointer.
        umd_log("DestroyDevice12: vkd3d-proton's device outlives the runtime's; its callbacks stay allocated");
        p->pBackend = NULL;
        return;
    }
    umd_device_callbacks_fini(&dev->base);
    HeapFree(GetProcessHeap(), 0, dev);
    p->pBackend = NULL;
}

BOOL triton12_backend_host_takes_row_major_texture(ID3D12Device *pDevice)
{
    // vkd3d-proton makes ROW_MAJOR textures on cross-adapter heaps only.
    (void)pDevice;
    return FALSE;
}

HRESULT triton12_backend_serialize_root_signature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *pDesc,
                                                  ID3DBlob **ppBlob,
                                                  ID3DBlob **ppError)
{
    const struct umd_vkd3d_api *api = umd_vkd3d();

    *ppBlob = NULL;
    if (ppError != NULL)
    {
        *ppError = NULL;
    }
    if (api == NULL)
    {
        return E_FAIL;
    }
    return api->serialize_versioned_root_signature(pDesc, (void **)ppBlob, (void **)ppError);
}

HRESULT triton12_backend_create_queue(struct TRITON12_DEVICE *p, struct TRITON12_QUEUE *q)
{
    struct umd12_device *dev = umd12_device(p);
    VkSemaphoreTypeCreateInfo type;
    VkExportSemaphoreCreateInfo exportInfo;
    VkSemaphoreCreateInfo create;
    VkSemaphoreGetWin32HandleInfoKHR get;
    struct umd12_queue *gate;
    HANDLE handle = NULL;
    VkResult result;

    if (dev == NULL || q->pQueue == NULL || q->hKMContext == NULL)
    {
        return E_INVALIDARG;
    }
    gate = (struct umd12_queue *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*gate));
    if (gate == NULL)
    {
        return E_OUTOFMEMORY;
    }
    InitializeSRWLock(&gate->lock);
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
    result = dev->vk.createSemaphore(dev->vk.device, &create, NULL, &gate->semaphore);
    if (result != VK_SUCCESS)
    {
        umd_log("queue gate: Venus made no exportable timeline semaphore: %d", (int)result);
        HeapFree(GetProcessHeap(), 0, gate);
        return E_FAIL;
    }
    memset(&get, 0, sizeof(get));
    get.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR;
    get.semaphore = gate->semaphore;
    get.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT;
    result = dev->vk.getSemaphoreWin32Handle(dev->vk.device, &get, &handle);
    gate->sync = (D3DKMT_HANDLE)(UINT_PTR)handle;
    if (result != VK_SUCCESS || gate->sync == 0)
    {
        umd_log("queue gate: Venus gave no sync object for the semaphore: %d", (int)result);
        dev->vk.destroySemaphore(dev->vk.device, gate->semaphore, NULL);
        HeapFree(GetProcessHeap(), 0, gate);
        return E_FAIL;
    }
    q->pBackend = gate;
    return S_OK;
}

void triton12_backend_destroy_queue(struct TRITON12_DEVICE *p, struct TRITON12_QUEUE *q)
{
    struct umd12_device *dev = umd12_device(p);
    struct umd12_queue *gate = (struct umd12_queue *)q->pBackend;

    if (gate == NULL)
    {
        return;
    }
    if (dev != NULL && gate->semaphore != VK_NULL_HANDLE)
    {
        dev->vk.destroySemaphore(dev->vk.device, gate->semaphore, NULL);
    }
    HeapFree(GetProcessHeap(), 0, gate);
    q->pBackend = NULL;
}

BOOL triton12_backend_gate_queue(struct TRITON12_QUEUE *q)
{
    struct umd12_device *dev = q->pDev != NULL ? umd12_device(q->pDev) : NULL;
    struct umd12_queue *gate = (struct umd12_queue *)q->pBackend;
    const struct umd_vkd3d_api *api = umd_vkd3d();
    D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU wait;
    VkTimelineSemaphoreSubmitInfo timeline;
    VkSubmitInfo submit;
    VkQueue queue;
    VkResult result;
    UINT64 value;
    HRESULT hr;

    if (dev == NULL || gate == NULL || api == NULL)
    {
        return FALSE;
    }
    AcquireSRWLockExclusive(&gate->lock);
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
    // vkd3d-proton hands its VkQueue out once everything submitted to the queue so far has reached it.
    queue = api->acquire_vk_queue(q->pQueue);
    result = dev->vk.queueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    api->release_vk_queue(q->pQueue);
    if (result != VK_SUCCESS)
    {
        ReleaseSRWLockExclusive(&gate->lock);
        umd_log("queue gate: the semaphore signal failed: %d", (int)result);
        return FALSE;
    }
    gate->value = value;
    memset(&wait, 0, sizeof(wait));
    wait.hContext = q->hKMContext;
    wait.ObjectCount = 1;
    wait.ObjectHandleArray = &gate->sync;
    wait.MonitoredFenceValueArray = &value;
    hr = dev->base.ktCallbacks.pfnWaitForSynchronizationObjectFromGpuCb(dev->base.hRTDevice, &wait);
    ReleaseSRWLockExclusive(&gate->lock);
    if (FAILED(hr))
    {
        umd_log("queue gate: the kernel context's wait for value %llu failed 0x%08lx",
                (unsigned long long)value,
                (unsigned long)hr);
        return FALSE;
    }
    return TRUE;
}

// vkd3d-proton takes a borrowed image to be in its common layout already.
static HRESULT umd12_initial_layout(struct umd12_device *dev, ID3D12Device *device, ID3D12Resource *resource)
{
    const struct umd_vkd3d_api *api = umd_vkd3d();
    D3D12_COMMAND_QUEUE_DESC desc;
    HRESULT hr = S_OK;
    UINT64 value;

    AcquireSRWLockExclusive(&dev->initLock);
    if (dev->initQueue == NULL)
    {
        memset(&desc, 0, sizeof(desc));
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = ID3D12Device_CreateCommandQueue(device, &desc, &IID_ID3D12CommandQueue, (void **)&dev->initQueue);
        if (SUCCEEDED(hr))
        {
            hr = ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&dev->initFence);
        }
        if (FAILED(hr) && dev->initQueue != NULL)
        {
            ID3D12CommandQueue_Release(dev->initQueue);
            dev->initQueue = NULL;
        }
    }
    if (SUCCEEDED(hr))
    {
        api->enqueue_initial_transition(dev->initQueue, resource);
        value = ++dev->initValue;
        hr = ID3D12CommandQueue_Signal(dev->initQueue, dev->initFence, value);
        if (SUCCEEDED(hr))
        {
            // No event: the call returns once the fence has the value.
            hr = ID3D12Fence_SetEventOnCompletion(dev->initFence, value, NULL);
        }
    }
    ReleaseSRWLockExclusive(&dev->initLock);
    if (FAILED(hr))
    {
        umd_log("shared surface: the initial layout transition failed 0x%08lx", (unsigned long)hr);
    }
    return hr;
}

static void umd12_free_surface(struct umd12_device *dev, struct umd12_surface *surface)
{
    if (surface->image != VK_NULL_HANDLE)
    {
        dev->vk.destroyImage(dev->vk.device, surface->image, NULL);
    }
    // Venus frees the kernel allocation with the memory, through the callbacks.
    if (surface->memory != VK_NULL_HANDLE)
    {
        dev->vk.freeMemory(dev->vk.device, surface->memory, NULL);
    }
    HeapFree(GetProcessHeap(), 0, surface);
}

HRESULT triton12_backend_create_shared_surface(struct TRITON12_DEVICE *p,
                                               struct TRITON12_RESOURCE *r,
                                               const D3D12_RESOURCE_DESC *pDesc,
                                               UINT flags,
                                               ID3D12Resource **ppSurface,
                                               D3DKMT_HANDLE *phAllocation)
{
    const BOOL primary = (flags & TRITON12_SHARED_SURFACE_PRIMARY) != 0;
    struct umd12_device *dev = umd12_device(p);
    const struct umd_vkd3d_api *api = umd_vkd3d();
    struct umd_d3d12_texture texture;
    struct umd_texture_desc desc;
    enum umd_d3d12_shared_check check;
    VkFormat viewFormats[2];
    UINT viewFormatCount;
    VkImageFormatListCreateInfo formatList;
    VkExternalMemoryImageCreateInfo external;
    VkImageCreateInfo imageInfo;
    VkMemoryRequirements requirements;
    VIOGPU3D_SURFACE record;
    D3D11DDIARG_CREATERESOURCE create11;
    DXGI_DDI_PRIMARY_DESC primaryDesc;
    VkD3DDDICreateResourceMESA d3d;
    VkExportMemoryAllocateInfo exportInfo;
    VkMemoryDedicatedAllocateInfo dedicated;
    VkMemoryAllocateInfo allocateInfo;
    struct umd_vk_pending pending;
    D3D12_RESOURCE_DESC1 desc1;
    struct umd12_surface *surface;
    D3DKMT_HANDLE allocation = 0;
    VkResult result;
    int memoryType;
    HRESULT hr;

    *ppSurface = NULL;
    *phAllocation = 0;
    if (dev == NULL || api == NULL || dev->ext == NULL || r->pBackend != NULL)
    {
        return E_NOTIMPL;
    }
    if (pDesc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    {
        return E_INVALIDARG;
    }
    memset(&texture, 0, sizeof(texture));
    texture.width = pDesc->Width;
    texture.height = pDesc->Height;
    texture.depth_or_array_size = pDesc->DepthOrArraySize;
    texture.mip_levels = pDesc->MipLevels;
    texture.format = (UINT)pDesc->Format;
    texture.sample_count = pDesc->SampleDesc.Count;
    texture.flags = (UINT)pDesc->Flags;
    check = umd_d3d12_shared_desc(&texture, primary, &desc);
    if (check != UMD_D3D12_SHARED_OK)
    {
        umd_log("shared surface %llux%u format %u: refused: %s",
                (unsigned long long)pDesc->Width,
                pDesc->Height,
                (unsigned)pDesc->Format,
                umd_d3d12_shared_check_text(check));
        return E_INVALIDARG;
    }

    memset(&imageInfo, 0, sizeof(imageInfo));
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = api->get_vk_format(desc.format);
    imageInfo.extent.width = desc.width;
    imageInfo.extent.height = desc.height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    // A host that shares Venus memory as host memory binds only linear images to it, here and in every opener.
    imageInfo.tiling = VK_IMAGE_TILING_LINEAR;
    imageInfo.usage = umd_d3d12_shared_usage(texture.flags);
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (imageInfo.format == VK_FORMAT_UNDEFINED)
    {
        umd_log("shared surface: vkd3d-proton has no Vulkan format for %u", desc.format);
        return E_INVALIDARG;
    }
    memset(&external, 0, sizeof(external));
    external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT;
    imageInfo.pNext = &external;
    viewFormatCount = umd_d3d12_view_formats(desc.format, viewFormats, 2);
    if (viewFormatCount > 1)
    {
        // An sRGB view of an 8-bit back buffer, as swapchains make them.
        memset(&formatList, 0, sizeof(formatList));
        formatList.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
        formatList.viewFormatCount = viewFormatCount;
        formatList.pViewFormats = viewFormats;
        external.pNext = &formatList;
        imageInfo.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    }

    surface = (struct umd12_surface *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*surface));
    if (surface == NULL)
    {
        return E_OUTOFMEMORY;
    }
    result = dev->vk.createImage(dev->vk.device, &imageInfo, NULL, &surface->image);
    if (result != VK_SUCCESS)
    {
        umd_log("shared surface %ux%u format %u: Venus made no linear image: %d",
                desc.width,
                desc.height,
                desc.format,
                (int)result);
        HeapFree(GetProcessHeap(), 0, surface);
        return result == VK_ERROR_OUT_OF_HOST_MEMORY ? E_OUTOFMEMORY : E_INVALIDARG;
    }
    dev->vk.getImageMemoryRequirements(dev->vk.device, surface->image, &requirements);
    memoryType = umd_pick_memory_type(requirements.memoryTypeBits,
                                      &dev->vk.memory,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memoryType < 0)
    {
        umd_log("shared surface: no memory type for the image");
        umd12_free_surface(dev, surface);
        return E_FAIL;
    }

    // The record follows the blob request in the allocation's private data, for the processes that open it.
    umd_resource_record_fill(&record, &desc);
    memset(&create11, 0, sizeof(create11));
    memset(&primaryDesc, 0, sizeof(primaryDesc));
    if (primary)
    {
        // The Direct3D 12 DDI names no source for a primary; viogpu3d scans out one.
        primaryDesc.VidPnSourceId = 0;
        primaryDesc.ModeDesc.Width = desc.width;
        primaryDesc.ModeDesc.Height = desc.height;
        primaryDesc.ModeDesc.Format = (DXGI_FORMAT)desc.format;
        create11.pPrimaryDesc = &primaryDesc;
    }
    memset(&d3d, 0, sizeof(d3d));
    d3d.sType = UMD_VK_STRUCTURE_TYPE_D3DDDI_CREATE_RESOURCE_MESA;
    d3d.hRTResource = r->hRTResource.handle;
    d3d.pCreateResource11 = &create11;
    d3d.pExtraData = &record;
    d3d.ExtraDataSize = sizeof(record);
    memset(&exportInfo, 0, sizeof(exportInfo));
    exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    exportInfo.pNext = &d3d;
    exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT;
    memset(&dedicated, 0, sizeof(dedicated));
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.pNext = &exportInfo;
    dedicated.image = surface->image;
    memset(&allocateInfo, 0, sizeof(allocateInfo));
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.pNext = &dedicated;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = (uint32_t)memoryType;

    memset(&pending, 0, sizeof(pending));
    pending.kind = UMD_VK_ALLOC_OTHER;
    pending.hRTDevice = dev->base.hRTDevice;
    pending.hRTResource = r->hRTResource.handle;
    pending.pAllocation = &allocation;
    umd_vk_pending_begin(&pending);
    result = dev->vk.allocateMemory(dev->vk.device, &allocateInfo, NULL, &surface->memory);
    umd_vk_pending_end(&pending);
    if (result != VK_SUCCESS || allocation == 0)
    {
        umd_log("shared surface %ux%u: %s (%d)",
                desc.width,
                desc.height,
                result != VK_SUCCESS ? "Venus made no shared memory" : "the runtime made no kernel allocation",
                (int)result);
        umd12_free_surface(dev, surface);
        return result == VK_ERROR_OUT_OF_DEVICE_MEMORY ? E_OUTOFMEMORY : E_FAIL;
    }
    surface->allocation = allocation;
    result = dev->vk.bindImageMemory(dev->vk.device, surface->image, surface->memory, 0);
    if (result != VK_SUCCESS)
    {
        umd_log("shared surface: binding its memory failed: %d", (int)result);
        umd12_free_surface(dev, surface);
        return E_FAIL;
    }

    memset(&desc1, 0, sizeof(desc1));
    desc1.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc1.Width = desc.width;
    desc1.Height = desc.height;
    desc1.DepthOrArraySize = 1;
    desc1.MipLevels = 1;
    desc1.Format = (DXGI_FORMAT)desc.format;
    desc1.SampleDesc.Count = 1;
    desc1.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc1.Flags = pDesc->Flags;
    hr = dev->ext->lpVtbl->CreateResourceFromBorrowedHandle(dev->ext,
                                                            &desc1,
                                                            (UINT64)surface->image,
                                                            (void **)ppSurface);
    if (FAILED(hr))
    {
        umd_log("shared surface: vkd3d-proton took no borrowed image: 0x%08lx", (unsigned long)hr);
        *ppSurface = NULL;
        umd12_free_surface(dev, surface);
        return hr;
    }
    hr = umd12_initial_layout(dev, p->pDev, *ppSurface);
    if (FAILED(hr))
    {
        ID3D12Resource_Release(*ppSurface);
        *ppSurface = NULL;
        umd12_free_surface(dev, surface);
        return hr;
    }
    r->pBackend = surface;
    *phAllocation = allocation;
    return S_OK;
}

void triton12_backend_destroy_shared_surface(struct TRITON12_DEVICE *p, struct TRITON12_RESOURCE *r)
{
    struct umd12_device *dev = umd12_device(p);
    struct umd12_surface *surface = (struct umd12_surface *)r->pBackend;

    if (dev == NULL || surface == NULL)
    {
        return;
    }
    if (r->hKMAllocation == surface->allocation)
    {
        r->hKMAllocation = 0;
    }
    umd12_free_surface(dev, surface);
    r->pBackend = NULL;
}

D3DKMT_HANDLE triton12_backend_allocate_empty(struct TRITON12_DEVICE *p, HANDLE hRTResource)
{
    VIRTIO_WDDM_CREATE_ALLOCATION create;
    D3D12DDI_ALLOCATION_INFO_0022 info;
    D3D12DDICB_ALLOCATE_0022 allocate;
    HRESULT hr;

    if (p->pUMCallbacks == NULL || p->pUMCallbacks->pfnAllocateCb == NULL)
    {
        return 0;
    }
    umd_empty_allocation_fill(&create);
    memset(&info, 0, sizeof(info));
    info.pPrivateDriverData = &create;
    info.PrivateDriverDataSize = sizeof(create);
    memset(&allocate, 0, sizeof(allocate));
    allocate.hResource = hRTResource;
    allocate.NumAllocations = 1;
    allocate.pAllocationInfo = &info;
    hr = p->pUMCallbacks->pfnAllocateCb(p->hRTDevice, &allocate);
    if (FAILED(hr))
    {
        umd_log("residency: the memory-less allocation failed 0x%08lx", (unsigned long)hr);
        return 0;
    }
    return info.hAllocation;
}
