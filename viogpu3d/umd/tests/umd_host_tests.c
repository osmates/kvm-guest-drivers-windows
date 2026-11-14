/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "umd_adapter_info.h"
#include "umd_blob_map.h"
#include "umd_d3d12.h"
#include "umd_format.h"
#include "umd_path.h"
#include "umd_pipeline.h"
#include "umd_resource_record.h"
#include "umd_shared.h"
#include "umd_vk_chain.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static int failures;
static int checks;

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        checks++;                                                                                                      \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            failures++;                                                                                                \
            fprintf(stderr, "%s:%d: %s: failed: %s\n", __FILE__, __LINE__, __func__, #cond);                           \
        }                                                                                                              \
    } while (0)

static VIRTIO_WDDM_ADAPTER_INFO venus_adapter(void)
{
    VIRTIO_WDDM_ADAPTER_INFO info;

    memset(&info, 0, sizeof(info));
    info.Tag = VIRTIO_WDDM_ADAPTER_INFO_TAG;
    info.Luid = 0x0000000100000002ull;
    info.CapsetMask = VIRTIO_WDDM_CAPSET_MASK(VIRTIO_WDDM_CAPSET_ID_VIRGL2) |
                      VIRTIO_WDDM_CAPSET_MASK(VIRTIO_WDDM_CAPSET_ID_VENUS);
    info.Supports3d = 1;
    info.HasShmem = 1;
    return info;
}

static void adapterWithVenusIsTaken(void)
{
    VIRTIO_WDDM_ADAPTER_INFO info = venus_adapter(), out;

    memset(&out, 0, sizeof(out));
    CHECK(umd_adapter_info_check(&info, sizeof(info), &out) == UMD_ADAPTER_OK);
    CHECK(memcmp(&info, &out, sizeof(info)) == 0);
}

static void adapterAnswersAreCheckedInOrder(void)
{
    VIRTIO_WDDM_ADAPTER_INFO info = venus_adapter();

    CHECK(umd_adapter_info_check(NULL, sizeof(info), NULL) == UMD_ADAPTER_SHORT);
    CHECK(umd_adapter_info_check(&info, sizeof(info) - 1, NULL) == UMD_ADAPTER_SHORT);

    info.Tag = VIRTIO_WDDM_ESCAPE_CAPSET_TAG;
    CHECK(umd_adapter_info_check(&info, sizeof(info), NULL) == UMD_ADAPTER_NOT_VIOGPU3D);

    info = venus_adapter();
    info.Supports3d = 0;
    CHECK(umd_adapter_info_check(&info, sizeof(info), NULL) == UMD_ADAPTER_NO_3D);

    info = venus_adapter();
    info.HasShmem = 0;
    CHECK(umd_adapter_info_check(&info, sizeof(info), NULL) == UMD_ADAPTER_NO_HOST_VISIBLE);

    info = venus_adapter();
    info.CapsetMask = VIRTIO_WDDM_CAPSET_MASK(VIRTIO_WDDM_CAPSET_ID_VIRGL2);
    CHECK(umd_adapter_info_check(&info, sizeof(info), NULL) == UMD_ADAPTER_NO_VENUS);
}

static void adapterTextsNameEveryOutcome(void)
{
    for (int c = UMD_ADAPTER_OK; c <= UMD_ADAPTER_NO_VENUS; c++)
    {
        CHECK(strcmp(umd_adapter_check_text((enum umd_adapter_check)c), "unknown") != 0);
    }
}

static void adapterLuidSplitsAsTheKernelDriverPacksIt(void)
{
    VIRTIO_WDDM_ADAPTER_INFO info = venus_adapter();
    LUID luid = umd_adapter_luid(&info);

    CHECK(luid.LowPart == 2);
    CHECK(luid.HighPart == 1);

    info.Luid = 0xFFFFFFFF80000001ull;
    luid = umd_adapter_luid(&info);
    CHECK(luid.LowPart == 0x80000001u);
    CHECK(luid.HighPart == -1);
}

static void pipelineLevelsMapToFeatureLevels(void)
{
    CHECK(umd_feature_level_from_pipeline(UMD_PIPELINE_LEVEL_10_0) == UMD_FEATURE_LEVEL_10_0);
    CHECK(umd_feature_level_from_pipeline(UMD_PIPELINE_LEVEL_10_1) == UMD_FEATURE_LEVEL_10_1);
    CHECK(umd_feature_level_from_pipeline(UMD_PIPELINE_LEVEL_11_0) == UMD_FEATURE_LEVEL_11_0);
    CHECK(umd_feature_level_from_pipeline(UMD_PIPELINE_LEVEL_11_1) == UMD_FEATURE_LEVEL_11_1);
    // 9_x and 12_x pipeline levels come from other DDIs.
    CHECK(umd_feature_level_from_pipeline(4) == 0);
    CHECK(umd_feature_level_from_pipeline(0xFFFFFFFFu) == 0);
}

static void featureLevelListsRunHighestFirst(void)
{
    UINT levels[4];

    CHECK(umd_feature_levels_upto(UMD_FEATURE_LEVEL_11_1, levels, 4) == 4);
    CHECK(levels[0] == UMD_FEATURE_LEVEL_11_1 && levels[3] == UMD_FEATURE_LEVEL_10_0);
    CHECK(umd_feature_levels_upto(UMD_FEATURE_LEVEL_11_0, levels, 4) == 3);
    CHECK(levels[0] == UMD_FEATURE_LEVEL_11_0 && levels[2] == UMD_FEATURE_LEVEL_10_0);
    CHECK(umd_feature_levels_upto(UMD_FEATURE_LEVEL_10_0, levels, 4) == 1);
    CHECK(umd_feature_levels_upto(0x9300u, levels, 4) == 0);
    // 12_0 asks for nothing this driver cannot make.
    CHECK(umd_feature_levels_upto(0xc000u, levels, 4) == 4);
    CHECK(umd_feature_levels_upto(UMD_FEATURE_LEVEL_11_1, levels, 2) == 2);
    CHECK(levels[1] == UMD_FEATURE_LEVEL_11_0);
}

