/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "umd_types.h"

enum umd_adapter_check
{
    UMD_ADAPTER_OK,
    UMD_ADAPTER_SHORT,
    UMD_ADAPTER_NOT_VIOGPU3D,
    UMD_ADAPTER_NO_3D,
    UMD_ADAPTER_NO_HOST_VISIBLE,
    UMD_ADAPTER_NO_VENUS,
};

// What the kernel driver answers to DXGKQAITYPE_UMDRIVERPRIVATE, checked for what Venus needs.
enum umd_adapter_check umd_adapter_info_check(const void *data, size_t size, VIRTIO_WDDM_ADAPTER_INFO *out);

const char *umd_adapter_check_text(enum umd_adapter_check check);

// The kernel driver packs the LUID as HighPart << 32 | LowPart.
LUID umd_adapter_luid(const VIRTIO_WDDM_ADAPTER_INFO *info);
