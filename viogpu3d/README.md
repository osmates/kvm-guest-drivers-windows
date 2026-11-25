# viogpu3d

A full WDDM driver (render and display) for a virtio GPU with 3D: virgl contexts, blob resources and Venus. It lives
in its own directory with its own copy of viogpu's `common` code, its own names, GUIDs and INF, so that the
display-only driver in `viogpu` stays as it is.

## Status

- Compiled for Windows 11 ARM64 with clang and lld-link through `Tools/build-macos.sh`. Not built with MSVC yet.
- Run in a Windows 11 guest on OSMates' accelerated GPU (2026-11-18): the driver starts (problem code 0) and dxgkrnl
  reports the adapter as render and display, WDDM 2.0, GpuMmu. Direct3D 11 and 12 devices run on it through DXVK,
  vkd3d-proton and Venus in ARM64, x64 and x86 processes, and a Direct3D 11 draw into a texture reads back right. With
  a host that has OSMates' next KosmicKrisp (osmates/mesa `kk-linear-color-attachment`), Windows puts the desktop on
  this adapter at the host's 2880 x 1800 from boot on (2026-11-19): the lock screen and the signed-in desktop show on
  the Mac's screen, DWM composes on Venus, a Direct3D 11 and a Direct3D 12 window present 600 frames each, and D3D9
  apps that ask for D3D9On12 get a HAL device on this adapter.
- dxgkrnl fails the adapter after a successful start, code 43, when its software key has no `UserModeDriverName`
  (`Microsoft-Windows-DxgKrnl-Admin` event 549, "Object Name not found", `StartAdapter_AddAdapterFailed`). So the
  package always carries the Direct3D driver, and the build refuses to pack it without the Direct3D driver's DLLs,
  which come from this or an earlier `--d3d-umd`.
- The package carries Mesa's Venus Vulkan driver, cross-built with llvm-mingw: `vulkan_virtio.dll` (ARM64X, for
  ARM64 and x64 processes, named by `VulkanDriverName`) and `vulkan_virtio_x86.dll` (named by
  `VulkanDriverNameWow`). And the Direct3D 10, 11 and 12 user-mode driver of `umd/`: `viogpu3d_umd.dll` (ARM64X,
  named by `UserModeDriverName`) and `viogpu3d_umd_x86.dll` (named by `UserModeDriverNameWow`), with vkd3d-proton
  beside them as `viogpu3d_vkd3d.dll` and `viogpu3d_vkd3d_x86.dll`. Windows loads them from the driver store in
  ARM64, x64 and x86 processes; what works and what fails after that: `umd/README.md`.

## Device

- Needs `VIRTIO_GPU_F_VIRGL` and Venus. Without either the device fails to start and Windows keeps Basic Display.
- Takes `EDID`, `RESOURCE_BLOB`, `CONTEXT_INIT` and `BLOB_ALIGNMENT` when offered.
- Venus needs blobs, context init, the `VENUS` capability set and the host-visible region: shared memory region 1
  of a `VIRTIO_PCI_CAP_SHARED_MEMORY_CFG` capability, which may sit behind a 64-bit BAR of 4 GiB. The driver needs it
  for the surfaces dxgkrnl asks for (below).
- The INF matches QEMU's virtio-gpu-gl-pci (subsystem `1100`, revision 1, the IDs of QEMU's 2D GPU too) and
  OSMates' accelerated GPU (subsystem `1af4:1101`, revision 2). OSMates' 2D GPU is revision 1, so viogpudo keeps it
  with both drivers installed.

## Model

- WDDM 2.0 with GPU virtual addressing (GpuMmu). The page tables are written by the CPU, in the implicit system
  memory segment (segment 0), one page each, as `DXGK_PAGE_TABLE_LEVEL_DESC` documents for that segment: two levels
  of 256 entries and a root of 16 over 32 bits. The device never reads a GPU virtual address: it addresses memory by
  resource. The driver allocates no contiguous memory for them, so a start never fails for want of it. VidMm makes
  and fills them through `UPDATE_PAGE_TABLE`, which logs each update; `DxgkDdiSetRootPageTable` keeps the root it
  gets on the context and logs it, though no boot has called it yet (2026-11-19).
- One segment: a 1 GiB aperture of system memory.
- 64 nodes: 0 for the system, 1 for Venus, 2 for paging, 3 to 63 for Venus rings 0 to 60. Mesa's Venus ICD opens
  one virtual context a ring on nodes 3 to 63, and the Direct3D drivers open their present and queue contexts on node
  1 (`VIOGPU3D_NODE_RUNTIME`, `shared/viogpu3d_uapi.h`). Every node but the system's reports `GpuMmuSupported`:
  without it dxgkrnl refuses every virtual context with `STATUS_INVALID_PARAMETER` before the driver sees it, and takes
  the change only after a restart of Windows, not of the device. The system node runs in physical mode, so CDD's
  context there gets the documented allocation list in `DxgkDdiPresent` (below).
