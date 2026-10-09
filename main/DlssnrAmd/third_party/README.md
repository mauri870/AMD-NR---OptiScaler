# Third-party code the runtime is built from

Both are compiled into or against `DlssnrAmdRuntime.dll`, so they are kept here, unmodified and with their
licences, and the build needs no download for them (`linux/build/build_amdnr_runtime.sh` prefers this folder).

| Directory | What | Version | Licence |
| --- | --- | --- | --- |
| `Vulkan-Headers/include` | the C headers of [KhronosGroup/Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) (`vulkan/*.h`, `vk_video/*.h`; the C++ bindings are left out) | v1.4.357, commit `e3b1eec08173d6b825cd3ac88c885a63b621504a` | Apache-2.0 or MIT (`Vulkan-Headers/LICENSES`) |
| `minhook` | [MinHook](https://github.com/TsudaKageyu/minhook), the API hooking library the Vulkan device watcher uses, as bundled in [DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder) at commit `76a08db83652617cfe4192eeecf9e46bfc008382` | as bundled there | BSD-2-Clause (`minhook/LICENSE.txt`) |

Not here: glslang, the compiler that turns the shaders into SPIR-V. It is a build tool, not code in the DLL;
`fetch_deps.sh --runtime` downloads the pinned 16.5.0 release and checks its SHA-256.
