/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "umd_format.h"

#include <vulkan/vulkan_core.h>

UINT umd_shared_format(UINT dxgiFormat)
{
    switch (dxgiFormat)
    {
        case UMD_DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case UMD_DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return UMD_DXGI_FORMAT_R8G8B8A8_UNORM;
        case UMD_DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case UMD_DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return UMD_DXGI_FORMAT_B8G8R8A8_UNORM;
        case UMD_DXGI_FORMAT_B8G8R8X8_TYPELESS:
        case UMD_DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            return UMD_DXGI_FORMAT_B8G8R8X8_UNORM;
        case UMD_DXGI_FORMAT_R10G10B10A2_TYPELESS:
            return UMD_DXGI_FORMAT_R10G10B10A2_UNORM;
    }
    return dxgiFormat;
}

UINT umd_scanout_vk_format(UINT dxgiFormat)
{
    switch (dxgiFormat)
    {
        case UMD_DXGI_FORMAT_B8G8R8A8_UNORM:
        // DXVK stores X8 as A8 and swizzles alpha to one in its views.
        case UMD_DXGI_FORMAT_B8G8R8X8_UNORM:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case UMD_DXGI_FORMAT_R8G8B8A8_UNORM:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case UMD_DXGI_FORMAT_R10G10B10A2_UNORM:
            return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    }
    return VK_FORMAT_UNDEFINED;
}
