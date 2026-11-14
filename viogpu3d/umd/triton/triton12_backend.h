/*
 * Copyright 2026 OSMates contributors
 * SPDX-License-Identifier: MIT
 */

// What Triton's D3D12 layer needs from the stack below it: the ID3D12Device it forwards to, the queue gate, shared
// surfaces and memory-less kernel allocations. triton_backend_null.c links and makes nothing. Include through
// triton12.h.

#ifndef TRITON12_BACKEND_H_INCLUDED
#define TRITON12_BACKEND_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

struct TRITON12_ADAPTER;
struct TRITON12_DEVICE;
struct TRITON12_QUEUE;
struct TRITON12_RESOURCE;

// The TRITON_HOSTCAP_* bits of the adapter, latched at OpenAdapter12.
UINT triton12_backend_host_caps(struct TRITON12_ADAPTER *pAdapter);
void triton12_backend_close_adapter(struct TRITON12_ADAPTER *pAdapter);

// GetCaps runs before the runtime makes a device. When *pAdoptable is FALSE, Triton releases the device after the
// snapshot instead of handing it to the first CreateDevice.
HRESULT triton12_backend_create_caps_device(struct TRITON12_ADAPTER *pAdapter, ID3D12Device **ppDevice,
                                            BOOL *pAdoptable);

HRESULT triton12_backend_create_device(struct TRITON12_DEVICE *p, const D3D12DDIARG_CREATEDEVICE_0003 *pArgs,
                                       ID3D12Device **ppDevice);
// After Triton released its D3D12 objects; also after a failed CreateDevice.
void triton12_backend_destroy_device(struct TRITON12_DEVICE *p);

// Whether textures on CPU-visible custom heaps can be made ROW_MAJOR, so that they have a CPU address.
BOOL triton12_backend_host_takes_row_major_texture(ID3D12Device *pDevice);

HRESULT triton12_backend_serialize_root_signature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *pDesc,
                                                  ID3DBlob **ppBlob, ID3DBlob **ppError);

// After the inner queue and q->hKMContext exist.
HRESULT triton12_backend_create_queue(struct TRITON12_DEVICE *p, struct TRITON12_QUEUE *q);
void triton12_backend_destroy_queue(struct TRITON12_DEVICE *p, struct TRITON12_QUEUE *q);

// Holds q->hKMContext until the GPU has finished everything submitted to the inner queue so far, so the runtime's
// fence signals and presents on that context come after the work. FALSE when nothing holds the context back.
BOOL triton12_backend_gate_queue(struct TRITON12_QUEUE *q);

#define TRITON12_SHARED_SURFACE_PRIMARY (1u << 0)

// A texture of pDesc whose memory is a kernel allocation of r's runtime resource, for presents and for the processes
// that open it. On success *phAllocation is that allocation; it goes with the backend's
// triton12_backend_destroy_shared_surface, not with pfnDeallocateCb.
HRESULT triton12_backend_create_shared_surface(struct TRITON12_DEVICE *p, struct TRITON12_RESOURCE *r,
                                               const D3D12_RESOURCE_DESC *pDesc, UINT flags,
                                               ID3D12Resource **ppSurface, D3DKMT_HANDLE *phAllocation);

// After Triton released the surface. Clears r->hKMAllocation when it is the surface's.
void triton12_backend_destroy_shared_surface(struct TRITON12_DEVICE *p, struct TRITON12_RESOURCE *r);

// A kernel allocation of hRTResource that names no memory, for the runtime's residency calls; 0 when there is none.
D3DKMT_HANDLE triton12_backend_allocate_empty(struct TRITON12_DEVICE *p, HANDLE hRTResource);

// The driver's OpenAdapter12 calls this once its backend accepted the adapter.
HRESULT tritonOpenAdapter12(D3D12DDIARG_OPENADAPTER *pOpenData, void *pBackendAdapter);

#ifdef __cplusplus
}
#endif

#endif
