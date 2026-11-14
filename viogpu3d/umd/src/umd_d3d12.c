/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "umd_d3d12.h"
#include "umd_format.h"

#include <string.h>

// D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION.
#define UMD_D3D12_MAX_TEXTURE_SIZE 16384u

enum umd_d3d12_shared_check umd_d3d12_shared_desc(const struct umd_d3d12_texture *texture,
                                                  BOOL primary,
                                                  struct umd_texture_desc *out)
{
    memset(out, 0, sizeof(*out));
    if (texture->depth_or_array_size > 1 || texture->mip_levels > 1 || texture->sample_count > 1)
    {
        return UMD_D3D12_SHARED_NOT_ONE_PLANE;
    }
    if (texture->width == 0 || texture->height == 0 || texture->width > UMD_D3D12_MAX_TEXTURE_SIZE ||
        texture->height > UMD_D3D12_MAX_TEXTURE_SIZE)
    {
        return UMD_D3D12_SHARED_TOO_LARGE;
    }
    // A linear image is never a depth target.
    if (texture->flags & UMD_D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
    {
        return UMD_D3D12_SHARED_DEPTH_STENCIL;
    }
    out->format = umd_shared_format(texture->format);
    if (primary && umd_scanout_vk_format(out->format) == 0)
    {
        return UMD_D3D12_SHARED_NOT_SCANOUT;
    }
    out->width = (UINT)texture->width;
    out->height = texture->height;
    out->mip_levels = 1;
    out->array_size = 1;
    out->sample_count = 1;
    out->usage = VIOGPU3D_D3D11_USAGE_DEFAULT;
    if (!(texture->flags & UMD_D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
    {
        out->bind_flags |= VIOGPU3D_D3D11_BIND_SHADER_RESOURCE;
    }
    if (texture->flags & UMD_D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
    {
        out->bind_flags |= VIOGPU3D_D3D11_BIND_RENDER_TARGET;
    }
    if (texture->flags & UMD_D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
    {
        out->bind_flags |= UMD_D3D11_BIND_UNORDERED_ACCESS;
    }
    out->misc_flags = VIOGPU3D_D3D11_RESOURCE_MISC_SHARED;
    out->kind = VIOGPU3D_SURFACE_SHARED_TEXTURE;
    out->flags = VIOGPU3D_SURFACE_FLAG_LINEAR;
    return UMD_D3D12_SHARED_OK;
}

const char *umd_d3d12_shared_check_text(enum umd_d3d12_shared_check check)
{
    switch (check)
    {
        case UMD_D3D12_SHARED_OK:
            return "ok";
        case UMD_D3D12_SHARED_NOT_ONE_PLANE:
            return "more than one level, layer or sample: a shared surface is one linear plane";
        case UMD_D3D12_SHARED_TOO_LARGE:
            return "empty, or larger than a Direct3D 12 texture can be";
        case UMD_D3D12_SHARED_DEPTH_STENCIL:
            return "a depth-stencil target, which a linear image cannot be";
        case UMD_D3D12_SHARED_NOT_SCANOUT:
            return "a primary in a format the kernel driver does not scan out";
    }
    return "unknown";
}

VkImageUsageFlags umd_d3d12_shared_usage(UINT flags)
{
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    if (!(flags & UMD_D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
    {
        usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (flags & UMD_D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
    {
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (flags & UMD_D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
    {
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    return usage;
}

UINT umd_d3d12_view_formats(UINT dxgiFormat, VkFormat *formats, UINT capacity)
{
    VkFormat own = VK_FORMAT_UNDEFINED, srgb = VK_FORMAT_UNDEFINED;
    UINT count = 0;

    switch (umd_shared_format(dxgiFormat))
    {
        case UMD_DXGI_FORMAT_R8G8B8A8_UNORM:
            own = VK_FORMAT_R8G8B8A8_UNORM;
            srgb = VK_FORMAT_R8G8B8A8_SRGB;
            break;
        case UMD_DXGI_FORMAT_B8G8R8A8_UNORM:
        case UMD_DXGI_FORMAT_B8G8R8X8_UNORM:
            own = VK_FORMAT_B8G8R8A8_UNORM;
            srgb = VK_FORMAT_B8G8R8A8_SRGB;
            break;
        default:
            return 0;
    }
    if (count < capacity)
    {
        formats[count++] = own;
    }
    if (count < capacity)
    {
        formats[count++] = srgb;
    }
    return count;
}

int umd_pick_physical_device(const VkPhysicalDeviceIDProperties *ids, UINT count, LUID adapter)
{
    UINT8 luid[VK_LUID_SIZE];
    BOOL anyLuid = FALSE;

    memcpy(luid, &adapter, sizeof(luid));
    for (UINT i = 0; i < count; i++)
    {
        if (!ids[i].deviceLUIDValid)
        {
            continue;
        }
        anyLuid = TRUE;
        if (memcmp(ids[i].deviceLUID, luid, sizeof(luid)) == 0)
        {
            return (int)i;
        }
    }
    return !anyLuid && count == 1 ? 0 : -1;
}

int umd_pick_memory_type(UINT32 typeBits,
                         const VkPhysicalDeviceMemoryProperties *properties,
                         VkMemoryPropertyFlags want)
{
    int first = -1;

    for (UINT32 i = 0; i < properties->memoryTypeCount && i < 32; i++)
    {
        if (!(typeBits & (1u << i)))
        {
            continue;
        }
        if ((properties->memoryTypes[i].propertyFlags & want) == want)
        {
            return (int)i;
        }
        if (first < 0)
        {
            first = (int)i;
        }
    }
    return first;
}

void umd_empty_allocation_fill(VIRTIO_WDDM_CREATE_ALLOCATION *create)
{
    memset(create, 0, sizeof(*create));
    create->Base.Tag = VIOGPU3D_ALLOCATE_EMPTY_TAG;
}

BOOL umd_d3d12_allocate_args(const struct umd_kt_allocate *in,
                             struct umd_d3d12_allocation_info *infos,
                             struct umd_d3d12_allocate *out)
{
    if (in->NumAllocations == 0 || in->NumAllocations > UMD_D3D12_ALLOCATE_MAX || in->pAllocationInfo == NULL)
    {
        return FALSE;
    }
    memset(infos, 0, UMD_D3D12_ALLOCATE_MAX * sizeof(*infos));
    for (UINT i = 0; i < in->NumAllocations; i++)
    {
        const struct umd_kt_allocation_info *from = &in->pAllocationInfo[i];
        struct umd_d3d12_allocation_info *to = &infos[i];

        to->pSystemMem = from->pSystemMem;
        to->pPrivateDriverData = from->pPrivateDriverData;
        to->PrivateDriverDataSize = from->PrivateDriverDataSize;
        to->VidPnSourceId = from->VidPnSourceId;
        to->Flags |= (from->Flags & UMD_KT_ALLOCATION_PRIMARY) ? UMD_D3D12_ALLOCATION_PRIMARY : 0;
        to->Flags |= (from->Flags & UMD_KT_ALLOCATION_STEREO) ? UMD_D3D12_ALLOCATION_STEREO : 0;
    }
    memset(out, 0, sizeof(*out));
    out->pPrivateDriverData = in->pPrivateDriverData;
    out->PrivateDriverDataSize = in->PrivateDriverDataSize;
    out->hResource = in->hResource;
    out->NumAllocations = in->NumAllocations;
    out->pAllocationInfo = infos;
    return TRUE;
}

void umd_d3d12_allocate_results(const struct umd_d3d12_allocate *done, struct umd_kt_allocate *in)
{
    in->hKMResource = done->hKMResource;
    for (UINT i = 0; i < in->NumAllocations && i < done->NumAllocations; i++)
    {
        in->pAllocationInfo[i].hAllocation = done->pAllocationInfo[i].hAllocation;
    }
}

void umd_d3d12_deallocate_args(const struct umd_kt_deallocate *in, struct umd_d3d12_deallocate *out)
{
    memset(out, 0, sizeof(*out));
    out->hResource = in->hResource;
    out->NumAllocations = in->NumAllocations;
    out->HandleList = in->HandleList;
}

static size_t umd_wide_length(const WCHAR *s)
{
    size_t n = 0;

    while (s[n] != 0)
    {
        n++;
    }
    return n;
}

static BOOL umd_wide_append(WCHAR *out, size_t capacity, size_t *length, const WCHAR *part)
{
    size_t n = umd_wide_length(part);

    if (*length + n + 1 > capacity)
    {
        return FALSE;
    }
    memcpy(out + *length, part, n * sizeof(WCHAR));
    *length += n;
    out[*length] = 0;
    return TRUE;
}

enum umd_cache_rule umd_vkd3d_cache_rule(BOOL userSet, const WCHAR *localAppData, WCHAR *folder, size_t capacity)
{
    size_t length = 0;

    if (userSet)
    {
        return UMD_CACHE_KEEP;
    }
    if (localAppData == NULL || localAppData[0] == 0 || folder == NULL || capacity == 0)
    {
        return UMD_CACHE_OFF;
    }
    folder[0] = 0;
    // A relative LOCALAPPDATA would put the cache in the current folder again.
    if (!(umd_wide_length(localAppData) >= 3 && localAppData[1] == L':' &&
          (localAppData[2] == L'\\' || localAppData[2] == L'/')) &&
        !(localAppData[0] == L'\\' && localAppData[1] == L'\\'))
    {
        return UMD_CACHE_OFF;
    }
    if (!umd_wide_append(folder, capacity, &length, localAppData))
    {
        return UMD_CACHE_OFF;
    }
    if (folder[length - 1] != L'\\' && folder[length - 1] != L'/' && !umd_wide_append(folder, capacity, &length, L"\\"))
    {
        return UMD_CACHE_OFF;
    }
    if (!umd_wide_append(folder, capacity, &length, UMD_VKD3D_CACHE_PARENT L"\\" UMD_VKD3D_CACHE_LEAF))
    {
        folder[0] = 0;
        return UMD_CACHE_OFF;
    }
    return UMD_CACHE_FOLDER;
}
