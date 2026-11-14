/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "umd_types.h"

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
    // VIOGPU3D_SURFACE_*.
    UINT kind;
    UINT flags;
    // Known for the kernel driver's surfaces only.
    UINT pitch;
    UINT standard_type;
};

void umd_resource_record_fill(VIOGPU3D_SURFACE *record, const struct umd_texture_desc *desc);

BOOL umd_texture_can_be_linear(const struct umd_texture_desc *desc);

enum umd_open_check
{
    UMD_OPEN_OK,
    UMD_OPEN_SHORT,
    UMD_OPEN_KERNEL_CPU_SURFACE,
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
