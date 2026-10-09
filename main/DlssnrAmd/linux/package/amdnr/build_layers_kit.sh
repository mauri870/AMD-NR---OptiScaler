#!/usr/bin/env bash
# Copyright (c) 2026 Mauri de Souza Meneguzzo (mauri870). MIT, see LICENSE.
# The Windows kit that finds the first kernel the AMD driver gets wrong: nr_graph.exe runs the 1080p network layer by
# layer (persistent kernels off) on the Windows-built RDNA3 shaders and prints a hash of every layer's values; compare
# what comes back with the golden using linux/test/compare_layers.py. The model is not in the kit.
#   linux/package/amdnr/build_layers_kit.sh [output directory]
# Needs the Windows RDNA3 network built (python3 windows/build/build_network.py rdna3), glslang and a mingw cross
# compiler; NR_KIT_INPUT is the 1080p frame (default docs/ngx-verification/single-frame-inputs/1920x1080.png).
set -euo pipefail
cd -- "$(dirname -- "$0")/../../.."
stamp=$(bash linux/build/version.sh)
dir=$(realpath -m -- "${1:-artifacts/windows/amdnr-layers-kit-$stamp}")
case "$dir" in "$(pwd)"/*) ;; *) echo 'output directory must be in the project' >&2; exit 2;; esac
input_png=${NR_KIT_INPUT:-docs/ngx-verification/single-frame-inputs/1920x1080.png}
[[ -f "$input_png" ]] || { echo "need NR_KIT_INPUT (a 1920x1080 PNG)" >&2; exit 1; }
[[ -d build/windows/rdna3/network ]] || python3 windows/build/build_network.py rdna3
NR_BUILD_TESTS=1 NR_GPU=rdna3 bash linux/build/build_amdnr_runtime.sh >/dev/null
source linux/build/arch/rdna3.sh
g++ -std=c++20 -O1 -w -Ilinux/src/core "${NR_PRODUCT_DEFINES[@]}" linux/test/mkplan.cpp linux/src/core/nr_native_plan.cpp -o "$dir.mkplan"
rm -rf -- "$dir"; mkdir -p -- "$dir/dlssnr-amd"
"$dir.mkplan" 1920 1080 > "$dir/plan_1080.txt"; rm -f -- "$dir.mkplan"
cp -- artifacts/amdnr/nr/nr_graph.exe "$dir/"
cp -r -- build/windows/rdna3/network "$dir/dlssnr-amd/shaders"
python3 -I - "$dir" "$input_png" <<'PY'
import sys
import numpy as np
from PIL import Image
d, src = sys.argv[1:3]
rgba = np.array(Image.open(src).convert("RGBA"), np.uint8)
assert rgba.shape == (1080, 1920, 4)
(rgba.reshape(-1, 4).astype(np.float32) / 255).tofile(f"{d}/in_1080p.f32")
PY
cat > "$dir/Run test.bat" <<'BAT'
@echo off
cd /d "%~dp0"
if not exist dlssnr-amd\dlssnr.bin (echo Put dlssnr.bin into the folder dlssnr-amd first. & pause & exit /b 1)
wmic path win32_VideoController get name,driverversion > gpu.txt 2>nul
set ARGS=--plan plan_1080.txt --model-pack dlssnr-amd\dlssnr.bin --spv-dir dlssnr-amd\shaders --host-boundary --no-reuse --no-persist --source-width 1920 --source-height 1080 --in-image in_1080p.f32 --value-stats
echo Run 1 of 2: tile counters. The first run compiles the shaders, this takes a few minutes...
nr_graph.exe %ARGS% > layers1.txt 2> layers1.err
echo Run 2 of 2: barriers between steps...
set NR_TCHAIN=0
nr_graph.exe %ARGS% > layers2.txt 2> layers2.err
echo Done. Send layers1.txt, layers2.txt, layers1.err, layers2.err and gpu.txt.
pause
BAT
cat > "$dir/README.txt" <<'TXT'
Which part of the network does the AMD Windows driver compute wrongly?  (not a game package)

1. Unzip this whole folder (not from inside the zip), and put the extra file you were sent, dlssnr.bin, into the
   folder dlssnr-amd.
2. Close games and other programs that use the graphics card, then double-click "Run test.bat". If Windows
   says "Windows protected your PC", click "More info", then "Run anyway" (the programs are not signed).
   It takes 2 to 10 minutes; the screen may flicker once.
3. Send back layers1.txt, layers2.txt, layers1.err, layers2.err and gpu.txt, all in the same folder.
   If a window closes by itself or the screen goes black, say which run it was in.
TXT
echo "kit in $dir (add dlssnr-amd/dlssnr.bin before running)"
