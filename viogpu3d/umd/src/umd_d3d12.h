/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

// The platform-free parts of the Direct3D 12 driver, unit tested on the host.

#include "umd_resource_record.h"

#include <stdint.h>
#include <vulkan/vulkan_core.h>

// D3D12_RESOURCE_FLAGS values; umd_backend12.c checks them against d3d12.h.
#define UMD_D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET    0x1u
#define UMD_D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL    0x2u
#define UMD_D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS 0x4u
#define UMD_D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE   0x8u

// D3D11_BIND_* values the record carries for an opener; umd_backend.c checks them against d3d11.h.
#define UMD_D3D11_BIND_UNORDERED_ACCESS                0x80u

// The texture a shared surface stands for, as Direct3D 12 describes it.
struct umd_d3d12_texture
{
    UINT64 width;
    UINT height;
    UINT depth_or_array_size;
    UINT mip_levels;
    UINT format;
    UINT sample_count;
    UINT flags;
};

enum umd_d3d12_shared_check
{
    UMD_D3D12_SHARED_OK,
    UMD_D3D12_SHARED_NOT_ONE_PLANE,
    UMD_D3D12_SHARED_TOO_LARGE,
    UMD_D3D12_SHARED_DEPTH_STENCIL,
    UMD_D3D12_SHARED_NOT_SCANOUT,
};

// A shared surface is one linear plane, as every shared texture of this driver: what the D3D10 and 11 driver of a
// process that opens it reads from its record. A primary also needs a format the kernel driver scans out.
enum umd_d3d12_shared_check umd_d3d12_shared_desc(const struct umd_d3d12_texture *texture,
                                                  BOOL primary,
                                                  struct umd_texture_desc *out);

const char *umd_d3d12_shared_check_text(enum umd_d3d12_shared_check check);

// The Vulkan usage of a shared surface's image: copies always, then what the resource flags allow.
VkImageUsageFlags umd_d3d12_shared_usage(UINT flags);

// The formats views of a shared surface may take: its own, and the sRGB one of an 8-bit RGBA or BGRA format. Returns
// how many it wrote.
UINT umd_d3d12_view_formats(UINT dxgiFormat, VkFormat *formats, UINT capacity);

// The index of the Vulkan physical device of the adapter: the one whose LUID is the adapter's, else the only device
// there is when none reports a LUID; -1 when there is none of either.
int umd_pick_physical_device(const VkPhysicalDeviceIDProperties *ids, UINT count, LUID adapter);

// The first memory type typeBits allows with every flag of want, else the first it allows; -1 when it allows none.
int umd_pick_memory_type(UINT32 typeBits,
                         const VkPhysicalDeviceMemoryProperties *properties,
                         VkMemoryPropertyFlags want);

// The private data of a kernel allocation that names no memory (VIOGPU3D_ALLOCATE_EMPTY_TAG).
void umd_empty_allocation_fill(VIRTIO_WDDM_CREATE_ALLOCATION *create);

// What Venus passes the kernel-thunk pfnAllocateCb and pfnDeallocateCb (D3DDDI_ALLOCATIONINFO, D3DDDICB_ALLOCATE,
// D3DDDICB_DEALLOCATE), and what the Direct3D 12 core layer's take (D3D12DDI_ALLOCATION_INFO_0022,
// D3D12DDICB_ALLOCATE_0022, D3D12DDICB_DEALLOCATE_0022). umd_vk.c checks each layout against the kits' headers.
struct umd_kt_allocation_info
{
    UINT32 hAllocation;
    const void *pSystemMem;
    void *pPrivateDriverData;
    UINT PrivateDriverDataSize;
    UINT VidPnSourceId;
    UINT Flags;
};

struct umd_kt_allocate
{
    const void *pPrivateDriverData;
    UINT PrivateDriverDataSize;
    void *hResource;
    UINT32 hKMResource;
    UINT NumAllocations;
    struct umd_kt_allocation_info *pAllocationInfo;
};

struct umd_kt_deallocate
{
    void *hResource;
    UINT NumAllocations;
    const UINT32 *HandleList;
};

struct umd_d3d12_allocation_info
{
    UINT32 hAllocation;
    const void *pSystemMem;
    void *pPrivateDriverData;
    UINT PrivateDriverDataSize;
    UINT VidPnSourceId;
    UINT Flags;
    UINT64 GpuVirtualAddress;
    UINT Priority;
    uintptr_t Reserved[5];
};

struct umd_d3d12_allocate
{
    const void *pPrivateDriverData;
    UINT PrivateDriverDataSize;
    void *hResource;
    UINT32 hKMResource;
    UINT NumAllocations;
    struct umd_d3d12_allocation_info *pAllocationInfo;
};

struct umd_d3d12_deallocate
{
    void *hResource;
    UINT NumAllocations;
    const UINT32 *HandleList;
    UINT Flags;
};

// D3DDDI_ALLOCATIONINFO's Flags.Value bits, as d3dukmdt.h numbers them.
#define UMD_KT_ALLOCATION_PRIMARY    0x1u
#define UMD_KT_ALLOCATION_STEREO     0x2u
// D3D12DDI_ALLOCATION_INFO_FLAGS_0022 values; umd_vk.c checks them against d3d12umddi.h.
#define UMD_D3D12_ALLOCATION_PRIMARY 0x1u
#define UMD_D3D12_ALLOCATION_STEREO  0x2u

// The most allocations one allocate call may carry; Venus makes one at a time.
#define UMD_D3D12_ALLOCATE_MAX       4u

// Venus's allocate call in the core layer's form, its allocations written to infos, which holds
// UMD_D3D12_ALLOCATE_MAX. FALSE, with nothing written, when the call carries no allocation or more than infos holds.
BOOL umd_d3d12_allocate_args(const struct umd_kt_allocate *in,
                             struct umd_d3d12_allocation_info *infos,
                             struct umd_d3d12_allocate *out);

// What the core layer made, back into Venus's call: the kernel resource and each allocation's handle.
void umd_d3d12_allocate_results(const struct umd_d3d12_allocate *done, struct umd_kt_allocate *in);

void umd_d3d12_deallocate_args(const struct umd_kt_deallocate *in, struct umd_d3d12_deallocate *out);

enum umd_cache_rule
{
    // The user's own VKD3D_SHADER_CACHE_PATH stays.
    UMD_CACHE_KEEP,
    // A folder of the driver's under the user's local application data.
    UMD_CACHE_FOLDER,
    // 0, vkd3d-proton's switch that turns its disk cache off.
    UMD_CACHE_OFF,
};

// Where vkd3d-proton keeps its disk cache, which it otherwise writes into the process's current folder. userSet says
// the process has a VKD3D_SHADER_CACHE_PATH; localAppData is its LOCALAPPDATA, NULL when unset. folder gets the
// driver's folder for UMD_CACHE_FOLDER.
enum umd_cache_rule umd_vkd3d_cache_rule(BOOL userSet, const WCHAR *localAppData, WCHAR *folder, size_t capacity);

// The folders under LOCALAPPDATA, outermost first, that UMD_CACHE_FOLDER names.
#define UMD_VKD3D_CACHE_PARENT L"viogpu3d"
#define UMD_VKD3D_CACHE_LEAF   L"vkd3d-proton"
