#pragma once
// The DLSS-NR model, once, for both of the DLLs that publish it.
//
// Two different OptiScaler lineages reach feature 18 by two different doors and neither is going
// away:
//
//   nvngx.dll_dlssnr.dll   Dagherbou's OptiScaler_DLSSNR (and the v0.2.0 release) loads this beside
//                          itself and calls dlssnr_call_* / dlssnr_vk_* -- linux/src/pe/nr_dlssnr_forwarder.cpp
//   _nvngx.dll             wilsjo2's fork from 0.8.1 on loads NO forwarder at all. It takes the NGX
//                          core's capability block, sets DLSSNR.* on it, and calls
//                          NVSDK_NGX_D3D12_CreateFeature(cmd, 18, params, &handle) -- linux/src/pe/nr_ngx_core.cpp
//
// Behind both doors the work is identical: one nr::pe::Session per graphics API, a Feature handle
// carrying the extent and the six controls, a seed copy of colour into output, and Session::run_after
// (D3D12) or Session::run_vulkan. That work lives here so the two entry points cannot drift -- the
// forwarder was the original and this file is its body, lifted out unchanged.
//
// The parameter block is written through its vtable in both cases, even from _nvngx.dll where the
// block is our own class: the write is what OptiScaler reads back to report what landed, and one code
// path for it is one thing that can be wrong. Reading is different -- see nr_ngx_core.cpp, which reads
// its own map directly.

#include "nr_ngx_abi.hpp"

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace nr::dlssnr {

// The six controls the NGX surface carries, in the order every caller passes them. Defaults are
// nr::Controls's own, so a caller that reads nothing from the block still gets a sane model.
struct Controls6 {
    float intensity = 1.0f;
    int style = 0;
    float local_structure = 1.0f;
    float local_tone = 1.0f;
    float skin_structure = -1.0f;
    int use_auto_mask = 1;
};

// A guide subrect: where in the allocation this frame's data is, and how much of it.
// Base and extent are both honoured, as NVIDIA's DLL does -- nr_pe_session.cpp apply_guide_subrect.
struct Rect {
    unsigned int x = 0, y = 0, width = 0, height = 0;
};

// One live feature. Opaque: a handle to the caller, a struct to this file.
struct Feature;

// ---------------------------------------------------------------------------------------------
// The caller's parameter block.
// ---------------------------------------------------------------------------------------------

// Which vtable slot floats live in. Defaults to MSVC's 6 (nr_ngx_abi.hpp), which is both what our own
// block publishes and where NVIDIA's driver block keeps them.
void set_float_slot(int slot);
// Write a float through an arbitrary slot, so a host can find the right one by probing.
void probe_float(void* params, const char* name, float value, int slot);

// What the reference writes at create, and again at every evaluate. Nothing here reads these back:
// the controls arrive as arguments. They are written because OptiScaler reads them back to report
// what landed, and a block answering 0xBAD00000 to all of them looks like a broken integration.
void write_create_keys(void* params, unsigned int width, unsigned int height, int preset,
                       int ui_correction, const Controls6& controls);
void write_evaluate_keys(void* params, const Controls6& controls, bool reset, int depth_inverted);

// Why the last create or evaluate declined, in words; empty when nothing has.
const char* last_error();

// ---------------------------------------------------------------------------------------------
// Direct3D 12, through vkd3d-proton.
// ---------------------------------------------------------------------------------------------

// Returns null on failure, and writes the NGX result code into *out_result either way.
Feature* create_d3d12(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, void* params,
                      unsigned int width, unsigned int height, int preset, int ui_correction,
                      const Controls6& controls, int* out_result);

// NGX result codes, except 0 for "bad arguments", which is what the reference forwarder answers and
// what the forwarder ABI's callers test for.
int evaluate_d3d12(ID3D12GraphicsCommandList* cmd, Feature* feature, void* params,
                   ID3D12Resource* color, ID3D12Resource* depth, ID3D12Resource* motion,
                   ID3D12Resource* output, unsigned int width, unsigned int height,
                   const Rect& depth_rect, const Rect& motion_rect, int depth_inverted, int reset,
                   const Controls6& controls, float mv_scale_x, float mv_scale_y);

void release_d3d12(Feature* feature);
// What the D3D12 session's network costs on the GPU, in milliseconds (smoothed); zero until measured.
float gpu_ms_d3d12();

// ---------------------------------------------------------------------------------------------
// Vulkan.
// ---------------------------------------------------------------------------------------------

// Learns the device and looks for a queue to upload the weights on. Returns an NGX result code;
// FAIL_UnableToInitializeFeature means no queue is known, which is the documented decline.
int vk_init(void* instance, void* physical_device, void* device);
// Whether vk_init has succeeded, i.e. whether a create can be attempted at all.
bool vk_ready();

Feature* create_vk(void* cmd_buffer, void* params, unsigned int width, unsigned int height,
                   int preset, int ui_correction, const Controls6& controls, int* out_result);

// The resources are NVSDK_NGX_Resource_VK*; buffers are declined rather than reinterpreted.
int evaluate_vk(void* cmd_buffer, Feature* feature, void* params, const void* color,
                const void* depth, const void* motion, const void* output, unsigned int width,
                unsigned int height, const Rect& depth_rect, const Rect& motion_rect,
                int depth_inverted, int reset, const Controls6& controls, float mv_scale_x,
                float mv_scale_y);

void release_vk(Feature* feature);

}  // namespace nr::dlssnr
