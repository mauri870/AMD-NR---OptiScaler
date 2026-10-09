// Copyright (c) 2026 Mauri de Souza Meneguzzo (mauri870). MIT, see LICENSE.
// The inline transport: the host's D3D12 device is vkd3d-proton, so the network is recorded into the
// host's own command list through nr::dlssnr::evaluate_d3d12 (the model the NGX core drives) and runs
// in queue order with everything else in it. See nr_amdnr_transport.hpp.
//
// A job's output texture is written by the list RecordInputs recorded into and read as a shader
// resource by the host after RecordOutputs. It keeps the state its last user left it in; the next
// job moves it back before writing.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>

#include "nr_amdnr_transport.hpp"
#include "nr_pe_interop.hpp"
#include "nr_pe_log.hpp"

#include <algorithm>
#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

using nr::pe::log;

namespace nr::amdnr {
namespace {

constexpr D3D12_RESOURCE_STATES kSrv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr D3D12_RESOURCE_STATES kUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr DXGI_FORMAT kFp16 = DXGI_FORMAT_R16G16B16A16_FLOAT;
// OptiScaler parks a retired feature for 32 evaluates before releasing it, and the model layer's
// release_d3d12 relies on that (it never waits). What this transport retires waits as long.
constexpr uint64_t kParkFrames = 40;

template <class T> struct Releaser {
    void operator()(T* p) const { if (p) p->Release(); }
};
template <class T> using Owned = std::unique_ptr<T, Releaser<T>>;

struct Output {
    Owned<ID3D12Resource> resource;
    uint32_t width = 0, height = 0;
    D3D12_RESOURCE_STATES state = kUav;
    bool busy = false;
};

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after) {
    if (before == after) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
}

struct InlineJob final : Job {
    FrameRequest request;
    Output* output_slot = nullptr;
    nr::dlssnr::Feature* feature = nullptr;
    ID3D12Resource* output() const override { return output_slot->resource.get(); }
};

class InlineTransport final : public Transport {
  public:
    InlineTransport(ID3D12Device* device, ID3D12CommandQueue*) {
        device->AddRef();
        device_.reset(device);
    }

    ~InlineTransport() override {
        for (auto& entry : features_) nr::dlssnr::release_d3d12(entry.second);
        for (auto& r : retired_features_) nr::dlssnr::release_d3d12(r.first);
    }

    const char* name() const override { return "inline (vkd3d-proton)"; }

    Result prepare(const FrameRequest& request, std::unique_ptr<Job>* job, std::string* why) override {
        nr::dlssnr::Feature* feature = feature_for(request);
        if (!feature) {
            *why = std::string("could not create the model feature: ") + nr::dlssnr::last_error();
            return Result::Failed;
        }
        Output* output = output_for(request.width, request.height);
        if (!output) {
            *why = "could not create the output texture";
            return Result::Failed;
        }
        auto j = std::make_unique<InlineJob>();
        j->request = request;
        j->output_slot = output;
        j->feature = feature;
        output->busy = true;
        ++frames_;
        sweep();
        *job = std::move(j);
        return Result::Ok;
    }

    Result record_inputs(Job& base, ID3D12GraphicsCommandList* list, std::string* why) override {
        auto& job = static_cast<InlineJob&>(base);
        const FrameRequest& r = job.request;
        Output& out = *job.output_slot;
        // The model layer holds the colour and the motion in the shader-resource state and the output
        // in the unordered-access state across its call; the host's states are put to that and back.
        transition(list, out.resource.get(), out.state, kUav);
        transition(list, r.colour, r.colour_state, kSrv);
        if (r.motion) transition(list, r.motion, r.motion_state, kSrv);
        const nr::dlssnr::Rect none{};
        const nr::dlssnr::Rect motion_rect{0, 0, r.motion_width, r.motion_height};
        const int result = nr::dlssnr::evaluate_d3d12(list, job.feature, nullptr, r.colour, nullptr, r.motion,
                                                      out.resource.get(), r.width, r.height, none, motion_rect, 0,
                                                      r.reset || reset_pending_ ? 1 : 0, r.controls,
                                                      r.motion_scale_x, r.motion_scale_y);
        transition(list, r.colour, kSrv, r.colour_state);
        if (r.motion) transition(list, r.motion, kSrv, r.motion_state);
        out.state = kUav;
        if (result != int(NVSDK_NGX_Result_Success)) {
            // 0 is "bad arguments"; anything else is a latched fault. Neither fixes itself.
            *why = nr::dlssnr::last_error();
            return Result::Failed;
        }
        reset_pending_ = false;
        return Result::Ok;
    }

