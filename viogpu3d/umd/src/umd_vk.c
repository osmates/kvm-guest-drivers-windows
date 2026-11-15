/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "umd.h"
#include "umd_blob_map.h"
#include "umd_d3d12.h"
#include "umd_path.h"

#include "../triton/triton12.h"

#include <d3d11_umd.h>

#include <stddef.h>
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

void umd_vk_note_instance(VkInstance instance)
{
    if (umd_icd_gdpa == NULL && umd_icd_gipa != NULL && instance != VK_NULL_HANDLE)
    {
        umd_icd_gdpa = (PFN_vkGetDeviceProcAddr)umd_icd_gipa(instance, "vkGetDeviceProcAddr");
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
        umd_vk_note_instance(instance);
        return hook;
    }
    return umd_icd_gipa(instance, name);
}

PFN_vkGetInstanceProcAddr umd_vk_loader(void)
{
    InitOnceExecuteOnce(&umd_icd_once, umd_icd_load, NULL, NULL);
    return umd_icd_gipa != NULL ? umd_vk_get_instance_proc_addr : NULL;
}

PFN_vkVoidFunction umd_vk_icd_instance_proc(VkInstance instance, const char *name)
{
    return umd_icd_gipa != NULL ? umd_icd_gipa(instance, name) : NULL;
}

