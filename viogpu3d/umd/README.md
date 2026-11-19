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
- Run in a Windows 11 guest (2026-11-18): Direct3D 11 and 12 devices on Venus in ARM64, x64 and x86 processes, and
  a Direct3D 11 draw into a texture that reads back right in all three; with OSMates' next KosmicKrisp a Direct3D 12
  draw too, LogonUI's shared textures, a sign-in, and from 2026-11-19 the desktop on this adapter: DWM composes on
  Venus, both APIs present in windows that show on the Mac's screen, and D3D9On12 makes a HAL device. See "Measured in
  Windows" below.
- The platform-free parts have unit tests that run on macOS: `sh viogpu3d/umd/tests/run-host-tests.sh`.
- The build checks the package with `tests/check-package.sh`.

## Layers

- `triton/`: Triton's D3D10/11, D3D12 and DXGI DDI layers, MIT, vendored at a pinned commit (`triton/README.md`).
  They turn DDI calls into `ID3D11Device` and `ID3D12Device` calls and ask the rest of the driver for what lies
  below through `triton/triton_backend.h` and `triton/triton12_backend.h`.
- `src/`: this driver's own code: `OpenAdapter10_2`, `OpenAdapter12` and the backends Triton calls.
- `dxvk/`: DXVK, D3D11 on Vulkan, linked in statically (submodule, below).
- `vkd3d-proton/`: vkd3d-proton, D3D12 on Vulkan, LGPL-2.1, OSMates' fork (submodule, below), and `vkd3d/`: the
  version resource its `vkd3d-proton.dll` is linked again with, and the check of this driver's copy of its API.
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
  present. DXVK makes that allocation dedicated and exportable whether or not Venus offers
  `VK_KHR_external_memory_win32`: the runtime, not a Win32 handle, shares the resource ("DXVK" below). Primaries
  are `HOST3D` blobs in a format the kernel driver scans out. Every shared texture with one level, layer and sample
  is linear: a host that shares Venus memory as host memory, as OSMates' does, binds only linear images to it. The
  texture's record (`VIOGPU3D_SURFACE`, `shared/viogpu3d_uapi.h`) follows the blob request in the allocation's
  private data, for the process that opens it; its layout is the same in 32-bit and 64-bit processes.
- `OpenResource` reads the record from the opened allocation and imports the blob with `VkD3DDDIOpenResourceMESA`,
  linear when its maker made it so. The kernel driver's shared primary and GDI textures carry the same record, as
  linear blobs of its own Venus context, and open the same way. Its shadow and staging surfaces are guest memory with
  no host resource: those are refused, with that reason in the log.
- Venus escapes through the runtime's `pfnEscapeCb`, which the driver wraps: a blob map whose address does not fit
  this process's pointers is unmapped and fails, so that a 32-bit process never uses the low half of a 64-bit address.
- The DXGI DDI (present, blt, residency) is Triton's: it presents with `pfnPresentCb` and the kernel allocation above.
- The flip gate, for a flip of a scanout primary (DWM's, a full-screen app's): dxgkrnl runs a flip once the earlier
  packets of the present's own kernel context are done, while the frame may still render on Venus's ring contexts, and
  the kernel driver shows the blob the flip names at its next vsync tick, when the host copies it. So before the
  present the driver has DXVK submit the frame (`IDXGIVkInteropDevice::FlushRenderingCommands`), signals a timeline
  semaphore of Venus's on DXVK's queue (`LockSubmissionQueue`), and makes the present's kernel context wait on the GPU
  for the sync object Venus exports for it (`pfnWaitForSynchronizationObjectFromGpuCb`), as the Direct3D 12 queue gate
  does. `VIOGPU3D_UMD_DEBUG=no_flip_gate` leaves it out; every 600 flips the log counts those handed to the kernel
  while their frame's fence had not yet read done. Composited surfaces (an app's windowed swapchain) already wait on
  the CPU for their frame before the present (Triton).
