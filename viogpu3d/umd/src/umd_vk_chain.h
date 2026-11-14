/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "umd_types.h"

#include <vulkan/vulkan_core.h>

// Mesa's sType values for the D3DDDI structs of merge request 38731 (src/vulkan/util/vk_internal_d3dddi_ext.h).
#define UMD_VK_STRUCTURE_TYPE_D3DDDI_CALLBACKS_MESA       ((VkStructureType)4281808695u)
#define UMD_VK_STRUCTURE_TYPE_D3DDDI_CREATE_RESOURCE_MESA ((VkStructureType)4281808696u)
#define UMD_VK_STRUCTURE_TYPE_D3DDDI_OPEN_RESOURCE_MESA   ((VkStructureType)4281808697u)

enum umd_vk_alloc_kind
{
    UMD_VK_ALLOC_OTHER,
    // DXVK's storage of a shared texture: its own dedicated, exportable allocation.
    UMD_VK_ALLOC_SHARED_IMAGE,
    // DXVK's import of a shared texture.
    UMD_VK_ALLOC_IMPORT,
};

// An allocation that already names a D3D resource is OTHER: it is chained once.
enum umd_vk_alloc_kind umd_vk_alloc_kind(const VkMemoryAllocateInfo *info);

// Puts ext first in copy's chain, ahead of what info had; info itself is const and stays as it is.
void umd_vk_chain_prepend(const VkMemoryAllocateInfo *info, VkMemoryAllocateInfo *copy, VkBaseOutStructure *ext);
