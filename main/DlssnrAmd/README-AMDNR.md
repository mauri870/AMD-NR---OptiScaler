# DLSSNR-AMD, vendored for AMDNR's `dlssnr-amd` runtime

This folder holds the source of [DLSSNR-AMD](https://github.com/mochizuki0323/DLSSNR-AMD) (MIT, see `LICENSE`)
that the `dlssnr-amd` neural runtime is built from: the Vulkan compute shaders of the network
(`linux/shaders`, and `windows/shaders` for the AMD Windows driver), the host that runs them (`linux/src/core`),
and `DlssnrAmdRuntime.dll`'s source (`linux/src/pe/nr_amdnr_*`). How the runtime plugs into AMDNR's OptiScaler is
in `../AMDNR-source/optiscaler/dlssnr/dlssnr-amd/README.md`; what it is and what was tested is in
`docs/AMDNR-RUNTIME.md`. `CLAUDE.md` and `README.md` are that project's own and describe its rules (no NVIDIA files,
host and shaders change together, reproducible SPIR-V).

Only the RDNA 3 network is here: the RDNA 4 kernels of the original project are not part of this folder. Its own
documents (`README.md`, `README-RDNA3.md`, `CLAUDE.md`, `docs/ARCHITECTURE.md`) are kept as they are and also
describe RDNA 4.

No binary is committed (`.gitignore` forbids `*.dll`, `*.bin`, `*.spv`): the DLL and the SPIR-V are built here,
and the model (`dlssnr.bin`) is extracted by the player from their own `nvngx_dlssnr.dll` with `linux/package/model-tools`.

## Credits

The RDNA 3 kernels (`linux/shaders/rdna3`, `windows/shaders/rdna3`), the host changes that run them and the
`dlssnr-amd` runtime (`linux/src/pe/nr_amdnr_*` and its tooling) are by **Mauri de Souza Meneguzzo**
([mauri870](https://github.com/mauri870), [DLSSNR-RDNA3](https://github.com/mauri870/DLSSNR-RDNA3)), MIT. The network,
its host and the shared passes are **mochizuki0323**'s
([DLSSNR-AMD](https://github.com/mochizuki0323/DLSSNR-AMD)), MIT, and their copyright stays in `LICENSE`.
`../../Licenses/DLSSNR-AMD_ATTRIBUTION.txt` says the same.

## Build `DlssnrAmdRuntime.dll`

The DLL is cross-compiled to a Windows PE by bash scripts (MinGW-w64, Python 3, a Linux glslang), so it is built on
Linux, or on Windows inside WSL2; there is no Visual Studio project and MSVC has not been tried. What comes out is an
ordinary Windows DLL, the same wherever it was built.

On Windows, once: `wsl --install -d Ubuntu`, then in the Ubuntu shell

    sudo apt update && sudo apt install -y git curl python3 g++-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix
    sudo update-alternatives --set x86_64-w64-mingw32-g++ /usr/bin/x86_64-w64-mingw32-g++-posix
    sudo update-alternatives --set x86_64-w64-mingw32-gcc /usr/bin/x86_64-w64-mingw32-gcc-posix

(the POSIX thread model matters: the code uses std::thread and std::mutex). Clone the repository inside the WSL
file system (`~`, not `/mnt/c`), and from `main/DlssnrAmd`:

    bash fetch_deps.sh --runtime                         # downloads glslang 16.5.0 (pinned, checksummed): the only thing fetched
    python3 windows/build/build_network.py rdna3         # SPIR-V for the AMD Windows driver -> build/windows/rdna3/network
    NR_GPU=rdna3 bash linux/build/build_amdnr_runtime.sh # artifacts/amdnr/nr/DlssnrAmdRuntime.dll

Everything compiled into or against the DLL is in this folder: the runtime and network sources, the Vulkan C headers
and MinHook (`third_party/`, with their licences). glslang is a build tool, not code in the DLL, so it is downloaded.
Tested with GCC 16.2 (Arch Linux), from a fresh copy of this folder: the three commands above took about 25 seconds
and produced shaders byte-identical to the development tree's. Ubuntu's older MinGW has not been tried; if a compile
fails there, that is the first thing to suspect. The DLL's version stamp comes from git tags and reads
`0.0.0-unknown` without git.

`dlssnr-amd/shaders` next to the DLL must be `build/windows/rdna3/network`: the Windows driver's compiler crashes on
the Linux set (`linux/build/build_network.py rdna3`, which is for RADV and the Proton route); see
`docs/AMDNR-RUNTIME.md`, "RDNA3 shaders on the AMD Windows driver", for what is known and what is not.

`linux/package/amdnr/build_dropin.sh` and `build_test_kit.sh` lay the files out for a game folder and for a Windows
test.