- `VIOGPU3D_UMD_DEBUG=frame_trace` logs lines with the time (`QueryPerformanceCounter`) for each Direct3D 12 present
  (its companion's host resource, the queue's drain values) and each `ExecuteCommandLists` (the present whose copy
  it carries), and in a Direct3D 11 process (DWM) each open of another process's texture (its host resource), the
  first bind of such a texture in each of the device's frames, each flip of a scanout primary (the frame's fence value
  and its completed value, the flip gate's value) and, from a thread, each value the gate's semaphore reaches. A
  flip's values also go to the kernel driver as the present's private data, which it logs with the time it sets the
  source address. With `no_flip_gate` too, the semaphore is still signalled, for the trace, and nothing waits for it.
- The driver's lines and DXVK's go to `OutputDebugString`. With `VIOGPU3D_UMD_LOG` naming a folder, each process also
  appends them to `<folder>\<exe>-<pid>.log`: LogonUI and DWM run in a session no debug-output collector reaches, and
  a machine-wide variable (`setx /M`, then a restart) reaches them.
- Tiled resources: Triton forwards the tile DDIs (`UpdateTileMappings`, `CopyTileMappings`, `CopyTiles`,
  `UpdateTiles`, `TiledResourceBarrier`, `ResizeTilePool`, `GetMipPacking`) to DXVK's `ID3D11DeviceContext2` and
  `ID3D11Device2`, which back tile pools with Vulkan sparse binding through Venus. The backend sets
  `TRITON_HOSTCAP_HOST_FEATURE_LEVEL`, so `D3D11_OPTIONS1` reports DXVK's own tier and each format DXVK's `TILED`
  bit (`triton/tritonTiled.h`). On OSMates' KosmicKrisp (osmates/mesa `81f3ba31`, sparse binding and residency for
  buffers and 2D images, not residency in shaders or strict reads of unmapped tiles) that is tier 1; with residency
  in shaders and strict reads of unmapped tiles it is tier 2.
- Feature level: `GetCaps` reports the level of DXVK's own device, which it makes at the highest level DXVK takes, up
  to 12_1, the last the Direct3D 11 DDI has (its 12_0 and 12_1 pipeline levels are values of the WDDM 2.0 DDI, which
  the driver offers). The level is held where the caps the driver reports do not back it (`triton/tritonLevel.h`):
  12_0 needs tiled resources tier 2 and the typed UAV loads, 12_1 also conservative rasterization and ROVs. Every DDI
  those need forwards to DXVK: the tile DDIs, min/max samplers, conservative rasterization in the WDDM 2.0 rasterizer
  state, and the shaders' residency feedback and ROV declarations, which reach DXVK in the shader's own tokens.
  `CreateDevice` takes all six levels from 10_0 to 12_1. On KosmicKrisp with residency in shaders and strict reads
  of unmapped tiles, DXVK reaches 12_0: no conservative rasterization or ROVs.

## How Direct3D 12 works

- `OpenAdapter12` takes the adapter with the same check as `OpenAdapter10_2`. Triton's D3D12 layer answers the caps
  from a vkd3d-proton device it makes once per adapter in `GetCaps`, on a Venus instance through `D3DKMT` (no runtime
  device exists yet) that it then releases. DDI `_0082` with DXIL: vkd3d-proton takes DXIL through dxil-spirv.
- Feature level: the pipeline level is that device's own highest feature level, with nothing claimed for a label,
  and every cap the runtime checks at that level is vkd3d-proton's: resource binding tier, tiled resources tier, typed
  UAV loads, conservative rasterization and ROVs, and the shader models up to 6.6 (6.7 and later need cap types of a
  newer DDI). The level is held where those caps do not back it (`triton/tritonLevel.h`), and at 12_1 at most: 12_2
  needs raytracing, variable rate shading and sampler feedback, whose DDIs Triton leaves as stubs. The answer to
  `3DPIPELINESUPPORT1` is no higher than the runtime's own highest, and `3DPIPELINESUPPORT`'s stops at 12_1, as the
  feature level 12_2 specification asks. Nothing else holds it: the kernel driver reports no level.
