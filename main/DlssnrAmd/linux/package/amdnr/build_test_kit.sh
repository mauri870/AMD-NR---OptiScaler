#!/usr/bin/env bash
# Copyright (c) 2026 Mauri de Souza Meneguzzo (mauri870). MIT, see LICENSE.
# The Windows test kit for DlssnrAmdRuntime.dll: the DLL, the test host that drives it the way AMDNR's
# OptiScaler does, a 1080p frame and NVIDIA's output for it, the shaders. The model is not in it.
#   NR_GPU=rdna3 linux/package/amdnr/build_test_kit.sh [output directory]
set -euo pipefail
cd -- "$(dirname -- "$0")/../../.."
gpu=${NR_GPU:-rdna3}
stamp=$(bash linux/build/version.sh)
dir=$(realpath -m -- "${1:-artifacts/windows/amdnr-test-kit-$stamp-$gpu}")
case "$dir" in "$(pwd)"/*) ;; *) echo 'output directory must be in the project' >&2; exit 2;; esac
NR_GPU=$gpu bash linux/build/build_amdnr_runtime.sh >/dev/null
[[ -d "build/windows/$gpu/network" ]] || python3 windows/build/build_network.py "$gpu"   # the shaders the AMD Windows driver compiles
rm -rf -- "$dir"; mkdir -p -- "$dir/dlssnr-amd"
cp -- artifacts/amdnr/nr/DlssnrAmdRuntime.dll "$dir/"
x86_64-w64-mingw32-g++ -std=c++17 -O1 -static linux/test/amdnr_abi_test.cpp -o "$dir/amdnr_abi_test.exe" -ld3d12 -ldxgi -ldxguid -luuid
cp -r -- "build/windows/$gpu/network" "$dir/dlssnr-amd/shaders"
cp -- linux/package/amdnr/README.txt "$dir/"
# A 1080p frame and NVIDIA's output for it (docs/ngx-verification in the DLSSNR-AMD repository; set
# NR_KIT_INPUT and NR_KIT_REFERENCE to PNGs of your own where that folder is not there).
input_png=${NR_KIT_INPUT:-docs/ngx-verification/single-frame-inputs/1920x1080.png}
reference_png=${NR_KIT_REFERENCE:-docs/ngx-verification/single-frame-outputs/1920x1080_nvidia.png}
[[ -f "$input_png" && -f "$reference_png" ]] || { echo "need NR_KIT_INPUT and NR_KIT_REFERENCE (1920x1080 PNGs)" >&2; exit 1; }
python3 -I - "$dir" "$input_png" "$reference_png" <<'PY'
import sys
from PIL import Image
d, source, reference = sys.argv[1:4]
for name, src in (("in_1080.rgba8", source), ("nvidia_1080.rgba8", reference)):
    open(f"{d}/{name}", "wb").write(Image.open(src).convert("RGBA").tobytes())
PY
printf '@echo off\r\ncd /d "%%~dp0"\r\necho Running, this takes a few minutes...\r\namdnr_abi_test.exe DlssnrAmdRuntime.dll in_1080.rgba8 1920 1080 out.rgba8 5 1 nvidia_1080.rgba8 90\r\ncopy /y out.rgba8.log results.txt >nul\r\necho Done. Send results.txt and dlssnr-amd.log.\r\npause\r\n' > "$dir/Run test.bat"
echo "kit in $dir (add dlssnr-amd/dlssnr.bin before running)"