- An allocation's driver object comes from `DxgkCbAcquireHandleData`, released with `DxgkCbReleaseHandleData` when
  the call is done. dxgkrnl answers `DxgkCbGetHandleData` with NULL to a WDDM 2.0 driver (DxgKrnl event 494, "WDDM2
  driver calls WDDM1.x DDI!"), which failed every `DxgkDdiOpenAllocation`, so the creation of every allocation.
- A DMA buffer completes once every fenced command it sent has signalled, in the order the scheduler gave them.
- One Venus context of the driver's own, made at start, which gets no Venus command: its blobs are plain shared
  memory on the host.
- Preemption only between DMA buffers: a preemption is reported once the node has nothing left.
- A GPU timeout resets its node alone (`SupportPerEngineTDR`, each node its own engine group). The host already has
  every command of the node's DMA buffers, so `DxgkDdiResetEngine` aborts all of them and reports the last as the last
  aborted fence. The WDK has dxgkrnl put the devices of aborted buffers in the error state, not every device. dxgkrnl
  starts the adapter with the cap and `DxgkDdiQueryDependentEngineGroup` (problem code 0, 2026-11-25); a driver that
  set the cap without that DDI failed to start with `StartAdapter_AddAdapterFailed`.

## Interface for user-mode drivers

`shared/viogpu3d_uapi.h` is the interface of Mesa's Venus driver for Windows (`src/virtio/virtio-gpu/wddm_hw.h`,
merge request 38731): the same tags and layouts.

- `DXGKQAITYPE_UMDRIVERPRIVATE`: the adapter's LUID, capability sets, 3D and host-visible region.
- Escapes: `CAPSET`, `CONTEXT_INIT` (on a device), `RESOURCE_INFO`, `BLOB_MAP` and its unmap, `EXEC_BUF` (on a
  context, returns the fence). `BLOB_MAP` maps with `DxgkCbMapMemory`, and for a 32-bit process with
  `MmMapLockedPagesSpecifyCache`, which maps into its own 32-bit range: a WOW64 process reads only the low half of
  the pointer. A 32-bit process that still gets a view past 4 GiB gets the map refused.
- Allocation private data: `ALLOCATE_BLOB` of `HOST3D` memory, and `ALLOCATE_3D`. The resource id and, for a blob,
  its real size come back. A blob's size is rounded up to the device's blob alignment, 16 KiB on a Mac host.
- Resource private data: `CREATE_RESOURCE`, and a `SUBMIT` command that the host runs before it makes the blob.
- Submissions on virtual contexts: `SUBMIT`, `FENCE` (waits for an `EXEC_BUF` fence), `TRANSFER_TO_HOST`,
  `TRANSFER_FROM_HOST` and `NOP`.
- Not there: hardware queues (the ICD then opens a context per ring; merge request 38731 alone has to be built
  without `HW_CONTEXT`), `RESOURCE_BUSY`, `BLIT_INIT`, guest memory blobs.
- OSMates' own: `VIOGPU3D_SURFACE`, a surface's record after `VIRTIO_WDDM_CREATE_ALLOCATION` in the allocation's
  private data. The user-mode driver writes it for its shared textures, this driver for its own surfaces, and a
  process that opens an allocation reads what it is from it.
- OSMates' own: `VIOGPU3D_ALLOCATE_EMPTY_TAG`, an allocation that names no memory: no host resource, a page of
  aperture, refused for presents, maps and opens, and not CPU visible. The Direct3D 12 driver gives one to heaps
  and committed resources that have no memory of their own in the kernel, because the Direct3D 12 runtime answers
  `SetResidencyPriority` itself and refuses an object without an allocation.

## Surfaces dxgkrnl asks for

- The shared primary and GDI textures: Venus blobs of the driver's own context (`HOST3D`, blob id 0, mappable), one
  linear plane of rows `width * 4` rounded up to 16 bytes, as Metal pads a linear texture's rows on Apple GPUs.
  Not CPU visible (`NonCpuVisiblePrimary`). Any process's Venus driver imports them, so the Direct3D driver opens them.
- Shadow and staging surfaces, and CPU-visible GDI staging surfaces: guest memory the CPU draws in, with no host
  resource, accessed physically (`DXGK_ALLOCATIONINFOFLAGS_WDDM2_0.AccessedPhysically`). For CDD's boot blts on the
  physical system node VidMm then maps the shadow surface into the aperture segment (`MAP_APERTURE_SEGMENT`, with its
  MDL), and the allocation list names it there; without the flag it stayed in segment 0 at address 0, with no map and
  so no CPU view. The work thread's CPU view comes from that map alone. The first 64 maps and unmaps of these surfaces
  are logged. A process cannot open them as textures.
