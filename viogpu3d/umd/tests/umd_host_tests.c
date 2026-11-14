/*
 * Copyright (C) 2026 OSMates contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "umd_adapter_info.h"
#include "umd_format.h"
#include "umd_path.h"
#include "umd_pipeline.h"
#include "umd_resource_record.h"
#include "umd_vk_chain.h"

#include <stdio.h>
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

// The kernel driver's own record after VIRTIO_WDDM_CREATE_ALLOCATION (viogpu3d/viogpu3d/allocation.cpp).
#define KMD_STANDARD_MAGIC 0x56334453u

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
    UMD_RESOURCE_RECORD record;
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
    CHECK(offsetof(UMD_RESOURCE_RECORD, Magic) == 0);
    CHECK(UMD_RESOURCE_RECORD_MAGIC != KMD_STANDARD_MAGIC);
    // Below the kernel driver's limit for a resource's extra data.
    CHECK(sizeof(UMD_RESOURCE_RECORD) <= VIRTIO_WDDM_MAX_SUBMIT_COMMAND_VIRTUAL_SIZE);
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

    // A kernel driver surface (GDI, staging) is a virgl 3D resource.
    a = shared_allocation();
    a.create.Base.Tag = VIRTIO_WDDM_ALLOCATE_3D_TAG;
    CHECK(umd_resource_record_open(&a, sizeof(a), 1, &info, &got) == UMD_OPEN_NOT_BLOB);

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
    a.record.Magic = KMD_STANDARD_MAGIC;
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

    BAD_RECORD(Size, 47);
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
    a.record.Width = 16384;
    a.record.Height = 16384;
    a.record.MipLevels = 15;
    a.record.ArraySize = 2048;
    a.record.SampleCount = 32;
    CHECK(umd_resource_record_open(&a, sizeof(a), 1, &info, &got) == UMD_OPEN_OK);
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
    allocationsAreSortedByTheirChain();
    aChainThatLoopsIsNotFollowed();
    prependLeavesTheCallersChain();
    icdPathIsBesideTheDriver();

    printf("umd host tests: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
