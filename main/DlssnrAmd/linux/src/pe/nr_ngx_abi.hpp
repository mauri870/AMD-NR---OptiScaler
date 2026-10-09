// The slice of the NGX ABI our two OptiScaler-side DLLs have to speak, declared without the SDK.
//
// Why not include NVIDIA's own nvsdk_ngx.h: it drags in d3d11.h, dxgi.h and vulkan.h, is written for
// MSVC, and we only need the shapes OptiScaler actually calls through. Every type below is either
// pointer-sized or an int-sized enum, so the layout is the Microsoft x64 ABI's regardless of which
// header declared it. The declarations were read from, and must stay in step with:
//
//   OptiScaler_DLSSNR/external/nvngx_dlss_sdk/nvsdk_ngx{,_defs,_params,_vk}.h
//   OptiScaler_DLSSNR/OptiScaler/proxies/NVNGX_Proxy.h   (the caller's own typedefs)
//
// NVNGX_Proxy.h is the authority for the *core* entry points, because those typedefs are literally
// what OptiScaler casts our GetProcAddress results to.

#pragma once

#include "nr_ngx_slots.hpp"

#include <cstddef>
#include <cstdint>

// Opaque D3D interfaces. Only ever passed and stored as pointers on this side, so a forward
// declaration is the whole ABI. (The forwarder's own translation unit includes the real d3d12.h and
// its ID3D12Resource is the same pointer.)
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Resource;
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct IDXGIAdapter;

// Vulkan dispatchable handles are pointers on every 64-bit platform, which is the only platform this
// builds for. Kept opaque for the same reason the reference forwarder keeps them opaque: so neither
// DLL carries a Vulkan dependency purely to name a type it never dereferences.
using NrVkInstance = void*;
using NrVkPhysicalDevice = void*;
using NrVkDevice = void*;
using NrVkCommandBuffer = void*;

typedef enum NVSDK_NGX_Result
{
    NVSDK_NGX_Result_Success = 0x1,
    NVSDK_NGX_Result_Fail = 0xBAD00000,
    NVSDK_NGX_Result_FAIL_FeatureNotSupported = NVSDK_NGX_Result_Fail | 1,
    NVSDK_NGX_Result_FAIL_PlatformError = NVSDK_NGX_Result_Fail | 2,
    NVSDK_NGX_Result_FAIL_FeatureAlreadyExists = NVSDK_NGX_Result_Fail | 3,
    NVSDK_NGX_Result_FAIL_FeatureNotFound = NVSDK_NGX_Result_Fail | 4,
    NVSDK_NGX_Result_FAIL_InvalidParameter = NVSDK_NGX_Result_Fail | 5,
    NVSDK_NGX_Result_FAIL_NotInitialized = NVSDK_NGX_Result_Fail | 7,
    NVSDK_NGX_Result_FAIL_UnableToInitializeFeature = NVSDK_NGX_Result_Fail | 11,
    NVSDK_NGX_Result_FAIL_UnsupportedParameter = NVSDK_NGX_Result_Fail | 16,
    NVSDK_NGX_Result_FAIL_NotImplemented = NVSDK_NGX_Result_Fail | 18,
} NVSDK_NGX_Result;

typedef enum NVSDK_NGX_Feature
{
    NVSDK_NGX_Feature_SuperSampling = 1,
    NVSDK_NGX_Feature_FrameGeneration = 11,
    NVSDK_NGX_Feature_RayReconstruction = 13,
    // 18 is Neural Rendering. It is not in the public SDK enum; the value comes from the snippet and
    // is what the reference forwarder passes to CreateFeature.
    NVSDK_NGX_Feature_NeuralRendering = 18,
} NVSDK_NGX_Feature;

typedef enum NVSDK_NGX_Version
{
    NVSDK_NGX_Version_API = 0x0000015,
} NVSDK_NGX_Version;

typedef enum NVSDK_NGX_EngineType
{
    NVSDK_NGX_ENGINE_TYPE_CUSTOM = 0,
} NVSDK_NGX_EngineType;

typedef struct NVSDK_NGX_Handle
{
    unsigned int Id;
} NVSDK_NGX_Handle;

