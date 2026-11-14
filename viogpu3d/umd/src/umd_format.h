/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "umd_types.h"

// DXGI_FORMAT values; umd_backend.c checks them against dxgiformat.h.
#define UMD_DXGI_FORMAT_R10G10B10A2_TYPELESS 23u
#define UMD_DXGI_FORMAT_R10G10B10A2_UNORM    24u
#define UMD_DXGI_FORMAT_R8G8B8A8_TYPELESS    27u
#define UMD_DXGI_FORMAT_R8G8B8A8_UNORM       28u
#define UMD_DXGI_FORMAT_R8G8B8A8_UNORM_SRGB  29u
#define UMD_DXGI_FORMAT_B8G8R8A8_UNORM       87u
#define UMD_DXGI_FORMAT_B8G8R8X8_UNORM       88u
#define UMD_DXGI_FORMAT_B8G8R8A8_TYPELESS    90u
#define UMD_DXGI_FORMAT_B8G8R8A8_UNORM_SRGB  91u
#define UMD_DXGI_FORMAT_B8G8R8X8_TYPELESS    92u
#define UMD_DXGI_FORMAT_B8G8R8X8_UNORM_SRGB  93u

// Shared and scanout textures take the UNORM format of an sRGB or typeless one; views keep the app's.
UINT umd_shared_format(UINT dxgiFormat);

// The VkFormat DXVK makes for a format the kernel driver can scan out, after umd_shared_format; 0 for any other.
// Venus describes a blob to the kernel driver only for these.
UINT umd_scanout_vk_format(UINT dxgiFormat);
