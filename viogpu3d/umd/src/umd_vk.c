/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "umd.h"
#include "umd_path.h"

#include <d3d11_umd.h>

#include <string.h>

// The loader-ICD interface version this driver speaks to the ICD, as the Vulkan loader would.
#define UMD_ICD_INTERFACE_VERSION 7

typedef VkResult(VKAPI_PTR *PFN_umd_icdNegotiate)(uint32_t *pSupportedVersion);

static INIT_ONCE umd_icd_once = INIT_ONCE_STATIC_INIT;
// The ICD stays loaded for the life of the process: its objects outlive any one device.
static HMODULE umd_icd;
static PFN_vkGetInstanceProcAddr umd_icd_gipa;
static PFN_vkGetDeviceProcAddr volatile umd_icd_gdpa;
static PFN_vkAllocateMemory volatile umd_icd_allocate;

static _Thread_local struct umd_vk_pending *umd_pending;

static SRWLOCK umd_devices_lock = SRWLOCK_INIT;
static struct umd_device *umd_devices;

static BOOL CALLBACK umd_icd_load(PINIT_ONCE once, PVOID param, PVOID *context)
{
    const WCHAR *name = umd_icd_name(UMD_X86);
    WCHAR self[MAX_PATH], path[MAX_PATH];
    PFN_umd_icdNegotiate negotiate;
    uint32_t version = UMD_ICD_INTERFACE_VERSION;
    DWORD length;

    (void)once;
    (void)param;
    (void)context;
    length = GetModuleFileNameW(umd_module(), self, MAX_PATH);
    if (length == 0 || length == MAX_PATH || !umd_path_beside(self, name, path, MAX_PATH))
    {
        umd_log("no path for %ls beside this driver", name);
        return TRUE;
    }
    // Only its own folder and System32: the ICD imports system DLLs only.
    umd_icd = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (umd_icd == NULL)
    {
        umd_log("cannot load %ls: error %lu", path, (unsigned long)GetLastError());
        return TRUE;
    }
    negotiate = (PFN_umd_icdNegotiate)(void *)GetProcAddress(umd_icd, "vk_icdNegotiateLoaderICDInterfaceVersion");
    umd_icd_gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(umd_icd, "vk_icdGetInstanceProcAddr");
    if (negotiate == NULL || umd_icd_gipa == NULL || negotiate(&version) != VK_SUCCESS)
    {
        umd_log("%ls is not a Vulkan ICD this driver can use", path);
        umd_icd_gipa = NULL;
        return TRUE;
    }
    umd_log("loaded the Venus ICD %ls, interface version %u", path, version);
    return TRUE;
}

