/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "umd_types.h"

// A shared texture's D3D11 description. It follows VIRTIO_WDDM_CREATE_ALLOCATION in the allocation's private data
// (Venus appends it as the extra data of VkD3DDDICreateResourceMESA) and reaches the process that opens the texture.
// Its magic differs from the kernel driver's own record at that offset, which the kernel driver then ignores.
#define UMD_RESOURCE_RECORD_MAGIC 0x55443356u

#pragma pack(push, 1)
typedef struct _UMD_RESOURCE_RECORD
{
    UINT32 Magic;
    UINT32 Size;
    UINT32 Width;
    UINT32 Height;
    UINT32 MipLevels;
    UINT32 ArraySize;
    UINT32 Format;
    UINT32 SampleCount;
    UINT32 Usage;
    UINT32 BindFlags;
    UINT32 CpuAccessFlags;
    UINT32 MiscFlags;
} UMD_RESOURCE_RECORD;
#pragma pack(pop)

C_ASSERT(sizeof(UMD_RESOURCE_RECORD) == 48);

struct umd_texture_desc
{
    UINT width;
    UINT height;
    UINT mip_levels;
    UINT array_size;
    UINT format;
    UINT sample_count;
    UINT usage;
    UINT bind_flags;
    UINT cpu_access_flags;
    UINT misc_flags;
};

void umd_resource_record_fill(UMD_RESOURCE_RECORD *record, const struct umd_texture_desc *desc);

enum umd_open_check
{
    UMD_OPEN_OK,
    UMD_OPEN_SHORT,
    UMD_OPEN_NOT_BLOB,
    UMD_OPEN_NOT_HOST3D,
    UMD_OPEN_NOT_SHAREABLE,
    UMD_OPEN_NO_RESOURCE,
    UMD_OPEN_NO_RECORD,
    UMD_OPEN_BAD_RECORD,
};

// An opened allocation's private data, as the runtime hands it over: the resource info Venus imports from, and the
// texture's description. Data from another process, so every field is checked.
enum umd_open_check umd_resource_record_open(const void *data,
                                             size_t size,
                                             UINT32 hAllocation,
                                             VIRTIO_WDDM_RESOURCE_INFO *info,
                                             struct umd_texture_desc *desc);

const char *umd_open_check_text(enum umd_open_check check);
