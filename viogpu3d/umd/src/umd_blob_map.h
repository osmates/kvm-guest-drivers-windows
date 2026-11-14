/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "umd_types.h"

enum umd_blob_map_check
{
    // Another escape, an unmap, or a map the kernel driver refused.
    UMD_BLOB_MAP_OTHER,
    UMD_BLOB_MAP_FITS,
    // A pointer this process cannot hold: a 32-bit process reads only its low half.
    UMD_BLOB_MAP_TOO_HIGH,
};

// An escape's private data as the kernel driver answered it, for a process whose pointers have pointerBytes bytes.
enum umd_blob_map_check umd_blob_map_check(const void *data,
                                           size_t size,
                                           size_t pointerBytes,
                                           VIRTIO_WDDM_BLOB_MAP *map);