// The parameter block, laid out by hand rather than by the compiler.
//
// NVSDK_NGX_Parameter is declared in the SDK as a pure-virtual C++ class (eight Set overloads, then
// eight Get overloads, then Reset -- external/nvngx_dlss_sdk/nvsdk_ngx_params.h). Every caller on the
// other side of this ABI is MSVC-built, and MSVC does NOT lay overloaded virtual functions out in
// declaration order: within a run of overloads sharing a name it emits them in REVERSE declaration
// order. GCC emits them in declaration order. So a mingw class deriving from that SDK declaration is
// binary-incompatible with every MSVC caller, in the worst possible way -- the call lands on a
// different overload of the right name, with the wrong argument in the wrong register, and succeeds.
//
// That is not a theory. OptiScaler's DiscoverFloatSlot round-trips 0.375 through each slot of the
// block our _nvngx.dll returns and settled on slot 5, and its readback of DLSSNR.Intensity through
// Get(const char*, float*) answered 0xBAD00000. The reference forwarder found the same thing against
// NVIDIA's own block from the other direction and wrote it down: dlssnr/DlssNr_Proxy.h, "floats at
// vtable slot 6, not the header's 1".
//
// So the vtable is written out here, in MSVC's order, and both sides of this project index it by
// hand. The two groups reverse independently and Reset -- not overloaded -- keeps its place:
//
//   0 Set(void*)            8  Get(void**)
//   1 Set(ID3D12Resource*)  9  Get(ID3D12Resource**)
//   2 Set(ID3D11Resource*)  10 Get(ID3D11Resource**)
//   3 Set(int)              11 Get(int*)
//   4 Set(unsigned int)     12 Get(unsigned int*)
//   5 Set(double)           13 Get(double*)
//   6 Set(float)            14 Get(float*)
//   7 Set(unsigned long long)  15 Get(unsigned long long*)
//   16 Reset()
//
// The slot numbers and NR_NGX_MSABI itself are in nr_ngx_slots.hpp, included above, because
// a module that hooks the game's NGX calls needs them while including NVIDIA's own <nvsdk_ngx.h> and so cannot
// include this file.

struct NVSDK_NGX_Parameter;

struct NVSDK_NGX_ParameterVtbl
{
    void (NR_NGX_MSABI* SetVoidPtr)(NVSDK_NGX_Parameter*, const char*, void*);
    void (NR_NGX_MSABI* SetD3D12Resource)(NVSDK_NGX_Parameter*, const char*, ID3D12Resource*);
    void (NR_NGX_MSABI* SetD3D11Resource)(NVSDK_NGX_Parameter*, const char*, ID3D11Resource*);
    void (NR_NGX_MSABI* SetInt)(NVSDK_NGX_Parameter*, const char*, int);
    void (NR_NGX_MSABI* SetUInt)(NVSDK_NGX_Parameter*, const char*, unsigned int);
    void (NR_NGX_MSABI* SetDouble)(NVSDK_NGX_Parameter*, const char*, double);
    void (NR_NGX_MSABI* SetFloat)(NVSDK_NGX_Parameter*, const char*, float);
    void (NR_NGX_MSABI* SetULL)(NVSDK_NGX_Parameter*, const char*, unsigned long long);

    NVSDK_NGX_Result (NR_NGX_MSABI* GetVoidPtr)(const NVSDK_NGX_Parameter*, const char*, void**);
    NVSDK_NGX_Result (NR_NGX_MSABI* GetD3D12Resource)(const NVSDK_NGX_Parameter*, const char*, ID3D12Resource**);
    NVSDK_NGX_Result (NR_NGX_MSABI* GetD3D11Resource)(const NVSDK_NGX_Parameter*, const char*, ID3D11Resource**);
    NVSDK_NGX_Result (NR_NGX_MSABI* GetInt)(const NVSDK_NGX_Parameter*, const char*, int*);
    NVSDK_NGX_Result (NR_NGX_MSABI* GetUInt)(const NVSDK_NGX_Parameter*, const char*, unsigned int*);
    NVSDK_NGX_Result (NR_NGX_MSABI* GetDouble)(const NVSDK_NGX_Parameter*, const char*, double*);
    NVSDK_NGX_Result (NR_NGX_MSABI* GetFloat)(const NVSDK_NGX_Parameter*, const char*, float*);
    NVSDK_NGX_Result (NR_NGX_MSABI* GetULL)(const NVSDK_NGX_Parameter*, const char*, unsigned long long*);

    void (NR_NGX_MSABI* Reset)(NVSDK_NGX_Parameter*);
};