static void sharedFormatsDropSrgbAndTypeless(void)
{
    CHECK(umd_shared_format(UMD_DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) == UMD_DXGI_FORMAT_B8G8R8A8_UNORM);
    CHECK(umd_shared_format(UMD_DXGI_FORMAT_B8G8R8A8_TYPELESS) == UMD_DXGI_FORMAT_B8G8R8A8_UNORM);
    CHECK(umd_shared_format(UMD_DXGI_FORMAT_B8G8R8X8_UNORM_SRGB) == UMD_DXGI_FORMAT_B8G8R8X8_UNORM);
    CHECK(umd_shared_format(UMD_DXGI_FORMAT_B8G8R8X8_TYPELESS) == UMD_DXGI_FORMAT_B8G8R8X8_UNORM);
    CHECK(umd_shared_format(UMD_DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) == UMD_DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK(umd_shared_format(UMD_DXGI_FORMAT_R8G8B8A8_TYPELESS) == UMD_DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK(umd_shared_format(UMD_DXGI_FORMAT_R10G10B10A2_TYPELESS) == UMD_DXGI_FORMAT_R10G10B10A2_UNORM);
    CHECK(umd_shared_format(UMD_DXGI_FORMAT_B8G8R8A8_UNORM) == UMD_DXGI_FORMAT_B8G8R8A8_UNORM);
    // R16G16B16A16_FLOAT stays as it is.
    CHECK(umd_shared_format(10u) == 10u);
}

static void scanoutFormatsAreTheOnesVenusDescribes(void)
{
    // Venus's virtgpu_format_from_vk gives these a virgl format, so the kernel driver gets the blob's layout.
    CHECK(umd_scanout_vk_format(UMD_DXGI_FORMAT_B8G8R8A8_UNORM) == VK_FORMAT_B8G8R8A8_UNORM);
    CHECK(umd_scanout_vk_format(UMD_DXGI_FORMAT_B8G8R8X8_UNORM) == VK_FORMAT_B8G8R8A8_UNORM);
    CHECK(umd_scanout_vk_format(UMD_DXGI_FORMAT_R8G8B8A8_UNORM) == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(umd_scanout_vk_format(UMD_DXGI_FORMAT_R10G10B10A2_UNORM) == VK_FORMAT_A2B10G10R10_UNORM_PACK32);
    CHECK(umd_scanout_vk_format(UMD_DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) == VK_FORMAT_UNDEFINED);
    CHECK(umd_scanout_vk_format(10u) == VK_FORMAT_UNDEFINED);
    CHECK(umd_scanout_vk_format(umd_shared_format(UMD_DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)) == VK_FORMAT_B8G8R8A8_UNORM);
}

struct shared_allocation
{
    VIRTIO_WDDM_CREATE_ALLOCATION create;
    VIOGPU3D_SURFACE record;
};

static struct umd_texture_desc back_buffer_desc(void)
{
    struct umd_texture_desc desc;

    memset(&desc, 0, sizeof(desc));
    desc.width = 2880;
    desc.height = 1800;
    desc.mip_levels = 1;
    desc.array_size = 1;
    desc.format = UMD_DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.sample_count = 1;
    desc.bind_flags = 0x28;
    desc.misc_flags = 0x2;
    desc.kind = VIOGPU3D_SURFACE_SHARED_TEXTURE;
    desc.flags = VIOGPU3D_SURFACE_FLAG_LINEAR;
    return desc;
}

// What the opening process receives: Venus's blob request as the kernel driver answered it, then the record.
static struct shared_allocation shared_allocation(void)
{
    struct shared_allocation a;
    struct umd_texture_desc desc = back_buffer_desc();

    memset(&a, 0, sizeof(a));
    a.create.Base.Blob.Tag = VIRTIO_WDDM_ALLOCATE_BLOB_TAG;
    a.create.Base.Blob.Id = 42;
    a.create.Base.Blob.Mem = VIRTIO_WDDM_BLOB_MEM_HOST3D;
    a.create.Base.Blob.Flags = VIRTIO_WDDM_BLOB_FLAG_SHAREABLE | VIRTIO_WDDM_BLOB_FLAG_MAPPABLE;
    a.create.Base.Blob.Size = 2880ull * 1800ull * 4ull;
    a.create.ResId = 7;
    a.create.NumPlanes = 1;
    a.create.Planes[0].Stride = 2880 * 4;
    umd_resource_record_fill(&a.record, &desc);
    return a;
}

static void recordLayoutFollowsTheInterface(void)
{
    // Venus copies the extra data right after VIRTIO_WDDM_CREATE_ALLOCATION, whose size the interface fixes.
    CHECK(sizeof(VIRTIO_WDDM_CREATE_ALLOCATION) == 168);
    CHECK(offsetof(struct shared_allocation, record) == 168);
    CHECK(sizeof(VIRTIO_WDDM_RESOURCE_INFO) == 184);
    CHECK(offsetof(VIRTIO_WDDM_RESOURCE_INFO, Info) == 16);
    CHECK(offsetof(VIOGPU3D_SURFACE, Magic) == 0);
    CHECK(sizeof(VIOGPU3D_SURFACE) == 80);
    CHECK(offsetof(VIOGPU3D_SURFACE, Cookie) == 72);
    // Below the kernel driver's limit for a resource's extra data.
    CHECK(sizeof(VIOGPU3D_SURFACE) <= VIRTIO_WDDM_MAX_SUBMIT_COMMAND_VIRTUAL_SIZE);
}

static void openReadsWhatTheCreatorWrote(void)
{
    struct shared_allocation a = shared_allocation();
    struct umd_texture_desc want = back_buffer_desc(), got;
    VIRTIO_WDDM_RESOURCE_INFO info;

    CHECK(umd_resource_record_open(&a, sizeof(a), 0x40000041u, &info, &got) == UMD_OPEN_OK);
    CHECK(memcmp(&want, &got, sizeof(want)) == 0);
    CHECK(info.Tag == VIRTIO_WDDM_ESCAPE_RESOURCE_INFO_TAG);
    CHECK(info.Handle == 0x40000041u);
    CHECK(memcmp(&info.Info, &a.create, sizeof(a.create)) == 0);
    CHECK(info.Info.Base.Blob.Size == 2880ull * 1800ull * 4ull);

    // A larger buffer, as a later record version would make, still opens.
    unsigned char bigger[sizeof(a) + 16];
    memset(bigger, 0, sizeof(bigger));
    memcpy(bigger, &a, sizeof(a));
    CHECK(umd_resource_record_open(bigger, sizeof(bigger), 1, &info, &got) == UMD_OPEN_OK);
}

static void openRefusesWhatVenusCannotImport(void)
{
    struct shared_allocation a;
    struct umd_texture_desc got;
    VIRTIO_WDDM_RESOURCE_INFO info;

    a = shared_allocation();
    CHECK(umd_resource_record_open(NULL, sizeof(a), 1, &info, &got) == UMD_OPEN_SHORT);
    CHECK(umd_resource_record_open(&a, sizeof(a.create) - 1, 1, &info, &got) == UMD_OPEN_SHORT);

    // A virgl 3D resource.
    a = shared_allocation();
    a.create.Base.Tag = VIRTIO_WDDM_ALLOCATE_3D_TAG;
    CHECK(umd_resource_record_open(&a, sizeof(a), 1, &info, &got) == UMD_OPEN_NOT_BLOB);

    // The kernel driver's shadow or staging surface: no host resource at all.
    a = shared_allocation();
    a.create.Base.Tag = 0;
    a.record.Kind = VIOGPU3D_SURFACE_KERNEL_CPU;
    CHECK(umd_resource_record_open(&a, sizeof(a), 1, &info, &got) == UMD_OPEN_KERNEL_CPU_SURFACE);

    a = shared_allocation();
    a.create.Base.Blob.Mem = VIRTIO_WDDM_BLOB_MEM_GUEST;
    CHECK(umd_resource_record_open(&a, sizeof(a), 1, &info, &got) == UMD_OPEN_NOT_HOST3D);

    a = shared_allocation();
    a.create.Base.Blob.Size = 0;
    CHECK(umd_resource_record_open(&a, sizeof(a), 1, &info, &got) == UMD_OPEN_NOT_HOST3D);

    a = shared_allocation();
    a.create.Base.Blob.Flags = VIRTIO_WDDM_BLOB_FLAG_MAPPABLE;
    CHECK(umd_resource_record_open(&a, sizeof(a), 1, &info, &got) == UMD_OPEN_NOT_SHAREABLE);

    a = shared_allocation();
    a.create.ResId = 0;
    CHECK(umd_resource_record_open(&a, sizeof(a), 1, &info, &got) == UMD_OPEN_NO_RESOURCE);

    a = shared_allocation();
    CHECK(umd_resource_record_open(&a, sizeof(a.create), 1, &info, &got) == UMD_OPEN_NO_RECORD);
    CHECK(umd_resource_record_open(&a, sizeof(a) - 1, 1, &info, &got) == UMD_OPEN_NO_RECORD);

    a = shared_allocation();
    a.record.Magic = 0x56334453u;
    CHECK(umd_resource_record_open(&a, sizeof(a), 1, &info, &got) == UMD_OPEN_NO_RECORD);
}

static void openRefusesRecordsOutOfRange(void)
{
    struct shared_allocation a;
    struct umd_texture_desc got;
    VIRTIO_WDDM_RESOURCE_INFO info;

#define BAD_RECORD(field, value)                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        a = shared_allocation();                                                                                       \
        a.record.field = (value);                                                                                      \
        CHECK(umd_resource_record_open(&a, sizeof(a), 1, &info, &got) == UMD_OPEN_BAD_RECORD);                         \
    } while (0)

    BAD_RECORD(Size, 79);
    BAD_RECORD(Kind, 0);
    BAD_RECORD(Kind, 4);
    BAD_RECORD(Flags, 2);
    BAD_RECORD(MipLevels, 2);
    BAD_RECORD(ArraySize, 2);
    BAD_RECORD(SampleCount, 4);
    BAD_RECORD(Width, 0);
    BAD_RECORD(Width, 16385);
    BAD_RECORD(Height, 0);
    BAD_RECORD(Height, 16385);
    BAD_RECORD(MipLevels, 0);
    BAD_RECORD(MipLevels, 16);
    BAD_RECORD(ArraySize, 0);
    BAD_RECORD(ArraySize, 2049);
    BAD_RECORD(SampleCount, 0);
    BAD_RECORD(SampleCount, 3);
    BAD_RECORD(SampleCount, 64);
    BAD_RECORD(Format, 0);
#undef BAD_RECORD

    a = shared_allocation();
    a.record.Flags = 0;
    a.record.Width = 16384;
    a.record.Height = 16384;
    a.record.MipLevels = 15;
    a.record.ArraySize = 2048;
    a.record.SampleCount = 32;
    CHECK(umd_resource_record_open(&a, sizeof(a), 1, &info, &got) == UMD_OPEN_OK);
}

// GetStandardAllocationDriverData's shared primary as an opening process gets it: mark zeroed, resource id given.
static struct shared_allocation kernel_primary(UINT32 width, UINT32 height)
{
    struct shared_allocation a;
    UINT32 pitch = (width * 4 + VIOGPU3D_SURFACE_PITCH_ALIGNMENT - 1) & ~(VIOGPU3D_SURFACE_PITCH_ALIGNMENT - 1);

    memset(&a, 0, sizeof(a));
    a.create.Base.Blob.Tag = VIRTIO_WDDM_ALLOCATE_BLOB_TAG;
    a.create.Base.Blob.Mem = VIRTIO_WDDM_BLOB_MEM_HOST3D;
    a.create.Base.Blob.Flags = VIRTIO_WDDM_BLOB_FLAG_MAPPABLE | VIRTIO_WDDM_BLOB_FLAG_SHAREABLE;
    a.create.Base.Blob.Size = ((UINT64)pitch * height + 16383) & ~16383ull;
    a.create.Base.Blob.Info.Width = width;
    a.create.Base.Blob.Info.Height = height;
    a.create.ResId = 3;
    a.create.NumPlanes = 1;
    a.create.Modifier = VIRTIO_WDDM_MODIFIER_INVALID;
    a.create.Planes[0].Size = (UINT64)pitch * height;
    a.create.Planes[0].Stride = pitch;
    a.record.Magic = VIOGPU3D_SURFACE_MAGIC;
    a.record.Size = sizeof(a.record);
    a.record.Kind = VIOGPU3D_SURFACE_KERNEL_BLOB;
    a.record.Flags = VIOGPU3D_SURFACE_FLAG_LINEAR;
    a.record.Width = width;
    a.record.Height = height;
    a.record.MipLevels = 1;
    a.record.ArraySize = 1;
    a.record.Format = VIOGPU3D_DXGI_FORMAT_B8G8R8X8_UNORM;
    a.record.SampleCount = 1;
    a.record.Usage = VIOGPU3D_D3D11_USAGE_DEFAULT;
    a.record.BindFlags = VIOGPU3D_D3D11_BIND_RENDER_TARGET | VIOGPU3D_D3D11_BIND_SHADER_RESOURCE;
    a.record.MiscFlags = VIOGPU3D_D3D11_RESOURCE_MISC_SHARED;
    a.record.Pitch = pitch;
    a.record.StandardType = 1;
    return a;
}

static void kernelSurfacesOpenAsLinearBlobs(void)
{
    struct shared_allocation a = kernel_primary(2880, 1800);
    struct umd_texture_desc got;
    VIRTIO_WDDM_RESOURCE_INFO info;

    CHECK(umd_resource_record_open(&a, sizeof(a), 0x40000081u, &info, &got) == UMD_OPEN_OK);
    CHECK(got.kind == VIOGPU3D_SURFACE_KERNEL_BLOB);
    CHECK(got.flags == VIOGPU3D_SURFACE_FLAG_LINEAR);
    CHECK(got.width == 2880 && got.height == 1800 && got.mip_levels == 1 && got.array_size == 1);
    CHECK(got.format == VIOGPU3D_DXGI_FORMAT_B8G8R8X8_UNORM);
    CHECK(got.misc_flags == VIOGPU3D_D3D11_RESOURCE_MISC_SHARED);
    CHECK(got.pitch == 11520);
    CHECK(got.standard_type == 1);
    CHECK(info.Handle == 0x40000081u);
    CHECK(info.Info.ResId == 3);
    CHECK(info.Info.Modifier == VIRTIO_WDDM_MODIFIER_INVALID);

    // A width whose rows Metal pads: 1366 pixels are 5464 bytes, in rows of 5472.
    a = kernel_primary(1366, 768);
    CHECK(umd_resource_record_open(&a, sizeof(a), 1, &info, &got) == UMD_OPEN_OK);
    CHECK(got.pitch == 5472);
    // The 5K screen's.
    a = kernel_primary(5120, 2880);
    CHECK(umd_resource_record_open(&a, sizeof(a), 1, &info, &got) == UMD_OPEN_OK);
    CHECK(got.pitch == 20480);
    CHECK(info.Info.Base.Blob.Size == 20480ull * 2880ull);

    a = kernel_primary(2880, 1800);
    a.record.Flags = 0;
    CHECK(umd_resource_record_open(&a, sizeof(a), 1, &info, &got) == UMD_OPEN_BAD_RECORD);
    a = kernel_primary(2880, 1800);
    a.record.Pitch = 2880 * 4 - 4;
    CHECK(umd_resource_record_open(&a, sizeof(a), 1, &info, &got) == UMD_OPEN_BAD_RECORD);
    a = kernel_primary(2880, 1800);
    a.create.Base.Blob.Size = 11520ull * 1799ull;
    CHECK(umd_resource_record_open(&a, sizeof(a), 1, &info, &got) == UMD_OPEN_BAD_RECORD);
    // A shared texture names no pitch, whatever its record carries.
    a = shared_allocation();
    a.record.Pitch = 64;
    a.record.StandardType = 1;
    CHECK(umd_resource_record_open(&a, sizeof(a), 1, &info, &got) == UMD_OPEN_OK);
    CHECK(got.pitch == 0 && got.standard_type == 0);
}

static void sharedTexturesAreLinearWhenTheyCanBe(void)
{
    struct umd_texture_desc desc = back_buffer_desc();
    VIOGPU3D_SURFACE record;

    CHECK(umd_texture_can_be_linear(&desc));
    desc.mip_levels = 2;
    CHECK(!umd_texture_can_be_linear(&desc));
    desc = back_buffer_desc();
    desc.array_size = 6;
    CHECK(!umd_texture_can_be_linear(&desc));
    desc = back_buffer_desc();
    desc.sample_count = 4;
    CHECK(!umd_texture_can_be_linear(&desc));

    // The record is always a shared texture's, and keeps the linear flag alone.
    desc = back_buffer_desc();
    desc.kind = VIOGPU3D_SURFACE_KERNEL_BLOB;
    desc.flags = 0xffu;
    desc.pitch = 4;
    umd_resource_record_fill(&record, &desc);
    CHECK(record.Magic == VIOGPU3D_SURFACE_MAGIC && record.Size == sizeof(record));
    CHECK(record.Kind == VIOGPU3D_SURFACE_SHARED_TEXTURE);
    CHECK(record.Flags == VIOGPU3D_SURFACE_FLAG_LINEAR);
    CHECK(record.Pitch == 0 && record.Cookie == 0);
}

static VIRTIO_WDDM_BLOB_MAP blob_map(UINT64 ptr)
{
    VIRTIO_WDDM_BLOB_MAP map;

    memset(&map, 0, sizeof(map));
    map.Tag = VIRTIO_WDDM_ESCAPE_BLOB_MAP_TAG;
    map.Handle = 0x40000042u;
    map.Ptr = ptr;
    return map;
}

static void blobMapsA32BitProcessCannotHoldAreCaught(void)
{
    VIRTIO_WDDM_BLOB_MAP map = blob_map(0x7ffe0000ull), got;
    unsigned char escape[64];

    CHECK(umd_blob_map_check(&map, sizeof(map), 4, &got) == UMD_BLOB_MAP_FITS);
    CHECK(got.Handle == 0x40000042u && got.Ptr == 0x7ffe0000ull);
    map = blob_map(0xffff0000ull);
    CHECK(umd_blob_map_check(&map, sizeof(map), 4, &got) == UMD_BLOB_MAP_FITS);
    map = blob_map(0x100000000ull);
    CHECK(umd_blob_map_check(&map, sizeof(map), 4, &got) == UMD_BLOB_MAP_TOO_HIGH);
    CHECK(got.Ptr == 0x100000000ull);
    map = blob_map(0x7ff612340000ull);
    CHECK(umd_blob_map_check(&map, sizeof(map), 4, &got) == UMD_BLOB_MAP_TOO_HIGH);
    CHECK(umd_blob_map_check(&map, sizeof(map), 8, &got) == UMD_BLOB_MAP_FITS);

    // Venus sends its escape union, larger than the map.
    memset(escape, 0, sizeof(escape));
    map = blob_map(0x200000000ull);
    memcpy(escape, &map, sizeof(map));
    CHECK(umd_blob_map_check(escape, sizeof(escape), 4, &got) == UMD_BLOB_MAP_TOO_HIGH);

    // An unmap, a refused map, another escape and a short buffer are left alone.
    map = blob_map(0x200000000ull);
    map.Flags = VIRTIO_WDDM_BLOB_MAP_FLAGS_UNMAP;
    CHECK(umd_blob_map_check(&map, sizeof(map), 4, &got) == UMD_BLOB_MAP_OTHER);
    map = blob_map(0);
    CHECK(umd_blob_map_check(&map, sizeof(map), 4, &got) == UMD_BLOB_MAP_OTHER);
    map = blob_map(0x200000000ull);
    map.Tag = VIRTIO_WDDM_ESCAPE_RESOURCE_INFO_TAG;
    CHECK(umd_blob_map_check(&map, sizeof(map), 4, &got) == UMD_BLOB_MAP_OTHER);
    map = blob_map(0x200000000ull);
    CHECK(umd_blob_map_check(&map, sizeof(map) - 1, 4, &got) == UMD_BLOB_MAP_OTHER);
    CHECK(umd_blob_map_check(NULL, sizeof(map), 4, &got) == UMD_BLOB_MAP_OTHER);
}

static void openTextsNameEveryOutcome(void)
{
    for (int c = UMD_OPEN_OK; c <= UMD_OPEN_BAD_RECORD; c++)
    {
        CHECK(strcmp(umd_open_check_text((enum umd_open_check)c), "unknown") != 0);
    }
}

static void allocationsAreSortedByTheirChain(void)
{
    VkMemoryAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    VkExportMemoryAllocateInfo exportInfo = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    VkMemoryDedicatedAllocateInfo dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    VkBaseInStructure import = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
    VkBaseInStructure named = {.sType = UMD_VK_STRUCTURE_TYPE_D3DDDI_CREATE_RESOURCE_MESA};

    CHECK(umd_vk_alloc_kind(&info) == UMD_VK_ALLOC_OTHER);

    info.pNext = &exportInfo;
    CHECK(umd_vk_alloc_kind(&info) == UMD_VK_ALLOC_OTHER);

    dedicated.image = (VkImage)(uintptr_t)0x1000;
    exportInfo.pNext = &dedicated;
    CHECK(umd_vk_alloc_kind(&info) == UMD_VK_ALLOC_SHARED_IMAGE);

    // A dedicated buffer is never a shared texture's storage.
    dedicated.image = VK_NULL_HANDLE;
    dedicated.buffer = (VkBuffer)(uintptr_t)0x2000;
    CHECK(umd_vk_alloc_kind(&info) == UMD_VK_ALLOC_OTHER);

    dedicated.image = (VkImage)(uintptr_t)0x1000;
    dedicated.buffer = VK_NULL_HANDLE;
    dedicated.pNext = &named;
    CHECK(umd_vk_alloc_kind(&info) == UMD_VK_ALLOC_OTHER);

    info.pNext = &import;
    CHECK(umd_vk_alloc_kind(&info) == UMD_VK_ALLOC_IMPORT);
}

static void aChainThatLoopsIsNotFollowed(void)
{
    VkMemoryAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    VkBaseInStructure a = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    VkBaseInStructure b = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};

    a.pNext = &b;
    b.pNext = &a;
    info.pNext = &a;
    CHECK(umd_vk_alloc_kind(&info) == UMD_VK_ALLOC_OTHER);
}

static void prependLeavesTheCallersChain(void)
{
    VkMemoryAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}, copy;
    VkExportMemoryAllocateInfo exportInfo = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    VkBaseOutStructure ext = {.sType = UMD_VK_STRUCTURE_TYPE_D3DDDI_CREATE_RESOURCE_MESA};

    info.allocationSize = 65536;
    info.memoryTypeIndex = 3;
    info.pNext = &exportInfo;
    umd_vk_chain_prepend(&info, &copy, &ext);
    CHECK(copy.pNext == &ext);
    CHECK(ext.pNext == (VkBaseOutStructure *)&exportInfo);
    CHECK(info.pNext == &exportInfo);
    CHECK(copy.allocationSize == 65536 && copy.memoryTypeIndex == 3 && copy.sType == info.sType);
    CHECK(umd_vk_alloc_kind(&copy) == UMD_VK_ALLOC_OTHER);
}

