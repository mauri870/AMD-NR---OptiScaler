# The dlssnr-amd runtime

A third neural runtime beside danielblnc's and lmxxf's: the Vulkan network of
[DLSSNR-AMD](https://github.com/mochizuki0323/DLSSNR-AMD) (MIT), built as `DlssnrAmdRuntime.dll`.
The RDNA 3 kernels and this runtime are by Mauri de Souza Meneguzzo
([DLSSNR-RDNA3](https://github.com/mauri870/DLSSNR-RDNA3), MIT); the original network it is derived from is
mochizuki0323's. Chosen with
`[DlssNr] NrBackend=dlssnr-amd` or in the Neural tab's runtime combo; never the default.

## How it fits

It speaks lmxxf's runtime C ABI (`lmxxf/runtime/LmxxfNrApi.h`, export `LmxxfNrGetApi`), so no new backend class
exists: `Lmxxf::Backend` takes a `Lmxxf::Flavor` and loads `DlssnrAmdRuntime.dll` instead of
`LmxxfNrRuntime.dll`. Everything else of the lmxxf path (the one-frame-late edit, the temporal carry, the edit
shaper, the controls) is shared, so the capability matrix gives it lmxxf's column. What differs:

| | lmxxf | dlssnr-amd |
| --- | --- | --- |
| runtime | `LmxxfNrRuntime.dll` | `DlssnrAmdRuntime.dll` |
| assets | `LmxxfNrRuntime.pak` (or `DLSS5-AMD\native-game-tiled-assets`) | `dlssnr-amd\dlssnr.bin` + `dlssnr-amd\shaders\` |
| GPU API | HIP | Vulkan, on a device of its own on the game's adapter |
| `NrBackend` | `lmxxf` | `dlssnr-amd` |

`dlssnr.bin` is the model, extracted from the player's own `nvngx_dlssnr.dll` by DLSSNR-AMD's
`model-tools`; it is never shipped.

## Code

- `lmxxf/LmxxfBackend.{h,cpp}`: `Flavor`, `DlssnrAmd*Path` / `*Present`, the flavor in `Impl::Load` and the constructor.
- `amd/AmdBridge.{h,cpp}`: `NeuralRuntime::DlssnrAmd`, `IsLmxxfFamily`, `DlssnrAmdWanted` (chosen and installed
  completely; no HIP check), `activeRuntime` 3.
- `amd/RuntimeCaps.h`: the third table row; `Menu()` follows the runtime in use.
- `amd/PresentExperimental.h`, `menu/*`, `misc/AmdnrReport.cpp`: the runtime's name where lmxxf's was assumed.

## Source

The runtime and the shaders it runs are vendored in `main/DlssnrAmd/` (see its `README-AMDNR.md` for the build):
`DlssnrAmdRuntime.dll` is built from `linux/src/pe/nr_amdnr_*`, the network's Vulkan shaders are in
`linux/shaders` for RADV and `windows/shaders/rdna3` for the AMD Windows driver (the Linux network with its loops
unrolled, which that driver's compiler needs; built by `windows/build/build_network.py rdna3`). No binary is
committed. The network does not yet give a correct picture on the Windows driver; `docs/AMDNR-RUNTIME.md` in that
folder says where that stands.

## Not built

This tree is AMDNR's overlay on OptiScaler and does not build on its own; these changes have been read through,
not compiled.
