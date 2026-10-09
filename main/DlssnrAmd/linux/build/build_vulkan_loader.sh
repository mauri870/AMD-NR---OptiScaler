#!/usr/bin/env bash
# The Vulkan loader the Vulkan/D3D9 route ships, built from the Khronos source
# with three patches, for both bitnesses, plus the winevulkan.dll forwarder.
#
# Why a loader at all: ReShade's Vulkan backend is a Vulkan layer, and only the
# Khronos loader loads layers - Wine's own vulkan-1/winevulkan do not. Why a
# patched one: under Wine every process reads as high integrity, so the stock
# loader ignores every VK_* environment override and falls back to the
# registry, where its DXGI-based driver verification either deadlocks inside
# DXVK's dxgi.dll (which creates a Vulkan instance through this very loader
# while holding its lock) or drags Wine's OpenGL in. So:
#   patch-local-override.py  manifests next to the DLL: vk-override/nr-icd.json,
#                            vk-override/implicit_layer/, vk-override/explicit_layer/
#   patch-no-dxgi.py         never touch DXGI (only verification and sorting used it)
#   patch-queue-info.py      vkGetDeviceQueue2 reaches the driver with its padding
#                            zeroed: Wine 11 memcmp()s the whole struct and ReShade
#                            leaves the padding uninitialised (64-bit only)
# Why a forwarder: DXVK loads "winevulkan.dll" before it ever looks at
# vulkan-1.dll, and ReShade looks the loader up as "vulkan-1.dll"; a
# winevulkan.dll whose exports forward to vulkan-1.dll satisfies both with one
# loader instance. The real winevulkan (the ICD) is then loaded by the loader
# through its full system32 path, which Wine keeps apart from ours.
#
# Output: artifacts/vulkan-loader/{i686,x86_64}/{vulkan-1.dll,winevulkan.dll}
#         plus LICENSE.txt and the patch scripts (Apache-2.0 asks for the changes
#         to be stated).
set -euo pipefail
cd "$(dirname "$0")/../.."
src=artifacts/ref/Vulkan-Loader
tag=v1.4.357
if [[ ! -d "$src" ]]; then
    git clone -q --depth 1 --branch "$tag" https://github.com/KhronosGroup/Vulkan-Loader.git "$src"
fi
root=$PWD
( cd "$src" && git checkout -q -- loader && python3 "$root/linux/vulkan-loader/patch-local-override.py" && python3 "$root/linux/vulkan-loader/patch-no-dxgi.py" && python3 "$root/linux/vulkan-loader/patch-queue-info.py" )
for arch in i686 x86_64; do
    out=artifacts/vulkan-loader/$arch
    mkdir -p "$out"
    cmake -S "$src" -B "$src/build-$arch" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$PWD/linux/vulkan-loader/toolchain-$arch.cmake" \
        -DCMAKE_BUILD_TYPE=Release -DUPDATE_DEPS=ON -DBUILD_TESTS=OFF -DUSE_GAS=ON > "$src/cmake-$arch.log" 2>&1
    ninja -C "$src/build-$arch" > "$src/ninja-$arch.log" 2>&1
    cp -- "$src/build-$arch/loader/vulkan-1.dll" "$out/vulkan-1.dll"
    extra=(); [[ $arch == i686 ]] && extra=(-Wl,--kill-at)
    "$arch-w64-mingw32-gcc" -shared -o "$out/winevulkan.dll" linux/vulkan-loader/winevulkan_forwarder.c \
        linux/vulkan-loader/winevulkan_forwarder.def "${extra[@]}"
    cp -- "$src/LICENSE.txt" "$out/LICENSE.txt"
    cp -- linux/vulkan-loader/patch-local-override.py linux/vulkan-loader/patch-no-dxgi.py \
        linux/vulkan-loader/patch-queue-info.py "$out/"
    ( cd "$src" && git diff -- loader ) > "$out/PATCHES.diff"
    echo "built $out: $(ls "$out" | tr '\n' ' ')"
done
