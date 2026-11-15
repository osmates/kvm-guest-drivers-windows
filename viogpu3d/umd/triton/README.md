# Triton

Triton's D3D10/11, D3D12 and DXGI DDI layers: they turn the Direct3D runtime's DDI calls into `ID3D11Device` and
`ID3D11DeviceContext` calls, and into `ID3D12Device` calls. MIT, Copyright 2026 Turing Software LLC: see `LICENSE`.
The package carries it beside the drivers in `viogpu3d_umd_notices.txt`, which `Tools/build-macos.sh` writes and
`umd/tests/check-package.sh` checks.

## Source

- github.com/osy/virtio-win-mesa, branch `neptune`, commit `fc5f1fc8a389e125e7d5f979f7b88a2708d161ee`
  (2026-10-29), folder `src/virtio/neptune/triton`.
- Taken: `triton.h`, `triton_log.h`, `tritonBlit.hlsl`, `tritonBlitShaders.h`, `tritonDDI.c`, `tritonDxbc.c`,
  `tritonDxbc.h`, `tritonDxgi.c`, `tritonDxgi.h`, `tritonHostCaps.c`, `tritonHostCaps.h`, `tritonPresent.c`,
  `tritonPresent.h`, `tritonPresentTiming.h`, `tritonQuery.c`, `tritonResource.c`, `tritonShader.c`, `tritonState.c`,
  `tritonView.c`; and the D3D12 layer: `triton12.h`, `triton12_stubs.h`, `triton12_tables.h`, `tritonDDI12.c`,
  `tritonDescriptor12.c`, `tritonDxil.c`, `tritonDxil.h`, `tritonList12.c`, `tritonPipeline12.c`, `tritonPresent12.c`,
  `tritonQueue12.c`, `tritonResource12.c`, and `tools/gen_triton12_stubs.py`, which writes `triton12_stubs.h`.
- Left out: the files tied to Neptune's transport or to another kernel driver's interface (`tritonSharedBridge.*`,
  `tritonKm.*`, `triton.c`), Neptune's DLL definition and resources (`neptune_umd.def`, `neptune_umd.rc`), the tests,
  the other tools, and the meson file.

## Changes here

All in one direction: Triton's calls into Neptune go through `triton_backend.h` and, for D3D12, `triton12_backend.h`,
which name what Triton needs from the stack below it.

- `triton_backend.h`, `triton12_backend.h`, `triton_backend_null.c` and `triton_null.def` are new. The null backend
  makes nothing and links: the build links it with Triton into `triton_null.dll`, which exports `OpenAdapter10_2` and
  `OpenAdapter12`, to show no Neptune symbol is left.
- `tritonTiled.h` is new: the tiled resources tier both layers report, without platform headers, so the driver's host
  tests build it. With `TRITON_HOSTCAP_HOST_FEATURE_LEVEL` it is the inner device's tier, at most tier 3 (the DDIs'
  ceiling; every tiled DDI forwards to the inner device); without it, Triton's own answers stay: none for Direct3D 11,
  and for Direct3D 12 tier 2 at a 12_x label, none below. It also holds the `D3D_FEATURE_LEVEL` values
  (`TRITON_LEVEL_*`) both new headers use.
- `tritonLevel.h` is new: the feature level both layers report and the caps behind it, without platform headers, for
  the same tests. With `TRITON_HOSTCAP_HOST_FEATURE_LEVEL` it is the inner device's level, held where what Triton
  reports does not back it: 12_0 needs tiled tier 2 and the typed UAV loads (and, for Direct3D 12, resource binding
  tier 2), 12_1 conservative rasterization and ROVs. Direct3D 12 stops at 12_1, as raytracing, variable rate shading
  and sampler feedback are stubs; Direct3D 11 at 12_1, its DDI's last, whose 12_x values need the WDDM 2.0 DDI. Without
  the flag, Triton's own labels stay.
- `tritonDDI.c`: the device comes from `triton_backend_create_device` and goes with `triton_backend_destroy_device`,
  also after a failed create; the version ladder and the tile-based caps read `triton_backend_adapter_probe`;
  `OpenAdapter10_2` is `tritonOpenAdapter10_2`, not exported, and keeps the backend's adapter; CloseAdapter calls
  `triton_backend_close_adapter`; a caps device the backend calls not adoptable is released after the snapshot.
