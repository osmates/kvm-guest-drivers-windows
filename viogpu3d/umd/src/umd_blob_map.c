/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "umd_blob_map.h"

#include <string.h>

enum umd_blob_map_check umd_blob_map_check(const void *data,
                                           size_t size,
                                           size_t pointerBytes,
                                           VIRTIO_WDDM_BLOB_MAP *map)
{
    memset(map, 0, sizeof(*map));
    if (data == NULL || size < sizeof(*map))
    {
        return UMD_BLOB_MAP_OTHER;
    }
    memcpy(map, data, sizeof(*map));
    if (map->Tag != VIRTIO_WDDM_ESCAPE_BLOB_MAP_TAG || (map->Flags & VIRTIO_WDDM_BLOB_MAP_FLAGS_UNMAP) != 0 ||
        map->Ptr == 0)
    {
        return UMD_BLOB_MAP_OTHER;
    }
    if (pointerBytes < sizeof(UINT64) && (map->Ptr >> (pointerBytes * 8)) != 0)
    {
        return UMD_BLOB_MAP_TOO_HIGH;
    }
    return UMD_BLOB_MAP_FITS;
}
