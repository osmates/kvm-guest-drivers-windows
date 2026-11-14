/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "umd_vk_chain.h"

// A longer chain is taken as a cycle.
#define UMD_VK_CHAIN_MAX 64

enum umd_vk_alloc_kind umd_vk_alloc_kind(const VkMemoryAllocateInfo *info)
{
    BOOL exported = FALSE, dedicatedImage = FALSE, imported = FALSE;
    const VkBaseInStructure *s = (const VkBaseInStructure *)info->pNext;

    for (UINT n = 0; s != NULL; s = s->pNext, n++)
    {
        if (n == UMD_VK_CHAIN_MAX)
        {
            return UMD_VK_ALLOC_OTHER;
        }
        switch ((UINT32)s->sType)
        {
            case VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO:
                exported = TRUE;
                break;
            case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO:
                dedicatedImage = ((const VkMemoryDedicatedAllocateInfo *)s)->image != VK_NULL_HANDLE;
                break;
            case VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR:
                imported = TRUE;
                break;
            case (UINT32)UMD_VK_STRUCTURE_TYPE_D3DDDI_CREATE_RESOURCE_MESA:
            case (UINT32)UMD_VK_STRUCTURE_TYPE_D3DDDI_OPEN_RESOURCE_MESA:
                return UMD_VK_ALLOC_OTHER;
        }
    }
    if (imported)
    {
        return UMD_VK_ALLOC_IMPORT;
    }
    if (exported && dedicatedImage)
    {
        return UMD_VK_ALLOC_SHARED_IMAGE;
    }
    return UMD_VK_ALLOC_OTHER;
}

void umd_vk_chain_prepend(const VkMemoryAllocateInfo *info, VkMemoryAllocateInfo *copy, VkBaseOutStructure *ext)
{
    *copy = *info;
    ext->pNext = (VkBaseOutStructure *)info->pNext;
    copy->pNext = ext;
}
