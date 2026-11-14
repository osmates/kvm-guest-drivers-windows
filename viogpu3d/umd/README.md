# viogpu3d Direct3D user-mode driver

The Direct3D 10 and 11 driver of viogpu3d, on DXVK and Mesa's Venus Vulkan driver, in two DLLs that export
`OpenAdapter10_2` only:

- `viogpu3d_umd.dll`, ARM64X, in the four `UserModeDriverName` slots: ARM64 processes load its native view, x64
  processes under emulation its ARM64EC view.
- `viogpu3d_umd_x86.dll`, x86, in the four `UserModeDriverNameWow` slots, for x86 processes.

## Status

- Compiled and linked for Windows 11 on Arm with llvm-mingw through `Tools/build-macos.sh --d3d-umd`, for
  aarch64, arm64ec and i686.
- Not loaded in Windows yet: everything that happens at run time is unproven (see the last section).
- The platform-free parts have unit tests that run on macOS: `sh viogpu3d/umd/tests/run-host-tests.sh`.
- The build checks the package with `tests/check-package.sh`.

## Layers

- `triton/`: Triton's D3D10/11 and DXGI DDI layer, MIT, vendored at a pinned commit (`triton/README.md`). It turns
  DDI calls into `ID3D11Device` calls and asks the rest of the driver for what lies below through
  `triton/triton_backend.h`.
- `src/`: this driver's own code: `OpenAdapter10_2` and the backend Triton calls.
- `dxvk/`: DXVK, D3D11 on Vulkan, linked in statically (submodule, below).
- Mesa's Venus ICD, which the package installs next to these DLLs: `vulkan_virtio.dll` (ARM64X) for the ARM64X driver,
  `vulkan_virtio_x86.dll` for the x86 one.

## How it works

- `OpenAdapter10_2` asks the kernel driver for its adapter info (`DXGKQAITYPE_UMDRIVERPRIVATE`,
  `shared/viogpu3d_uapi.h`) and takes the adapter only with the tag, 3D, the host-visible region and the Venus
  capability set.
- `CreateDevice` loads the Venus ICD of its own architecture from this DLL's own folder by path and makes a Vulkan
  instance with `VkD3DDDICallbacksMESA` (Mesa merge request 38731): the runtime's adapter and device handles and
  callbacks, so Venus makes its contexts, allocations and submissions through the runtime, not `D3DKMT*`. DXVK takes
  that instance over (`DXVKUmdCreateFactory`), finds the adapter by the kernel driver's LUID and makes the D3D11
  device at the level the runtime asked for. No `vulkan-1.dll` anywhere.
- `GetCaps` comes before any runtime device: it gets a Venus instance of its own, through `D3DKMT*`, and a DXVK device
  on it for Triton's caps snapshot. That instance lives until `CloseAdapter`.
- Shared textures and display primaries: while DXVK makes one, its memory allocation carries
  `VkD3DDDICreateResourceMESA`, so Venus makes the blob through `pfnAllocateCb` for the runtime's resource (with
  `Primary` and the source of a primary), and the driver keeps the kernel allocation the runtime returns for the
  present. Primaries are linear `HOST3D` blobs in a format the kernel driver scans out. The texture's D3D11 description
  follows the blob request in the allocation's private data, for the process that opens it; its layout is the same
  in 32-bit and 64-bit processes.
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
- The build runs DXVK's meson three times on that copy: aarch64-w64-mingw32, arm64ec-w64-mingw32 and
  i686-w64-mingw32. ARM64EC gets the CPU family `aarch64`: its code is ARM64 code, and DXVK's own ARM64EC paths key on
  `__arm64ec__`.

## Building

```sh
Tools/build-macos.sh --d3d-umd --venus-icd <aarch64 Mesa build>/src/virtio/vulkan \
  --venus-icd-arm64ec <arm64ec Mesa build>/src/virtio/vulkan --venus-icd-x86 <i686 Mesa build>/src/virtio/vulkan \
  viogpu3d/viogpu3d/viogpu3d.vcxproj
```

- Needs the kits and the llvm-mingw of `Tools/build-macos.sh fetch` (llvm-mingw next to this clone, or `MINGW_DIR`),
  meson, ninja, glslang, rsync, patch and python3, and
  `git submodule update --init --recursive viogpu3d/umd/dxvk`.
- Needs the DXVK patches above in `dxvk-patches` next to this clone, or in the folder `--dxvk-patches` names.
- Needs three Mesa builds of the `venus-windows` branch of github.com/osmates/mesa, each configured with
  `-Dvulkan-icd-dir=bin` and the same other options: for aarch64-w64-mingw32, for arm64ec-w64-mingw32 with the Meson
  CPU family `aarch64`, and for i686-w64-mingw32. The package build links the first two into one ARM64X
  `vulkan_virtio.dll` and checks they link the same objects with the same flags.
- Output in `viogpu3d/umd/objfre_win11_arm64`: `viogpu3d_umd.dll` and `viogpu3d_umd_x86.dll` with their `.pdb`
  files; `triton_null.dll` (ARM64X) and `triton_null_x86.dll`, Triton linked with the null backend to show Triton
  needs nothing else; one folder per target with its DXVK build, Triton and the driver's objects.