// The object is one pointer: a vtable pointer, exactly as the MSVC class is. The Set/Get members are
// ORDINARY (non-virtual) functions that dispatch through that table, so every existing call site --
// `params->Get(name, &resource)`, `params->Set(name, 1u)` -- keeps its overload resolution and starts
// landing on the right slot. Adding a virtual function to this struct would put a second vtable
// pointer in front of the block's own and break everything.
struct NVSDK_NGX_Parameter
{
    const NVSDK_NGX_ParameterVtbl* vtbl;

    void Set(const char* n, void* v)                { vtbl->SetVoidPtr(this, n, v); }
    void Set(const char* n, ID3D12Resource* v)      { vtbl->SetD3D12Resource(this, n, v); }
    void Set(const char* n, ID3D11Resource* v)      { vtbl->SetD3D11Resource(this, n, v); }
    void Set(const char* n, int v)                  { vtbl->SetInt(this, n, v); }
    void Set(const char* n, unsigned int v)         { vtbl->SetUInt(this, n, v); }
    void Set(const char* n, double v)               { vtbl->SetDouble(this, n, v); }
    void Set(const char* n, float v)                { vtbl->SetFloat(this, n, v); }
    void Set(const char* n, unsigned long long v)   { vtbl->SetULL(this, n, v); }

    NVSDK_NGX_Result Get(const char* n, void** v) const               { return vtbl->GetVoidPtr(this, n, v); }
    NVSDK_NGX_Result Get(const char* n, ID3D12Resource** v) const     { return vtbl->GetD3D12Resource(this, n, v); }
    NVSDK_NGX_Result Get(const char* n, ID3D11Resource** v) const     { return vtbl->GetD3D11Resource(this, n, v); }
    NVSDK_NGX_Result Get(const char* n, int* v) const                 { return vtbl->GetInt(this, n, v); }
    NVSDK_NGX_Result Get(const char* n, unsigned int* v) const        { return vtbl->GetUInt(this, n, v); }
    NVSDK_NGX_Result Get(const char* n, double* v) const              { return vtbl->GetDouble(this, n, v); }
    NVSDK_NGX_Result Get(const char* n, float* v) const               { return vtbl->GetFloat(this, n, v); }
    NVSDK_NGX_Result Get(const char* n, unsigned long long* v) const  { return vtbl->GetULL(this, n, v); }

    void Reset() { vtbl->Reset(this); }
};

static_assert(sizeof(NVSDK_NGX_Parameter) == sizeof(void*),
              "the parameter block is a vtable pointer and nothing else");
static_assert(sizeof(NVSDK_NGX_ParameterVtbl) == 17 * sizeof(void*),
              "17 slots: eight setters, eight getters, Reset");

// NVSDK_NGX_Resource_VK, the shape the Vulkan path hands us. Only the image view / image / format /
// extent and the read-write flag are read here; the union's buffer arm exists so the struct's size
// and the offsets of everything after it are right. Field order and types read from
// external/nvngx_dlss_sdk/nvsdk_ngx_vk.h.
typedef enum NVSDK_NGX_Resource_VK_Type
{
    NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW = 0,
    NVSDK_NGX_RESOURCE_VK_TYPE_VK_BUFFER = 1,
} NVSDK_NGX_Resource_VK_Type;

typedef struct NVSDK_NGX_ImageViewInfo_VK
{
    void* ImageView;       // VkImageView (non-dispatchable: uint64 on 64-bit, same size as a pointer)
    void* Image;           // VkImage
    struct
    {
        unsigned int aspectMask;
        unsigned int baseMipLevel;
        unsigned int levelCount;
        unsigned int baseArrayLayer;
        unsigned int layerCount;
    } SubresourceRange;
    unsigned int Format;   // VkFormat
    unsigned int Width;
    unsigned int Height;
} NVSDK_NGX_ImageViewInfo_VK;

typedef struct NVSDK_NGX_BufferInfo_VK
{
    void* Buffer;
    unsigned int SizeInBytes;
} NVSDK_NGX_BufferInfo_VK;

typedef struct NVSDK_NGX_Resource_VK
{
    union
    {
        NVSDK_NGX_ImageViewInfo_VK ImageViewInfo;
        NVSDK_NGX_BufferInfo_VK BufferInfo;
    } Resource;
    NVSDK_NGX_Resource_VK_Type Type;
    bool ReadWrite;
} NVSDK_NGX_Resource_VK;