- The first `GetCaps` or `CreateDevice` of a Direct3D 12 process loads `viogpu3d_vkd3d.dll` beside this DLL by path,
  and the driver calls the `vkd3d.h` API by `GetProcAddress`. vkd3d-proton makes its Vulkan instance through this
  driver's `vkGetInstanceProcAddr`, with the device's `VkD3DDDICallbacksMESA` chained to its `VkInstanceCreateInfo`
  (`vk_instance_create_info_next` of the fork's `vkd3d_instance_create_info`), so Venus sends its contexts, sync
  objects, submissions and escapes through the Direct3D 12 runtime's kernel-thunk callbacks. Its kernel-thunk table
  has no `pfnAllocateCb`: allocations go to the core layer's `pfnAllocateCb` and `pfnDeallocateCb`, translated to
  their Direct3D 12 forms (`umd_d3d12_allocate_args`), with Venus's resource handle, which is NULL for memory no
  runtime resource asked for. The device is on the Venus physical device with the adapter's LUID.
- `CreateDevice` logs which of the callbacks Venus calls this runtime fills, and refuses the device when the
  kernel-thunk `pfnCreateContextVirtualCb` is NULL: Venus makes its ring contexts with it. Nothing documents it under
  Direct3D 12; Windows 11's runtime fills it.
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
- Tiled resources: `OPTIONS.TiledResourcesTier` is vkd3d-proton's own, at most tier 3 (`triton/tritonTiled.h`): tier 1
  on KosmicKrisp at `81f3ba31`, with feature level 11_1; tier 2 and feature level 12_0 with residency in shaders and
  strict reads of unmapped tiles. Reserved textures and buffers are vkd3d-proton's reserved
  resources; `UpdateTileMappings`, `CopyTileMappings` and `CopyTiles` go to them, `GetMipPacking` asks vkd3d-proton's
  `GetResourceTiling`, each format carries vkd3d-proton's `TILED` bit, and each mapping change is ordered as a batch
  is: after the other queues' earlier batches, before the app's later fences.
- D3D9: the D3D9 slot names this DLL, which has no `OpenAdapter`. No documented key turns D3D9 over to D3D9On12 for
  a driver, so D3D9 apps get D3D9On12 on this driver only when they call `Direct3DCreate9On12` themselves; D3D9 and
  `Direct3DCreate9On12` list only the adapters that drive a display, which this one does since 2026-11-19.

## DXVK

- `dxvk` is a submodule of OSMates' DXVK fork, github.com/osmates/dxvk branch `osmates` (zlib): anonymix007's branch
  `umd` at `fd4bee93fd0f`, which builds DXVK for a driver behind the `DXVK_BUILD_UMD` define (an imported instance,
  no WSI, no global instance, no swapchain), and three commits on it: a meson `umd` option that builds static D3D11
  and DXGI libraries, the entry points of `src/d3d11/d3d11_umd.h` (`DXVKUmdCreateFactory`, `DXVKUmdCreateTexture2D`)
  with linear scanout textures, and GDI interop, keyed mutexes and NT handle sharing built out under the option, so
  the driver imports no `D3DKMT*` function. A fourth, in the umd build's extension provider: it asks for
  `VK_EXT_image_drm_format_modifier` only on a device that lists it (Venus on a Mac host does not, and the device
  then failed to create). A fifth: a umd build shares an image on a device without `VK_KHR_external_memory_win32` all
  the same, with its dedicated, exportable allocation, since above the driver the runtime shares the resource, not a
  Win32 handle (Venus offers the extension only on a host with `VK_EXT_external_memory_dma_buf`; a Mac host has none).
  It still chains the KMT handle types into the image and its allocation, and for an open
  `VkImportMemoryWin32HandleInfoKHR` with a placeholder handle, which is outside Vulkan's valid usage on such a device
  (`VUID-VkImageCreateInfo-pNext-00990`, `VUID-VkExportMemoryAllocateInfo-handleTypes-00656`). It works because
  Venus's Windows path takes them: it rewrites the handle types to its renderer's (none on a Mac host), makes or opens
  the blob of an allocation by the `VkD3DDDI*MESA` struct it carries, and takes an allocation with
  `VkImportMemoryWin32HandleInfoKHR` as an import (`vn_device_memory.c`, `vn_image.c`). The driver finds those
  allocations by the same structs (`src/umd_vk_chain.c`). A build without the `umd` option is unchanged by them.
- The build runs DXVK's meson three times on that copy: aarch64-w64-mingw32, arm64ec-w64-mingw32 and
  i686-w64-mingw32. ARM64EC gets the CPU family `aarch64`: its code is ARM64 code, and DXVK's own ARM64EC paths key on
  `__arm64ec__`.

## vkd3d-proton

- `vkd3d-proton` is a submodule of OSMates' vkd3d-proton fork, github.com/osmates/vkd3d-proton branch `osmates`
  (LGPL-2.1): upstream's `v3.1` release, `4522cc1a8b13`, and three commits on it, none of which changes a build or
  a caller that does not ask for it:
  - a meson `enable_library` option that builds `vkd3d-proton.dll`, which exports the API of `include/vkd3d.h`
    (`vkd3d_create_instance` takes a `vkGetInstanceProcAddr`) and nothing else, where the stock `d3d12core.dll`
    exports only `D3D12GetInterface`, which makes its device on whatever `vulkan-1.dll` it finds;
  - a meson `enable_d3dkmt` option, on by default; off, vkd3d-proton calls no `D3DKMT` function (below);
  - `vk_instance_create_info_next` in `vkd3d_instance_create_info`, chained to the `VkInstanceCreateInfo` of the
    instance (NULL keeps the old behaviour): this driver's `VkD3DDDICallbacksMESA` goes there, the device's own
    struct, which Venus keeps and writes to.
- The build takes the submodule at the commit this tree records, on top of `v3.1`, its own submodules at the commits
  it records. It refuses changes no commit holds; `VKD3D_ALLOW_CHANGES=1` builds them anyway, and the notices then
  call the DLLs a test build.
- The build runs vkd3d-proton's own meson twice, for ARM64X (as its own `build-arm64x.txt`: the ARM64EC compiler with
  `-marm64x`) and for i686, with `-Denable_library=true -Denable_d3dkmt=false`, builds its `vkd3d-proton.dll`, and
  links it again with ninja's own link command into `viogpu3d_vkd3d.dll` and `viogpu3d_vkd3d_x86.dll`, adding the
  version resource of `vkd3d/viogpu3d_vkd3d.rc`, as it does for the Venus DLLs.
