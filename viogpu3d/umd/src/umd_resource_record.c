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

void umd_resource_record_fill(VIOGPU3D_SURFACE *record, const struct umd_texture_desc *desc)
{
    memset(record, 0, sizeof(*record));
    record->Magic = VIOGPU3D_SURFACE_MAGIC;
    record->Size = sizeof(*record);
    record->Kind = VIOGPU3D_SURFACE_SHARED_TEXTURE;
    record->Flags = desc->flags & VIOGPU3D_SURFACE_FLAG_LINEAR;
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

BOOL umd_texture_can_be_linear(const struct umd_texture_desc *desc)
{
    return desc->mip_levels == 1 && desc->array_size == 1 && desc->sample_count == 1;
}

static BOOL umd_power_of_two(UINT32 value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

static BOOL umd_record_has(size_t size, VIOGPU3D_SURFACE *record, const void *data)
{
    if (size - sizeof(VIRTIO_WDDM_CREATE_ALLOCATION) < sizeof(*record))
    {
        return FALSE;
    }
    memcpy(record, (const UINT8 *)data + sizeof(VIRTIO_WDDM_CREATE_ALLOCATION), sizeof(*record));
    return record->Magic == VIOGPU3D_SURFACE_MAGIC;
}

enum umd_open_check umd_resource_record_open(const void *data,
                                             size_t size,
                                             UINT32 hAllocation,
                                             VIRTIO_WDDM_RESOURCE_INFO *info,
                                             struct umd_texture_desc *desc)
{
    VIRTIO_WDDM_CREATE_ALLOCATION create;
    VIOGPU3D_SURFACE record;
    BOOL hasRecord;

    if (data == NULL || size < sizeof(create))
    {
        return UMD_OPEN_SHORT;
    }
    memcpy(&create, data, sizeof(create));
    hasRecord = umd_record_has(size, &record, data);
    if (hasRecord && record.Kind == VIOGPU3D_SURFACE_KERNEL_CPU)
    {
        return UMD_OPEN_KERNEL_CPU_SURFACE;
    }
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
    if (!hasRecord)
    {
        return UMD_OPEN_NO_RECORD;
    }
    if (record.Size != sizeof(record) || record.Width == 0 || record.Width > UMD_MAX_TEXTURE_SIZE ||
        record.Height == 0 || record.Height > UMD_MAX_TEXTURE_SIZE || record.MipLevels == 0 ||
        record.MipLevels > UMD_MAX_MIP_LEVELS || record.ArraySize == 0 || record.ArraySize > UMD_MAX_ARRAY_SIZE ||
        !umd_power_of_two(record.SampleCount) || record.SampleCount > UMD_MAX_SAMPLE_COUNT || record.Format == 0 ||
        (record.Kind != VIOGPU3D_SURFACE_SHARED_TEXTURE && record.Kind != VIOGPU3D_SURFACE_KERNEL_BLOB) ||
        (record.Flags & ~VIOGPU3D_SURFACE_FLAG_LINEAR) != 0)
    {
        return UMD_OPEN_BAD_RECORD;
    }
    if ((record.Flags & VIOGPU3D_SURFACE_FLAG_LINEAR) &&
        (record.MipLevels != 1 || record.ArraySize != 1 || record.SampleCount != 1))
    {
        return UMD_OPEN_BAD_RECORD;
    }
    if (record.Kind == VIOGPU3D_SURFACE_KERNEL_BLOB &&
        (!(record.Flags & VIOGPU3D_SURFACE_FLAG_LINEAR) || record.Pitch < (UINT64)record.Width * 4 ||
         (UINT64)record.Pitch * record.Height > create.Base.Blob.Size))
    {
        return UMD_OPEN_BAD_RECORD;
    }

    memset(info, 0, sizeof(*info));
    info->Tag = VIRTIO_WDDM_ESCAPE_RESOURCE_INFO_TAG;
    info->Handle = hAllocation;
    info->Info = create;

    memset(desc, 0, sizeof(*desc));
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
    desc->kind = record.Kind;
    desc->flags = record.Flags;
    desc->pitch = record.Kind == VIOGPU3D_SURFACE_KERNEL_BLOB ? record.Pitch : 0;
    desc->standard_type = record.Kind == VIOGPU3D_SURFACE_KERNEL_BLOB ? record.StandardType : 0;
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
        case UMD_OPEN_KERNEL_CPU_SURFACE:
            return "the kernel driver's shadow or staging surface, guest memory only its present copies";
        case UMD_OPEN_NOT_BLOB:
            return "the allocation is not a blob (a virgl resource)";
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