- `tritonHostCaps.c`: the D3D11 snapshot takes its device from `triton_backend_create_caps_device`, the D3D12 one
  from `triton12_backend_create_caps_device`, which says whether the device may be adopted. The D3D11 snapshot asks
  for levels up to 12_1, where Triton asked up to 11_1. The D3D12 snapshot also asks `FEATURE_LEVELS` and logs the
  answer. The header comment no longer says the device is Neptune's.
- `tritonHostCaps.h`: `tritonHostCaps11Snapshot` takes the adapter (`pAdapter`, with a forward declaration of
  `struct TRITON_ADAPTER`) and says whether the device may be adopted (`pAdoptable`); its comment names the levels up
  to 12_1.
- `tritonDDI.c`, the feature level: `3DPIPELINESUPPORT` reports the inner device's level up to 12_1
  (`tritonLevel11`), where Triton stopped at 11_1 on the reading that the 12_x pipeline levels are D3D12's (the D3D11
  DDI has them as WDDM 2.0 values); `CreateDevice` takes the 12_0 and 12_1 pipeline levels (`tritonLevelFromPipeline11`);
  the WDDM 2.0 test of the version ladder is `tritonOffersWddm2_0`, which both use.
- `tritonQuery.c`: no `tritonSharedBridge.h` include.
- `tritonResource.c`: shared, primary and companion textures come from `triton_backend_create_shared_texture`, which
  also gives their kernel allocation and makes a primary's storage linear (`TRITON_D3D11_MISC_LINEAR_EXPORT` stays
  defined in `triton.h` for a backend that wants it); a primary without a kernel allocation is refused;
  `tritonOpenResource` takes the texture and its description from `triton_backend_open_shared_texture`;
  `tritonDestroyResource` ends with `triton_backend_destroy_resource`, after the D3D11 releases, where Neptune
  released its import first.
- `tritonDDI.c`: the slots Triton leaves unimplemented get one stub per slot type, with that slot's parameters, in
  place of one stub cast into every slot. On x86 the callee pops its arguments (stdcall), so the cast stub would
  have left the caller's stack wrong; on x64 and ARM64 nothing changes.
- Direct3D 11 tiled resources, which were stubs: `tritonResource.c` forwards `UpdateTileMappings`,
  `CopyTileMappings`, `CopyTiles`, `UpdateTiles`, `TiledResourceBarrier` and `ResizeTilePool` to the inner
  `ID3D11DeviceContext2`, and `GetMipPacking` to the inner `ID3D11Device2::GetResourceTiling`; `CreateResource` passes
  `MISC_TILED` and `MISC_TILE_POOL` on. `tritonDDI.c` puts them in the WDDM 1.3 table and answers `D3D11_OPTIONS1`
  from `tritonTiled.h` (`tritonTiledSupportFlags`, declared in `triton.h`); `tritonQuery.c` passes the inner
  device's per-format `TILED` bit on when that answer has a tier.
- `tritonDxgi.c`, `tritonPresentTiming.h`, `tritonShader.c`, `tritonDxbc.c`, `triton_log.h`: the debug switches, the
  shared format, the shader workaround bits and the log go through the backend.
- `triton.h`: `TRITON_ADAPTER` and `TRITON_DEVICE` carry `pBackend` in place of the kernel-mode state; it includes
  `triton_backend.h` and the standard headers `tritonKm.h` used to bring in.

The D3D12 layer:

- `triton12.h`: `TRITON12_ADAPTER`, `TRITON12_DEVICE`, `TRITON12_QUEUE` and `TRITON12_RESOURCE` carry `pBackend`; the
  device has no `TRITON_KM` and no tile-mapping ring edge; the queue has no present-arm event;
  `triton12RegisterSharedBlob` became `triton12CreateSharedResource`.
