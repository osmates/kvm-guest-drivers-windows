# viogpu3d

A full WDDM driver (render and display) for a virtio GPU with 3D: virgl contexts, blob resources and Venus. It lives
in its own directory with its own copy of viogpu's `common` code, its own names, GUIDs and INF, so that the
display-only driver in `viogpu` stays as it is.

## Status

- Compiled for Windows 11 ARM64 with clang and lld-link through `Tools/build-macos.sh`. Not built with MSVC yet.
- Not run in a guest yet.
- The package carries Mesa's Venus Vulkan driver (`vulkan_virtio.dll`, named by `VulkanDriverName`), cross-built
  for ARM64 with llvm-mingw. No Direct3D user-mode driver yet: Windows needs one for DWM, so without it the desktop
  stays on Basic Display.

## Device

- Needs `VIRTIO_GPU_F_VIRGL`. Without it the device fails to start and Windows keeps Basic Display.
- Takes `EDID`, `RESOURCE_BLOB`, `CONTEXT_INIT` and `BLOB_ALIGNMENT` when offered.
- Venus needs blobs, context init, the `VENUS` capability set and the host-visible region: shared memory region 1
  of a `VIRTIO_PCI_CAP_SHARED_MEMORY_CFG` capability, which may sit behind a 64-bit BAR of 4 GiB.
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
- Preemption only between DMA buffers: a preemption is reported once the node has nothing left.

## Interface for user-mode drivers

`shared/viogpu3d_uapi.h` is the interface of Mesa's Venus driver for Windows (`src/virtio/virtio-gpu/wddm_hw.h`,
merge request 38731): the same tags and layouts.

- `DXGKQAITYPE_UMDRIVERPRIVATE`: the adapter's LUID, capability sets, 3D and host-visible region.
- Escapes: `CAPSET`, `CONTEXT_INIT` (on a device), `RESOURCE_INFO`, `BLOB_MAP` and its unmap, `EXEC_BUF` (on a
  context, returns the fence).
- Allocation private data: `ALLOCATE_BLOB` of `HOST3D` memory, and `ALLOCATE_3D`. The resource id and, for a blob,
  its real size come back. A blob's size is rounded up to the device's blob alignment, 16 KiB on a Mac host.
- Resource private data: `CREATE_RESOURCE`, and a `SUBMIT` command that the host runs before it makes the blob.
- Submissions on virtual contexts: `SUBMIT`, `FENCE` (waits for an `EXEC_BUF` fence), `TRANSFER_TO_HOST`,
  `TRANSFER_FROM_HOST` and `NOP`.
- Not there: hardware queues (the ICD then opens a context per ring; merge request 38731 alone has to be built
  without `HW_CONTEXT`), `RESOURCE_BUSY`, `BLIT_INIT`, guest memory blobs.

## Display

- One scanout, at sizes up to 8184 a side and 16,777,216 pixels: a 5K screen's 5120 x 2880 fits.
- Modes come from `GET_DISPLAY_INFO` and the EDID, read by data block as viogpudo now does.
- A flip shows a 3D resource with `SET_SCANOUT` or a blob with `SET_SCANOUT_BLOB`. No blt presents and no hardware
  cursor yet.

## Not done yet

- A Direct3D user-mode driver, and a 32-bit or ARM64EC Vulkan driver for x86 and x64 apps.
- Hardware queues and the GPU's own scheduling.
- After a power transition the host has lost its contexts and resources; the driver does not report a lost device
  yet.
- Several scanouts, HDR (WDDM 2.7), the hardware cursor.

## Building

On Windows, as the other drivers (`viogpu3d.sln`), with `vulkan_virtio.dll` and `vulkan_virtio.json` put next to
the INF before Inf2Cat; the MSBuild projects do not copy them yet. On macOS,
`Tools/build-macos.sh --venus-icd <mesa-build>/src/virtio/vulkan viogpu3d/viogpu3d/viogpu3d.vcxproj` builds the
`.sys` and a stamped INF in `viogpu3d/Install/Win11/ARM64`, with the Vulkan driver of a Mesa build of the
`venus-windows` branch of github.com/osmates/mesa for `aarch64-w64-mingw32`, configured with
`-Dvulkan-icd-dir=bin`. The catalog is made on Windows with Inf2Cat.

## Credits

- viogpu, the display-only driver this copies its common code from: Vadim Rozenfeld and Red Hat.
- viogpu3d of pull request 943 by Max R: the VidPN code here and the shape of the driver.
- The plan for a separate 3D directory: the virtio-win maintainers on pull request 943.
- The user-mode interface: Mesa merge request 38731.
- What dxgkrnl asks of a WDDM 2.0 driver without GPU page tables of its own (the segments and queries it needs before
  it starts the adapter): the notes in the Yttrium driver's sources. No code of it is used here.
