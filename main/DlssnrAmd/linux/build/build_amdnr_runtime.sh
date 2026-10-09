#!/usr/bin/env bash
# Copyright (c) 2026 Mauri de Souza Meneguzzo (mauri870). MIT, see LICENSE.
# DlssnrAmdRuntime.dll: this project's network as a neural runtime for AMDNR's OptiScaler
# (https://github.com/3zwr1/AMD-NR---OptiScaler), cross-compiled to PE.
#
# The DLL exports the C ABI that host's lmxxf backend loads (LmxxfNrGetApi, linux/src/pe/nr_amdnr_abi.hpp)
# and runs the same model the NGX core does (linux/src/pe/nr_dlssnr_model.cpp). The objects below are the
# ones build_optiscaler_nr.sh builds, with the same flags, so the network cannot differ between the two.
# The Vulkan entry points are resolved at run time, for the reason given in that script.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."

arch=${NR_ARCH:-x86_64}
case "$arch" in x86_64|i686) ;; *) echo "NR_ARCH must be x86_64 or i686" >&2; exit 2;; esac
if [[ "$arch" == i686 ]]; then default_out=artifacts/amdnr/nr32; else default_out=artifacts/amdnr/nr; fi
out=$(realpath -m -- "${1:-$default_out}")
case "$out" in "$(pwd)"/*) ;; *) echo 'build directory must be in the project' >&2; exit 2;; esac
mkdir -p -- "$out"

cxx=${NR_MINGW:-$arch-w64-mingw32-g++}
cc=${NR_MINGW_CC:-$arch-w64-mingw32-gcc}
objdump=${NR_OBJDUMP:-$arch-w64-mingw32-objdump}
nm=${NR_NM:-$arch-w64-mingw32-nm}
command -v "$cxx" >/dev/null || { echo "no mingw cross compiler ($cxx)" >&2; exit 1; }

# MinHook and the Vulkan headers: third_party/ where it exists (the vendored copies), else what fetch_deps.sh downloads.
minhook=third_party/minhook
[[ -d "$minhook" ]] || minhook=artifacts/ref/DLSS5-Feeder/external/minhook
vulkan_headers=third_party/Vulkan-Headers/include
[[ -d "$vulkan_headers" ]] || vulkan_headers=toolchain/Vulkan-Headers/include
[[ -d "$minhook" ]] || { echo "missing MinHook: third_party/minhook, or run fetch_deps.sh" >&2; exit 1; }
[[ -d "$vulkan_headers" ]] || { echo "missing the Vulkan headers: third_party/Vulkan-Headers, or run fetch_deps.sh" >&2; exit 1; }

# Same flag set as the game module: these objects are the same code, and building them twice with
# different flags is how two builds of one network start disagreeing.
# The version the DLL was built from (version.sh), printed in the first line of its log. Without it there is no
# way to tell from a game whether a fix is in the binary that ran.
stamp=$(bash linux/build/version.sh)
common=(-std=c++17 -O2 -DNDEBUG -DNR_BUILD_STAMP="\"$stamp\"" -I"$vulkan_headers" -Ilinux/src -Ilinux/src/core -Ilinux/src/layer -Ilinux/src/pe -I"$out")

# The network the package ships (linux/build/arch/rdna3.sh): the host must be built
# with the same constants as the shaders in dlssnr-amd/shaders.
gpu=${NR_GPU:-rdna3}
[[ -f "linux/build/arch/$gpu.sh" ]] || { echo "NR_GPU must be rdna3" >&2; exit 2; }
source "linux/build/arch/$gpu.sh"
"$cxx" "${common[@]}" "${NR_PRODUCT_DEFINES[@]}" -c linux/src/core/nr_runtime.cpp -o "$out/nr_runtime.o"
"$cxx" "${common[@]}" -c linux/src/core/nr_native_plan.cpp -o "$out/nr_native_plan.o"
"$cxx" "${common[@]}" -c linux/src/pe/nr_pe_log.cpp -o "$out/log.o"
"$cxx" "${common[@]}" -c linux/src/pe/nr_pe_interop.cpp -o "$out/interop.o"
"$cxx" "${common[@]}" -I"$minhook/include" -c linux/src/pe/nr_pe_vkdevice.cpp -o "$out/vkdevice.o"
"$cxx" "${common[@]}" -c linux/src/pe/nr_pe_session.cpp -o "$out/session.o"
"$cxx" "${common[@]}" -c linux/src/pe/nr_pe_config.cpp -o "$out/config.o"
"$cxx" "${common[@]}" -c linux/src/pe/nr_dlssnr_model.cpp -o "$out/model.o"
for unit in runtime inline native vkleg; do
    # The leg builds the device itself, so it needs to know which network the host was built for.
    "$cxx" "${common[@]}" "${NR_PRODUCT_DEFINES[@]}" -c "linux/src/pe/nr_amdnr_$unit.cpp" -o "$out/amdnr_$unit.o"
done

# MinHook, for the device watcher the Vulkan path asks for a queue through.
for unit in hook buffer trampoline; do
    "$cc" -O2 -I"$minhook/include" -I"$minhook/src" -c "$minhook/src/$unit.c" -o "$out/mh_$unit.o"
done
hde=hde64; [[ "$arch" == i686 ]] && hde=hde32
"$cc" -O2 -I"$minhook/include" -I"$minhook/src" -c "$minhook/src/hde/$hde.c" -o "$out/mh_hde.o"

ldflags=(-ld3d12 -ldxgi -lole32 -static -static-libgcc -static-libstdc++)
[[ "$arch" == i686 ]] && ldflags+=(-Wl,--kill-at)

objs=("$out/amdnr_runtime.o" "$out/amdnr_inline.o" "$out/amdnr_native.o" "$out/amdnr_vkleg.o" "$out/model.o" "$out/session.o" "$out/config.o" "$out/interop.o" "$out/log.o"
      "$out/vkdevice.o" "$out/nr_runtime.o" "$out/nr_native_plan.o" "$out"/mh_*.o)
# NR_BUILD_TESTS=1 also builds two executables (the Vulkan leg's test, and nr_graph, the network's own command
# line tool); the Vulkan entry points they call join the ones the DLL needs.
test_vk=""
if [[ "${NR_BUILD_TESTS:-0}" == 1 ]]; then
    "$cxx" "${common[@]}" "${NR_PRODUCT_DEFINES[@]}" -w -c linux/test/amdnr_leg_test.cpp -o "$out/amdnr_leg_test.o"
    "$cxx" "${common[@]}" "${NR_PRODUCT_DEFINES[@]}" -w -c linux/src/core/nr_graph.cpp -o "$out/nr_graph.o"
    test_vk=$("$nm" -u "$out/amdnr_leg_test.o" "$out/nr_graph.o" | sed -n 's/^ *U //p' | grep -E '^_*vk' || true)
fi
undefined=$({ { "$cxx" -shared -o /dev/null "${objs[@]}" "${ldflags[@]}" 2>&1 || true; } |
    grep -o "undefined reference to \`_*vk[A-Za-z0-9_@]*'" | sed "s/.*\`//; s/'//"; echo "$test_vk"; } | grep . | sort -u)
count=$(echo "$undefined" | grep -c . || true)
plain() { echo "$1" | sed 's/^_//; s/@[0-9]*$//'; }

{
    echo '// Generated by linux/build/build_amdnr_runtime.sh. One tail jump per entry point.'
    echo '    .text'
    for sym in $undefined; do
        base=$(plain "$sym")
        if [[ "$arch" == i686 ]]; then
            label="_${sym#_}"
            printf '    .globl %s\n%s:\n    jmp *_nr_vk_p_%s\n' "$label" "$label" "$base"
        else
            printf '    .globl %s\n%s:\n    jmp *nr_vk_p_%s(%%rip)\n' "$sym" "$sym" "$base"
        fi
    done
    echo '    .data'
    for sym in $undefined; do
        base=$(plain "$sym")
        if [[ "$arch" == i686 ]]; then
            printf '    .globl _nr_vk_p_%s\n    .align 4\n_nr_vk_p_%s:\n    .long _nr_vk_unresolved\n' "$base" "$base"
        else
            printf '    .globl nr_vk_p_%s\n    .align 8\nnr_vk_p_%s:\n    .quad nr_vk_unresolved\n' "$base" "$base"
        fi
    done
} > "$out/vulkan_lazy.s"

{
    echo '// Generated by linux/build/build_amdnr_runtime.sh.'
    echo '#include <windows.h>'
    echo 'long nr_vk_unresolved(void) { return -3; }   // VK_ERROR_INITIALIZATION_FAILED'
    for sym in $undefined; do printf 'extern void* nr_vk_p_%s;\n' "$(plain "$sym")"; done
    echo 'void nr_vk_load(void) {'
    echo '    HMODULE m = GetModuleHandleW(L"vulkan-1.dll");'
    echo '    if (!m) m = LoadLibraryW(L"vulkan-1.dll");'
    echo '    if (!m) return;'
    for sym in $undefined; do
        base=$(plain "$sym")
        printf '    { void* p = (void*)GetProcAddress(m, "%s"); if (p) nr_vk_p_%s = p; else OutputDebugStringA("nr_vk_load: no export %s"); }\n' "$base" "$base" "$base"
    done
    echo '}'
} > "$out/vulkan_lazy.c"

"$cc" -c "$out/vulkan_lazy.s" -o "$out/vulkan_lazy_s.o"
"$cc" -O2 -c "$out/vulkan_lazy.c" -o "$out/vulkan_lazy_c.o"
echo "vulkan entry points resolved at run time: $count"

"$cxx" -shared -o "$out/DlssnrAmdRuntime.dll" "${objs[@]}" "$out/vulkan_lazy_s.o" "$out/vulkan_lazy_c.o" "${ldflags[@]}"

echo
echo "built $out/DlssnrAmdRuntime.dll ($(stat -c %s "$out/DlssnrAmdRuntime.dll") bytes), exports:"
"$objdump" -p "$out/DlssnrAmdRuntime.dll" | grep -o '\bLmxxfNr[A-Za-z0-9_]*' | sort -u | sed 's/^/    /'

# NR_BUILD_TESTS=1: the executables, linked against the same objects and the same run-time Vulkan thunks.
if [[ "${NR_BUILD_TESTS:-0}" == 1 ]]; then
    "$cxx" -o "$out/amdnr_leg_test.exe" "$out/amdnr_leg_test.o" "${objs[@]:1}" "$out/vulkan_lazy_s.o" "$out/vulkan_lazy_c.o" "${ldflags[@]}"
    # nr_graph has its own main and builds the runtime in with it; only the device-watcher objects are not its.
    # Nothing calls nr_vk_load for it (the DLL's DllMain does), so a constructor does, before main.
    printf 'extern void nr_vk_load(void);\n__attribute__((constructor)) static void nr_vk_autoload(void) { nr_vk_load(); }\n' > "$out/vulkan_autoload.c"
    "$cc" -O2 -c "$out/vulkan_autoload.c" -o "$out/vulkan_autoload.o"
    "$cxx" -o "$out/nr_graph.exe" "$out/nr_graph.o" "$out/log.o" "$out/vulkan_autoload.o" "$out/vulkan_lazy_s.o" "$out/vulkan_lazy_c.o" "${ldflags[@]}"
    echo "built $out/amdnr_leg_test.exe and $out/nr_graph.exe"
fi