static void icdPathIsBesideTheDriver(void)
{
    WCHAR out[260];
    const WCHAR *dll = L"C:\\Windows\\System32\\DriverStore\\FileRepository\\viogpu3d.inf_arm64_1\\viogpu3d_umd.dll";
    const WCHAR *want = L"C:\\Windows\\System32\\DriverStore\\FileRepository\\viogpu3d.inf_arm64_1\\vulkan_virtio.dll";

    CHECK(umd_path_beside(dll, L"vulkan_virtio.dll", out, 260));
    CHECK(wcscmp(out, want) == 0);

    CHECK(umd_path_beside(dll, L"vulkan_virtio.dll", out, wcslen(want) + 1));
    CHECK(!umd_path_beside(dll, L"vulkan_virtio.dll", out, wcslen(want)));

    CHECK(!umd_path_beside(L"viogpu3d_umd.dll", L"vulkan_virtio.dll", out, 260));
    CHECK(!umd_path_beside(dll, L"..\\vulkan-1.dll", out, 260));
    CHECK(!umd_path_beside(dll, L"", out, 260));
    CHECK(!umd_path_beside(NULL, L"vulkan_virtio.dll", out, 260));
}

static void eachDriverLoadsItsOwnIcd(void)
{
    WCHAR out[260];
    const WCHAR *x86 = L"C:\\Windows\\System32\\DriverStore\\FileRepository\\viogpu3d.inf_arm64_1\\"
                       L"viogpu3d_umd_x86.dll";
    const WCHAR *want = L"C:\\Windows\\System32\\DriverStore\\FileRepository\\viogpu3d.inf_arm64_1\\"
                        L"vulkan_virtio_x86.dll";

    CHECK(wcscmp(umd_icd_name(FALSE), L"vulkan_virtio.dll") == 0);
    CHECK(wcscmp(umd_icd_name(TRUE), L"vulkan_virtio_x86.dll") == 0);
    CHECK(umd_path_beside(x86, umd_icd_name(TRUE), out, 260));
    CHECK(wcscmp(out, want) == 0);
}

