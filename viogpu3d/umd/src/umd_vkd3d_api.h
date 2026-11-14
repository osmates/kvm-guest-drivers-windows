/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

// What this driver calls of vkd3d-proton, which it loads from viogpu3d_vkd3d.dll at run time: the vkd3d.h API and
// ID3D12DeviceExt1 of vkd3d-proton v3.1 (include/vkd3d.h, include/vkd3d_device_vkd3d_ext.idl), with the same layouts.
// vkd3d/vkd3d_abi_check.c checks them against those headers when the package is built. Only plain Windows and Vulkan
// types here, so that the check can include this beside vkd3d-proton's own headers.

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>

#include <vulkan/vulkan_core.h>

#ifndef STDMETHODCALLTYPE
#define STDMETHODCALLTYPE __stdcall
#endif

struct vkd3d_instance;

struct umd_vkd3d_instance_create_info
{
    PFN_vkGetInstanceProcAddr pfn_vkGetInstanceProcAddr;
    const char *const *instance_extensions;
    uint32_t instance_extension_count;
    const char *const *optional_instance_extensions;
    uint32_t optional_instance_extension_count;
};

struct umd_vkd3d_device_create_info
{
    // D3D_FEATURE_LEVEL.
    INT32 minimum_feature_level;
    struct vkd3d_instance *instance;
    const struct umd_vkd3d_instance_create_info *instance_create_info;
    VkPhysicalDevice vk_physical_device;
    const char *const *device_extensions;
    uint32_t device_extension_count;
    const char *const *optional_device_extensions;
    uint32_t optional_device_extension_count;
    // IUnknown.
    void *parent;
    LUID adapter_luid;
    // D3D12_DEVICE_FACTORY_FLAGS.
    UINT32 device_factory_flags;
    bool independent;
};

typedef HRESULT (*PFN_umd_vkd3d_create_instance)(const struct umd_vkd3d_instance_create_info *create_info,
                                                 struct vkd3d_instance **instance);
typedef ULONG (*PFN_umd_vkd3d_instance_decref)(struct vkd3d_instance *instance);
typedef VkInstance (*PFN_umd_vkd3d_instance_get_vk_instance)(struct vkd3d_instance *instance);
// The device, queue and resource arguments are the COM objects of vkd3d-proton's ID3D12Device.
typedef HRESULT (*PFN_umd_vkd3d_create_device)(const struct umd_vkd3d_device_create_info *create_info,
                                               const GUID *iid,
                                               void **device);
typedef VkDevice (*PFN_umd_vkd3d_get_vk_device)(void *device);
typedef VkPhysicalDevice (*PFN_umd_vkd3d_get_vk_physical_device)(void *device);
typedef struct vkd3d_instance *(*PFN_umd_vkd3d_instance_from_device)(void *device);
typedef VkQueue (*PFN_umd_vkd3d_acquire_vk_queue)(void *queue);
typedef void (*PFN_umd_vkd3d_release_vk_queue)(void *queue);
typedef void (*PFN_umd_vkd3d_enqueue_initial_transition)(void *queue, void *resource);
// D3D12_VERSIONED_ROOT_SIGNATURE_DESC, and ID3DBlob out.
typedef HRESULT (*PFN_umd_vkd3d_serialize_versioned_root_signature)(const void *desc, void **blob, void **error_blob);
// DXGI_FORMAT in.
typedef VkFormat (*PFN_umd_vkd3d_get_vk_format)(UINT32 format);

struct umd_vkd3d_api
{
    PFN_umd_vkd3d_create_instance create_instance;
    PFN_umd_vkd3d_instance_decref instance_decref;
    PFN_umd_vkd3d_instance_get_vk_instance instance_get_vk_instance;
    PFN_umd_vkd3d_create_device create_device;
    PFN_umd_vkd3d_get_vk_device get_vk_device;
    PFN_umd_vkd3d_get_vk_physical_device get_vk_physical_device;
    PFN_umd_vkd3d_instance_from_device instance_from_device;
    PFN_umd_vkd3d_acquire_vk_queue acquire_vk_queue;
    PFN_umd_vkd3d_release_vk_queue release_vk_queue;
    PFN_umd_vkd3d_enqueue_initial_transition enqueue_initial_transition;
    PFN_umd_vkd3d_serialize_versioned_root_signature serialize_versioned_root_signature;
    PFN_umd_vkd3d_get_vk_format get_vk_format;
};

// ID3D12DeviceExt1 of vkd3d-proton: uuid(099a73fd-2199-4f45-bf48-0eb86f6fdb65).
#define UMD_IID_ID3D12DEVICEEXT1_INIT                                                                                  \
    {                                                                                                                  \
        0x099a73fd, 0x2199, 0x4f45,                                                                                    \
        {                                                                                                              \
            0xbf, 0x48, 0x0e, 0xb8, 0x6f, 0x6f, 0xdb, 0x65                                                             \
        }                                                                                                              \
    }

struct umd_vkd3d_device_ext1;

// IUnknown, then ID3D12DeviceExt's seven methods, then ID3D12DeviceExt1's. Only the ones this driver calls are typed.
struct umd_vkd3d_device_ext1_vtbl
{
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(struct umd_vkd3d_device_ext1 *self, const GUID *iid, void **out);
    ULONG(STDMETHODCALLTYPE *AddRef)(struct umd_vkd3d_device_ext1 *self);
    ULONG(STDMETHODCALLTYPE *Release)(struct umd_vkd3d_device_ext1 *self);
    void *GetVulkanHandles;
    void *GetExtensionSupport;
    void *CreateCubinComputeShaderWithName;
    void *DestroyCubinComputeShader;
    void *GetCudaTextureObject;
    void *GetCudaSurfaceObject;
    void *CaptureUAVInfo;
    // D3D12_RESOURCE_DESC1 in, ID3D12Resource out.
    HRESULT(STDMETHODCALLTYPE *CreateResourceFromBorrowedHandle)
    (struct umd_vkd3d_device_ext1 *self, const void *desc, UINT64 vk_handle, void **resource);
    void *GetVulkanQueueInfoEx;
};

struct umd_vkd3d_device_ext1
{
    const struct umd_vkd3d_device_ext1_vtbl *lpVtbl;
};
