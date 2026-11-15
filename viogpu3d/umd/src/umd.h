/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "../triton/triton.h"

#include <dxgi1_6.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "umd_adapter_info.h"
#include "umd_d3dddi_ext.h"
#include "umd_vk_chain.h"

void umd_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
void umd_log_line(const char *line);

// This driver's own module, for the path of the Venus ICD next to it.
HMODULE umd_module(void);

struct umd_vk;

struct umd_adapter
{
    HANDLE hRTAdapter;
    const D3DDDI_ADAPTERCALLBACKS *pAdapterCallbacks;
    VIRTIO_WDDM_ADAPTER_INFO info;
    LUID luid;
    // This process's GetCaps instance, held from this adapter's first GetCaps until CloseAdapter.
    struct umd_vk *caps;
};

// One Venus instance and the DXGI factory DXVK made on it.
struct umd_vk
{
    VkInstance instance;
    PFN_vkDestroyInstance destroyInstance;
    IDXGIFactory4 *factory;
};

// callbacks NULL: Venus opens the adapter itself through D3DKMT, for a device outside the runtime.
HRESULT umd_vk_create(const VkD3DDDICallbacksMESA *callbacks, struct umd_vk *vk);
void umd_vk_destroy(struct umd_vk *vk);

// While one is open on a thread, the allocations of its kind on that thread carry ext, and the kernel allocation the
// runtime makes for hRTResource is written to *pAllocation.
struct umd_vk_pending
{
    enum umd_vk_alloc_kind kind;
    VkBaseOutStructure *ext;
    HANDLE hRTDevice;
    HANDLE hRTResource;
    D3DKMT_HANDLE *pAllocation;
    BOOL chained;
};

void umd_vk_pending_begin(struct umd_vk_pending *pending);
void umd_vk_pending_end(struct umd_vk_pending *pending);

struct D3D12DDI_CORELAYER_DEVICECALLBACKS_0022;

struct umd_device
{
    struct umd_device *next;
    HANDLE hRTDevice;
    PFND3DDDI_ALLOCATECB runtimeAllocateCb;
    PFND3DDDI_ESCAPECB runtimeEscapeCb;
    // Direct3D 12: allocations go through the core layer, whose kernel-thunk table has no pfnAllocateCb.
    const struct D3D12DDI_CORELAYER_DEVICECALLBACKS_0022 *coreLayer12;
    // The runtime's callbacks, two of them wrapped; Venus keeps a pointer to callbacks for the instance's life.
    D3DDDI_DEVICECALLBACKS ktCallbacks;
    VkD3DDDICallbacksMESA callbacks;
    struct umd_vk vk;
};

void umd_device_callbacks_init(struct umd_device *dev, HANDLE hRTDevice, const D3DDDI_DEVICECALLBACKS *runtime);
// The Direct3D 12 table: allocations and deallocations through coreLayer12, in their Direct3D 12 forms.
void umd_device_callbacks_init12(struct umd_device *dev,
                                 HANDLE hRTDevice,
                                 const D3DDDI_DEVICECALLBACKS *runtime,
                                 const struct D3D12DDI_CORELAYER_DEVICECALLBACKS_0022 *coreLayer);
void umd_device_callbacks_fini(struct umd_device *dev);

// The ICD's vkGetInstanceProcAddr with this driver's hooks, which vkd3d-proton loads Vulkan through; NULL without
// the ICD.
PFN_vkGetInstanceProcAddr umd_vk_loader(void);

// The ICD's device entry points come from an instance of it: the first one this driver sees.
void umd_vk_note_instance(VkInstance instance);

// The ICD's own entry points, without this driver's hooks.
PFN_vkVoidFunction umd_vk_icd_instance_proc(VkInstance instance, const char *name);
PFN_vkVoidFunction umd_vk_icd_device_proc(VkDevice device, const char *name);