- Other GDI surfaces are refused: the driver has no GDI hardware acceleration.
- `DxgkDdiPresent` with blt or color fill, on the contexts of GDI and of kernel clients (CDD's, the system's): the
  work thread copies or fills the rows with the CPU when the DMA buffer runs, between those surfaces and blobs whose
  rows Venus described, then flushes the rectangle if the blob is on screen. No stretch, color keys, rotation or
  overlapping blts in one surface. These contexts are on the system node, in physical mode, with a 64 KiB DMA buffer;
  a context with virtual addressing there would have its DMA buffer in the aperture segment: VidMm maps a virtual
  context's DMA buffer into the process's GPU virtual address space, which takes an allocation in a segment. Without a
  DMA buffer CDD's device on this adapter failed in its first context ("Couldn't allocate a contiguous buffer for the
  DMA buffer"), and one in contiguous memory (`DmaBufferSegmentSet` 0) on a virtual context made dxgmms2 fault in
  `AddDmaBufferToPool` (bug check 0x3B). The contexts of user-mode drivers get no DMA buffer, and their blts and fills
  are refused. The first 16 and every 256th present on a physical context log both surfaces' segment and address.
- A present the driver cannot do fails with a status `DxgkDdiPresent` documents: `STATUS_INVALID_HANDLE` when the
  allocation list has no surface where the present needs one, `STATUS_ILLEGAL_INSTRUCTION` for one the device cannot
  run (color keys, rotation, stretch, a surface it cannot copy), and the first eight and every 256th log the context's
  flags and both surfaces. On a context with virtual addressing dxgkrnl 10.0.26100.9444 passes `DxgkDdiPresent` an
  array of `DXGK_PRESENTALLOCATIONINFO`, which Microsoft reserves for the system, where `pAllocationList` lives; read as
  documented, the list names neither surface. So CDD's blts on this adapter failed that way while its system node had
  virtual addressing (until 2026-11-19).

## Display

- One scanout, at sizes up to 8184 a side and 16,777,216 pixels: a 5K screen's 5120 x 2880 fits.
- The VidPN: target modes of every listed size at 60 Hz with the active size equal to the total size, the host's
  display size preferred, only the pinned source's size once a source is pinned, and the other way round; identity and
  centered scaling (the same thing at equal sizes) and identity rotation, since the scanout shows the source as it is.
  The rotation support keeps the offset bits dxgkrnl set: clearing them is bug check 0x113, subtype 0x1C.
  `DxgkDdiIsSupportedVidPn` refuses a path whose pinned source and target differ in size. `DxgkDdiIsSupportedVidPn`,
  `DxgkDdiEnumVidPnCofuncModality` and `DxgkDdiCommitVidPn` log the paths, pins and mode counts they saw;
  `DxgkDdiRecommendMonitorModes`, `DxgkDdiUpdateActiveVidPnPresentPath`, `DxgkDdiQueryChildStatus`,
  `DxgkDdiSetVidPnSourceVisibility` and each new source address log one line.
- The flip thread reports `DXGK_INTERRUPT_CRTC_VSYNC` every 16 ms while `DxgkDdiControlInterrupt` has it on, and
  each `DxgkDdiControlInterrupt` call is logged. dxgkrnl turns it on and off for this adapter from a few seconds after
  start, before any VidPN is committed (measured 2026-11-18).
- Modes come from `GET_DISPLAY_INFO` and the EDID, read by data block as viogpudo now does. The host's display size
  is the custom mode from start on; the framebuffer memory is then sized for the largest mode, and a later, larger
  display waits for the next start.
- A flip shows a 3D resource with `SET_SCANOUT` or a blob with `SET_SCANOUT_BLOB`, by the allocation: dxgkrnl gives
  these primaries segment 0 and address 0. No hardware cursor yet.
- A flip whose present carries a `VIOGPU3D_FLIP_TRACE` as its private data (the user-mode driver's
  `VIOGPU3D_UMD_DEBUG=frame_trace`, `shared/viogpu3d_uapi.h`) is logged when `DxgkDdiSetVidPnSourceAddress` sets it:
  the flip's number, its frame's fence value and the gate value, the time of the present and the time now
  (`KeQueryPerformanceCounter`).

## Not done yet