    // The network is in the list the host has just submitted.
    Result launch(Job&, ID3D12CommandQueue*, std::string*) override { return Result::Ok; }

    // The network is in a list that runs before any list the host records next.
    bool ready(Job&) override { return true; }

    Result record_outputs(Job& base, ID3D12GraphicsCommandList* list, std::string*) override {
        auto& job = static_cast<InlineJob&>(base);
        transition(list, job.output_slot->resource.get(), job.output_slot->state, kSrv);
        job.output_slot->state = kSrv;
        return Result::Ok;
    }

    void finish(std::unique_ptr<Job> base, bool) override {
        auto* job = static_cast<InlineJob*>(base.get());
        job->output_slot->busy = false;
    }

    void reset_history() override { reset_pending_ = true; }
    float gpu_ms() const override { return nr::dlssnr::gpu_ms_d3d12(); }
    void drain() override {}

  private:
    nr::dlssnr::Feature* feature_for(const FrameRequest& r) {
        const uint64_t key = (uint64_t(r.width) << 32) | r.height;
        auto it = features_.find(key);
        if (it != features_.end()) return it->second;
        // A new size is a new network and a new history; the sizes that went before are retired.
        for (auto& entry : features_) retired_features_.emplace_back(entry.second, frames_);
        features_.clear();
        reset_pending_ = true;
        int result = 0;
        nr::dlssnr::Feature* feature = nr::dlssnr::create_d3d12(device_.get(), r.list, nullptr, r.width, r.height, 0, 0,
                                                                r.controls, &result);
        if (feature) features_[key] = feature;
        return feature;
    }

    Output* output_for(uint32_t width, uint32_t height) {
        for (auto& o : outputs_)
            if (!o->busy && o->width == width && o->height == height) return o.get();
        // Idle outputs of other sizes are parked, not freed: a list that reads one may still be queued.
        for (auto it = outputs_.begin(); it != outputs_.end();) {
            if (!(*it)->busy && ((*it)->width != width || (*it)->height != height)) {
                retired_outputs_.emplace_back(std::move(*it), frames_);
                it = outputs_.erase(it);
            } else {
                ++it;
            }
        }
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = kFp16;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ID3D12Resource* resource = nullptr;
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, kUav, nullptr,
                                                    __uuidof(ID3D12Resource), reinterpret_cast<void**>(&resource))))
            return nullptr;
        auto output = std::make_unique<Output>();
        output->resource.reset(resource);
        output->width = width;
        output->height = height;
        output->state = kUav;
        outputs_.push_back(std::move(output));
        return outputs_.back().get();
    }

    void sweep() {
        while (!retired_outputs_.empty() && frames_ - retired_outputs_.front().second >= kParkFrames)
            retired_outputs_.pop_front();
        while (!retired_features_.empty() && frames_ - retired_features_.front().second >= kParkFrames) {
            nr::dlssnr::release_d3d12(retired_features_.front().first);
            retired_features_.pop_front();
        }
    }

    Owned<ID3D12Device> device_;
    std::map<uint64_t, nr::dlssnr::Feature*> features_;   // by width << 32 | height
    std::deque<std::pair<nr::dlssnr::Feature*, uint64_t>> retired_features_;
    std::vector<std::unique_ptr<Output>> outputs_;
    std::deque<std::pair<std::unique_ptr<Output>, uint64_t>> retired_outputs_;
    uint64_t frames_ = 0;
    bool reset_pending_ = true;   // the first frame has no history
};

}  // namespace

std::unique_ptr<Transport> make_inline_transport(ID3D12Device* device, ID3D12CommandQueue* queue, std::string* why) {
    if (!nr::pe::device_handles(device).valid()) {
        *why = "this D3D12 device exposes no Vulkan handles (the inline transport needs vkd3d-proton)";
        return nullptr;
    }
    return std::make_unique<InlineTransport>(device, queue);
}

}  // namespace nr::amdnr