- The DLLs' version resource (`viogpu3d_umd.rc`) takes the defines of `viogpu3d.sys`, so their file version is the
  INF's `DriverVer`. Mesa's builds have none: the package build links both Venus DLLs again from their builds' link
  commands with `vulkan_virtio.rc`, the same way.
- Packing needs the llvm-mingw of `Tools/build-macos.sh fetch` even without `--d3d-umd`: its `llvm-cvtres` for the
  Venus DLLs' resource and its licences for the notices.
- The package build writes `viogpu3d_umd_notices.txt` into the package (below).
- For the Win11 package only (`--config "Win11 Release"`, the default). The runtime looks for the DLLs' bare names in
  the driver store, where that package installs them. The Win10 package installs them in `System32\drivers`, where
  the runtime does not look: it builds and passes the package check, but Windows would not load the drivers.
- No MSBuild project yet: a Windows build of this repository does not build it.

## Tests

- `tests/run-host-tests.sh`: the adapter info check and the LUID, pipeline levels to feature levels, shared and
  scanout formats, the texture description in an allocation's private data against `viogpu3d_uapi.h`'s layouts, the
  Vulkan allocation chains the driver adds to, and the name and path of each driver's ICD.
- `tests/check-package.sh <Install/Win11/ARM64>`, with `MESA_SRC` the Mesa tree of the Venus builds and `MINGW_DIR`
  the llvm-mingw the DLLs were linked with: the INF and the package list the same files; `UserModeDriverName` and
  `UserModeDriverNameWow` fill their four slots;
  `VulkanDriverName` and `VulkanDriverNameWow` name the two manifests, whose `library_path` is the DLL beside them;
  the Direct3D and Vulkan drivers are ARM64X with an ARM64EC view, or x86; every view exports `OpenAdapter10_2` only
  (undecorated on x86, as the runtime looks it up) or the three `vk_icd` entry points; every view imports system
  DLLs only (no `vulkan-1.dll`, `d3d11.dll`, `dxgi.dll`, libc++ or winpthread); the file version of all four DLLs is
  `DriverVer`'s; the notices file has the full text of every third-party licence below.

## Unproven, and not done

- Nothing ran in Windows: that the runtime takes the DDI tables, that Venus works through the runtime's callbacks,
  that DXVK reaches feature level 11_0 or 11_1 through Venus on KosmicKrisp, that DWM composes, that the runtime finds
  the DLLs in the driver store by their bare names.
- x64 apps: that the D3D runtime of an emulated x64 process takes the ARM64EC view of `UserModeDriverName`'s DLL
  (Windows' own system DLLs for x64 apps are ARM64X; no display driver page says it for drivers).
- x86 apps: that the WOW64 runtime and the Vulkan loader read the `Wow` values and find the DLLs beside the 64-bit
  ones; that the kernel driver maps blobs below 4 GiB for a 32-bit process (the blob map escape returns a 64-bit
  pointer).
- Shared textures need Venus to export image memory as `OPAQUE_WIN32_KMT`; primaries need linear render targets on
  KosmicKrisp, and the kernel driver's flip of a `HOST3D` primary.
- Opening the kernel driver's own surfaces (GDI and staging allocations, virgl 3D resources) is refused: Venus imports
  blobs only, and DWM opens such surfaces.
- DXVK code that calls `D3DKMT*` directly (GDI interop, keyed mutexes, NT handle sharing) is linked in; Triton's calls
  never reach it.
- No D3D12 or D3D9 DDI.

## Licences

- `src/` and `tests/`: BSD-3-Clause, as `viogpu3d/LICENSE`.
- `triton/`: MIT, Turing Software LLC, `triton/LICENSE`.
- `dxvk/`: zlib, `dxvk/LICENSE`.
- `dxvk/subprojects/libdisplay-info/`, linked into the DLLs with DXVK: MIT, the libdisplay-info contributors, its
  `LICENSE`.
- `dxvk/subprojects/dxbc-spirv/`, linked into the DLLs with DXVK: MIT, Philip Rebohle, its `LICENSE`.
- Mesa's Venus ICD in the package: mostly MIT, with some files under Apache-2.0, BSL-1.0, SGI-B-2.0, BSD and the
  DirectX-Headers licence. The package build reads which from the Mesa files the three builds compiled (ninja's
  dependency records): each file's SPDX identifier, else the licence its text states, else its subproject's licence
  file, else Mesa's MIT; with every copyright line of those files.
- The llvm-mingw runtime, linked statically into all four DLLs: mingw-w64's startup code, `libmingw32` and
  `libmingwex` (gdtoa among it: David M. Gay and Lucent Technologies, whose notice must be in all copies) under
  `COPYING.MinGW-w64-runtime.txt`, and LLVM's libc++, libc++abi, libunwind and compiler-rt builtins under LLVM's
  `LICENSE.TXT` (Apache-2.0 with LLVM exceptions). The package build takes both from `MINGW_DIR` and from the
  llvm-mingw each Venus build linked with.
- The package carries all of the above in `viogpu3d_umd_notices.txt`, in full: MIT asks every copy of the code to
  carry its notice.