PFN_vkVoidFunction umd_vk_icd_device_proc(VkDevice device, const char *name)
{
    PFN_vkGetDeviceProcAddr gdpa = umd_icd_gdpa;

    return gdpa != NULL ? gdpa(device, name) : NULL;
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

// A device's callbacks run only while the runtime keeps the device, so the entry outlives the lookup.
static struct umd_device *umd_device_of(HANDLE hRTDevice)
{
    struct umd_device *found = NULL;

    AcquireSRWLockShared(&umd_devices_lock);
    for (struct umd_device *dev = umd_devices; dev != NULL; dev = dev->next)
    {
        if (dev->hRTDevice == hRTDevice)
        {
            found = dev;
            break;
        }
    }
    ReleaseSRWLockShared(&umd_devices_lock);
    return found;
}

static PFND3DDDI_ESCAPECB umd_runtime_escape(HANDLE hRTDevice)
{
    struct umd_device *dev = umd_device_of(hRTDevice);

    return dev != NULL ? dev->runtimeEscapeCb : NULL;
}

// umd_d3d12.c maps Venus's allocate and deallocate calls to the core layer's on these mirrors, so each must be the
// kits' struct field for field.
#define UMD_SAME_FIELD(mirror, kit, field)                                                                             \
    C_ASSERT(offsetof(mirror, field) == offsetof(kit, field) &&                                                        \
             sizeof(((mirror *)NULL)->field) == sizeof(((kit *)NULL)->field))

C_ASSERT(sizeof(struct umd_kt_allocation_info) == sizeof(D3DDDI_ALLOCATIONINFO));
UMD_SAME_FIELD(struct umd_kt_allocation_info, D3DDDI_ALLOCATIONINFO, hAllocation);
UMD_SAME_FIELD(struct umd_kt_allocation_info, D3DDDI_ALLOCATIONINFO, pSystemMem);
UMD_SAME_FIELD(struct umd_kt_allocation_info, D3DDDI_ALLOCATIONINFO, pPrivateDriverData);
UMD_SAME_FIELD(struct umd_kt_allocation_info, D3DDDI_ALLOCATIONINFO, PrivateDriverDataSize);
UMD_SAME_FIELD(struct umd_kt_allocation_info, D3DDDI_ALLOCATIONINFO, VidPnSourceId);
UMD_SAME_FIELD(struct umd_kt_allocation_info, D3DDDI_ALLOCATIONINFO, Flags);
C_ASSERT(sizeof(struct umd_kt_allocate) == sizeof(D3DDDICB_ALLOCATE));
UMD_SAME_FIELD(struct umd_kt_allocate, D3DDDICB_ALLOCATE, pPrivateDriverData);
UMD_SAME_FIELD(struct umd_kt_allocate, D3DDDICB_ALLOCATE, PrivateDriverDataSize);
UMD_SAME_FIELD(struct umd_kt_allocate, D3DDDICB_ALLOCATE, hResource);
UMD_SAME_FIELD(struct umd_kt_allocate, D3DDDICB_ALLOCATE, hKMResource);
UMD_SAME_FIELD(struct umd_kt_allocate, D3DDDICB_ALLOCATE, NumAllocations);
UMD_SAME_FIELD(struct umd_kt_allocate, D3DDDICB_ALLOCATE, pAllocationInfo);
C_ASSERT(sizeof(struct umd_kt_deallocate) == sizeof(D3DDDICB_DEALLOCATE));
UMD_SAME_FIELD(struct umd_kt_deallocate, D3DDDICB_DEALLOCATE, hResource);
UMD_SAME_FIELD(struct umd_kt_deallocate, D3DDDICB_DEALLOCATE, NumAllocations);
UMD_SAME_FIELD(struct umd_kt_deallocate, D3DDDICB_DEALLOCATE, HandleList);
C_ASSERT(sizeof(struct umd_d3d12_allocation_info) == sizeof(D3D12DDI_ALLOCATION_INFO_0022));
UMD_SAME_FIELD(struct umd_d3d12_allocation_info, D3D12DDI_ALLOCATION_INFO_0022, hAllocation);
UMD_SAME_FIELD(struct umd_d3d12_allocation_info, D3D12DDI_ALLOCATION_INFO_0022, pSystemMem);
UMD_SAME_FIELD(struct umd_d3d12_allocation_info, D3D12DDI_ALLOCATION_INFO_0022, pPrivateDriverData);
UMD_SAME_FIELD(struct umd_d3d12_allocation_info, D3D12DDI_ALLOCATION_INFO_0022, PrivateDriverDataSize);
UMD_SAME_FIELD(struct umd_d3d12_allocation_info, D3D12DDI_ALLOCATION_INFO_0022, VidPnSourceId);
UMD_SAME_FIELD(struct umd_d3d12_allocation_info, D3D12DDI_ALLOCATION_INFO_0022, Flags);
UMD_SAME_FIELD(struct umd_d3d12_allocation_info, D3D12DDI_ALLOCATION_INFO_0022, GpuVirtualAddress);
UMD_SAME_FIELD(struct umd_d3d12_allocation_info, D3D12DDI_ALLOCATION_INFO_0022, Priority);
UMD_SAME_FIELD(struct umd_d3d12_allocation_info, D3D12DDI_ALLOCATION_INFO_0022, Reserved);
C_ASSERT(sizeof(struct umd_d3d12_allocate) == sizeof(D3D12DDICB_ALLOCATE_0022));
UMD_SAME_FIELD(struct umd_d3d12_allocate, D3D12DDICB_ALLOCATE_0022, pPrivateDriverData);
UMD_SAME_FIELD(struct umd_d3d12_allocate, D3D12DDICB_ALLOCATE_0022, PrivateDriverDataSize);
UMD_SAME_FIELD(struct umd_d3d12_allocate, D3D12DDICB_ALLOCATE_0022, hResource);
UMD_SAME_FIELD(struct umd_d3d12_allocate, D3D12DDICB_ALLOCATE_0022, hKMResource);
UMD_SAME_FIELD(struct umd_d3d12_allocate, D3D12DDICB_ALLOCATE_0022, NumAllocations);
UMD_SAME_FIELD(struct umd_d3d12_allocate, D3D12DDICB_ALLOCATE_0022, pAllocationInfo);
C_ASSERT(sizeof(struct umd_d3d12_deallocate) == sizeof(D3D12DDICB_DEALLOCATE_0022));
UMD_SAME_FIELD(struct umd_d3d12_deallocate, D3D12DDICB_DEALLOCATE_0022, hResource);
UMD_SAME_FIELD(struct umd_d3d12_deallocate, D3D12DDICB_DEALLOCATE_0022, NumAllocations);
UMD_SAME_FIELD(struct umd_d3d12_deallocate, D3D12DDICB_DEALLOCATE_0022, HandleList);
UMD_SAME_FIELD(struct umd_d3d12_deallocate, D3D12DDICB_DEALLOCATE_0022, Flags);
C_ASSERT(UMD_D3D12_ALLOCATION_PRIMARY == D3D12DDI_ALLOCATION_INFO_FLAGS_0022_PRIMARY);
C_ASSERT(UMD_D3D12_ALLOCATION_STEREO == D3D12DDI_ALLOCATION_INFO_FLAGS_0022_STEREO);

static HRESULT umd_allocate12(const struct umd_device *dev, HANDLE hRTDevice, D3DDDICB_ALLOCATE *pData)
{
    struct umd_kt_allocate *venus = (struct umd_kt_allocate *)pData;
    struct umd_d3d12_allocation_info infos[UMD_D3D12_ALLOCATE_MAX];
    struct umd_d3d12_allocate allocate;
    D3D12DDI_HRTDEVICE device;
    HRESULT hr;

    if (dev->coreLayer12->pfnAllocateCb == NULL)
    {
        return E_INVALIDARG;
    }
    if (!umd_d3d12_allocate_args(venus, infos, &allocate))
    {
        umd_log("allocate: refused %u allocations: the driver passes 1 to %u",
                (unsigned)venus->NumAllocations,
                (unsigned)UMD_D3D12_ALLOCATE_MAX);
        return E_INVALIDARG;
    }
    device.handle = hRTDevice;
    hr = dev->coreLayer12->pfnAllocateCb(device, (D3D12DDICB_ALLOCATE_0022 *)&allocate);
    if (FAILED(hr))
    {
        // Venus passes no resource for its own memory and every VkDeviceMemory a runtime resource did not ask for.
        umd_log("allocate: the core layer's pfnAllocateCb failed with hResource %s, %u allocation(s): 0x%08lx",
                allocate.hResource == NULL ? "NULL" : "set",
                (unsigned)allocate.NumAllocations,
                (unsigned long)hr);
        return hr;
    }
    umd_d3d12_allocate_results(&allocate, venus);
    return hr;
}

static HRESULT APIENTRY umd_deallocate_cb(HANDLE hRTDevice, const D3DDDICB_DEALLOCATE *pData)
{
    struct umd_device *dev = umd_device_of(hRTDevice);
    struct umd_d3d12_deallocate deallocate;
    D3D12DDI_HRTDEVICE device;

    if (dev == NULL || dev->coreLayer12 == NULL || dev->coreLayer12->pfnDeallocateCb == NULL || pData == NULL)
    {
        return E_INVALIDARG;
    }
    umd_d3d12_deallocate_args((const struct umd_kt_deallocate *)pData, &deallocate);
    device.handle = hRTDevice;
    return dev->coreLayer12->pfnDeallocateCb(device, (const D3D12DDICB_DEALLOCATE_0022 *)&deallocate);
}

// A blob map this process cannot hold is undone and fails, so that Venus never uses half a pointer.
static HRESULT APIENTRY umd_escape_cb(HANDLE hAdapter, const D3DDDICB_ESCAPE *pData)
{
    PFND3DDDI_ESCAPECB escape = pData != NULL ? umd_runtime_escape(pData->hDevice) : NULL;
    VIRTIO_WDDM_BLOB_MAP map;
    HRESULT hr;

    if (escape == NULL)
    {
        return E_INVALIDARG;
    }
    hr = escape(hAdapter, pData);
    if (SUCCEEDED(hr) && umd_blob_map_check(pData->pPrivateDriverData,
                                            pData->PrivateDriverDataSize,
                                            sizeof(void *),
                                            &map) == UMD_BLOB_MAP_TOO_HIGH)
    {
        D3DDDICB_ESCAPE undo = *pData;
        VIRTIO_WDDM_BLOB_MAP unmap = map;

        unmap.Flags = VIRTIO_WDDM_BLOB_MAP_FLAGS_UNMAP;
        undo.pPrivateDriverData = &unmap;
        undo.PrivateDriverDataSize = sizeof(unmap);
        if (FAILED(escape(hAdapter, &undo)))
        {
            umd_log("blob map: the unmap of 0x%llx failed", (unsigned long long)map.Ptr);
        }
        map.Ptr = 0;
        memcpy(pData->pPrivateDriverData, &map, sizeof(map));
        umd_log("blob map: the kernel driver mapped allocation 0x%x at 0x%llx, past this 32-bit process's 4 GiB",
                (unsigned)map.Handle,
                (unsigned long long)unmap.Ptr);
        return E_FAIL;
    }
    return hr;
}

// Venus allocates through this: it gives the kernel allocation of a resource being created back to that resource.
static HRESULT APIENTRY umd_allocate_cb(HANDLE hRTDevice, D3DDDICB_ALLOCATE *pData)
{
    struct umd_device *dev = umd_device_of(hRTDevice);
    struct umd_vk_pending *pending = umd_pending;
    HRESULT hr;

    if (dev == NULL || pData == NULL)
    {
        return E_INVALIDARG;
    }
    if (dev->coreLayer12 != NULL)
    {
        hr = umd_allocate12(dev, hRTDevice, pData);
    }
    else if (dev->runtimeAllocateCb != NULL)
    {
        hr = dev->runtimeAllocateCb(hRTDevice, pData);
    }
    else
    {
        return E_INVALIDARG;
    }
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
    dev->runtimeEscapeCb = runtime->pfnEscapeCb;
    if (runtime->pfnEscapeCb != NULL)
    {
        dev->ktCallbacks.pfnEscapeCb = umd_escape_cb;
    }

    AcquireSRWLockExclusive(&umd_devices_lock);
    dev->next = umd_devices;
    umd_devices = dev;
    ReleaseSRWLockExclusive(&umd_devices_lock);
}

void umd_device_callbacks_init12(struct umd_device *dev,
                                 HANDLE hRTDevice,
                                 const D3DDDI_DEVICECALLBACKS *runtime,
                                 const struct D3D12DDI_CORELAYER_DEVICECALLBACKS_0022 *coreLayer)
{
    dev->coreLayer12 = coreLayer;
    umd_device_callbacks_init(dev, hRTDevice, runtime);
    dev->ktCallbacks.pfnDeallocateCb = umd_deallocate_cb;
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
