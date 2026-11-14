# viogpu3d Direct3D user-mode driver

The Direct3D 10, 11 and 12 driver of viogpu3d: Direct3D 10 and 11 on DXVK, Direct3D 12 on vkd3d-proton, both on Mesa's
Venus Vulkan driver. Two DLLs that export `OpenAdapter10_2` and `OpenAdapter12`:

- `viogpu3d_umd.dll`, ARM64X, in the four `UserModeDriverName` slots: ARM64 processes load its native view, x64
  processes under emulation its ARM64EC view.
- `viogpu3d_umd_x86.dll`, x86, in the four `UserModeDriverNameWow` slots, for x86 processes.

Both run from the driver store (DIRID 13), named there by full path (`%13%\`), with the Venus ICD beside them. Each
loads vkd3d-proton from a DLL of its own beside it, for Direct3D 12 only: `viogpu3d_vkd3d.dll` (ARM64X) and
`viogpu3d_vkd3d_x86.dll`.

## Status

- Compiled and linked for Windows on Arm with llvm-mingw through `Tools/build-macos.sh --d3d-umd`, for aarch64,
  arm64ec and i686, in the Win11 and the Win10 package; vkd3d-proton for ARM64X and i686.
- Not loaded in Windows yet: everything that happens at run time is unproven (see the last section).
- The platform-free parts have unit tests that run on macOS: `sh viogpu3d/umd/tests/run-host-tests.sh`.
- The build checks the package with `tests/check-package.sh`.

## Layers

- `triton/`: Triton's D3D10/11, D3D12 and DXGI DDI layers, MIT, vendored at a pinned commit (`triton/README.md`).
  They turn DDI calls into `ID3D11Device` and `ID3D12Device` calls and ask the rest of the driver for what lies
  below through `triton/triton_backend.h` and `triton/triton12_backend.h`.
- `src/`: this driver's own code: `OpenAdapter10_2`, `OpenAdapter12` and the backends Triton calls.
- `dxvk/`: DXVK, D3D11 on Vulkan, linked in statically (submodule, below).
- `vkd3d-proton/`: vkd3d-proton, D3D12 on Vulkan, LGPL-2.1, unmodified (submodule, below), and `vkd3d/`: the export
  list and version resource its own `d3d12core.dll` objects are linked again with.
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
- `GetCaps` comes before any runtime device, so Triton's caps snapshot runs on a Venus instance that Venus opens
  through `D3DKMT*` itself, and a DXVK device on it that the snapshot releases. Every adapter of a process shares one
  such instance: the first `GetCaps` makes it, the last `CloseAdapter` of an adapter that used it releases it, and
  the next `GetCaps` makes a new one. Adapters may open and close on any thread (`src/umd_shared.c`).
- Shared textures and display primaries: while DXVK makes one, its memory allocation carries
  `VkD3DDDICreateResourceMESA`, so Venus makes the blob through `pfnAllocateCb` for the runtime's resource (with
  `Primary` and the source of a primary), and the driver keeps the kernel allocation the runtime returns for the
  present. Primaries are `HOST3D` blobs in a format the kernel driver scans out. Every shared texture with one level,
  layer and sample is linear: a host that shares Venus memory as host memory, as OSMates' does, binds only linear
  images to it. The texture's record (`VIOGPU3D_SURFACE`, `shared/viogpu3d_uapi.h`) follows the blob request in the
  allocation's private data, for the process that opens it; its layout is the same in 32-bit and 64-bit processes.
- `OpenResource` reads the record from the opened allocation and imports the blob with `VkD3DDDIOpenResourceMESA`,
  linear when its maker made it so. The kernel driver's shared primary and GDI textures carry the same record, as
  linear blobs of its own Venus context, and open the same way. Its shadow and staging surfaces are guest memory with
  no host resource: those are refused, with that reason in the log.
- Venus escapes through the runtime's `pfnEscapeCb`, which the driver wraps: a blob map whose address does not fit
  this process's pointers is unmapped and fails, so that a 32-bit process never uses the low half of a 64-bit address.
- The DXGI DDI (present, blt, residency) is Triton's: it presents with `pfnPresentCb` and the kernel allocation above.

## How Direct3D 12 works

- `OpenAdapter12` takes the adapter with the same check as `OpenAdapter10_2`. Triton's D3D12 layer answers the caps
  from a vkd3d-proton device it makes once per adapter in `GetCaps`, on a Venus instance through `D3DKMT` (no runtime
  device exists yet) that it then releases. The pipeline level is that device's own highest feature level; nothing
  is claimed for a label. DDI `_0082` with DXIL: vkd3d-proton takes DXIL through dxil-spirv.
- The first `GetCaps` or `CreateDevice` of a Direct3D 12 process loads `viogpu3d_vkd3d.dll` beside this DLL by path,
  and the driver calls the `vkd3d.h` API by `GetProcAddress`. vkd3d-proton makes its Vulkan instance through this
  driver's `vkGetInstanceProcAddr`, whose `vkCreateInstance` puts `VkD3DDDICallbacksMESA` in front of the create
  info, so Venus sends its contexts, sync objects, submissions and escapes through the Direct3D 12 runtime's
  kernel-thunk callbacks. Its kernel-thunk table has no `pfnAllocateCb`: allocations go to the core layer's
  `pfnAllocateCb` and `pfnDeallocateCb`, translated to their Direct3D 12 forms (`umd_d3d12_allocate_args`), with
  Venus's resource handle, which is NULL for memory no runtime resource asked for. The device is on the Venus
  physical device with the adapter's LUID.
- `CreateDevice` logs which of the callbacks Venus calls this runtime fills, and refuses the device when the
  kernel-thunk `pfnCreateContextVirtualCb` is NULL: Venus makes its ring contexts with it, and nothing documents it
  under Direct3D 12.
- Fences and presents: the runtime services an app's fences with monitored-fence packets on each queue's kernel
  context, which complete as soon as the context's earlier work has, and the work runs on vkd3d-proton's own Venus
  contexts. So after each `ExecuteCommandLists`, and before each flip present returns, the driver signals a timeline
  semaphore of Venus's on vkd3d-proton's `VkQueue` (`vkd3d_acquire_vk_queue`) and makes the queue's kernel context
  wait for it on the GPU (`pfnWaitForSynchronizationObjectFromGpuCb`, with the sync object Venus exports for the
  semaphore as a D3D11 fence). The runtime's fence signals and present packets on that context then come after the
  work.
- Swapchain back buffers and primaries: vkd3d-proton cannot share a texture's memory after the fact, so the
  driver makes these shared surfaces itself: a linear `VkImage` on vkd3d-proton's `VkDevice`, on Venus memory that
  carries `VkD3DDDICreateResourceMESA` (the runtime's resource and the `VIOGPU3D_SURFACE` record), handed to
  vkd3d-proton with `ID3D12DeviceExt1::CreateResourceFromBorrowedHandle` and moved to its common layout on a queue of
  the driver's. A primary is such a surface itself. A back buffer stays vkd3d-proton's own, optimally tiled texture,
  with such a surface as its present companion, which the present copies the frame into: DWM opens the companion
  through the Direct3D 10 and 11 driver, as it opens a Direct3D 11 app's back buffer.
- Heaps and committed resources with no shared surface get a kernel allocation that names no memory
  (`VIOGPU3D_ALLOCATE_EMPTY_TAG` of `shared/viogpu3d_uapi.h`), because the runtime answers `SetResidencyPriority`
  itself and refuses an object without an allocation.
- vkd3d-proton's disk cache: unless the process sets `VKD3D_SHADER_CACHE_PATH`, the driver sets it before
  vkd3d-proton loads, to `%LOCALAPPDATA%\viogpu3d\vkd3d-proton`, or to `0` (no disk cache) when that folder cannot
  be made: vkd3d-proton would otherwise write into the app's current folder.
- D3D9: the D3D9 slot names this DLL, which has no `OpenAdapter`. No documented key turns D3D9 over to D3D9On12 for
  a driver, so D3D9 apps get D3D9On12 on this driver only when they call `Direct3DCreate9On12` themselves.

## DXVK

- `dxvk` is a submodule of OSMates' DXVK fork, github.com/osmates/dxvk branch `osmates` (zlib): anonymix007's branch
  `umd` at `fd4bee93fd0f`, which builds DXVK for a driver behind the `DXVK_BUILD_UMD` define (an imported instance,
  no WSI, no global instance, no swapchain), and three commits on it: a meson `umd` option that builds static D3D11
  and DXGI libraries, the entry points of `src/d3d11/d3d11_umd.h` (`DXVKUmdCreateFactory`, `DXVKUmdCreateTexture2D`)
  with linear scanout textures, and GDI interop, keyed mutexes and NT handle sharing built out under the option, so
  the driver imports no `D3DKMT*` function. A build without the `umd` option is unchanged by them.
- The build runs DXVK's meson three times on that copy: aarch64-w64-mingw32, arm64ec-w64-mingw32 and
  i686-w64-mingw32. ARM64EC gets the CPU family `aarch64`: its code is ARM64 code, and DXVK's own ARM64EC paths key on
  `__arm64ec__`.

## vkd3d-proton

- `vkd3d-proton` is a submodule of upstream vkd3d-proton (github.com/HansKristian-Work/vkd3d-proton, LGPL-2.1) at its
  `v3.1` release, `4522cc1a8b13`, with no change of OSMates'. The build refuses a checkout at another commit or with
  changes, its submodules included.
- The build runs vkd3d-proton's own meson twice, for ARM64X (as its own `build-arm64x.txt`: the ARM64EC compiler with
  `-marm64x`) and for i686, and builds its `d3d12core.dll`. That DLL exports only `D3D12GetInterface`, which makes its
  device on whatever `vulkan-1.dll` it finds, with no way to give it Venus with the runtime's callbacks. So the build
  links the same objects again with ninja's own link command and two changes: `vkd3d/viogpu3d_vkd3d.def`, which
  exports the API of vkd3d-proton's `include/vkd3d.h` (`vkd3d_create_instance` takes a `vkGetInstanceProcAddr`), and
  the version resource of `vkd3d/viogpu3d_vkd3d.rc`.
- `vkd3d/vkd3d_abi_check.c`, compiled against the headers of the build, checks that `src/umd_vkd3d_api.h`, this
  driver's copy of the `vkd3d.h` structs and of `ID3D12DeviceExt1`, has their layouts.
- The driver loads the DLL by path at run time and calls only functions of `include/vkd3d.h` and
  `ID3D12DeviceExt1::CreateResourceFromBorrowedHandle`, so a vkd3d-proton built and linked the same way can take its
  place.
- vkd3d-proton's `D3DKMT` imports (`GDI32`) and its `dxgi.dll` import come with it. Of these, the driver's path
  reaches only `D3DKMTOpenAdapterFromLuid` and `D3DKMTCreateDevice`, with which vkd3d-proton opens a kernel device of
  its own beside each of its devices; its shared-resource code is never reached (the driver passes no
  `D3D12_HEAP_FLAG_SHARED`), and neither is `dxgi.dll` (the driver never calls `D3D12GetInterface`).

## Building

```sh
Tools/build-macos.sh --d3d-umd --venus-icd <aarch64 Mesa build>/src/virtio/vulkan \
  --venus-icd-arm64ec <arm64ec Mesa build>/src/virtio/vulkan --venus-icd-x86 <i686 Mesa build>/src/virtio/vulkan \
  viogpu3d/viogpu3d/viogpu3d.vcxproj
```

- Needs the kits and the llvm-mingw of `Tools/build-macos.sh fetch` (llvm-mingw next to this clone, or `MINGW_DIR`),
  meson, ninja, glslang, rsync and python3, and
  `git submodule update --init --recursive viogpu3d/umd/dxvk`, and vkd3d-proton with the submodules its build uses:
  `git submodule update --init viogpu3d/umd/vkd3d-proton`, then `git submodule update --init` in it and
  `git submodule update --init third_party/spirv-headers subprojects/dxbc-spirv` in its `subprojects/dxil-spirv`.
- Needs three Mesa builds of the `venus-windows` branch of github.com/osmates/mesa, each configured with
  `-Dvulkan-icd-dir=bin` and the same other options: for aarch64-w64-mingw32, for arm64ec-w64-mingw32 with the Meson
  CPU family `aarch64`, and for i686-w64-mingw32. The package build links the first two into one ARM64X
  `vulkan_virtio.dll` and checks they link the same objects with the same flags.
- Output in `viogpu3d/umd/objfre_win11_arm64`: `viogpu3d_umd.dll` and `viogpu3d_umd_x86.dll` with their `.pdb`
  files, `viogpu3d_vkd3d.dll` and `viogpu3d_vkd3d_x86.dll`; `triton_null.dll` (ARM64X) and `triton_null_x86.dll`,
  Triton linked with the null backend to show Triton needs nothing else; one folder per target with its DXVK build,
  Triton and the driver's objects; `vkd3d/` with vkd3d-proton's two builds.
- The build fails when the driver's own objects or Triton's call a `D3DKMT` function: they go through the runtime's
  callbacks.
- The DLLs' version resource (`viogpu3d_umd.rc`, `vkd3d/viogpu3d_vkd3d.rc`) takes the defines of `viogpu3d.sys`, so
  their file version is the INF's `DriverVer`. Mesa's builds have none: the package build links both Venus DLLs
  again from their builds' link commands with `vulkan_virtio.rc`, the same way.
- Packing needs the llvm-mingw of `Tools/build-macos.sh fetch` even without `--d3d-umd`: its `llvm-cvtres` for the
  Venus DLLs' resource and its licences for the notices.
- The package build writes `viogpu3d_umd_notices.txt` into the package (below).
- Both packages (`--config "Win11 Release"`, the default, and `"Win10 Release"`) install the user-mode files in the
  driver store and name them by full path, as Microsoft documents for graphics drivers that run from it (WDDM 2.1,
  Windows 10 1607 and later; Windows 10 on Arm64 began with 1709). The Win10 package keeps `viogpu3d.sys` in
  `System32\drivers`, as this repository's other Win10 packages do. WOW64 does not redirect the driver store, so x86
  processes find their files there too.
- No MSBuild project yet: a Windows build of this repository does not build it.

## Tests

- `tests/run-host-tests.sh`: the adapter info check and the LUID, pipeline levels to feature levels, shared and
  scanout formats, the texture record in an allocation's private data against `viogpu3d_uapi.h`'s layouts, the
  kernel driver's surface records as it writes them, which shared textures are linear, the blob map check for
  32-bit processes, the Vulkan allocation chains the driver adds to, the name and path of each driver's ICD, and the
  shared `GetCaps` instance: made once, freed by its last holder, made again after, and adapters opening and closing
  on eight threads. For Direct3D 12: which textures can be shared surfaces and the record a Direct3D 11 opener reads
  back from one, their image usage and view formats, the physical device by LUID, the memory type, the memory-less
  allocation, Venus's allocate and deallocate calls in the core layer's forms (fields, `Primary` and `Stereo`, the
  allocation count's bounds, the handles copied back; `umd_vk.c` asserts the mirrored structs against the kits'
  headers), the vkd3d-proton cache folder rule and the vkd3d-proton DLL beside each driver. Built and run twice: with
  ASan and UBSan, and with TSan.
- `tests/check-package.sh <Install/Win11/ARM64 or Install/Win10/ARM64>`, with `MESA_SRC` the Mesa tree of the Venus
  builds and `MINGW_DIR` the llvm-mingw the DLLs were linked with: the INF and the package list the same files; the
  INF copies both Direct3D drivers, both vkd3d-proton DLLs, both Vulkan drivers, their manifests and the notices into
  the driver store; `UserModeDriverName` and `UserModeDriverNameWow` name the drivers there (`%13%\`) in their four
  slots; `VulkanDriverName` and `VulkanDriverNameWow` name the two manifests there, whose `library_path` is the DLL
  beside them; the Direct3D, vkd3d-proton and Vulkan DLLs are ARM64X with an ARM64EC view, or x86; every view exports
  `OpenAdapter10_2` and `OpenAdapter12` only (undecorated on x86, as the runtime looks them up), the `vkd3d.h` API of
  `vkd3d/viogpu3d_vkd3d.def`, or the three `vk_icd` entry points; every view imports system DLLs only (no
  `vulkan-1.dll`, `d3d11.dll`, `dxgi.dll`, libc++ or winpthread; vkd3d-proton's `dxgi.dll` aside), and no view of a
  Direct3D driver imports a `D3DKMT*` function; the file version of all six DLLs is `DriverVer`'s; the notices file
  has the full text of every third-party licence below, and names vkd3d-proton's commit and where its source is.

## Unproven, and not done

- Nothing ran in Windows: that the runtime takes the DDI tables, that Venus works through the runtime's callbacks,
  that DXVK reaches feature level 11_0 or 11_1 through Venus on KosmicKrisp, that DWM composes, that the runtime loads
  the DLLs from their driver store paths. No Windows 10 guest has had the Win10 package.
- x64 apps: that the D3D runtime of an emulated x64 process takes the ARM64EC view of `UserModeDriverName`'s DLL
  (Windows' own system DLLs for x64 apps are ARM64X; no display driver page says it for drivers).
- x86 apps: that the WOW64 runtime and the Vulkan loader read the `Wow` values and find the DLLs beside the 64-bit
  ones; that the kernel driver's blob maps land below 4 GiB for a 32-bit process, as Microsoft documents for
  `MmMapLockedPagesSpecifyCache`.
- Shared textures need Venus to export image memory as `OPAQUE_WIN32_KMT`; primaries and shared textures need linear
  render targets on KosmicKrisp, and the kernel driver's flip of a `HOST3D` primary.
- Opening another process's shared texture or the kernel driver's surfaces: the host side exists (OSMates with the
  virglrenderer pin that imports Venus memory into another context), but that Windows opens these textures through
  this driver is unproven in a guest. Which of the kernel driver's surfaces Windows opens in a Direct3D process at
  all: Microsoft documents the shared primary for full-screen apps and GDI textures for DWM.
- DXVK's GDI interop, keyed mutexes and NT handle sharing are built out (they called `D3DKMT*` from `GDI32.dll`):
  above this driver the runtime does them itself, and Triton never calls DXVK's. Should a call reach DXVK's
  `GetDC`, `AcquireSync`, `ReleaseSync` or `CreateSharedHandle`, it fails with `DXGI_ERROR_UNSUPPORTED`, `E_FAIL` or
  `E_NOTIMPL`, as `OpenSharedResource` and `OpenSharedResource1` already did.
- No D3D9 DDI.
- Direct3D 12: that the runtime takes `OpenAdapter12` and DDI `_0082` from a WDDM 2.0 kernel driver; which
  kernel-thunk callbacks it fills, above all `pfnCreateContextVirtualCb` with the device handle, which Venus makes its
  ring contexts with; that the core layer's `pfnAllocateCb` takes an allocation with a NULL `hResource`, which Venus
  passes for its own memory and every `VkDeviceMemory` no runtime resource asked for (Microsoft documents `hResource`
  only as "the handle of a resource", and its sample passes a heap's runtime handle; a refusal is logged as
  `allocate: the core layer's pfnAllocateCb failed with hResource NULL`); that vkd3d-proton makes a device on Venus
  over KosmicKrisp (with texel buffer views at any texel offset, which vkd3d-proton requires: OSMates' KosmicKrisp
  from osmates/mesa `fa3c7b57` on); the feature level it reaches (11_1 is predicted); that the queue gate keeps app
  fences and presents GPU-true; DWM composing Direct3D 12 back buffers through their companions; the cache folder
  rule; the vkd3d-proton DLL loading in ARM64, x64 and x86 processes.
- Direct3D 12 gaps known now: app waits on CPU-signalled fences are kernel packets the driver never sees (Triton has
  the same gap); opened shared resources (`OpenHeapAndResource`) are refused, as in Triton; a Direct3D 12 primary
  scans out source 0.

## Licences

- `src/` and `tests/`: BSD-3-Clause, as `viogpu3d/LICENSE`.
- `triton/`: MIT, Turing Software LLC, `triton/LICENSE`.
- `dxvk/`: zlib, `dxvk/LICENSE`.
- `dxvk/subprojects/libdisplay-info/`, linked into the DLLs with DXVK: MIT, the libdisplay-info contributors, its
  `LICENSE`.
- `dxvk/subprojects/dxbc-spirv/`, linked into the DLLs with DXVK: MIT, Philip Rebohle, its `LICENSE`.
- `vkd3d-proton/`, in `viogpu3d_vkd3d.dll` and `viogpu3d_vkd3d_x86.dll`: LGPL-2.1, the vkd3d-proton project authors,
  its `LICENSE` and `COPYING`, unmodified, as DLLs of their own. Linked into them: dxil-spirv (MIT, Hans-Kristian
  Arntzen for Valve), dxbc-spirv (MIT, Philip Rebohle), and the files of vkd3d-proton and dxil-spirv that state
  another licence (MIT, BSD, public domain, Apache-2.0 or MIT), which the package build reads from ninja's dependency
  records of the two builds and quotes as each file states it. The notices name the vkd3d-proton commit and where
  its source is: this tree's submodule and build script, offered from the same place as the package.
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
