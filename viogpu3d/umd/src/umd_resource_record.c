/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "umd_resource_record.h"

#include <string.h>

// D3D11's own limits: D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION, _ARRAY_AXIS_DIMENSION and the mip count of 16384.
#define UMD_MAX_TEXTURE_SIZE 16384u
#define UMD_MAX_ARRAY_SIZE   2048u
#define UMD_MAX_MIP_LEVELS   15u
#define UMD_MAX_SAMPLE_COUNT 32u

void umd_resource_record_fill(UMD_RESOURCE_RECORD *record, const struct umd_texture_desc *desc)
{
    memset(record, 0, sizeof(*record));
    record->Magic = UMD_RESOURCE_RECORD_MAGIC;
    record->Size = sizeof(*record);
    record->Width = desc->width;
    record->Height = desc->height;
    record->MipLevels = desc->mip_levels;
    record->ArraySize = desc->array_size;
    record->Format = desc->format;
    record->SampleCount = desc->sample_count;
    record->Usage = desc->usage;
    record->BindFlags = desc->bind_flags;
    record->CpuAccessFlags = desc->cpu_access_flags;
    record->MiscFlags = desc->misc_flags;
}

static BOOL umd_power_of_two(UINT32 value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

enum umd_open_check umd_resource_record_open(const void *data,
                                             size_t size,
                                             UINT32 hAllocation,
                                             VIRTIO_WDDM_RESOURCE_INFO *info,
                                             struct umd_texture_desc *desc)
{
    VIRTIO_WDDM_CREATE_ALLOCATION create;
    UMD_RESOURCE_RECORD record;

    if (data == NULL || size < sizeof(create))
    {
        return UMD_OPEN_SHORT;
    }
    memcpy(&create, data, sizeof(create));
    if (create.Base.Tag != VIRTIO_WDDM_ALLOCATE_BLOB_TAG)
    {
        return UMD_OPEN_NOT_BLOB;
    }
    if (create.Base.Blob.Mem != VIRTIO_WDDM_BLOB_MEM_HOST3D || create.Base.Blob.Size == 0)
    {
        return UMD_OPEN_NOT_HOST3D;
    }
    if ((create.Base.Blob.Flags & VIRTIO_WDDM_BLOB_FLAG_SHAREABLE) == 0)
    {
        return UMD_OPEN_NOT_SHAREABLE;
    }
    if (create.ResId == 0)
    {
        return UMD_OPEN_NO_RESOURCE;
    }
    if (size - sizeof(create) < sizeof(record))
    {
        return UMD_OPEN_NO_RECORD;
    }
    memcpy(&record, (const UINT8 *)data + sizeof(create), sizeof(record));
    if (record.Magic != UMD_RESOURCE_RECORD_MAGIC)
    {
        return UMD_OPEN_NO_RECORD;
    }
    if (record.Size != sizeof(record) || record.Width == 0 || record.Width > UMD_MAX_TEXTURE_SIZE ||
        record.Height == 0 || record.Height > UMD_MAX_TEXTURE_SIZE || record.MipLevels == 0 ||
        record.MipLevels > UMD_MAX_MIP_LEVELS || record.ArraySize == 0 || record.ArraySize > UMD_MAX_ARRAY_SIZE ||
        !umd_power_of_two(record.SampleCount) || record.SampleCount > UMD_MAX_SAMPLE_COUNT || record.Format == 0)
    {
        return UMD_OPEN_BAD_RECORD;
    }

    memset(info, 0, sizeof(*info));
    info->Tag = VIRTIO_WDDM_ESCAPE_RESOURCE_INFO_TAG;
    info->Handle = hAllocation;
    info->Info = create;

    desc->width = record.Width;
    desc->height = record.Height;
    desc->mip_levels = record.MipLevels;
    desc->array_size = record.ArraySize;
    desc->format = record.Format;
    desc->sample_count = record.SampleCount;
    desc->usage = record.Usage;
    desc->bind_flags = record.BindFlags;
    desc->cpu_access_flags = record.CpuAccessFlags;
    desc->misc_flags = record.MiscFlags;
    return UMD_OPEN_OK;
}

const char *umd_open_check_text(enum umd_open_check check)
{
    switch (check)
    {
        case UMD_OPEN_OK:
            return "a shared Venus texture";
        case UMD_OPEN_SHORT:
            return "the allocation's private data is too short";
        case UMD_OPEN_NOT_BLOB:
            return "the allocation is not a blob (a kernel driver surface or a virgl resource)";
        case UMD_OPEN_NOT_HOST3D:
            return "the blob is not host memory";
        case UMD_OPEN_NOT_SHAREABLE:
            return "the blob is not shareable";
        case UMD_OPEN_NO_RESOURCE:
            return "the allocation has no resource id";
        case UMD_OPEN_NO_RECORD:
            return "the allocation carries no texture description";
        case UMD_OPEN_BAD_RECORD:
            return "the texture description is out of range";
    }
    return "unknown";
}
