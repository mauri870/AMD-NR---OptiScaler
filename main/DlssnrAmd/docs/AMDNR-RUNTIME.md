# DlssnrAmdRuntime.dll: this network as a neural runtime for AMDNR's OptiScaler

[AMDNR](https://github.com/3zwr1/AMD-NR---OptiScaler) is a Windows build of OptiScaler that runs a neural
runtime over each frame. Its lmxxf backend loads `LmxxfNrRuntime.dll` through a small C ABI
(`LmxxfNrGetApi`): the host hands over an FP16 proxy of the frame at the network's size (and a motion
image), and later takes the network's answer back as an FP16 texture; it forms the edit itself.
`DlssnrAmdRuntime.dll` implements that ABI with the Vulkan network of this project.

- `linux/src/pe/nr_amdnr_abi.hpp`: the ABI, laid out as the host reads it.
- `linux/src/pe/nr_amdnr_runtime.cpp`: the ABI front-end (argument checks, job states).
- `linux/src/pe/nr_amdnr_transport.hpp`: what a transport does; two implement it.
  - **inline** (`nr_amdnr_inline.cpp`): the host's D3D12 device is vkd3d-proton (Proton), so the network is
    recorded into the host's own command list through the model layer the NGX core uses. Tested.
  - **native** (`nr_amdnr_native.cpp`, `nr_amdnr_vkleg.cpp`): the system's D3D12 (Windows). The network
    runs on a Vulkan device of our own on the same adapter (matched by LUID). Frames cross in three D3D12
    buffers per size, created shared and imported into Vulkan, ordered by one D3D12 fence imported as a
    timeline semaphore. Vulkan is submitted first (wait for d, signal d+1), then the D3D12 queue signals d;
    RecordOutputs queues a wait for d+1 and copies the answer into the job's output texture.
- Build: `NR_GPU=rdna3 linux/build/build_amdnr_runtime.sh` (`NR_BUILD_TESTS=1` adds the leg test).
- `DLSSNR_AMD_TRANSPORT=inline|native` forces a transport.

## What has been checked, and where

| Check | Where | Result |
| --- | --- | --- |
| ABI + inline transport, 1080p, RX 7900 XTX | `linux/test/amdnr_abi_test.cpp` under GE-Proton10-34 | 45.69 dB against NVIDIA (the nr::Runtime RDNA3 path scores 45.47); history path runs |
| Vulkan half on its own device, plain buffers | `linux/test/amdnr_leg_test.cpp` under GE-Proton10-34 | 45.69 dB |
| Fence: D3D12 shared fence imported as a timeline semaphore | Proton (vkd3d-proton + winevulkan) | works |
| Buffer: D3D12 shared buffer imported into Vulkan | Proton | **cannot be tested**: vkd3d-proton refuses `CreateSharedHandle` on a buffer |
| Native transport end to end | needs Windows with the AMD driver | not yet run: `linux/package/amdnr/build_test_kit.sh` builds the kit |

The network at 1080p costs about 28 ms on the 7900 XTX in the first Proton run (the RADV figure is 15 ms
with the GPU idle); the native transport adds two buffer copies and two image copies of the frame.

## RDNA3 shaders on the AMD Windows driver

The Linux network (`linux/shaders/rdna3`) does not compile as written with the Windows driver's compiler (LLPC,
`amdvlk64.dll` 26.9.2, tested on an RX 7900 XT). Measured by `Run test.bat` kits on that machine:

- The compiler **crashes** (access violation inside `amdvlk64.dll` while creating the pipeline) on every pipeline of
  `fswin_t.comp`, `attn.comp` and `ffwd3_t.comp` (40 of 101 shaders, the temporal variants of `fswin_t` included). All
  `gemm*`, `vitattn` and the passes compile.
- Unrolling the constant-trip loops of those sources before compiling (the way `windows/build/build_network.py` already
  builds the RDNA4 network in the full project) makes all of them compile. `windows/shaders/rdna3` is the Linux network carried over and
  `python3 windows/build/build_network.py rdna3` builds it that way (`unroll_glsl.py` drops `[[dont_unroll]]` from the
  loops it unrolls). On RADV that set is **bit-exact** with the Linux one (`check_rdna3.py` EXACT on all three frames,
  every layer hash identical), so the unrolling does not change the arithmetic.
- The persistent kernels (`fswinp*`: 7 crash, 2 fail to create) still do not compile. The Windows leg therefore
  runs layer by layer (`NR_NO_PERSIST`, set by `nr_amdnr_vkleg.cpp`).
- Layer by layer the network then **runs** on the Windows driver (27 ms at 1080p, 16 ms at 720p) but gives a wrong picture
  (9.8 dB against the Linux driver's, 0.2 % identical pixels). Where it first goes wrong is not known yet:
  `linux/package/amdnr/build_layers_kit.sh` builds a kit whose `nr_graph.exe` prints a hash of every layer, and
  `linux/test/compare_layers.py layers.txt` names the first layer that differs from the golden.

## Known limits

- Detail/Colour strength below 1 are not applied by the runtime (the answer is the network's own).
- One network pass per frame; the host's Full network, tier and mask flags are accepted and the full
  network always runs.
- The leg runs on the graphics+compute queue family because the network converts an FP16 frame with a
  blit; an RGBA32F frame would allow an async compute queue.
- The network on the AMD Windows driver computes a wrong picture until the layer the driver gets wrong is found and
  worked around (section above).
