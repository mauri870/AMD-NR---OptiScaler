#pragma once
// Where the NGX parameter block's methods actually live, and how to call one.
//
// Split out of nr_ngx_abi.hpp for one reason: a module that hooks the game's NGX calls includes NVIDIA's own
// <nvsdk_ngx.h> -- it hooks the game's whole NGX surface and needs the SDK's types for that -- so it
// cannot also include nr_ngx_abi.hpp, which declares the same names. It still has to index the same
// vtable, and two files guessing the same numbers separately is how they drift. They come from here.
//
// NVSDK_NGX_Parameter is declared in the SDK as a pure-virtual C++ class: eight Set overloads, then
// eight Get overloads, then Reset (external/nvngx_dlss_sdk/nvsdk_ngx_params.h:55-62). Every block a
// caller hands us was laid out by MSVC, and MSVC does NOT emit overloaded virtual functions in
// declaration order: within a run of overloads sharing a name it emits them in REVERSE declaration
// order. GCC emits them in declaration order. So a mingw class deriving from -- or calling through --
// that SDK declaration is binary-incompatible with every MSVC-built block, in the worst possible way:
// the call lands on a different overload of the right name, with the wrong argument in the wrong
// register, and succeeds.
//
// That is measured, from both directions. OptiScaler's DiscoverFloatSlot round-trips 0.375 through
// each slot of the block our _nvngx.dll returns and settles on slot 6; the reference DLSS-NR
// forwarder found the same thing against NVIDIA's own block and wrote it down in
// dlssnr/DlssNr_Proxy.h -- "floats at vtable slot 6, not the header's 1".
//
// The two groups reverse independently and Reset -- not overloaded -- keeps its place:
//
//   0 Set(void*)               8  Get(void**)
//   1 Set(ID3D12Resource*)     9  Get(ID3D12Resource**)
//   2 Set(ID3D11Resource*)     10 Get(ID3D11Resource**)
//   3 Set(int)                 11 Get(int*)
//   4 Set(unsigned int)        12 Get(unsigned int*)
//   5 Set(double)              13 Get(double*)
//   6 Set(float)               14 Get(float*)
//   7 Set(unsigned long long)  15 Get(unsigned long long*)
//   16 Reset()
//
// Calling convention: on x86_64 Windows __thiscall is the ordinary Win64 convention with `this` in
// RCX, which is what mingw emits by default for this target. NR_NGX_MSABI says so out loud rather
// than relying on the default, so a build for any other target fails to compile instead of silently
// passing `this` in RDI.
#if defined(__GNUC__) && defined(__x86_64__)
#define NR_NGX_MSABI __attribute__((ms_abi))
#else
#define NR_NGX_MSABI
#endif

enum : int
{
    kNgxSlotSetVoidPtr = 0,
    kNgxSlotSetD3D12 = 1,
    kNgxSlotSetD3D11 = 2,
    kNgxSlotSetInt = 3,
    kNgxSlotSetUInt = 4,
    kNgxSlotSetDouble = 5,
    kNgxSlotSetFloat = 6,
    kNgxSlotSetULL = 7,
    kNgxGetterOffset = 8,
    kNgxSlotGetVoidPtr = 8,
    kNgxSlotGetD3D12 = 9,
    kNgxSlotGetD3D11 = 10,
    kNgxSlotGetInt = 11,
    kNgxSlotGetUInt = 12,
    kNgxSlotGetDouble = 13,
    kNgxSlotGetFloat = 14,
    kNgxSlotGetULL = 15,
    kNgxSlotReset = 16,
};
