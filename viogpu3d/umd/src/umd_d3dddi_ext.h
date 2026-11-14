/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

// The structs a D3D driver on Venus chains into Vulkan calls: Mesa's src/vulkan/util/vk_internal_d3dddi_ext.h
// (merge request 38731), the same layouts. Include after the D3D DDI headers.

#include "umd_vk_chain.h"

// VkInstanceCreateInfo: Venus sends contexts, allocations and submits through the runtime's callbacks, not D3DKMT.
typedef struct
{
    VkStructureType sType;
    void *pNext;

    LUID AdapterLuid;

    HANDLE hRTAdapter;
    HANDLE hRTDevice;
    const D3DDDI_ADAPTERCALLBACKS *pAdapterCallbacks;
    const D3DDDI_DEVICECALLBACKS *pKTCallbacks;
    const DXGI_DDI_BASE_CALLBACKS *pDXGIBaseCallbacks;

    HANDLE hRTCoreLayer;
    const D3D11DDI_CORELAYER_DEVICECALLBACKS *p11UMCallbacks;
} VkD3DDDICallbacksMESA;

// VkMemoryAllocateInfo: the allocation belongs to the runtime's resource; the extra data follows the blob request.
typedef struct
{
    VkStructureType sType;
    void *pNext;
    HANDLE hRTResource;
    union {
        const D3D10DDIARG_CREATERESOURCE *pCreateResource10;
        const D3D11DDIARG_CREATERESOURCE *pCreateResource11;
    };
    void *pExtraData;
    size_t ExtraDataSize;
} VkD3DDDICreateResourceMESA;

// VkMemoryAllocateInfo: imports the runtime's opened allocation; pResourceInfo is a VIRTIO_WDDM_RESOURCE_INFO.
typedef struct
{
    VkStructureType sType;
    void *pNext;
    HANDLE hRTResource;
    const D3D10DDIARG_OPENRESOURCE *pOpenResource;
    const void *pResourceInfo;
} VkD3DDDIOpenResourceMESA;
