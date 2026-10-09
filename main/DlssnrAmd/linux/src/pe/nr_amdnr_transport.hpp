// Copyright (c) 2026 Mauri de Souza Meneguzzo (mauri870). MIT, see LICENSE.
#pragma once
// How a frame gets from the host's D3D12 device to the network and back, behind the runtime's C ABI
// (nr_amdnr_abi.hpp). The ABI front-end (nr_amdnr_runtime.cpp) validates calls and keeps each job's
// state; a transport does the work.
//
//   inline   the host's D3D12 device is vkd3d-proton, so the network is recorded straight into the
//            host's own command list and runs in it (nr_amdnr_inline.cpp)
//   native   the host's D3D12 device is the system's: the network runs on a Vulkan device of our own
//            on the same adapter, and frames cross in D3D12 buffers that Vulkan imports, ordered by a
//            D3D12 fence that Vulkan imports as a timeline semaphore (nr_amdnr_native.cpp)
//
// All calls arrive under the front-end's lock, one at a time.

#include "nr_dlssnr_model.hpp"

#include <d3d12.h>

#include <cstdint>
#include <memory>
#include <string>

namespace nr::amdnr {

// What PrepareFrame was told about one frame. The resources are the host's and stay valid until the
// job is finished.
struct FrameRequest {
    ID3D12GraphicsCommandList* list = nullptr;   // the list PrepareFrame was called with
    ID3D12Resource* colour = nullptr;
    D3D12_RESOURCE_STATES colour_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    uint32_t width = 0, height = 0;
    ID3D12Resource* motion = nullptr;            // null: no history this frame
    D3D12_RESOURCE_STATES motion_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    uint32_t motion_width = 0, motion_height = 0;
    float motion_scale_x = 1.0f, motion_scale_y = 1.0f;
    nr::dlssnr::Controls6 controls{};
    bool reset = false;
};

struct Job {
    virtual ~Job() = default;
    // The texture the host reads the answer from, once RecordOutputs has run: R16G16B16A16_FLOAT at
    // the colour's size, left in the non-pixel-shader-resource state.
    virtual ID3D12Resource* output() const = 0;
};

enum class Result { Ok, Unavailable, Failed };

class Transport {
  public:
    virtual ~Transport() = default;
    virtual const char* name() const = 0;

    virtual Result prepare(const FrameRequest&, std::unique_ptr<Job>* job, std::string* why) = 0;
    // Record the frame's inputs into the host's list.
    virtual Result record_inputs(Job&, ID3D12GraphicsCommandList*, std::string* why) = 0;
    // The list holding the inputs has been submitted on `queue`.
    virtual Result launch(Job&, ID3D12CommandQueue* queue, std::string* why) = 0;
    // Whether the answer is there without waiting.
    virtual bool ready(Job&) = 0;
    // Record the answer's hand-over into a later list. Anything that must be ordered on the queue
    // (a wait for the network) is issued here, before the list the caller goes on to submit.
    virtual Result record_outputs(Job&, ID3D12GraphicsCommandList*, std::string* why) = 0;
    // The job is over. `wait` makes a launched job's work complete on the CPU first (a job given up
    // before its answer was consumed).
    virtual void finish(std::unique_ptr<Job> job, bool wait) = 0;

    // The next frame has no usable history.
    virtual void reset_history() = 0;
    // Network time of the last frames in milliseconds, 0 when unknown.
    virtual float gpu_ms() const = 0;
    // Waits until nothing of the transport's is in flight.
    virtual void drain() = 0;
};

// Both return null with the reason in *why when the transport cannot run on this device.
std::unique_ptr<Transport> make_inline_transport(ID3D12Device*, ID3D12CommandQueue*, std::string* why);
std::unique_ptr<Transport> make_native_transport(ID3D12Device*, ID3D12CommandQueue*, std::string* why);

}  // namespace nr::amdnr
