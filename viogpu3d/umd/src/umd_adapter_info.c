/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "umd_adapter_info.h"

#include <string.h>

enum umd_adapter_check umd_adapter_info_check(const void *data, size_t size, VIRTIO_WDDM_ADAPTER_INFO *out)
{
    VIRTIO_WDDM_ADAPTER_INFO info;

    if (data == NULL || size < sizeof(info))
    {
        return UMD_ADAPTER_SHORT;
    }
    memcpy(&info, data, sizeof(info));
    if (info.Tag != VIRTIO_WDDM_ADAPTER_INFO_TAG)
    {
        return UMD_ADAPTER_NOT_VIOGPU3D;
    }
    if (!info.Supports3d)
    {
        return UMD_ADAPTER_NO_3D;
    }
    if (!info.HasShmem)
    {
        return UMD_ADAPTER_NO_HOST_VISIBLE;
    }
    if ((info.CapsetMask & VIRTIO_WDDM_CAPSET_MASK(VIRTIO_WDDM_CAPSET_ID_VENUS)) == 0)
    {
        return UMD_ADAPTER_NO_VENUS;
    }
    if (out != NULL)
    {
        *out = info;
    }
    return UMD_ADAPTER_OK;
}

const char *umd_adapter_check_text(enum umd_adapter_check check)
{
    switch (check)
    {
        case UMD_ADAPTER_OK:
            return "a Venus adapter";
        case UMD_ADAPTER_SHORT:
            return "the kernel driver's adapter info is too short";
        case UMD_ADAPTER_NOT_VIOGPU3D:
            return "the kernel driver is not viogpu3d";
        case UMD_ADAPTER_NO_3D:
            return "the GPU has no 3D (VIRTIO_GPU_F_VIRGL)";
        case UMD_ADAPTER_NO_HOST_VISIBLE:
            return "the GPU has no host-visible region";
        case UMD_ADAPTER_NO_VENUS:
            return "the GPU offers no Venus capability set";
    }
    return "unknown";
}

LUID umd_adapter_luid(const VIRTIO_WDDM_ADAPTER_INFO *info)
{
    LUID luid;

    luid.LowPart = (DWORD)(info->Luid & 0xFFFFFFFFu);
    luid.HighPart = (LONG)(UINT32)(info->Luid >> 32);
    return luid;
}