- `vkd3d/vkd3d_abi_check.c`, compiled against the headers of the build, checks that `src/umd_vkd3d_api.h`, this
  driver's copy of the `vkd3d.h` structs and of `ID3D12DeviceExt1`, has their layouts.
- The driver loads the DLL by path at run time and calls only functions of `include/vkd3d.h` and
  `ID3D12DeviceExt1::CreateResourceFromBorrowedHandle`, so a vkd3d-proton built the same way can take its place.
- Without D3DKMT, vkd3d-proton opens no kernel device of its own beside each of its devices
  (`D3DKMTOpenAdapterFromLuid`, `D3DKMTCreateDevice`, `D3DKMTCloseAdapter`, `D3DKMTDestroyDevice`); shared heaps,
  resources and fences are no kernel objects of its own (`D3DKMTOpenResourceFromNtHandle`,
  `D3DKMTOpenSyncObjectFromNtHandle`, `D3DKMTEscape`, `D3DKMTDestroyAllocation`, `D3DKMTDestroyKeyedMutex`,
  `D3DKMTDestroySynchronizationObject`); and `ID3D12Device::CreateSharedHandle` and `OpenSharedHandle` return
  `E_NOTIMPL` (`D3DKMTShareObjects`, `D3DKMTQueryResourceInfo`, `D3DKMTOpenResource2`,
  `D3DKMTQueryResourceInfoFromNtHandle`), as on vkd3d-proton's other platforms. Above this driver the Direct3D 12
  runtime does all of that itself, and Triton never asks vkd3d-proton for it: it passes no `D3D12_HEAP_FLAG_SHARED`
  or `D3D12_FENCE_FLAG_SHARED`, calls neither method, and refuses `OpenHeapAndResource`. The DLLs import neither
  `GDI32.dll` nor `dxgi.dll`.

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
  callbacks. The package check fails a Direct3D driver or a vkd3d-proton DLL that imports one.
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
  headers), the vkd3d-proton cache folder rule and the vkd3d-proton DLL beside each driver. For both: the tiled
  resources tier Triton reports (`triton/tritonTiled.h`), for host tiers 0 to 3 at the 11_0, 11_1 and 12_0 labels,
  with and without `TRITON_HOSTCAP_HOST_FEATURE_LEVEL`; the feature level each reports (`triton/tritonLevel.h`), for
  inner levels 11_0 to 12_1 and tiers 0 to 3, with and without the flag, with the caps that hold it lower, 12_2 held
  at 12_1, the runtime's highest, the Direct3D 11 pipeline bits, and that Triton and the driver read the 12_0 and
  12_1 pipeline levels alike. Built and run twice: with ASan and UBSan, and with TSan.
