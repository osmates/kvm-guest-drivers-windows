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
- Preemption only between DMA buffers: a preemption is reported once the node has nothing left; the first 64 and
  every 256th that wait for a node's buffers are logged with its fences.
- At a GPU timeout (bug check 0x117 or 0x141) `DxgkDdiCollectDbgInfo` writes the nodes with buffers not done (their
  last submitted and completed fences, the oldest buffer's fence and how many host signals it still waits for) and the
  EXEC_BUF fences the host has not signalled into the dump's driver data (`viogpu3d debug info: ...`), and logs them;
  `DxgkDdiResetFromTimeout` and `DxgkDdiResetEngine` log the same. For any other bug check, where dxgkrnl may call it
  at any IRQL, it leaves the driver data empty. Windows puts that text in WER's kernel report, a minidump of about
  290 KB (`ProgramData\Microsoft\Windows\WER\ReportQueue\Kernel_117_*\WATCHDOG-*.dmp`). Of four timeouts in one
  guest, before and after a restart, only the first left a live dump in `LiveKernelReports`; for the others
  `WerLiveKernelCreateReport` failed with `0xc0000022` (2026-11-20).

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
  are refused. The first 16 and every 256th present on a physical context log both surfaces' segment and address, and
  the first 64 and every 256th blt or fill the work thread runs logs both surfaces' kinds and the rectangle it drew.
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
- The flip thread queues a flip's `SET_SCANOUT_BLOB` and `RESOURCE_FLUSH` at the vsync tick and reports the vsync at
  once, and dxgkrnl then hands the primary shown before back to DWM. The host copies a shown blob with the CPU when it
  runs these commands, before it answers them. Screenshots of a Direct3D 11 window sometimes show screen-aligned tiles
  of another frame (2026-11-19). A test package that reported the vsync only once the host answered the flush, so that
  DWM got no primary back while the host could still copy it, showed them too: 12 of 542 shots, against 6 of 1003 with
  this tree (2026-11-20). So they do not come from that overlap, and where they come from is open. The test package's
  flips waited 6.6 to 12.2 ms on average for the answer. A package of `a4bd0b1e`, from before the blt and timeout
  logging, showed them too (10 of 642 shots, against 6 of 573 with this tree in the next boot); of those 16 tile shots'
  19308 edge samples, 19032 lie on the screen's 64-pixel grid, 268 (all horizontal) on the window's and 8 on neither. In
  62 of 3000 copies of a shown blob the host copied while a fence the blob's Venus context had asked 3 to 65 ms before
  was not signalled yet; whether that work wrote the copied blob is not known (2026-11-20). On OSMates main 75402121
  with each such copy logged and this tree, none of the 16 tile shots in 875 shots of a Direct3D 11 window had one from
  100 ms before its screenshot to its end, against 10 of the other 859 shots; so in that run the tiles did not come with
  those copies (2026-11-21).
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
  "PresentFromCdd" failure is logged and the work thread copies it. VidMm unmaps it about four seconds later. After a
  DWM restart and a sign-out and sign-in, VidMm mapped it again for each of CDD's next three blts and unmapped it about
  a second after, and each had a CPU view (2026-11-20). Not measured: blts of staging surfaces, which have a CPU view
  only if VidMm maps them again. A live install of a package with the node split over one without it, and over
  another with it, gave `VIDEO_TDR_TIMEOUT_DETECTED` live dumps (0x117) while the device restarted; installs over the
  2D driver gave none (2026-11-19, 2026-11-20).
- GPU timeouts with apps (2026-11-20, OSMates main 3387ffce): this tree timed out 9 s after its install over an older
  package of this driver (DriverVer .58701, without the node split) restarted the device (the old driver also timed
  out as it stopped), 2.5 s after a Direct3D 11 app made its device right after another app ended, and 5 s after a
  DWM restart; another boot ran the same apps for 12 minutes without one. Each time the oldest buffer of one or two
  nodes waited for one signal from the host, and DWM and the shell's processes ended after the reset. On OSMates main
  3387ffce, the same steps with a package of `a4bd0b1e`, with the Mac's load average at 4 to 9, gave no timeout; this
  tree gave two, at the install and 10 s after the DWM restart, at 6 to 22 (2026-11-20). On OSMates main b78d1a5b with
  host logging, every fence the host signalled 0.5 s or more after the guest asked (133) had waited in the render server
  behind its own Venus context's command stream (`SUBMIT_CMD`); 6 of those streams, all DWM's, took 2.4 to 5.2 s and
  compiled Metal shaders meanwhile, at a load average of 24 to 44, and the guest timed out during each, its oldest
  unsignalled EXEC_BUF fence one of those the stream held. 2 of that run's 8 timeouts had no such wait on the host.
  Making the 143 Venus contexts of that run took the render server 12 ms at most, and nothing held OSMates' renderer
  thread for more than 184 ms (2026-11-20). On OSMates main 75402121 with the render server logging each Venus command
  of 20 ms or more, pipeline creation took most of the two longest of the three streams that ran when the guest timed
  out, and nearly all of a creation was KosmicKrisp compiling Metal libraries from MSL source: in a boot of 2.5 minutes
  with this tree, 62 `vkCreateGraphicsPipelines` of 20 ms or more took 16.4 s (938 ms at most), none with a
  `VkPipelineCache`, and its fragment libraries 15.0 s against 69 ms of pipeline states; the guest timed out as a stream
  of 2.4 s ended, 1.2 s of it in 4 pipeline creations. In another boot, at a load average of 4, a DWM restart's stream
  of 230 commands took 6.1 s, 4.3 s of it in 18 pipeline creations; the guest timed out during it and again 10 s later,
  during another context's stream of 2.0 s, 0.7 s of it in 2 pipeline creations. Each Venus context is a render server
  process of its own. Metal's shader cache for that program is split into parts; a process takes one at its first
  compile, keeps it until it ends and finds only what was compiled on it. So a new context on another part compiles the
  same pipelines again: in one boot a fragment library of about 320 ms was compiled in 5 contexts, each on another part,
  and a context's `vkCreateDevice` took up to 1.5 s compiling KosmicKrisp's own shaders on a part without them
  (2026-11-20, 2026-11-21). Four more runs of the install-to-DWM-restart steps on OSMates main 3387ffce, in this order:
  a package of `a4bd0b1e` timed out 5 times at a load average of 16 to 52 (the install, the restart and 3 after the DWM
  restart), this tree 7 times at 4 to 6 (3 at the install, 4 after the DWM restart), then, with Metal's cache filled by
  those runs, `a4bd0b1e` once at 5 to 12 (the install) and this tree twice at 3.5 to 5 (the install and after the DWM
  restart). The install in place over .58701 timed out in all four, and the render server's "Metal Compiling Shader"
  lines fell from 588 and 616 to 206 and 136 (2026-11-21).
- Processes keep their views of host-visible blobs past the device's reset. With a test package whose flips wait for
  the host, processes wrote blobs the host had dropped in 3 of 5 restarts and stops of Windows (2026-11-20; none with
  this tree), the two writes placed from Venus's command encoders (`vn_CmdBindIndexBuffer2`, `vn_CmdBeginRendering`).
  OSMates main 3387ffce gives such a write a zeroed scratch page, and 2 of the 3 still stopped the machine at a second
  write milliseconds later. On OSMates main 75402121, which also gives one to an access after the host-visible region's
  BAR stopped decoding, five restarts and stops of Windows with that test package went on without a stop: the first such
  write of each of the two machine runs came from DWM (`dwmcore.dll`), and of the runs' 13 and 5 such faults, 2 and 1
  came while the BAR did not decode (2026-11-21).
  Ending each view in its own process before `virtio_device_reset` (`KeStackAttachProcess`, then `DxgkCbUnmapMemory`
  or `MmUnmapLockedPages`) is the documented way; not built.
- dxgkrnl logs "The root page directory is not set" on the DMA packets of processes that map no GPU virtual address
  (Venus maps none), and refuses Venus's hardware queue context while the driver reports WDDM 2.0 ("the driver WDDM
  version is < 2.5"); Venus then opens a context per ring.
- Basic Display drives the firmware's framebuffer from a root device of its own beside this adapter, as beside
  viogpudo. OSMates' guest agent has Windows show the GPU's output alone (its `display` command at the first logon,
  `display/set` when the host sets the display), which Windows keeps in its display configuration database: with that
  entry Windows shows one display, this adapter's, at boot and after a sign-in. With no entry for the monitors it
  extends the desktop onto Basic Display's output at boot, until the agent's next `display/set` (measured 2026-11-19).
  No documented DDI or callback retires that Basic Display or chooses Windows' topology.
- `SupportPerEngineTDR` stays off: with it dxgkrnl refuses to start the adapter.
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