// The shared object stands in for the GetCaps Venus instance: made, used and freed as an adapter would.
struct shared_probe
{
    atomic_int alive;
};

static atomic_int sharedMade;
static atomic_int sharedFreed;
static atomic_int sharedLive;
static atomic_int sharedMostLive;
static HRESULT sharedMakeResult;

static HRESULT probeMake(void **object)
{
    struct shared_probe *probe;
    int live, most;

    if (FAILED(sharedMakeResult))
    {
        return sharedMakeResult;
    }
    probe = (struct shared_probe *)malloc(sizeof(*probe));
    if (probe == NULL)
    {
        return (HRESULT)0x8007000E;
    }
    atomic_init(&probe->alive, 1);
    atomic_fetch_add(&sharedMade, 1);
    live = atomic_fetch_add(&sharedLive, 1) + 1;
    most = atomic_load(&sharedMostLive);
    while (live > most && !atomic_compare_exchange_weak(&sharedMostLive, &most, live))
    {
    }
    *object = probe;
    return S_OK;
}

static void probeFree(void *object)
{
    struct shared_probe *probe = (struct shared_probe *)object;

    atomic_store(&probe->alive, 0);
    atomic_fetch_add(&sharedFreed, 1);
    atomic_fetch_sub(&sharedLive, 1);
    free(probe);
}

