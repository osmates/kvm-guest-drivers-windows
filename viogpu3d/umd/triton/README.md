# Triton

Triton's D3D10/11 and DXGI DDI layer: it turns the Direct3D runtime's DDI calls into `ID3D11Device` and
`ID3D11DeviceContext` calls. MIT, Copyright 2026 Turing Software LLC: see `LICENSE`. The package carries it beside
the driver in `viogpu3d_umd_notices.txt`, which `Tools/build-macos.sh --d3d-umd` writes and
`umd/tests/check-package.sh` checks.

## Source

- github.com/osy/virtio-win-mesa, branch `neptune`, commit `fc5f1fc8a389e125e7d5f979f7b88a2708d161ee`
  (2026-10-29), folder `src/virtio/neptune/triton`.
- Taken: `triton.h`, `triton_log.h`, `tritonBlit.hlsl`, `tritonBlitShaders.h`, `tritonDDI.c`, `tritonDxbc.c`,
  `tritonDxbc.h`, `tritonDxgi.c`, `tritonDxgi.h`, `tritonHostCaps.c`, `tritonHostCaps.h`, `tritonPresent.c`,
  `tritonPresent.h`, `tritonPresentTiming.h`, `tritonQuery.c`, `tritonResource.c`, `tritonShader.c`, `tritonState.c`,
  `tritonView.c`.
- Left out: the D3D12 DDI layer (`*12*`, `tritonDxil.*`), the files tied to Neptune's transport or to another kernel
  driver's interface (`tritonSharedBridge.*`, `tritonKm.*`, `triton.c`), Neptune's DLL definition and resources
  (`neptune_umd.def`, `neptune_umd.rc`), the tests and tools, and the meson file.

## Changes here

All in one direction: Triton's calls into Neptune go through `triton_backend.h`, which names what Triton needs from
the stack below it.

- `triton_backend.h`, `triton_backend_null.c` and `triton_null.def` are new. The null backend makes nothing and links:
  the build links it with Triton into `triton_null.dll` to show no Neptune symbol is left.
- `tritonDDI.c`: the device comes from `triton_backend_create_device` and goes with `triton_backend_destroy_device`,
  also after a failed create; the version ladder and the tile-based caps read `triton_backend_adapter_probe`;
  `OpenAdapter10_2` is `tritonOpenAdapter10_2`, not exported, and keeps the backend's adapter; CloseAdapter calls
  `triton_backend_close_adapter`; a caps device the backend calls not adoptable is released after the snapshot.
- `tritonHostCaps.c`: the D3D11 snapshot takes its device from `triton_backend_create_caps_device`; the D3D12
  snapshot is out.
- `tritonHostCaps.h`: `tritonHostCaps11Snapshot` takes the adapter (`pAdapter`, with a forward declaration of
  `struct TRITON_ADAPTER`) and says whether the device may be adopted (`pAdoptable`).
- `tritonQuery.c`: no `tritonSharedBridge.h` include.
- `tritonResource.c`: shared, primary and companion textures come from `triton_backend_create_shared_texture`, which
  also gives their kernel allocation and makes a primary's storage linear (`TRITON_D3D11_MISC_LINEAR_EXPORT` stays
  defined in `triton.h` for a backend that wants it); a primary without a kernel allocation is refused;
  `tritonOpenResource` takes the texture and its description from `triton_backend_open_shared_texture`;
  `tritonDestroyResource` ends with `triton_backend_destroy_resource`, after the D3D11 releases, where Neptune
  released its import first.
- `tritonDxgi.c`, `tritonPresentTiming.h`, `tritonShader.c`, `tritonDxbc.c`, `triton_log.h`: the debug switches, the
  shared format, the shader workaround bits and the log go through the backend.
- `triton.h`: `TRITON_ADAPTER` and `TRITON_DEVICE` carry `pBackend` in place of the kernel-mode state; it includes
  `triton_backend.h` and the standard headers `tritonKm.h` used to bring in.

`diff -ru` of these files against the commit above gives the change to offer upstream.

## Updating

Take the same files from a newer commit, carry the changes above over, and build with
`Tools/build-macos.sh --d3d-umd`: it fails while any `npt_` symbol is still undefined.
