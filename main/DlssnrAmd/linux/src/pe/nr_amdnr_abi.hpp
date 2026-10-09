// Copyright (c) 2026 Mauri de Souza Meneguzzo (mauri870). MIT, see LICENSE.
#pragma once
// The C ABI that AMDNR's OptiScaler (https://github.com/3zwr1/AMD-NR---OptiScaler) calls a neural
// runtime through: one exported function, LmxxfNrGetApi, filling a table of functions. The layouts
// below are the host's, byte for byte (the static_asserts pin the sizes and the offsets a host reads),
// so a runtime built from this header loads in that host under the file name it asks for.
//
// A job is one frame's trip through the runtime, in this order:
//   PrepareFrame   decide what this frame needs, hand back the output resource
//   RecordInputs   record the producer's work into the game's command list
//   EnqueueHip[Async]   the list has been submitted
//   RecordOutputs  record the consumer's work into a later list
//   Retire         the job is done
// No STL, exceptions or CRT-allocated objects cross this boundary.

#include <cstddef>
#include <cstdint>

extern "C" {

enum NrAbiStatus : int32_t {
    NR_ABI_OK = 0,
    NR_ABI_UNSUPPORTED_ABI = 1,
    NR_ABI_INVALID_ARGUMENT = 2,
    NR_ABI_NOT_IMPLEMENTED = 3,
    NR_ABI_UNAVAILABLE = 4,
    NR_ABI_FAILED = 5,
};

enum NrAbiJobState : uint32_t {
    NR_ABI_JOB_NONE = 0,
    NR_ABI_JOB_PREPARED = 1,
    NR_ABI_JOB_PRODUCER_SUBMITTED = 2,
    NR_ABI_JOB_NR_ENQUEUED = 3,
    NR_ABI_JOB_NR_COMPLETE = 4,
    NR_ABI_JOB_CONSUMER_COMPLETE = 5,
    NR_ABI_JOB_RETIRED = 6,
};

#define NR_ABI_VERSION 1u

struct NrAbiCapabilities {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t max_input_width;
    uint32_t max_input_height;
    uint32_t history_supported;
    uint32_t overlap_supported;
    uint32_t graph_supported;
    uint32_t hip_ready;
    uint32_t gfx1201_target;
};

struct NrAbiCreateInfo {
    uint32_t struct_size;
    void* device;   // ID3D12Device*
    void* queue;    // ID3D12CommandQueue*
    const wchar_t* assets_directory;
    uint32_t flags;
};

#define NR_ABI_FRAME_STRENGTH (1u << 0)
#define NR_ABI_FRAME_DEBUG_VIEW (1u << 1)
#define NR_ABI_FRAME_CODEC_PASSTHROUGH (1u << 2)
#define NR_ABI_FRAME_TEMPORAL (1u << 3)
#define NR_ABI_FRAME_VULKAN_BRIDGE (1u << 4)
#define NR_ABI_FRAME_FULL_NETWORK (1u << 5)
#define NR_ABI_FRAME_CONTROLS (1u << 6)
#define NR_ABI_FRAME_KNOWN_FLAGS (NR_ABI_FRAME_STRENGTH | NR_ABI_FRAME_DEBUG_VIEW | NR_ABI_FRAME_CODEC_PASSTHROUGH | \
                                  NR_ABI_FRAME_TEMPORAL | NR_ABI_FRAME_VULKAN_BRIDGE | NR_ABI_FRAME_FULL_NETWORK | \
                                  NR_ABI_FRAME_CONTROLS)

// Sizes a host may send; the fields past the one it sends read as zero.
struct NrAbiFrameInfo {
    uint32_t struct_size;
    uint64_t session_id;
    uint64_t frame_id;
    uint64_t list_generation;
    void* command_list;  // ID3D12GraphicsCommandList*
    uint32_t color_width;
    uint32_t color_height;
    void* color;  // ID3D12Resource*
    uint32_t color_state;
    uint32_t flags;
    float transfer_strength;
    float color_strength;
    uint32_t debug_view;
    float model_scale;
    void* motion;  // ID3D12Resource*, two float channels
    uint32_t motion_state;
    uint32_t motion_width;
    uint32_t motion_height;
    float motion_scale_x;
    float motion_scale_y;
    uint32_t history_reset;
    uint32_t passes;
    float output_smooth;
    float output_smooth_threshold;
    float control_tone;
    float control_structure;
    float control_skin;
    float control_other;
    float control_style;
};
static_assert(sizeof(NrAbiFrameInfo) == 144, "the host's frame info is 144 bytes");
static_assert(offsetof(NrAbiFrameInfo, session_id) == 8 && offsetof(NrAbiFrameInfo, command_list) == 32 &&
              offsetof(NrAbiFrameInfo, color) == 48 && offsetof(NrAbiFrameInfo, flags) == 60 &&
              offsetof(NrAbiFrameInfo, motion) == 80 && offsetof(NrAbiFrameInfo, history_reset) == 108 &&
              offsetof(NrAbiFrameInfo, output_smooth) == 116 && offsetof(NrAbiFrameInfo, control_tone) == 124,
              "field offsets are the host's");

struct NrAbiJob {
    uint32_t struct_size;
    void* handle;
    void* private_output;  // ID3D12Resource*
};

struct NrAbiApi {
    uint32_t struct_size;
    uint32_t abi_version;
    int32_t (*QueryCapabilities)(NrAbiCapabilities* out);
    int32_t (*Create)(const NrAbiCreateInfo* info, void** context);
    int32_t (*Destroy)(void* context);
    int32_t (*PrepareSession)(void* context);
    int32_t (*PrepareFrame)(void* context, const NrAbiFrameInfo* info, NrAbiJob* job);
    int32_t (*RecordInputs)(void* context, void* job, void* command_list);
    int32_t (*EnqueueHip)(void* context, void* job, void* command_queue);
    int32_t (*RecordOutputs)(void* context, void* job, void* command_list);
    int32_t (*ExecuteAfterProducer)(void* context, void* job, void* command_queue);
    int32_t (*CancelUnsubmitted)(void* context, void* job);
    int32_t (*Poll)(void* context, void* job, uint32_t* state);
    int32_t (*Retire)(void* context, void* job);
    int32_t (*ResetHistory)(void* context);
    int32_t (*Drain)(void* context);
    int32_t (*GetStatus)(void* context, char* buffer, uint32_t buffer_chars);
    int32_t (*GetLastError)(char* buffer, uint32_t buffer_chars);
    int32_t (*EnqueueHipAsync)(void* context, void* job, void* command_queue);
    int32_t (*AbandonJob)(void* context, void* job);
    int32_t (*OutputReady)(void* context, void* job, uint32_t* ready);
};

struct NrAbiImportPoolStats {
    uint32_t struct_size;
    uint32_t enabled;
    uint32_t buffers;
    uint32_t busy;
    uint64_t bytes;
    uint64_t imports;
    uint64_t reuses;
};

}  // extern "C"