static void sharedProbeReset(void)
{
    atomic_store(&sharedMade, 0);
    atomic_store(&sharedFreed, 0);
    atomic_store(&sharedLive, 0);
    atomic_store(&sharedMostLive, 0);
    sharedMakeResult = S_OK;
}

static void sharedObjectIsMadeOnceAndFreedByTheLastHolder(void)
{
    struct umd_shared shared = {UMD_LOCK_INIT, NULL, 0};
    void *first = NULL, *second = NULL;

    sharedProbeReset();
    CHECK(umd_shared_acquire(&shared, probeMake, &first) == S_OK);
    CHECK(umd_shared_acquire(&shared, probeMake, &second) == S_OK);
    CHECK(first != NULL && first == second);
    CHECK(atomic_load(&sharedMade) == 1 && shared.holders == 2);

    umd_shared_release(&shared, probeFree);
    CHECK(atomic_load(&sharedFreed) == 0 && shared.object == first);
    umd_shared_release(&shared, probeFree);
    CHECK(atomic_load(&sharedFreed) == 1 && shared.holders == 0 && shared.object == NULL);

    // A release without a hold frees nothing twice.
    umd_shared_release(&shared, probeFree);
    CHECK(atomic_load(&sharedFreed) == 1 && shared.holders == 0);
}

static void aFailedMakeLeavesNoHolder(void)
{
    struct umd_shared shared = {UMD_LOCK_INIT, NULL, 0};
    void *object = &shared;

    sharedProbeReset();
    sharedMakeResult = (HRESULT)0x887A0004;
    CHECK(umd_shared_acquire(&shared, probeMake, &object) == sharedMakeResult);
    CHECK(object == NULL && shared.holders == 0 && shared.object == NULL);

    sharedMakeResult = S_OK;
    CHECK(umd_shared_acquire(&shared, probeMake, &object) == S_OK);
    CHECK(object != NULL && shared.holders == 1 && atomic_load(&sharedMade) == 1);
    umd_shared_release(&shared, probeFree);
    CHECK(atomic_load(&sharedFreed) == 1);
}

static void reopenAfterTheLastCloseMakesANewOne(void)
{
    struct umd_shared shared = {UMD_LOCK_INIT, NULL, 0};
    void *object;

    sharedProbeReset();
    for (int round = 0; round < 3; round++)
    {
        CHECK(umd_shared_acquire(&shared, probeMake, &object) == S_OK);
        CHECK(object != NULL && atomic_load(&sharedLive) == 1);
        umd_shared_release(&shared, probeFree);
        CHECK(atomic_load(&sharedLive) == 0 && shared.object == NULL);
    }
    CHECK(atomic_load(&sharedMade) == 3 && atomic_load(&sharedFreed) == 3);
}

struct shared_churn
{
    struct umd_shared *shared;
    int rounds;
    atomic_int *errors;
};

// An adapter's life on one thread: open, GetCaps, use the instance, close.
static void *sharedChurn(void *arg)
{
    struct shared_churn *churn = (struct shared_churn *)arg;

    for (int round = 0; round < churn->rounds; round++)
    {
        struct shared_probe *probe = NULL;

        if (umd_shared_acquire(churn->shared, probeMake, (void **)&probe) != S_OK || probe == NULL)
        {
            atomic_fetch_add(churn->errors, 1);
            continue;
        }
        if ((round & 3) == 0)
        {
            sched_yield();
        }
        if (atomic_load(&probe->alive) != 1)
        {
            atomic_fetch_add(churn->errors, 1);
        }
        umd_shared_release(churn->shared, probeFree);
    }
    return NULL;
}

static void runChurn(struct umd_shared *shared, atomic_int *errors)
{
    enum
    {
        THREADS = 8,
        ROUNDS = 4000,
    };
    pthread_t threads[THREADS];
    struct shared_churn churn = {shared, ROUNDS, errors};
    int started = 0;

    for (int i = 0; i < THREADS; i++)
    {
        if (pthread_create(&threads[i], NULL, sharedChurn, &churn) == 0)
        {
            started++;
        }
    }
    CHECK(started == THREADS);
    for (int i = 0; i < started; i++)
    {
        pthread_join(threads[i], NULL);
    }
}

static void adaptersOpenAndCloseOnManyThreads(void)
{
    struct umd_shared shared = {UMD_LOCK_INIT, NULL, 0};
    atomic_int errors;

    sharedProbeReset();
    atomic_init(&errors, 0);
    runChurn(&shared, &errors);
    CHECK(atomic_load(&errors) == 0);
    CHECK(atomic_load(&sharedMostLive) == 1);
    CHECK(atomic_load(&sharedMade) >= 1);
    CHECK(atomic_load(&sharedMade) == atomic_load(&sharedFreed));
    CHECK(atomic_load(&sharedLive) == 0 && shared.holders == 0 && shared.object == NULL);
}

static void anOpenAdapterKeepsTheOneInstanceThroughOthersChurn(void)
{
    struct umd_shared shared = {UMD_LOCK_INIT, NULL, 0};
    atomic_int errors;
    void *held;

    sharedProbeReset();
    atomic_init(&errors, 0);
    CHECK(umd_shared_acquire(&shared, probeMake, &held) == S_OK);
    runChurn(&shared, &errors);
    CHECK(atomic_load(&errors) == 0);
    CHECK(atomic_load(&sharedMade) == 1 && atomic_load(&sharedFreed) == 0);
    CHECK(shared.holders == 1 && shared.object == held);
    umd_shared_release(&shared, probeFree);
    CHECK(atomic_load(&sharedFreed) == 1 && atomic_load(&sharedLive) == 0);
}

