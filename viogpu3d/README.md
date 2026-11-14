# viogpu3d

A full WDDM driver (render and display) for a virtio GPU with 3D: virgl contexts, blob resources and Venus. It lives
in its own directory with its own copy of viogpu's `common` code, its own names, GUIDs and INF, so that the
display-only driver in `viogpu` stays as it is.

## Status

- Compiled for Windows 11 ARM64 with clang and lld-link through `Tools/build-macos.sh`. Not built with MSVC yet.
- Not run in a guest yet.
- The package carries Mesa's Venus Vulkan driver, cross-built with llvm-mingw: `vulkan_virtio.dll` (ARM64X, for
  ARM64 and x64 processes, named by `VulkanDriverName`) and `vulkan_virtio_x86.dll` (named by
  `VulkanDriverNameWow`). And the Direct3D 10 and 11 user-mode driver of `umd/`: `viogpu3d_umd.dll` (ARM64X, named by
  `UserModeDriverName`) and `viogpu3d_umd_x86.dll` (named by `UserModeDriverNameWow`). Compiled, not loaded in
  Windows yet: `umd/README.md`.

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

- WDDM 2.0 with GPU virtual addressing (GpuMmu). The page tables are written by the CPU into a 16 MiB segment of
  contiguous memory. The device never reads a GPU virtual address: it addresses memory by resource.
- Two segments: a 1 GiB aperture of system memory, and the page tables.
- 64 nodes: 0 for the system, 1 for Venus, 2 for paging, 3 to 63 for Venus rings 0 to 60. Mesa's Venus ICD opens
  one virtual context a ring on nodes 3 to 63.
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
- Modes come from `GET_DISPLAY_INFO` and the EDID, read by data block as viogpudo now does.
- A flip shows a 3D resource with `SET_SCANOUT` or a blob with `SET_SCANOUT_BLOB`. No hardware cursor yet.

## Not done yet

- A run of the Direct3D user-mode driver in Windows, for ARM64, x64 and x86 apps; its D3D12 and D3D9 DDIs.
- A run of the surfaces above and the present's blts in Windows. On OSMates' host a Venus context cannot yet import
  another's shared memory: its render server has to learn that first.
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
and writes the licence notices of the third-party code in them (`umd/README.md`). The catalog is made on Windows with
Inf2Cat.

## Credits

- viogpu, the display-only driver this copies its common code from: Vadim Rozenfeld and Red Hat.
- viogpu3d of pull request 943 by Max R: the VidPN code here and the shape of the driver.
- The plan for a separate 3D directory: the virtio-win maintainers on pull request 943.
- The user-mode interface: Mesa merge request 38731.
- What dxgkrnl asks of a WDDM 2.0 driver without GPU page tables of its own (the segments and queries it needs before
  it starts the adapter): the notes in the Yttrium driver's sources. No code of it is used here.