- `tests/check-package.sh <Install/Win11/ARM64 or Install/Win10/ARM64>`, with `MESA_SRC` the Mesa tree of the Venus
  builds and `MINGW_DIR` the llvm-mingw the DLLs were linked with: the INF and the package list the same files; the
  INF copies both Direct3D drivers, both vkd3d-proton DLLs, both Vulkan drivers, their manifests and the notices into
  the driver store; `UserModeDriverName` and `UserModeDriverNameWow` name the drivers there (`%13%\`) in their four
  slots; `VulkanDriverName` and `VulkanDriverNameWow` name the two manifests there, whose `library_path` is the DLL
  beside them; the Direct3D, vkd3d-proton and Vulkan DLLs are ARM64X with an ARM64EC view, or x86; every view exports
  `OpenAdapter10_2` and `OpenAdapter12` only (undecorated on x86, as the runtime looks them up), the `vkd3d.h` API of
  the fork's `libs/vkd3d-proton/vkd3d-proton.def`, or the three `vk_icd` entry points; every view imports system DLLs
  only (no `vulkan-1.dll`, `d3d11.dll`, `dxgi.dll`, libc++ or winpthread), and no view of a Direct3D driver or of a
  vkd3d-proton DLL imports a `D3DKMT*` function; the file version of all six DLLs is `DriverVer`'s; the notices file
  has the full text of every third-party licence below, says vkd3d-proton is a modified one, and names the fork, its
  commit and where its source is.

## Measured in Windows

A Windows 11 guest on OSMates' accelerated GPU, the driver's lines collected from `OutputDebugString` (2026-11-18):

- The Direct3D 10 and 11 runtime and the Direct3D 12 runtime open the adapter through this driver: `OpenAdapter10_2`
  and `OpenAdapter12` log the LUID and capability sets `0x16` (Venus among them); Triton offers its versions; the
  driver loads `vulkan_virtio.dll` and `viogpu3d_vkd3d.dll` from the driver store folder, and their `_x86` builds in
  x86 processes. The Direct3D 12 runtime takes the version Triton offers from this WDDM 2.0 kernel driver.
- x64 and x86 processes, both APIs: the x64 runtimes take the ARM64EC view of `UserModeDriverName`'s DLL, the WOW64
  ones `UserModeDriverNameWow`'s, and Venus maps its memory in a 32-bit process.
- Venus's contexts need the kernel driver's nodes to report `GpuMmuSupported` (from a restart of Windows on), and its
  memory needs the kernel driver to open allocations with `DxgkCbAcquireHandleData` (`../README.md`). Before both,
  every device failed: `0xc000000d` for each context, then `failed to allocate/map ring shmem`.
- Direct3D 12: `D3D12CreateDevice` makes a vkd3d-proton device on Venus in ARM64, x64 and x86 processes, at feature
  level 12_0 at most, shader model 6.0, resource binding tier 3, tiled resources tier 2. A compute queue's copy that
  the direct queue waits for reads back right, a queue waiting for a CPU fence holds until the CPU signals,
  `MakeResident`, `Evict` and `SetResidencyPriority` succeed, and the tiled resources sample passes every check but
  one (a buffer tile read in the invocation that wrote it). The core layer's `pfnAllocateCb` takes `hResource` NULL
  for Venus's own memory, as `D3DDDICB_ALLOCATE` documents (NULL ties the allocations to the device; only shared
  resources need one), and the driver logs the first such allocation of each process; the `E_INVALIDARG` seen before
  was the kernel driver's failed open.
- Direct3D 12 graphics pipelines: on OSMates' KosmicKrisp the triangle's pipeline fails to build ("Compiler failed to
  build request"; the Metal compiler's message is "use of undeclared identifier 'ALU' ... 'fcanonicalize'": the
  shaders ask for denormal flushing, and KosmicKrisp printed no MSL for `fcanonicalize`). Venus makes pipelines without
  waiting for the answer, so the next `vkCmdBindPipeline` names an object the host does not have and the process ends
  with `vn_ring_submit abort on fatal`. With a KosmicKrisp that prints it (`kk: Emit fcanonicalize`, 2026-11-18, not
  in OSMates' pin yet) the triangle draws into a texture and reads back right.
- Shared Direct3D 11 textures (LogonUI's render targets, a swapchain's buffers): the driver makes them linear, and
  OSMates' KosmicKrisp refuses linear images with color attachment usage, so DXVK fails them ("Cannot create
  texture"); `kk: Allow linear 2D color attachments` (the same day, not in OSMates' pin yet) takes them, and on the
  host a draw, a blend and a blit into a linear 2D image read back right with it. Then DXVK did not share them: it
  shared an image only through `VK_KHR_external_memory_win32`, which Venus on Windows offers only on a host with
  `VK_EXT_external_memory_dma_buf` (a Mac host has none), so no kernel allocation reached the runtime's resource and
  LogonUI crashed (`0xc000027b` in `Windows.UI.Xaml.dll`, 20 times in a boot's first minute). DXVK's umd build now
  shares them without the extension ("DXVK" above): no LogonUI crash, and a sign-in works, with DWM, Explorer and the
  shell on this driver.
- Direct3D 12 presents, on a host before main `d3303957`: `CreateSwapChainForHwnd` failed (`E_OUTOFMEMORY` on the
  desktop, `DXGI_ERROR_NOT_CURRENTLY_AVAILABLE` in session 0). That host refused the back buffers' blob
  (`RESOURCE_CREATE_BLOB`, `ERR_UNSPEC`), so `pfnAllocateCb` with `hResource` set failed with `E_FAIL`. Main
  `d3303957` pins virglrenderer `b6e26840`, which makes these blobs: with it, the KosmicKrisp above and DXVK's sharing,
  a Direct3D 11 and a Direct3D 12 triangle each present 600 frames in an 800 x 600 flip swapchain on the signed-in
  desktop and read their centre pixel back right. The desktop is Basic Display's, so the Mac's screen stays black.
- Direct3D 11: `D3D11CreateDevice` makes a DXVK device on Venus in ARM64, x64 and x86 processes, at feature level
  12_0 when asked for it, and a triangle drawn into a texture 120 times reads back as drawn. Before, DXVK's
  `vkCreateDevice` failed with `VK_ERROR_EXTENSION_NOT_PRESENT`: DXVK's umd extension provider added
  `VK_EXT_image_drm_format_modifier` without asking the device, Venus on a Mac host does not list it, and Mesa's
  device creation refused it before anything reached the host (Venus's own log of the create info, `VN_DEBUG=init`).
  The provider now asks for it only where the device lists it (the `dxvk` submodule).
- The Direct3D 12 runtime's kernel-thunk table has `CreateContextVirtual`, `DestroyContext`,
  `CreateSynchronizationObject2`, `DestroySynchronizationObject`, `SignalFromGpu2`, `WaitFromGpu`, `SignalFromCpu`,
  `WaitFromCpu`, `SubmitCommand`, `Escape`, `CreatePagingQueue` and `MakeResident`, and no `Allocate` or
  `Deallocate`; the core layer has `Allocate`, `Deallocate` and `CreateContextVirtual`.
- Until 2026-11-18 Windows kept the desktop on a software display adapter, whose render side DXGI lists as "Microsoft
  Basic Render Driver" without `DXGI_ADAPTER_FLAG_SOFTWARE`: an app that takes the first adapter that is not software
  can render with WARP. D3D9 lists only the adapters that drive a display, so neither D3D9 nor `Direct3DCreate9On12`
  found this one (`d3d9-clear` found only Basic Render), and the Mac's screen stayed black (`../README.md`).
- The desktop on this adapter (2026-11-19, with the kernel driver's DMA buffers for CDD and its flips by allocation,
  `../README.md`): Windows sets the host's 2880 x 1800 on it at boot, the lock screen and the signed-in desktop show
  on the Mac's screen, and DWM, Explorer and the shell run on this driver. A Direct3D 11 and a Direct3D 12 triangle
  each present 600 frames in an 800 x 600 flip swapchain window (12.3 s and 16.4 s, against 23.9 s and 45.2 s on
  Basic Display's desktop), read their centre pixel back right, and show on the Mac's screen. D3D9 lists this adapter
  first: a plain HAL device fails with `D3DERR_NOTAVAILABLE` (no D3D9 DDI), and `Direct3DCreate9On12` makes a HAL
  device whose clear reads back right.
- Whole frames (2026-11-19, a window that changes its whole colour every frame and screenshots one after another): DWM's
  flips showed no torn frame with the flip gate left out (467 shots of a Direct3D 11 and a Direct3D 12 window) or in
  (463), at the same frame rates. Every one of DWM's flips was handed to the kernel before its frame's fence read
  done, with the gate or without it; the gate makes the present's kernel context wait for the frame on the GPU.
  Direct3D 11 triangle windows: no torn shot of 276. Direct3D 12 triangle windows: 117 of 200 shots torn without the
  flip gate, 68 of 196 with it, several frames in bands; that sample's presents are blts of a companion
  (`12.Present ... dst=0x0`), not flips. With the system node in physical mode (a variant, not in this tree) and the
  gate: 827 shots of the colour windows, one of them a frame drawn in part, in tiles; triangles, Direct3D 11 none of
  131 torn, Direct3D 12 66 of 191.
- Flips traced (2026-11-19, `frame_trace`): the flip gate holds DWM's flips: the kernel sets the source address about
  7 ms after the present with it, 0.06 ms without, at the same frame rates. A Direct3D 11 window that changes its
  whole colour every frame: 2 of 173 shots drawn in part without the gate (one in bands, one in tiles), none of the
  187 judged with it.

## Unproven, and not done

- No Windows 10 guest has had the Win10 package.
- Direct3D 12 draws wait for OSMates' KosmicKrisp pin to have `fcanonicalize` (above).
- Shared Direct3D 11 textures and primaries: OSMates' KosmicKrisp pin with `kk: Allow linear 2D color attachments`
  (measured with a host app that has it, not with main's).
- Not measured: whether Venus must keep the WSI extensions it implements itself (`VK_KHR_incremental_present`,
  `VK_EXT_hdr_metadata`, the swapchain maintenance ones) from the host when an app enables them without
  `VK_KHR_swapchain`. DXVK did so in session 0 in an earlier run; in later session 0 runs it enabled
  `VK_KHR_swapchain` with them, and the host took `VK_EXT_hdr_metadata` without it from vkd3d-proton. A Mesa change
  for it waits for a session 0 run that shows the device failing without the change and made with it.
- Opening another process's shared texture or the kernel driver's surfaces: the host side exists (OSMates with the
  virglrenderer pin that imports Venus memory into another context), but that Windows opens these textures through
  this driver is unproven in a guest. Which of the kernel driver's surfaces Windows opens in a Direct3D process at
  all: Microsoft documents the shared primary for full-screen apps and GDI textures for DWM.
- DXVK's GDI interop, keyed mutexes and NT handle sharing are built out (they called `D3DKMT*` from `GDI32.dll`):
  above this driver the runtime does them itself, and Triton never calls DXVK's. Should a call reach DXVK's
  `GetDC`, `AcquireSync`, `ReleaseSync` or `CreateSharedHandle`, it fails with `DXGI_ERROR_UNSUPPORTED`, `E_FAIL` or
  `E_NOTIMPL`, as `OpenSharedResource` and `OpenSharedResource1` already did.
- No D3D9 DDI.
- The flip gate: that a gated flip's frame is done when the kernel sets its source address (the trace shows the flip
  held; the kernel driver logs the frame's completed value at the present only).
- Direct3D 12: the torn frames of a triangle window (above), not explained: the queue gate holds each present until
  the copy into its companion is done, and a window that changes its colour every frame through the same path shows
  none; DWM composing Direct3D 12 back buffers through their companions; the cache folder rule;
  that the fork's build without D3DKMT changes nothing an app sees through the runtime (no call of the runtime's reaches
  the parts it built out).
- Direct3D 12 gaps known now: app waits on CPU-signalled fences are kernel packets the driver never sees (Triton has
  the same gap); opened shared resources (`OpenHeapAndResource`) are refused, as in Triton; a Direct3D 12 primary
  scans out source 0.
- Tiled resources, both APIs: that the runtimes take the tier with the per-format `TILED` bits Venus gives (no
  combined depth and stencil on KosmicKrisp), that the Direct3D 12 runtime sends a reserved buffer with no heap and
  no placement, as the driver takes it, and that Venus on Windows binds tiles of the memory DXVK and vkd3d-proton
  allocate. Tier 1 leaves reads of unmapped tiles undefined; on KosmicKrisp they read zero.
- Feature level 12_0, both APIs: that the runtimes take it from these caps (Microsoft documents what each level
  needs, not what the runtimes check), and residency feedback and strict reads of unmapped tiles through Venus on
  KosmicKrisp.

## Licences

- `src/` and `tests/`: BSD-3-Clause, as `viogpu3d/LICENSE`.
- `triton/`: MIT, Turing Software LLC, `triton/LICENSE`.
- `dxvk/`: zlib, `dxvk/LICENSE`.
- `dxvk/subprojects/libdisplay-info/`, linked into the DLLs with DXVK: MIT, the libdisplay-info contributors, its
  `LICENSE`.
- `dxvk/subprojects/dxbc-spirv/`, linked into the DLLs with DXVK: MIT, Philip Rebohle, its `LICENSE`.
- `vkd3d-proton/`, in `viogpu3d_vkd3d.dll` and `viogpu3d_vkd3d_x86.dll`: LGPL-2.1, the vkd3d-proton project authors,
  its `LICENSE` and `COPYING`, as DLLs of their own. It is a modified library (LGPL-2.1 section 2): the notices say
  so, and list the fork's commits on top of `v3.1` with their dates and the files they change. Linked into them:
  dxil-spirv (MIT, Hans-Kristian Arntzen for Valve), dxbc-spirv (MIT, Philip Rebohle), and the files of vkd3d-proton
  and dxil-spirv that state another licence (MIT, BSD, public domain, Apache-2.0 or MIT), which the package build
  reads from ninja's dependency records of the two builds and quotes as each file states it. The notices name the
  fork, its commit and where its source is: the fork at that commit, with this tree's version resource and build
  script, offered from the same place as the package.
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
