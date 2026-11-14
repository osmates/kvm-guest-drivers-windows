/*
 * The interface between viogpu3d and its user-mode drivers.
 *
 * It is the one Mesa's Venus driver for Windows uses (src/virtio/virtio-gpu/wddm_hw.h, merge request 38731):
 * the same tags, layouts and meanings, so that the Vulkan ICD built from that work drives this driver.
 *
 * Copyright (C) 2026 OSMates contributors
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met :
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and / or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of their contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#pragma once

// Each request starts with its tag, which says what follows.
#define VIRTIO_WDDM_ADAPTER_INFO_TAG                5066067239879590230ull
#define VIRTIO_WDDM_ESCAPE_CAPSET_TAG               6003370060433016150ull
#define VIRTIO_WDDM_ESCAPE_RESOURCE_INFO_TAG        5066067248504063318ull
#define VIRTIO_WDDM_ESCAPE_RESOURCE_BUSY_TAG        6004778599252706646ull
#define VIRTIO_WDDM_ESCAPE_BLOB_MAP_TAG             4778391004737914198ull
#define VIRTIO_WDDM_ESCAPE_CONTEXT_INIT_TAG         5285066810588349782ull
#define VIRTIO_WDDM_ESCAPE_BLIT_INIT_TAG            6073469419966907734ull
#define VIRTIO_WDDM_ESCAPE_EXEC_BUF_TAG             4847377628439725398ull
#define VIRTIO_WDDM_CREATE_RESOURCE_TAG             5932161353946775894ull
#define VIRTIO_WDDM_ALLOCATE_3D_TAG                 5496436857343066454ull
#define VIRTIO_WDDM_ALLOCATE_BLOB_TAG               4778121577295003990ull
#define VIRTIO_WDDM_SUBMIT_COMMAND_VIRTUAL_TAG      6216178336949162838ull

// The largest private data of one submission: its tag, command headers and their bodies.
#define VIRTIO_WDDM_MAX_SUBMIT_COMMAND_VIRTUAL_SIZE 8192

#define VIRTIO_WDDM_CAPSET_ID_VIRGL                 1
#define VIRTIO_WDDM_CAPSET_ID_VIRGL2                2
#define VIRTIO_WDDM_CAPSET_ID_GFXSTREAM             3
#define VIRTIO_WDDM_CAPSET_ID_VENUS                 4
#define VIRTIO_WDDM_CAPSET_ID_CROSS_DOMAIN          5
#define VIRTIO_WDDM_CAPSET_ID_DRM                   6

// Bit n says that capability set n is there.
#define VIRTIO_WDDM_CAPSET_MASK(id)                 (1ull << (id))

#define VIRTIO_WDDM_COMMAND_ID_NOP                  0
#define VIRTIO_WDDM_COMMAND_ID_SUBMIT               1
#define VIRTIO_WDDM_COMMAND_ID_TRANSFER_TO_HOST     2
#define VIRTIO_WDDM_COMMAND_ID_TRANSFER_FROM_HOST   3
#define VIRTIO_WDDM_COMMAND_ID_FENCE                4
// The driver's own; a user-mode request that names one is refused.
#define VIRTIO_WDDM_COMMAND_ID_MAP_APERTURE         0xF000
#define VIRTIO_WDDM_COMMAND_ID_UNMAP_APERTURE       0xF001

#define VIRTIO_WDDM_COMMAND_FLAG_RING_IDX           (1 << 0)
#define VIRTIO_WDDM_COMMAND_FLAG_SHADOW_VIRGL       (1 << 1)

#define VIRTIO_WDDM_BLOB_MEM_GUEST                  (1u << 0)
#define VIRTIO_WDDM_BLOB_MEM_HOST3D                 (1u << 1)
#define VIRTIO_WDDM_BLOB_MEM_HOST3D_GUEST           ((1u << 0) | (1u << 1))

#define VIRTIO_WDDM_BLOB_FLAG_NONE                  0u
#define VIRTIO_WDDM_BLOB_FLAG_MAPPABLE              (1u << 0)
#define VIRTIO_WDDM_BLOB_FLAG_SHAREABLE             (1u << 1)
#define VIRTIO_WDDM_BLOB_FLAG_CROSS_DEVICE          (1u << 2)

#define VIRTIO_WDDM_BLOB_MAP_FLAGS_UNMAP            (1u << 0)

#pragma pack(push, 1)

// DXGKQAITYPE_UMDRIVERPRIVATE.
typedef struct _VIRTIO_WDDM_ADAPTER_INFO
{
    UINT64 Tag;
    UINT64 Luid;
    UINT64 CapsetMask;
    UINT8 Supports3d;
    UINT8 HasShmem;
} VIRTIO_WDDM_ADAPTER_INFO;

// Allocation private data of a virgl 3D resource.
typedef struct _VIRTIO_WDDM_ALLOCATE_3D
{
    UINT64 Tag;
    UINT32 Target;
    UINT32 Format;
    UINT32 Bind;
    UINT32 Width;
    UINT32 Height;
    UINT32 Depth;
    UINT32 ArraySize;
    UINT32 LastLevel;
    UINT32 NrSamples;
    UINT32 Flags;
    UINT64 Size;
} VIRTIO_WDDM_ALLOCATE_3D;

typedef struct _VIRTIO_WDDM_BLOB_INFO
{
    UINT32 Width;
    UINT32 Height;
    UINT32 Format;
    UINT32 Bind;
} VIRTIO_WDDM_BLOB_INFO;

// Allocation private data of a blob. Size comes back as the blob's real size, at least what was asked.
typedef struct _VIRTIO_WDDM_ALLOCATE_BLOB
{
    UINT64 Tag;
    UINT64 Id;
    UINT32 Mem;
    UINT32 Flags;
    UINT64 Size;
    VIRTIO_WDDM_BLOB_INFO Info;
} VIRTIO_WDDM_ALLOCATE_BLOB;

typedef union _VIRTIO_WDDM_CREATE_ALLOCATION_INFO {
    UINT64 Tag;
    VIRTIO_WDDM_ALLOCATE_3D Resource3d;
    VIRTIO_WDDM_ALLOCATE_BLOB Blob;
} VIRTIO_WDDM_CREATE_ALLOCATION_INFO;

typedef struct _VIRTIO_WDDM_PLANE_LAYOUT
{
    UINT64 Offset;
    UINT64 Size;
    UINT64 Stride;
} VIRTIO_WDDM_PLANE_LAYOUT;

// The private data of each allocation; ResId comes back.
typedef struct _VIRTIO_WDDM_CREATE_ALLOCATION
{
    VIRTIO_WDDM_CREATE_ALLOCATION_INFO Base;
    UINT32 ResId;
    UINT32 NumPlanes;
    UINT64 Modifier;
    VIRTIO_WDDM_PLANE_LAYOUT Planes[4];
} VIRTIO_WDDM_CREATE_ALLOCATION;

// The resource's private data; a command header may follow, sent before the blob is made.
typedef struct _VIRTIO_WDDM_CREATE_RESOURCE
{
    UINT64 Tag;
} VIRTIO_WDDM_CREATE_RESOURCE;

// Escape: the capability set follows, as many bytes as the escape has room for.
typedef struct _VIRTIO_WDDM_CAPSET
{
    UINT64 Tag;
    UINT32 CapsetId;
    UINT32 Version;
} VIRTIO_WDDM_CAPSET;

// Escape on a device: makes the device's virtio context, of that capability set.
typedef struct _VIRTIO_WDDM_CONTEXT_INIT
{
    UINT64 Tag;
    UINT32 CapsetId;
    UINT32 NumRings;
    UINT8 DebugName[64];
} VIRTIO_WDDM_CONTEXT_INIT;

typedef struct _VIRTIO_WDDM_RESOURCE_INFO
{
    UINT64 Tag;
    UINT32 Handle;
    UINT32 Padding;
    VIRTIO_WDDM_CREATE_ALLOCATION Info;
} VIRTIO_WDDM_RESOURCE_INFO;

typedef struct _VIRTIO_WDDM_RESOURCE_BUSY
{
    UINT64 Tag;
    UINT64 Event;
    UINT32 Handle;
    UINT8 Wait;
    UINT8 IsBusy;
} VIRTIO_WDDM_RESOURCE_BUSY;

// Escape: maps a mappable blob into the calling process, or with VIRTIO_WDDM_BLOB_MAP_FLAGS_UNMAP takes Ptr out.
typedef struct _VIRTIO_WDDM_BLOB_MAP
{
    UINT64 Tag;
    UINT32 Handle;
    UINT32 Flags;
    UINT64 Ptr;
} VIRTIO_WDDM_BLOB_MAP;

// Escape on a context: one command header and its body follow; FenceId comes back, for a FENCE command.
typedef struct _VIRTIO_WDDM_EXEC_BUFFER
{
    UINT64 Tag;
    UINT64 FenceId;
} VIRTIO_WDDM_EXEC_BUFFER;

typedef struct _VIRTIO_WDDM_BOX_3D
{
    UINT32 X;
    UINT32 Y;
    UINT32 Z;
    UINT32 Width;
    UINT32 Height;
    UINT32 Depth;
} VIRTIO_WDDM_BOX_3D;

typedef struct _VIRTIO_WDDM_COMMAND_TRANSFER
{
    UINT32 ResId;
    UINT32 Stride;
    UINT64 Offset;
    UINT32 Level;
    UINT32 LayerStride;
    VIRTIO_WDDM_BOX_3D Box;
} VIRTIO_WDDM_COMMAND_TRANSFER;

// A command; Size bytes of body follow it.
typedef struct _VIRTIO_WDDM_COMMAND_HEADER
{
    UINT16 Id;
    UINT8 Flags;
    UINT8 Ring;
    UINT32 Size;
} VIRTIO_WDDM_COMMAND_HEADER;

// The private data of a submission; command headers follow until its end.
typedef struct _VIRTIO_WDDM_SUBMIT_COMMAND
{
    UINT64 Tag;
} VIRTIO_WDDM_SUBMIT_COMMAND;

#pragma pack(pop)

C_ASSERT(sizeof(VIRTIO_WDDM_ADAPTER_INFO) == 26);
C_ASSERT(sizeof(VIRTIO_WDDM_ALLOCATE_3D) == 56);
C_ASSERT(sizeof(VIRTIO_WDDM_ALLOCATE_BLOB) == 48);
C_ASSERT(sizeof(VIRTIO_WDDM_CREATE_ALLOCATION_INFO) == 56);
C_ASSERT(sizeof(VIRTIO_WDDM_CREATE_ALLOCATION) == 168);
C_ASSERT(FIELD_OFFSET(VIRTIO_WDDM_CREATE_ALLOCATION, ResId) == 56);
C_ASSERT(sizeof(VIRTIO_WDDM_CAPSET) == 16);
C_ASSERT(sizeof(VIRTIO_WDDM_CONTEXT_INIT) == 80);
C_ASSERT(sizeof(VIRTIO_WDDM_RESOURCE_INFO) == 184);
C_ASSERT(sizeof(VIRTIO_WDDM_RESOURCE_BUSY) == 22);
C_ASSERT(sizeof(VIRTIO_WDDM_BLOB_MAP) == 24);
C_ASSERT(sizeof(VIRTIO_WDDM_EXEC_BUFFER) == 16);
C_ASSERT(sizeof(VIRTIO_WDDM_COMMAND_TRANSFER) == 48);
C_ASSERT(sizeof(VIRTIO_WDDM_COMMAND_HEADER) == 8);
C_ASSERT(sizeof(VIRTIO_WDDM_SUBMIT_COMMAND) == 8);
