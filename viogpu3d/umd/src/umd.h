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
    // The instance that answered GetCaps; it lives until CloseAdapter.
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

struct umd_device
{
    struct umd_device *next;
    HANDLE hRTDevice;
    PFND3DDDI_ALLOCATECB runtimeAllocateCb;
    // The runtime's callbacks with pfnAllocateCb wrapped; Venus keeps a pointer to callbacks for the instance's life.
    D3DDDI_DEVICECALLBACKS ktCallbacks;
    VkD3DDDICallbacksMESA callbacks;
    struct umd_vk vk;
};

void umd_device_callbacks_init(struct umd_device *dev, HANDLE hRTDevice, const D3DDDI_DEVICECALLBACKS *runtime);
void umd_device_callbacks_fini(struct umd_device *dev);