static struct umd_d3d12_texture d3d12_back_buffer(void)
{
    struct umd_d3d12_texture t;

    memset(&t, 0, sizeof(t));
    t.width = 2880;
    t.height = 1800;
    t.depth_or_array_size = 1;
    t.mip_levels = 1;
    t.format = UMD_DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    t.sample_count = 1;
    t.flags = UMD_D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    return t;
}

static void d3d12SharedSurfacesAreOneLinearPlane(void)
{
    struct umd_d3d12_texture t = d3d12_back_buffer();
    struct umd_texture_desc desc;

    CHECK(umd_d3d12_shared_desc(&t, TRUE, &desc) == UMD_D3D12_SHARED_OK);
    CHECK(desc.width == 2880 && desc.height == 1800);
    CHECK(desc.mip_levels == 1 && desc.array_size == 1 && desc.sample_count == 1);
    // Shared and scanout textures take the UNORM format, as the D3D10 and 11 driver's do.
    CHECK(desc.format == UMD_DXGI_FORMAT_B8G8R8A8_UNORM);
    CHECK(desc.bind_flags == (VIOGPU3D_D3D11_BIND_SHADER_RESOURCE | VIOGPU3D_D3D11_BIND_RENDER_TARGET));
    CHECK(desc.usage == VIOGPU3D_D3D11_USAGE_DEFAULT);
    CHECK(desc.misc_flags == VIOGPU3D_D3D11_RESOURCE_MISC_SHARED);
    CHECK(desc.kind == VIOGPU3D_SURFACE_SHARED_TEXTURE);
    CHECK(desc.flags == VIOGPU3D_SURFACE_FLAG_LINEAR);

    t.flags = UMD_D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | UMD_D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    CHECK(umd_d3d12_shared_desc(&t, FALSE, &desc) == UMD_D3D12_SHARED_OK);
    CHECK(desc.bind_flags == UMD_D3D11_BIND_UNORDERED_ACCESS);

    t = d3d12_back_buffer();
    t.mip_levels = 2;
    CHECK(umd_d3d12_shared_desc(&t, FALSE, &desc) == UMD_D3D12_SHARED_NOT_ONE_PLANE);
    t = d3d12_back_buffer();
    t.depth_or_array_size = 2;
    CHECK(umd_d3d12_shared_desc(&t, FALSE, &desc) == UMD_D3D12_SHARED_NOT_ONE_PLANE);
    t = d3d12_back_buffer();
    t.sample_count = 4;
    CHECK(umd_d3d12_shared_desc(&t, FALSE, &desc) == UMD_D3D12_SHARED_NOT_ONE_PLANE);
    t = d3d12_back_buffer();
    t.flags |= UMD_D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    CHECK(umd_d3d12_shared_desc(&t, FALSE, &desc) == UMD_D3D12_SHARED_DEPTH_STENCIL);
    t = d3d12_back_buffer();
    t.width = 16385;
    CHECK(umd_d3d12_shared_desc(&t, FALSE, &desc) == UMD_D3D12_SHARED_TOO_LARGE);
    t.width = 1ull << 32;
    CHECK(umd_d3d12_shared_desc(&t, FALSE, &desc) == UMD_D3D12_SHARED_TOO_LARGE);
    t = d3d12_back_buffer();
    t.height = 0;
    CHECK(umd_d3d12_shared_desc(&t, FALSE, &desc) == UMD_D3D12_SHARED_TOO_LARGE);

    // A primary also needs a format the kernel driver scans out; R16G16B16A16_FLOAT (10) is none.
    t = d3d12_back_buffer();
    t.format = 10;
    CHECK(umd_d3d12_shared_desc(&t, FALSE, &desc) == UMD_D3D12_SHARED_OK);
    CHECK(umd_d3d12_shared_desc(&t, TRUE, &desc) == UMD_D3D12_SHARED_NOT_SCANOUT);
    CHECK(strcmp(umd_d3d12_shared_check_text(UMD_D3D12_SHARED_OK), "ok") == 0);
    CHECK(strcmp(umd_d3d12_shared_check_text((enum umd_d3d12_shared_check)99), "unknown") == 0);
    for (int c = UMD_D3D12_SHARED_NOT_ONE_PLANE; c <= UMD_D3D12_SHARED_NOT_SCANOUT; c++)
    {
        CHECK(strcmp(umd_d3d12_shared_check_text((enum umd_d3d12_shared_check)c), "unknown") != 0);
    }
}

// A Direct3D 12 back buffer DWM opens through the D3D10 and 11 driver: its record reads back as it was written.
static void d3d12BackBuffersOpenInTheD3D11Driver(void)
{
    struct umd_d3d12_texture t = d3d12_back_buffer();
    struct umd_texture_desc made, got;
    struct shared_allocation a = shared_allocation();
    VIRTIO_WDDM_RESOURCE_INFO info;

    CHECK(umd_d3d12_shared_desc(&t, TRUE, &made) == UMD_D3D12_SHARED_OK);
    umd_resource_record_fill(&a.record, &made);
    CHECK(umd_resource_record_open(&a, sizeof(a), 0x40000042u, &info, &got) == UMD_OPEN_OK);
    CHECK(memcmp(&made, &got, sizeof(made)) == 0);
    CHECK(umd_texture_can_be_linear(&got));
}

