#!/usr/bin/env bash
# Copyright (c) 2026 Mauri de Souza Meneguzzo (mauri870). MIT, see LICENSE.
# A test overlay that makes AMDNR's OptiScaler (unmodified, 0.3.5.x) run this project's network in
# place of the lmxxf runtime. The host recognises that runtime by two files beside OptiScaler.dll:
# LmxxfNrRuntime.dll, and LmxxfNrRuntime.pak, a regular file over 1 MiB (it only checks that it is
# there; this DLL never reads it). Copy the overlay over the game folder after installing AMDNR,
# add dlssnr-amd/dlssnr.bin (extracted from your own nvngx_dlssnr.dll), and pick "lmxxf" in the
# Neural tab. It replaces AMDNR's own LmxxfNrRuntime.dll, so keep that one if you want it back.
#   NR_GPU=rdna3 linux/package/amdnr/build_dropin.sh [output directory]
set -euo pipefail
cd -- "$(dirname -- "$0")/../../.."
gpu=${NR_GPU:-rdna3}
stamp=$(bash linux/build/version.sh)
dir=$(realpath -m -- "${1:-artifacts/windows/amdnr-dropin-$stamp-$gpu}")
case "$dir" in "$(pwd)"/*) ;; *) echo 'output directory must be in the project' >&2; exit 2;; esac
out=artifacts/amdnr/nr-$gpu
NR_GPU=$gpu bash linux/build/build_amdnr_runtime.sh "$out" >/dev/null
[[ -d "build/windows/$gpu/network" ]] || python3 windows/build/build_network.py "$gpu"   # the shaders the AMD Windows driver compiles
rm -rf -- "$dir"; mkdir -p -- "$dir/dlssnr-amd"
cp -- "$out/DlssnrAmdRuntime.dll" "$dir/LmxxfNrRuntime.dll"
head -c $((1 << 20 | 4096)) /dev/zero > "$dir/LmxxfNrRuntime.pak"
cp -r -- "build/windows/$gpu/network" "$dir/dlssnr-amd/shaders"
echo "overlay in $dir (add dlssnr-amd/dlssnr.bin; test use only: it stands in for AMDNR's own LmxxfNrRuntime.dll)"
