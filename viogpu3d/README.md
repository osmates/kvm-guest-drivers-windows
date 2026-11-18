# viogpu3d

A full WDDM driver (render and display) for a virtio GPU with 3D: virgl contexts, blob resources and Venus. It lives
in its own directory with its own copy of viogpu's `common` code, its own names, GUIDs and INF, so that the
display-only driver in `viogpu` stays as it is.

## Status

- Compiled for Windows 11 ARM64 with clang and lld-link through `Tools/build-macos.sh`. Not built with MSVC yet.
- Run in a Windows 11 guest on OSMates' accelerated GPU (2026-11-18): the driver starts (problem code 0) and dxgkrnl
  reports the adapter as render and display, WDDM 2.0, GpuMmu. Direct3D 11 and 12 devices run on it through DXVK,
  vkd3d-proton and Venus in ARM64, x64 and x86 processes, and a Direct3D 11 draw into a texture reads back right. With
  a host that has OSMates' next KosmicKrisp (osmates/mesa `kk-linear-color-attachment`), a sign-in works and both APIs
  draw and present. The desktop is not on this adapter yet, so the Mac's screen shows none of it (below, "Not done
  yet").
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
  resource. The driver allocates no contiguous memory for them, so a start never fails for want of it.
- One segment: a 1 GiB aperture of system memory.
- 64 nodes: 0 for the system, 1 for Venus, 2 for paging, 3 to 63 for Venus rings 0 to 60. Mesa's Venus ICD opens
  one virtual context a ring on nodes 3 to 63. Every node reports `GpuMmuSupported`: without it dxgkrnl refuses every
  virtual context with `STATUS_INVALID_PARAMETER` before the driver sees it, and takes the change only after a restart
  of Windows, not of the device.
- An allocation's driver object comes from `DxgkCbAcquireHandleData`, released with `DxgkCbReleaseHandleData` when
  the call is done. dxgkrnl answers `DxgkCbGetHandleData` with NULL to a WDDM 2.0 driver (DxgKrnl event 494, "WDDM2
  driver calls WDDM1.x DDI!"), which failed every `DxgkDdiOpenAllocation`, so the creation of every allocation.
- A DMA buffer completes once every fenced command it sent has signalled, in the order the scheduler gave them.
- One Venus context of the driver's own, made at start, which gets no Venus command: its blobs are plain shared
  memory on the host.
- Preemption only between DMA buffers: a preemption is reported once the node has nothing left.

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
  resource. A process cannot open them as textures.
- Other GDI surfaces are refused: the driver has no GDI hardware acceleration.
- `DxgkDdiPresent` with blt or color fill, on contexts without virtual addressing (CDD's): the work thread copies or
  fills the rows with the CPU when the DMA buffer runs, between those surfaces and blobs whose rows Venus described,
  then flushes the rectangle if the blob is on screen. No stretch, color keys, rotation or overlapping blts in one
  surface. A context with virtual addressing has no DMA buffer for them, so its blts and fills are refused.

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
- A flip shows a 3D resource with `SET_SCANOUT` or a blob with `SET_SCANOUT_BLOB`. No hardware cursor yet.

## Not done yet

- Presents on the Mac's screen. With virglrenderer `b6e26840` (main `575ca684`'s pin) and KosmicKrisp from
  osmates/mesa `2b0fd564`, which adds `kk: Allow linear 2D color attachments` and `kk: Emit fcanonicalize` to main's
  Mesa pin, a Direct3D 11 and a Direct3D 12 triangle present 600 frames each on the signed-in desktop (2026-11-18),
  which is still the Basic Display adapter's, so nothing reaches this adapter's scanout (below).
- Direct3D 12 graphics pipelines need a KosmicKrisp that prints `fcanonicalize` in MSL; OSMates' pin does not yet
  (`umd/README.md`).
- The desktop on this adapter. The driver supports every VidPN dxgkrnl asks about; dxgkrnl pins the host's size
  (2880 x 1800 on a Retina Mac, as viogpudo gets) and commits none: at boot, and when `SetDisplayConfig` asks for this
  adapter's path alone (validated, then `ERROR_GEN_FAILURE`), CDD's device on this adapter fails first. Its first
  context has virtual addressing and no DMA buffer, and dxgkrnl logs "Couldn't allocate a contiguous buffer for the
  DMA buffer"; every DMA packet here also logs "The root page directory is not set", and `DxgkDdiSetRootPageTable`
  does nothing yet. CDD then draws on Basic Render, and the desktop stays on the Basic Display adapter's no-EDID
  output. A 64 KiB DMA buffer for such contexts made dxgmms2 fault at install (bug check 0x3B), so the GPU virtual
  address space of a kernel client comes first (2026-11-18). viogpudo has no such device: CDD draws for it on Basic
  Render.
- `SupportPerEngineTDR` stays off: with it dxgkrnl refuses to start the adapter.
- A D3D9 DDI. D3D9 apps get D3D9On12 only when they ask for it themselves, and only once the desktop is on this
  adapter (above): D3D9 and `Direct3DCreate9On12` list only the adapters that drive a display.
- A run of the surfaces above and the present's blts in Windows, with another process importing them through Venus.
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
