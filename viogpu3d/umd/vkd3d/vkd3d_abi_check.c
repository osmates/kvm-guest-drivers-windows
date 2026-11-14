/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

// Compiled by Tools/build-macos.sh against the vkd3d-proton tree it links, never linked: the driver's copy of
// vkd3d-proton's API (src/umd_vkd3d_api.h) has the layouts of vkd3d-proton's own headers.

#include <vkd3d.h>
#include <vkd3d_device_vkd3d_ext.h>

#include "umd_vkd3d_api.h"

#include <stddef.h>

#define UMD_SAME_FIELD(theirs, ours, field)                                                                            \
    _Static_assert(offsetof(theirs, field) == offsetof(ours, field), #field " moved")

_Static_assert(sizeof(struct vkd3d_instance_create_info) == sizeof(struct umd_vkd3d_instance_create_info),
               "vkd3d_instance_create_info changed size");
UMD_SAME_FIELD(struct vkd3d_instance_create_info, struct umd_vkd3d_instance_create_info, pfn_vkGetInstanceProcAddr);
UMD_SAME_FIELD(struct vkd3d_instance_create_info, struct umd_vkd3d_instance_create_info, instance_extensions);
UMD_SAME_FIELD(struct vkd3d_instance_create_info, struct umd_vkd3d_instance_create_info, instance_extension_count);
UMD_SAME_FIELD(struct vkd3d_instance_create_info, struct umd_vkd3d_instance_create_info, optional_instance_extensions);
UMD_SAME_FIELD(struct vkd3d_instance_create_info,
               struct umd_vkd3d_instance_create_info,
               optional_instance_extension_count);

_Static_assert(sizeof(struct vkd3d_device_create_info) == sizeof(struct umd_vkd3d_device_create_info),
               "vkd3d_device_create_info changed size");
UMD_SAME_FIELD(struct vkd3d_device_create_info, struct umd_vkd3d_device_create_info, minimum_feature_level);
UMD_SAME_FIELD(struct vkd3d_device_create_info, struct umd_vkd3d_device_create_info, instance);
UMD_SAME_FIELD(struct vkd3d_device_create_info, struct umd_vkd3d_device_create_info, instance_create_info);
UMD_SAME_FIELD(struct vkd3d_device_create_info, struct umd_vkd3d_device_create_info, vk_physical_device);
UMD_SAME_FIELD(struct vkd3d_device_create_info, struct umd_vkd3d_device_create_info, device_extensions);
UMD_SAME_FIELD(struct vkd3d_device_create_info, struct umd_vkd3d_device_create_info, device_extension_count);
UMD_SAME_FIELD(struct vkd3d_device_create_info, struct umd_vkd3d_device_create_info, optional_device_extensions);
UMD_SAME_FIELD(struct vkd3d_device_create_info, struct umd_vkd3d_device_create_info, optional_device_extension_count);
UMD_SAME_FIELD(struct vkd3d_device_create_info, struct umd_vkd3d_device_create_info, parent);
UMD_SAME_FIELD(struct vkd3d_device_create_info, struct umd_vkd3d_device_create_info, adapter_luid);
UMD_SAME_FIELD(struct vkd3d_device_create_info, struct umd_vkd3d_device_create_info, device_factory_flags);
UMD_SAME_FIELD(struct vkd3d_device_create_info, struct umd_vkd3d_device_create_info, independent);
_Static_assert(sizeof(D3D_FEATURE_LEVEL) == sizeof(INT32), "D3D_FEATURE_LEVEL is not 32 bits");
_Static_assert(sizeof(D3D12_DEVICE_FACTORY_FLAGS) == sizeof(UINT32), "D3D12_DEVICE_FACTORY_FLAGS is not 32 bits");

_Static_assert(sizeof(ID3D12DeviceExt1Vtbl) == sizeof(struct umd_vkd3d_device_ext1_vtbl),
               "ID3D12DeviceExt1 changed its methods");
UMD_SAME_FIELD(ID3D12DeviceExt1Vtbl, struct umd_vkd3d_device_ext1_vtbl, Release);
UMD_SAME_FIELD(ID3D12DeviceExt1Vtbl, struct umd_vkd3d_device_ext1_vtbl, CaptureUAVInfo);
UMD_SAME_FIELD(ID3D12DeviceExt1Vtbl, struct umd_vkd3d_device_ext1_vtbl, CreateResourceFromBorrowedHandle);
UMD_SAME_FIELD(ID3D12DeviceExt1Vtbl, struct umd_vkd3d_device_ext1_vtbl, GetVulkanQueueInfoEx);