static VKAPI_ATTR VkResult VKAPI_CALL umd_vk_allocate_memory(VkDevice device,
                                                             const VkMemoryAllocateInfo *info,
                                                             const VkAllocationCallbacks *allocator,
                                                             VkDeviceMemory *memory)
{
    struct umd_vk_pending *pending = umd_pending;
    PFN_vkAllocateMemory allocate = umd_icd_allocate;
    VkMemoryAllocateInfo chained;

    if (allocate == NULL)
    {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    // DXVK retries a failed dedicated allocation in another memory type: the retry names the resource too.
    if (pending != NULL && info != NULL && umd_vk_alloc_kind(info) == pending->kind)
    {
        pending->chained = TRUE;
        umd_vk_chain_prepend(info, &chained, pending->ext);
        return allocate(device, &chained, allocator, memory);
    }
    return allocate(device, info, allocator, memory);
}

static PFN_vkVoidFunction VKAPI_CALL umd_vk_get_device_proc_addr(VkDevice device, const char *name);

// Every device and instance of the one ICD answers with the same function.
static void umd_vk_capture_allocate(PFN_vkVoidFunction icdAllocate)
{
    if (umd_icd_allocate == NULL && icdAllocate != NULL)
    {
        umd_icd_allocate = (PFN_vkAllocateMemory)icdAllocate;
    }
}

static PFN_vkVoidFunction umd_vk_hook(const char *name)
{
    if (strcmp(name, "vkGetDeviceProcAddr") == 0)
    {
        return (PFN_vkVoidFunction)umd_vk_get_device_proc_addr;
    }
    if (strcmp(name, "vkAllocateMemory") == 0)
    {
        return (PFN_vkVoidFunction)umd_vk_allocate_memory;
    }
    return NULL;
}

static PFN_vkVoidFunction VKAPI_CALL umd_vk_get_device_proc_addr(VkDevice device, const char *name)
{
    PFN_vkVoidFunction hook = umd_vk_hook(name);

    if (hook != NULL)
    {
        if (strcmp(name, "vkAllocateMemory") == 0)
        {
            umd_vk_capture_allocate(umd_icd_gdpa(device, name));
        }
        return hook;
    }
    return umd_icd_gdpa(device, name);
}

// What DXVK takes as its vkGetInstanceProcAddr: the ICD's, with the two hooks.
static PFN_vkVoidFunction VKAPI_CALL umd_vk_get_instance_proc_addr(VkInstance instance, const char *name)
{
    PFN_vkVoidFunction hook;

    if (strcmp(name, "vkGetInstanceProcAddr") == 0)
    {
        return (PFN_vkVoidFunction)umd_vk_get_instance_proc_addr;
    }
    hook = umd_vk_hook(name);
    if (hook != NULL && instance != VK_NULL_HANDLE)
    {
        if (strcmp(name, "vkAllocateMemory") == 0)
        {
            umd_vk_capture_allocate(umd_icd_gipa(instance, name));
        }
        return hook;
    }
    return umd_icd_gipa(instance, name);
}

HRESULT umd_vk_create(const VkD3DDDICallbacksMESA *callbacks, struct umd_vk *vk)
{
    PFN_vkCreateInstance createInstance;
    VkApplicationInfo app;
    VkInstanceCreateInfo create;
    VkResult result;
    HRESULT hr;

    memset(vk, 0, sizeof(*vk));
    InitOnceExecuteOnce(&umd_icd_once, umd_icd_load, NULL, NULL);
    if (umd_icd_gipa == NULL)
    {
        return DXGI_ERROR_UNSUPPORTED;
    }

    createInstance = (PFN_vkCreateInstance)umd_icd_gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (createInstance == NULL)
    {
        return DXGI_ERROR_UNSUPPORTED;
    }
    memset(&app, 0, sizeof(app));
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    // As DXVK names itself when it makes the instance: drivers key their DXVK settings on it.
    app.pEngineName = "DXVK";
    app.engineVersion = VK_MAKE_API_VERSION(0, 3, 2, 0);
    app.apiVersion = VK_API_VERSION_1_3;
    memset(&create, 0, sizeof(create));
    create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create.pNext = callbacks;
    create.pApplicationInfo = &app;
    result = createInstance(&create, NULL, &vk->instance);
    if (result != VK_SUCCESS)
    {
        umd_log("vkCreateInstance on Venus failed: %d", (int)result);
        vk->instance = VK_NULL_HANDLE;
        return result == VK_ERROR_OUT_OF_HOST_MEMORY ? E_OUTOFMEMORY : DXGI_ERROR_UNSUPPORTED;
    }
    vk->destroyInstance = (PFN_vkDestroyInstance)umd_icd_gipa(vk->instance, "vkDestroyInstance");
    if (umd_icd_gdpa == NULL)
    {
        umd_icd_gdpa = (PFN_vkGetDeviceProcAddr)umd_icd_gipa(vk->instance, "vkGetDeviceProcAddr");
    }
    if (vk->destroyInstance == NULL || umd_icd_gdpa == NULL)
    {
        umd_vk_destroy(vk);
        return DXGI_ERROR_UNSUPPORTED;
    }

    hr = DXVKUmdCreateFactory(umd_vk_get_instance_proc_addr,
                              vk->instance,
                              0,
                              NULL,
                              &IID_IDXGIFactory4,
                              (void **)&vk->factory);
    if (FAILED(hr))
    {
        umd_log("DXVK made no DXGI factory on Venus: 0x%08lx", (unsigned long)hr);
        umd_vk_destroy(vk);
        return hr;
    }
    return S_OK;
}

void umd_vk_destroy(struct umd_vk *vk)
{
    if (vk->factory != NULL)
    {
        IDXGIFactory4_Release(vk->factory);
        vk->factory = NULL;
    }
    // DXVK never destroys an instance it was given.
    if (vk->instance != VK_NULL_HANDLE && vk->destroyInstance != NULL)
    {
        vk->destroyInstance(vk->instance, NULL);
    }
    vk->instance = VK_NULL_HANDLE;
}

void umd_vk_pending_begin(struct umd_vk_pending *pending)
{
    pending->chained = FALSE;
    umd_pending = pending;
}

void umd_vk_pending_end(struct umd_vk_pending *pending)
{
    if (umd_pending == pending)
    {
        umd_pending = NULL;
    }
}

static PFND3DDDI_ALLOCATECB umd_runtime_allocate(HANDLE hRTDevice)
{
    PFND3DDDI_ALLOCATECB allocate = NULL;

    AcquireSRWLockShared(&umd_devices_lock);
    for (struct umd_device *dev = umd_devices; dev != NULL; dev = dev->next)
    {
        if (dev->hRTDevice == hRTDevice)
        {
            allocate = dev->runtimeAllocateCb;
            break;
        }
    }
    ReleaseSRWLockShared(&umd_devices_lock);
    return allocate;
}

// Venus allocates through this: it gives the kernel allocation of a resource being created back to that resource.
static HRESULT APIENTRY umd_allocate_cb(HANDLE hRTDevice, D3DDDICB_ALLOCATE *pData)
{
    PFND3DDDI_ALLOCATECB allocate = umd_runtime_allocate(hRTDevice);
    struct umd_vk_pending *pending = umd_pending;
    HRESULT hr;

    if (allocate == NULL)
    {
        return E_INVALIDARG;
    }
    hr = allocate(hRTDevice, pData);
    if (SUCCEEDED(hr) && pending != NULL && pending->pAllocation != NULL && pending->hRTDevice == hRTDevice &&
        pending->hRTResource != NULL && pData->hResource == pending->hRTResource && pData->NumAllocations == 1 &&
        pData->pAllocationInfo != NULL)
    {
        // hAllocation leads both D3DDDI_ALLOCATIONINFO and D3DDDI_ALLOCATIONINFO2.
        *pending->pAllocation = pData->pAllocationInfo[0].hAllocation;
    }
    return hr;
}

void umd_device_callbacks_init(struct umd_device *dev, HANDLE hRTDevice, const D3DDDI_DEVICECALLBACKS *runtime)
{
    dev->hRTDevice = hRTDevice;
    dev->ktCallbacks = *runtime;
    dev->runtimeAllocateCb = runtime->pfnAllocateCb;
    dev->ktCallbacks.pfnAllocateCb = umd_allocate_cb;

    AcquireSRWLockExclusive(&umd_devices_lock);
    dev->next = umd_devices;
    umd_devices = dev;
    ReleaseSRWLockExclusive(&umd_devices_lock);
}

void umd_device_callbacks_fini(struct umd_device *dev)
{
    AcquireSRWLockExclusive(&umd_devices_lock);
    for (struct umd_device **link = &umd_devices; *link != NULL; link = &(*link)->next)
    {
        if (*link == dev)
        {
            *link = dev->next;
            break;
        }
    }
    ReleaseSRWLockExclusive(&umd_devices_lock);
    dev->next = NULL;
}