- `tritonDDI12.c`: the inner device comes from `triton12_backend_create_device` and goes with
  `triton12_backend_destroy_device`, also after a failed create; the caps device is released after the snapshot when
  the backend calls it not adoptable; the host caps come from `triton12_backend_host_caps`; `OpenAdapter12` is
  `tritonOpenAdapter12`, not exported, and keeps the backend's adapter; CloseAdapter calls
  `triton12_backend_close_adapter`. With the new `TRITON_HOSTCAP_HOST_FEATURE_LEVEL`, the pipeline level is the inner
  device's own highest feature level, held where the caps do not back it and at 12_1 at most (`tritonLevel.h`),
  conservative rasterization is the inner device's tier, not claimed for the FL 12_1 label, and
  `OPTIONS.TiledResourcesTier` is the inner device's (`tritonTiled.h`), where Triton reported none at an 11_x label;
  `CheckFormatSupport` then passes the inner device's per-format `TILED` bit on. Level and tiers are worked out in
  `t12Level`, out of `GetCaps`. `3DPIPELINESUPPORT1` answers no higher than the runtime's highest and
  `3DPIPELINESUPPORT` no higher than 12_1, as the feature level 12_2 specification asks. Whether
  textures on CPU-visible heaps are row major comes from `triton12_backend_host_takes_row_major_texture`; the
  `EXECUTECOMMANDLISTS_PARALLELISM` answer's note names the queue gate, not Neptune's ring.
- `tritonHostCaps.h`, the D3D12 part: the new `TRITON_HC12_FEATURE_LEVELS` bit and `maxFeatureLevel` field;
  `tritonHostCaps12Snapshot` takes the adapter (`pAdapter`, with a forward declaration of `struct TRITON12_ADAPTER`)
  and says whether the device may be adopted (`pAdoptable`). The header comment names the backends' caps device in
  place of Neptune's.
- `tritonDescriptor12.c`: the header comment only, which named Neptune's device and its override file.
- `tritonQueue12.c`: the gate after `ExecuteCommandLists` signals the drain fence as before and then calls
  `triton12_backend_gate_queue`, in place of Neptune's gate arm and its kernel driver's gate packet; the backend's
  queue state is made and freed with the queue; no Neptune ring edges for tile mappings (the inner device orders its
  own queues); the debug switches go through `triton_backend_debug`. With `TRITON_HOSTCAP_HOST_FEATURE_LEVEL`, a
  forwarded `UpdateTileMappings` or `CopyTileMappings` is ordered as a batch is: it waits on the sibling queues' drain
  fences first and runs the same gate after, so the app's fences come after the mapping change.
- `tritonPresent12.c`: primaries and present companions are the backend's shared surfaces
  (`triton12_backend_create_shared_surface`), where Neptune exported a host texture as a blob; memory-less
  allocations come from `triton12_backend_allocate_empty`; a flip present calls the queue gate, where Neptune armed a
  present token, sent its kernel driver a gate escape and polled the drain fence; the CPU poll stays for a queue the
  gate cannot hold; companions made on demand share one helper, `t12EnsureCompanion`; the log goes through
  `TR_LOG`. `ID3D12Resource_GetDesc` goes through `lpVtbl`: mingw-w64's `d3d12.h` has the C macro only with
  `WIDL_C_INLINE_WRAPPERS`.
- `tritonResource12.c`: no shared heap flags reach the inner device; a primary is made by
  `triton12CreateSharedResource`; the companion's create moved to `tritonPresent12.c`; a resource's shared surface is
  freed with `triton12_backend_destroy_shared_surface` before its placeholder allocation. With
  `TRITON_HOSTCAP_HOST_FEATURE_LEVEL` only: a reserved buffer (no heap, no placement, the `ROW_MAJOR` layout every
  buffer has) takes the reserved path too, where it fell to a committed buffer whose tiles nothing maps, and keeps its
  layout; `GetMipPacking` of a reserved resource the host backs asks the inner device's `GetResourceTiling`: the host
  maps the tail by its own tiling, and Metal keeps some mips smaller than a tile out of it.
- `tritonPipeline12.c`: root signatures serialize through `triton12_backend_serialize_root_signature`; the vertex
  input rewrite asks `triton_backend_workaround_flags`.
- `tritonList12.c`: the list migration switch goes through `triton_backend_debug`.
- `triton12_stubs.h` and `tools/gen_triton12_stubs.py`: `<stddef.h>` for `offsetof`, which MSVC gets through
  `windows.h`.
- Comments that described Neptune's transport were changed where the code they describe changed.

`diff -ru` of these files against the commit above gives the change to offer upstream.

## Updating

Take the same files from a newer commit, carry the changes above over, and build with
`Tools/build-macos.sh --d3d-umd`: it fails while any `npt_` symbol is still undefined.
