# viogpu3d Direct3D user-mode driver

`viogpu3d_umd.dll`: the Direct3D 10 and 11 driver of viogpu3d, on DXVK and Mesa's Venus Vulkan driver. One DLL in the
four `UserModeDriverName` slots, exporting `OpenAdapter10_2` only.

## Status

- Compiled and linked for Windows 11 ARM64 with llvm-mingw through `Tools/build-macos.sh --d3d-umd`.
- Not loaded in Windows yet: everything that happens at run time is unproven (see the last section).
- The platform-free parts have unit tests that run on macOS: `sh viogpu3d/umd/tests/run-host-tests.sh`.
- The build checks the package with `tests/check-package.sh`.

## Layers

- `triton/`: Triton's D3D10/11 and DXGI DDI layer, MIT, vendored at a pinned commit (`triton/README.md`). It turns
  DDI calls into `ID3D11Device` calls and asks the rest of the driver for what lies below through
  `triton/triton_backend.h`.
- `src/`: this driver's own code: `OpenAdapter10_2` and the backend Triton calls.
- `dxvk/`: DXVK, D3D11 on Vulkan, linked in statically (submodule, below).
- Mesa's Venus ICD, `vulkan_virtio.dll`, which the package installs next to this DLL.

## How it works

- `OpenAdapter10_2` asks the kernel driver for its adapter info (`DXGKQAITYPE_UMDRIVERPRIVATE`,
  `shared/viogpu3d_uapi.h`) and takes the adapter only with the tag, 3D, the host-visible region and the Venus
  capability set.
- `CreateDevice` loads `vulkan_virtio.dll` from this DLL's own folder by path and makes a Vulkan instance with
  `VkD3DDDICallbacksMESA` (Mesa merge request 38731): the runtime's adapter and device handles and callbacks, so Venus
  makes its contexts, allocations and submissions through the runtime, not `D3DKMT*`. DXVK takes that instance over
  (`DXVKUmdCreateFactory`), finds the adapter by the kernel driver's LUID and makes the D3D11 device at the level the
  runtime asked for. No `vulkan-1.dll` anywhere.
- `GetCaps` comes before any runtime device: it gets a Venus instance of its own, through `D3DKMT*`, and a DXVK device
  on it for Triton's caps snapshot. That instance lives until `CloseAdapter`.
- Shared textures and display primaries: while DXVK makes one, its memory allocation carries
  `VkD3DDDICreateResourceMESA`, so Venus makes the blob through `pfnAllocateCb` for the runtime's resource (with
  `Primary` and the source of a primary), and the driver keeps the kernel allocation the runtime returns for the
  present. Primaries are linear `HOST3D` blobs in a format the kernel driver scans out. The texture's D3D11 description
  follows the blob request in the allocation's private data, for the process that opens it.
- `OpenResource` reads that description from the opened allocation and imports the blob with
  `VkD3DDDIOpenResourceMESA`.
- The DXGI DDI (present, blt, residency) is Triton's: it presents with `pfnPresentCb` and the kernel allocation above.

## DXVK

- `dxvk` is a submodule of anonymix007's DXVK, branch `umd` (zlib), at `fd4bee93fd0f`, unchanged. That branch builds
  DXVK for a driver behind the `DXVK_BUILD_UMD` define: an imported instance, no WSI, no global instance, no swapchain.
- It moves to an `osmates/dxvk` fork once OSMates changes it. OSMates already needs three changes: a meson `umd`
  option that builds static D3D11 and DXGI libraries, the entry points of `src/d3d11/d3d11_umd.h`
  (`DXVKUmdCreateFactory`, `DXVKUmdCreateTexture2D`), and linear scanout textures. Until the fork exists they are
  patches kept outside this repository, which the build applies to a copy of the submodule; the submodule itself
  stays as it is. A build without the `umd` option is unchanged by them.

## Building

```sh
Tools/build-macos.sh --d3d-umd --venus-icd <mesa-build>/src/virtio/vulkan viogpu3d/viogpu3d/viogpu3d.vcxproj
```

- Needs the kits and the llvm-mingw of `Tools/build-macos.sh fetch` (llvm-mingw next to this clone, or `MINGW_DIR`),
  meson, ninja, glslang, rsync and patch, and `git submodule update --init --recursive viogpu3d/umd/dxvk`.
- Needs the DXVK patches above in `dxvk-patches` next to this clone, or in the folder `--dxvk-patches` names.
- Output in `viogpu3d/umd/objfre_win11_arm64`: `viogpu3d_umd.dll` and its `.pdb`; `viogpu3d_umd_notices.txt`, the
  licences of the third-party code in the DLL (below), which the package installs beside it; and `triton_null.dll`,
  Triton linked with the null backend to show Triton needs nothing else.
- The DLL's version resource takes the defines of `viogpu3d.sys`, so its file version is the INF's `DriverVer`.
- For the Win11 package only (`--config "Win11 Release"`, the default). The runtime looks for the DLL's bare name in
  the driver store, where that package installs it. The Win10 package installs it in `System32\drivers`, where the
  runtime does not look: it builds and passes the package check, but Windows would not load the driver.
- No MSBuild project yet: a Windows build of this repository does not build it.

## Tests

- `tests/run-host-tests.sh`: the adapter info check and the LUID, pipeline levels to feature levels, shared and
  scanout formats, the texture description in an allocation's private data against `viogpu3d_uapi.h`'s layouts, the
  Vulkan allocation chains the driver adds to, and the path of the ICD.
- `tests/check-package.sh <Install/Win11/ARM64>`: the DLL is ARM64, exports `OpenAdapter10_2` only, imports system
  DLLs only (no `vulkan-1.dll`, `d3d11.dll`, `dxgi.dll`, libc++ or winpthread), its file version is `DriverVer`'s,
  `UserModeDriverName` fills the four slots, the INF and the package list the same files, and the notices file has the
  full text of every licence below but this tree's own.

## Unproven, and not done

- Nothing ran in Windows: that the runtime takes the DDI tables, that Venus works through the runtime's callbacks,
  that DXVK reaches feature level 11_0 or 11_1 through Venus on KosmicKrisp, that DWM composes, that the runtime finds
  the DLL in the driver store by its bare name.
- Shared textures need Venus to export image memory as `OPAQUE_WIN32_KMT`; primaries need linear render targets on
  KosmicKrisp, and the kernel driver's flip of a `HOST3D` primary.
- Opening the kernel driver's own surfaces (GDI and staging allocations, virgl 3D resources) is refused: Venus imports
  blobs only, and DWM opens such surfaces.
- DXVK code that calls `D3DKMT*` directly (GDI interop, keyed mutexes, NT handle sharing) is linked in; Triton's calls
  never reach it.
- No ARM64X or x86 build, no D3D12 or D3D9 DDI.

## Licences

- `src/` and `tests/`: BSD-3-Clause, as `viogpu3d/LICENSE`.
- `triton/`: MIT, Turing Software LLC, `triton/LICENSE`.
- `dxvk/`: zlib, `dxvk/LICENSE`.
- `dxvk/subprojects/libdisplay-info/`, linked into the DLL with DXVK: MIT, the libdisplay-info contributors, its
  `LICENSE`.
- `dxvk/subprojects/dxbc-spirv/`, linked into the DLL with DXVK: MIT, Philip Rebohle, its `LICENSE`.
- The package carries the text of each third-party licence above in `viogpu3d_umd_notices.txt`: MIT asks every copy of
  the code to carry its notice.