- The desktop and presents need virglrenderer `b6e26840` (main `575ca684`'s pin) and KosmicKrisp from osmates/mesa
  `2b0fd564`, which adds `kk: Allow linear 2D color attachments` and `kk: Emit fcanonicalize` to main's Mesa pin;
  Direct3D 12 graphics pipelines need the second (`umd/README.md`). Measured with a host app that has both, not with
  main's.
- CDD's boot blts (2026-11-19): with the system node in virtual mode its first four blts (full screen, its shadow
  surface to the primary) failed with `STATUS_INVALID_HANDLE` and dxgkrnl logged "PresentFromCdd ... failed" for
  each; with the node physical and the shadow surface in segment 0, none failed, but the copy had no CPU view. With
  the node physical and CPU surfaces accessed physically, VidMm maps the shadow surface (2880 x 1800, 5063 pages) into
  the aperture at its first page with one map, the boot's four blts name it in segment 1 at the aperture's base, no
  "PresentFromCdd" failure is logged and the work thread copies it. VidMm unmaps it about four seconds later. Not
  measured: CDD's blts after that unmap (a DWM restart, GDI reading the screen) and blts of staging surfaces, which
  have a CPU view only if VidMm maps them again. A live install of a package with the node split over one without it,
  and over another with it, gave `VIDEO_TDR_TIMEOUT_DETECTED` live dumps (0x117) while the device restarted; installs
  on the 2D driver and the boots and apps after gave none.
- What a node reset leaves running is not measured. The one GPU timeout seen with it came within a minute of a live
  install on a fresh guest, with no kernel trace running: a `VIDEO_ENGINE_TIMEOUT_DETECTED` live dump (0x141), no
  0x117 and no bug check. After the restart, at a host load of 33 to 37, vkcube ran 600 frames through Vulkan and
  through DXGI with no timeout (2026-11-25).
- dxgkrnl logs "The root page directory is not set" on the DMA packets of processes that map no GPU virtual address
  (Venus maps none), and refuses Venus's hardware queue context while the driver reports WDDM 2.0 ("the driver WDDM
  version is < 2.5"); Venus then opens a context per ring.
- Basic Display drives the firmware's framebuffer from a root device of its own beside this adapter, as beside
  viogpudo. OSMates' guest agent has Windows show the GPU's output alone (its `display` command at the first logon,
  `display/set` when the host sets the display), which Windows keeps in its display configuration database: with that
  entry Windows shows one display, this adapter's, at boot and after a sign-in. With no entry for the monitors it
  extends the desktop onto Basic Display's output at boot, until the agent's next `display/set` (measured 2026-11-19).
  No documented DDI or callback retires that Basic Display or chooses Windows' topology.
- A D3D9 DDI. D3D9 lists this adapter now that it drives the display, and a HAL device fails with
  `D3DERR_NOTAVAILABLE`; apps that call `Direct3DCreate9On12` get a HAL device through D3D9On12 (2026-11-19).
- Another process opening the surfaces above through Venus, and CDD's blts into the primary once DWM runs: not
  measured on their own; the screen shows DWM's flips of its own buffers.
- Hardware queues and the GPU's own scheduling.
- After a power transition the host has lost its contexts and resources; the driver does not report a lost device
  yet.
- Several scanouts, HDR (WDDM 2.7), the hardware cursor.

## Building

On Windows, as the other drivers (`viogpu3d.sln`), with the user-mode drivers, their manifests and
`viogpu3d_umd_notices.txt` put next to the INF before Inf2Cat; the MSBuild projects do not build or copy them yet. On
macOS, `Tools/build-macos.sh --d3d-umd viogpu3d/viogpu3d/viogpu3d.vcxproj` builds the `.sys`, the Direct3D drivers
and a stamped INF in `viogpu3d/Install/Win11/ARM64`, with the Vulkan drivers of three Mesa builds of the
`venus-windows` branch of github.com/osmates/mesa, for `aarch64-w64-mingw32`, `arm64ec-w64-mingw32` and
`i686-w64-mingw32`, configured with `-Dvulkan-icd-dir=bin` (`--venus-icd`, `--venus-icd-arm64ec`, `--venus-icd-x86`),
and vkd3d-proton from the `umd/vkd3d-proton` submodule, and writes the licence notices of the third-party code in
them, with where vkd3d-proton's source is (`umd/README.md`). The catalog is made on Windows with Inf2Cat.

## Credits

- viogpu, the display-only driver this copies its common code from: Vadim Rozenfeld and Red Hat.
- viogpu3d of pull request 943 by Max R: the VidPN code here and the shape of the driver.
- The plan for a separate 3D directory: the virtio-win maintainers on pull request 943.
- The user-mode interface: Mesa merge request 38731.
- What dxgkrnl asks of a WDDM 2.0 driver without GPU page tables of its own (the segments and queries it needs before
  it starts the adapter): the notes in the Yttrium driver's sources. No code of it is used here.