static void d3d12ImageUsageFollowsTheFlags(void)
{
    const VkImageUsageFlags copies = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    CHECK(umd_d3d12_shared_usage(0) == (copies | VK_IMAGE_USAGE_SAMPLED_BIT));
    CHECK(umd_d3d12_shared_usage(UMD_D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ==
          (copies | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT));
    CHECK(umd_d3d12_shared_usage(UMD_D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                                 UMD_D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ==
          (copies | VK_IMAGE_USAGE_STORAGE_BIT));
}

static void d3d12ViewFormatsAddSrgbFor8Bit(void)
{
    VkFormat formats[2];

    CHECK(umd_d3d12_view_formats(UMD_DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, formats, 2) == 2);
    CHECK(formats[0] == VK_FORMAT_R8G8B8A8_UNORM && formats[1] == VK_FORMAT_R8G8B8A8_SRGB);
    CHECK(umd_d3d12_view_formats(UMD_DXGI_FORMAT_B8G8R8X8_UNORM, formats, 2) == 2);
    CHECK(formats[0] == VK_FORMAT_B8G8R8A8_UNORM && formats[1] == VK_FORMAT_B8G8R8A8_SRGB);
    CHECK(umd_d3d12_view_formats(UMD_DXGI_FORMAT_R10G10B10A2_UNORM, formats, 2) == 0);
    CHECK(umd_d3d12_view_formats(UMD_DXGI_FORMAT_R8G8B8A8_UNORM, formats, 1) == 1);
    CHECK(formats[0] == VK_FORMAT_R8G8B8A8_UNORM);
}

static VkPhysicalDeviceIDProperties device_with_luid(BOOL valid, LUID luid)
{
    VkPhysicalDeviceIDProperties id;

    memset(&id, 0, sizeof(id));
    id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    id.deviceLUIDValid = valid ? VK_TRUE : VK_FALSE;
    memcpy(id.deviceLUID, &luid, sizeof(luid));
    return id;
}

static void physicalDeviceIsTheAdaptersByLuid(void)
{
    LUID adapter = {0x00000002u, 0x00000001};
    LUID other = {0x00000003u, 0x00000001};
    VkPhysicalDeviceIDProperties ids[3];

    ids[0] = device_with_luid(TRUE, other);
    ids[1] = device_with_luid(TRUE, adapter);
    ids[2] = device_with_luid(FALSE, adapter);
    CHECK(umd_pick_physical_device(ids, 3, adapter) == 1);
    CHECK(umd_pick_physical_device(ids, 1, adapter) == -1);
    // No device knows its LUID: the only one there is.
    ids[0] = device_with_luid(FALSE, other);
    CHECK(umd_pick_physical_device(ids, 1, adapter) == 0);
    ids[1] = device_with_luid(FALSE, other);
    CHECK(umd_pick_physical_device(ids, 2, adapter) == -1);
    CHECK(umd_pick_physical_device(ids, 0, adapter) == -1);
    // The LUID's bytes in memory order: LowPart first.
    ids[0] = device_with_luid(TRUE, adapter);
    CHECK(ids[0].deviceLUID[0] == 0x02 && ids[0].deviceLUID[4] == 0x01);
}

static void memoryTypePrefersDeviceLocal(void)
{
    VkPhysicalDeviceMemoryProperties props;

    memset(&props, 0, sizeof(props));
    props.memoryTypeCount = 3;
    props.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    props.memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    props.memoryTypes[2].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    CHECK(umd_pick_memory_type(0x7, &props, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 2);
    CHECK(umd_pick_memory_type(0x3, &props, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0);
    CHECK(umd_pick_memory_type(0x2, &props, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 1);
    CHECK(umd_pick_memory_type(0x0, &props, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == -1);
    // Bits past the types there are mean nothing.
    CHECK(umd_pick_memory_type(0x8, &props, 0) == -1);
}

static void emptyAllocationsNameNoMemory(void)
{
    VIRTIO_WDDM_CREATE_ALLOCATION create, zero;
    VIRTIO_WDDM_RESOURCE_INFO info;
    struct umd_texture_desc desc;

    memset(&create, 0xab, sizeof(create));
    umd_empty_allocation_fill(&create);
    CHECK(create.Base.Tag == VIOGPU3D_ALLOCATE_EMPTY_TAG);
    memset(&zero, 0, sizeof(zero));
    zero.Base.Tag = VIOGPU3D_ALLOCATE_EMPTY_TAG;
    CHECK(memcmp(&create, &zero, sizeof(create)) == 0);
    // The kernel driver takes nothing shorter, and tells the kinds apart by the tag alone.
    CHECK(sizeof(create) == 168);
    CHECK(VIOGPU3D_ALLOCATE_EMPTY_TAG != VIRTIO_WDDM_ALLOCATE_BLOB_TAG);
    CHECK(VIOGPU3D_ALLOCATE_EMPTY_TAG != VIRTIO_WDDM_ALLOCATE_3D_TAG);
    CHECK(VIOGPU3D_ALLOCATE_EMPTY_TAG != VIRTIO_WDDM_CREATE_RESOURCE_TAG);
    CHECK(memcmp("VG3DEMPT", &create.Base.Tag, 8) == 0);
    // No other process opens one.
    CHECK(umd_resource_record_open(&create, sizeof(create), 1, &info, &desc) == UMD_OPEN_NOT_BLOB);
}

static void venusAllocationsTakeTheCoreLayersForm(void)
{
    UINT8 callData[8], data0[16], data1[16];
    int resource;
    struct umd_kt_allocation_info venusInfos[2];
    struct umd_kt_allocate venus;
    struct umd_d3d12_allocation_info infos[UMD_D3D12_ALLOCATE_MAX];
    struct umd_d3d12_allocate out;

    memset(venusInfos, 0, sizeof(venusInfos));
    venusInfos[0].hAllocation = 0x11;
    venusInfos[0].pSystemMem = data0;
    venusInfos[0].pPrivateDriverData = data0;
    venusInfos[0].PrivateDriverDataSize = sizeof(data0);
    venusInfos[0].VidPnSourceId = 2;
    venusInfos[0].Flags = UMD_KT_ALLOCATION_PRIMARY;
    venusInfos[1].pPrivateDriverData = data1;
    venusInfos[1].PrivateDriverDataSize = 12;
    // Stereo, and a bit d3dukmdt.h reserves, which has no meaning in the core layer's flags.
    venusInfos[1].Flags = UMD_KT_ALLOCATION_STEREO | 0x80000000u;
    memset(&venus, 0, sizeof(venus));
    venus.pPrivateDriverData = callData;
    venus.PrivateDriverDataSize = sizeof(callData);
    venus.hResource = &resource;
    venus.hKMResource = 0x77;
    venus.NumAllocations = 2;
    venus.pAllocationInfo = venusInfos;

    memset(infos, 0xab, sizeof(infos));
    memset(&out, 0xab, sizeof(out));
    CHECK(umd_d3d12_allocate_args(&venus, infos, &out));
    CHECK(out.pPrivateDriverData == callData);
    CHECK(out.PrivateDriverDataSize == sizeof(callData));
    CHECK(out.hResource == &resource);
    // Out only: the core layer fills it.
    CHECK(out.hKMResource == 0);
    CHECK(out.NumAllocations == 2);
    CHECK(out.pAllocationInfo == infos);
    CHECK(infos[0].hAllocation == 0);
    CHECK(infos[0].pSystemMem == data0);
    CHECK(infos[0].pPrivateDriverData == data0);
    CHECK(infos[0].PrivateDriverDataSize == sizeof(data0));
    CHECK(infos[0].VidPnSourceId == 2);
    CHECK(infos[0].Flags == UMD_D3D12_ALLOCATION_PRIMARY);
    CHECK(infos[0].GpuVirtualAddress == 0 && infos[0].Priority == 0 && infos[0].Reserved[4] == 0);
    CHECK(infos[1].pSystemMem == NULL);
    CHECK(infos[1].pPrivateDriverData == data1);
    CHECK(infos[1].PrivateDriverDataSize == 12);
    CHECK(infos[1].VidPnSourceId == 0);
    CHECK(infos[1].Flags == UMD_D3D12_ALLOCATION_STEREO);
    // The rest of infos is cleared, not left as it was.
    CHECK(infos[2].pPrivateDriverData == NULL && infos[3].Flags == 0);

    // Venus's own memory carries no runtime resource; the call goes as it is.
    venus.hResource = NULL;
    CHECK(umd_d3d12_allocate_args(&venus, infos, &out));
    CHECK(out.hResource == NULL);
}

static void allocateCallsOutOfBoundsAreRefused(void)
{
    struct umd_kt_allocation_info venusInfos[UMD_D3D12_ALLOCATE_MAX + 1];
    struct umd_kt_allocate venus;
    struct umd_d3d12_allocation_info infos[UMD_D3D12_ALLOCATE_MAX];
    struct umd_d3d12_allocate out, untouched;

    memset(venusInfos, 0, sizeof(venusInfos));
    memset(&venus, 0, sizeof(venus));
    venus.pAllocationInfo = venusInfos;
    memset(&out, 0xcd, sizeof(out));
    untouched = out;

    venus.NumAllocations = 0;
    CHECK(!umd_d3d12_allocate_args(&venus, infos, &out));
    venus.NumAllocations = UMD_D3D12_ALLOCATE_MAX + 1;
    CHECK(!umd_d3d12_allocate_args(&venus, infos, &out));
    venus.NumAllocations = 1;
    venus.pAllocationInfo = NULL;
    CHECK(!umd_d3d12_allocate_args(&venus, infos, &out));
    CHECK(memcmp(&out, &untouched, sizeof(out)) == 0);

    venus.pAllocationInfo = venusInfos;
    venus.NumAllocations = UMD_D3D12_ALLOCATE_MAX;
    CHECK(umd_d3d12_allocate_args(&venus, infos, &out));
    CHECK(out.NumAllocations == UMD_D3D12_ALLOCATE_MAX);
}

static void allocationHandlesGoBackToVenus(void)
{
    struct umd_kt_allocation_info venusInfos[2];
    struct umd_kt_allocate venus;
    struct umd_d3d12_allocation_info infos[UMD_D3D12_ALLOCATE_MAX];
    struct umd_d3d12_allocate out;

    memset(venusInfos, 0, sizeof(venusInfos));
    memset(&venus, 0, sizeof(venus));
    venus.NumAllocations = 2;
    venus.pAllocationInfo = venusInfos;
    CHECK(umd_d3d12_allocate_args(&venus, infos, &out));
    // What the core layer writes.
    out.hKMResource = 0x40000042;
    infos[0].hAllocation = 0x40000081;
    infos[1].hAllocation = 0x40000082;
    infos[2].hAllocation = 0x40000083;
    umd_d3d12_allocate_results(&out, &venus);
    CHECK(venus.hKMResource == 0x40000042);
    CHECK(venusInfos[0].hAllocation == 0x40000081);
    CHECK(venusInfos[1].hAllocation == 0x40000082);
}

static void deallocationsTakeTheCoreLayersForm(void)
{
    const UINT32 handles[2] = {0x40000081, 0x40000082};
    int resource;
    struct umd_kt_deallocate venus;
    struct umd_d3d12_deallocate out;

    memset(&venus, 0, sizeof(venus));
    venus.NumAllocations = 2;
    venus.HandleList = handles;
    memset(&out, 0xab, sizeof(out));
    umd_d3d12_deallocate_args(&venus, &out);
    CHECK(out.hResource == NULL);
    CHECK(out.NumAllocations == 2);
    CHECK(out.HandleList == handles);
    // D3D12DDI_DEALLOCATE_FLAGS_0022_NONE: D3DDDICB_DEALLOCATE has no flags to carry over.
    CHECK(out.Flags == 0);

    venus.hResource = &resource;
    venus.NumAllocations = 0;
    venus.HandleList = NULL;
    umd_d3d12_deallocate_args(&venus, &out);
    CHECK(out.hResource == &resource);
    CHECK(out.NumAllocations == 0);
    CHECK(out.HandleList == NULL);
}

static void vkd3dCacheGoesUnderLocalAppData(void)
{
    WCHAR folder[64];

    CHECK(umd_vkd3d_cache_rule(TRUE, L"C:\\Users\\a\\AppData\\Local", folder, 64) == UMD_CACHE_KEEP);
    CHECK(umd_vkd3d_cache_rule(FALSE, L"C:\\Users\\a\\AppData\\Local", folder, 64) == UMD_CACHE_FOLDER);
    CHECK(wcscmp(folder, L"C:\\Users\\a\\AppData\\Local\\viogpu3d\\vkd3d-proton") == 0);
    CHECK(umd_vkd3d_cache_rule(FALSE, L"C:\\Users\\a\\AppData\\Local\\", folder, 64) == UMD_CACHE_FOLDER);
    CHECK(wcscmp(folder, L"C:\\Users\\a\\AppData\\Local\\viogpu3d\\vkd3d-proton") == 0);
    CHECK(umd_vkd3d_cache_rule(FALSE, L"\\\\server\\home\\Local", folder, 64) == UMD_CACHE_FOLDER);
    CHECK(wcscmp(folder, L"\\\\server\\home\\Local\\viogpu3d\\vkd3d-proton") == 0);
    // Unset, empty or relative: the cache stays off rather than in the current folder.
    CHECK(umd_vkd3d_cache_rule(FALSE, NULL, folder, 64) == UMD_CACHE_OFF);
    CHECK(umd_vkd3d_cache_rule(FALSE, L"", folder, 64) == UMD_CACHE_OFF);
    CHECK(umd_vkd3d_cache_rule(FALSE, L"AppData", folder, 64) == UMD_CACHE_OFF);
    CHECK(umd_vkd3d_cache_rule(FALSE, L"\\", folder, 64) == UMD_CACHE_OFF);
    // Too long for the buffer: off, with no partial path.
    CHECK(umd_vkd3d_cache_rule(FALSE, L"C:\\Users\\a\\AppData\\Local", folder, 30) == UMD_CACHE_OFF);
    CHECK(umd_vkd3d_cache_rule(FALSE, L"C:\\Users\\a\\AppData\\Local", folder, 46) == UMD_CACHE_OFF);
    CHECK(folder[0] == 0);
    CHECK(umd_vkd3d_cache_rule(FALSE, L"C:\\Users\\a\\AppData\\Local", folder, 47) == UMD_CACHE_FOLDER);
}

static void eachDriverLoadsItsOwnVkd3d(void)
{
    WCHAR out[260];
    const WCHAR *arm64x = L"C:\\Windows\\System32\\DriverStore\\FileRepository\\viogpu3d.inf_arm64_1\\"
                          L"viogpu3d_umd.dll";
    const WCHAR *want = L"C:\\Windows\\System32\\DriverStore\\FileRepository\\viogpu3d.inf_arm64_1\\"
                        L"viogpu3d_vkd3d.dll";

    CHECK(wcscmp(umd_vkd3d_name(FALSE), L"viogpu3d_vkd3d.dll") == 0);
    CHECK(wcscmp(umd_vkd3d_name(TRUE), L"viogpu3d_vkd3d_x86.dll") == 0);
    CHECK(umd_path_beside(arm64x, umd_vkd3d_name(FALSE), out, 260));
    CHECK(wcscmp(out, want) == 0);
}

int main(void)
{
    adapterWithVenusIsTaken();
    adapterAnswersAreCheckedInOrder();
    adapterTextsNameEveryOutcome();
    adapterLuidSplitsAsTheKernelDriverPacksIt();
    pipelineLevelsMapToFeatureLevels();
    featureLevelListsRunHighestFirst();
    sharedFormatsDropSrgbAndTypeless();
    scanoutFormatsAreTheOnesVenusDescribes();
    recordLayoutFollowsTheInterface();
    openReadsWhatTheCreatorWrote();
    openRefusesWhatVenusCannotImport();
    openRefusesRecordsOutOfRange();
    openTextsNameEveryOutcome();
    kernelSurfacesOpenAsLinearBlobs();
    sharedTexturesAreLinearWhenTheyCanBe();
    blobMapsA32BitProcessCannotHoldAreCaught();
    allocationsAreSortedByTheirChain();
    aChainThatLoopsIsNotFollowed();
    prependLeavesTheCallersChain();
    icdPathIsBesideTheDriver();
    eachDriverLoadsItsOwnIcd();
    sharedObjectIsMadeOnceAndFreedByTheLastHolder();
    aFailedMakeLeavesNoHolder();
    reopenAfterTheLastCloseMakesANewOne();
    adaptersOpenAndCloseOnManyThreads();
    anOpenAdapterKeepsTheOneInstanceThroughOthersChurn();
    d3d12SharedSurfacesAreOneLinearPlane();
    d3d12BackBuffersOpenInTheD3D11Driver();
    d3d12ImageUsageFollowsTheFlags();
    d3d12ViewFormatsAddSrgbFor8Bit();
    physicalDeviceIsTheAdaptersByLuid();
    memoryTypePrefersDeviceLocal();
    emptyAllocationsNameNoMemory();
    venusAllocationsTakeTheCoreLayersForm();
    allocateCallsOutOfBoundsAreRefused();
    allocationHandlesGoBackToVenus();
    deallocationsTakeTheCoreLayersForm();
    vkd3dCacheGoesUnderLocalAppData();
    eachDriverLoadsItsOwnVkd3d();

    printf("umd host tests: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
