// Copyright (c) 2026 Mauri de Souza Meneguzzo (mauri870). MIT, see LICENSE.
// The native transport: the host's D3D12 device is the system's, so the network runs on a Vulkan
// device of our own on the same adapter (nr_amdnr_vkleg.hpp) and frames cross in memory both APIs
// can see. See nr_amdnr_transport.hpp.
//
// Per frame size there are three D3D12 buffers created shared - the colour, the motion, the answer -
// each imported into Vulkan as a VkBuffer, and one D3D12 fence imported as a timeline semaphore.
//
//   RecordInputs   copies the host's colour (and motion) textures into the shared buffers, in the
//                  host's own list
//   launch         once that list is submitted: Vulkan is submitted first, waiting for value d on the
//                  timeline and signalling d+1 when the answer is in the output buffer; then the
//                  D3D12 queue signals d. (In that order a Vulkan submit that fails leaves no
//                  signal behind for a later wait to hang on.)
//   RecordOutputs  queues a wait for d+1 on the D3D12 queue and records the copy of the answer into
//                  the job's output texture in the host's list
//
// The values only ever go up: a job is launched after the previous job's answer was waited for, on
// the queue, or given up with a CPU wait (finish with wait).
//
// Nothing in this file can run under vkd3d-proton (it cannot share a buffer), so it is exercised
// only by the Vulkan half's own test and, for the sharing, on Windows.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include "nr_amdnr_transport.hpp"
#include "nr_amdnr_vkleg.hpp"
#include "nr_pe_log.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using nr::pe::log;

namespace nr::amdnr {
namespace {

constexpr D3D12_RESOURCE_STATES kSrv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr D3D12_RESOURCE_STATES kUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr DXGI_FORMAT kFp16 = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT kMotionFormat = DXGI_FORMAT_R16G16_FLOAT;
constexpr uint64_t kParkFrames = 40;
constexpr DWORD kWaitMs = 2000;   // a job given up waits this long for its answer

template <class T> struct Releaser {
    void operator()(T* p) const { if (p) p->Release(); }
};
template <class T> using Owned = std::unique_ptr<T, Releaser<T>>;

uint64_t align_up(uint64_t value, uint64_t alignment) { return (value + alignment - 1) / alignment * alignment; }

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

struct OutputTexture {
    Owned<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES state = kUav;
    bool busy = false;
};

// Everything one frame size needs on both sides.
struct Exchange {
    VulkanLeg* leg = nullptr;
    uint32_t width = 0, height = 0, motion_width = 0, motion_height = 0;
    uint64_t colour_pitch = 0, motion_pitch = 0;
    Owned<ID3D12Resource> colour_buffer, motion_buffer, out_buffer;
    LegBuffer colour_import, motion_import, out_import;
    LegExchange* leg_exchange = nullptr;
    std::vector<std::unique_ptr<OutputTexture>> outputs;
    ~Exchange() {
        if (!leg) return;
        leg->destroy_exchange(leg_exchange);
        leg->destroy_buffer(colour_import);
        leg->destroy_buffer(motion_import);
        leg->destroy_buffer(out_import);
    }
};

struct NativeJob final : Job {
    FrameRequest request;
    Exchange* exchange = nullptr;
    OutputTexture* output_slot = nullptr;
    uint64_t d3d_value = 0, vk_value = 0;
    bool launched = false;
    bool reset = false;
    ID3D12Resource* output() const override { return output_slot->resource.get(); }
};

class NativeTransport final : public Transport {
  public:
    ~NativeTransport() override {
        drain();
        exchanges_.clear();
        retired_.clear();
        if (leg_) leg_->destroy_timeline(timeline_);
        if (event_) CloseHandle(event_);
    }

    const char* name() const override { return "native (own Vulkan device, shared buffers)"; }

    bool init(ID3D12Device* device, ID3D12CommandQueue* queue, std::string* why) {
        device->AddRef();
        device_.reset(device);
        queue->AddRef();
        queue_.reset(queue);
        const LUID luid = device->GetAdapterLuid();
        uint8_t bytes[8];
        std::memcpy(bytes, &luid, sizeof bytes);   // LowPart, then HighPart: Vulkan's deviceLUID layout
        VulkanLeg::Options options;
        options.luid = bytes;
        options.external_win32 = true;
        leg_ = VulkanLeg::create(options, why);
        if (!leg_) return false;

        ID3D12Fence* fence = nullptr;
        HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence));
        if (FAILED(hr)) {
            *why = "could not create a shared D3D12 fence (" + hex(hr) + ")";
            return false;
        }
        fence_.reset(fence);
        HANDLE handle = nullptr;
        hr = device->CreateSharedHandle(fence, nullptr, GENERIC_ALL, nullptr, &handle);
        if (FAILED(hr)) {
            *why = "could not share the D3D12 fence (" + hex(hr) + ")";
            return false;
        }
        timeline_ = leg_->make_timeline(handle, why);
        CloseHandle(handle);
        if (!timeline_) return false;

        // Prove the buffer import works on this driver before any frame depends on it.
        Owned<ID3D12Resource> probe;
        LegBuffer imported;
        if (!shared_buffer(1 << 16, &probe, &imported, why)) return false;
        leg_->destroy_buffer(imported);
        event_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        log("[amdnr] native transport ready on %s: D3D12 fence and buffer import checked", leg_->gpu_name().c_str());
        return true;
    }

    Result prepare(const FrameRequest& r, std::unique_ptr<Job>* job, std::string* why) override {
        if (r.colour->GetDesc().Format != kFp16) {
            *why = "the colour is not R16G16B16A16_FLOAT";
            return Result::Failed;
        }
        if (r.motion && r.motion->GetDesc().Format != kMotionFormat) {
            *why = "the motion is not R16G16_FLOAT";
            return Result::Failed;
        }
        const uint32_t mw = r.motion ? r.motion_width : 0, mh = r.motion ? r.motion_height : 0;
        const Key key{r.width, r.height, mw, mh};
        Exchange* exchange = nullptr;
        auto it = exchanges_.find(key);
        if (it != exchanges_.end()) {
            exchange = it->second.get();
        } else {
            // A new size is a new network and a new history; the sizes that went before are retired.
            for (auto& entry : exchanges_) retired_.emplace_back(std::move(entry.second), frames_);
            exchanges_.clear();
            auto made = make_exchange(r, why);
            if (!made) return Result::Unavailable;
            exchange = made.get();
            exchanges_[key] = std::move(made);
            reset_pending_ = true;
        }
        OutputTexture* slot = nullptr;
        for (auto& o : exchange->outputs)
            if (!o->busy) { slot = o.get(); break; }
        if (!slot) {
            auto made = make_output(r.width, r.height);
            if (!made) {
                *why = "could not create the output texture";
                return Result::Failed;
            }
            slot = made.get();
            exchange->outputs.push_back(std::move(made));
        }
        auto j = std::make_unique<NativeJob>();
        j->request = r;
        j->exchange = exchange;
        j->output_slot = slot;
        slot->busy = true;
        ++frames_;
        while (!retired_.empty() && frames_ - retired_.front().second >= kParkFrames) retired_.pop_front();
        *job = std::move(j);
        return Result::Ok;
    }

    Result record_inputs(Job& base, ID3D12GraphicsCommandList* list, std::string*) override {
        auto& job = static_cast<NativeJob&>(base);
        const FrameRequest& r = job.request;
        Exchange& x = *job.exchange;
        copy_to_buffer(list, r.colour, r.colour_state, x.colour_buffer.get(), kFp16, r.width, r.height, x.colour_pitch);
        if (r.motion)
            copy_to_buffer(list, r.motion, r.motion_state, x.motion_buffer.get(), kMotionFormat, r.motion_width,
                           r.motion_height, x.motion_pitch);
        job.reset = r.reset || reset_pending_;
        reset_pending_ = false;
        return Result::Ok;
    }

    Result launch(Job& base, ID3D12CommandQueue* queue, std::string* why) override {
        auto& job = static_cast<NativeJob&>(base);
        job.d3d_value = ++counter_;
        job.vk_value = ++counter_;
        LegJob lj;
        lj.exchange = job.exchange->leg_exchange;
        lj.timeline = timeline_;
        lj.wait_value = job.d3d_value;
        lj.signal_value = job.vk_value;
        lj.use_motion = job.request.motion != nullptr;
        lj.reset = job.reset;
        lj.controls = job.request.controls;
        lj.motion_scale_x = job.request.motion_scale_x;
        lj.motion_scale_y = job.request.motion_scale_y;
        if (!leg_->submit(lj, why)) {
            counter_ -= 2;
            return Result::Failed;
        }
        const HRESULT hr = queue->Signal(fence_.get(), job.d3d_value);
        if (FAILED(hr)) {
            // Vulkan is waiting for a value that will not come: the device is lost or going.
            *why = "ID3D12CommandQueue::Signal failed (" + hex(hr) + ")";
            return Result::Failed;
        }
        queue->AddRef();
        launch_queue_.reset(queue);
        job.launched = true;
        last_value_ = job.vk_value;
        return Result::Ok;
    }

    bool ready(Job& base) override {
        auto& job = static_cast<NativeJob&>(base);
        return !job.launched || fence_->GetCompletedValue() >= job.vk_value;
    }

    Result record_outputs(Job& base, ID3D12GraphicsCommandList* list, std::string* why) override {
        auto& job = static_cast<NativeJob&>(base);
        if (!job.launched || !launch_queue_) {
            *why = "the job was not launched";
            return Result::Failed;
        }
        // The queue waits for the answer before the list this goes into runs.
        const HRESULT hr = launch_queue_->Wait(fence_.get(), job.vk_value);
        if (FAILED(hr)) {
            *why = "ID3D12CommandQueue::Wait failed (" + hex(hr) + ")";
            return Result::Failed;
        }
        Exchange& x = *job.exchange;
        OutputTexture& out = *job.output_slot;
        transition(list, out.resource.get(), out.state, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = out.resource.get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = x.out_buffer.get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint = {kFp16, x.width, x.height, 1, UINT(x.colour_pitch)};
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(list, out.resource.get(), D3D12_RESOURCE_STATE_COPY_DEST, kSrv);
        out.state = kSrv;
        return Result::Ok;
    }

    void finish(std::unique_ptr<Job> base, bool wait) override {
        auto* job = static_cast<NativeJob*>(base.get());
        if (wait && job->launched) wait_for(job->vk_value);
        job->output_slot->busy = false;
    }

    void reset_history() override { reset_pending_ = true; }
    float gpu_ms() const override { return leg_ ? leg_->gpu_ms() : 0.0f; }

    void drain() override {
        if (leg_ && last_value_) wait_for(last_value_);
        if (leg_) leg_->wait_idle();
    }

  private:
    struct Key {
        uint32_t w, h, mw, mh;
        bool operator<(const Key& o) const {
            return std::tie(w, h, mw, mh) < std::tie(o.w, o.h, o.mw, o.mh);
        }
    };

    static std::string hex(HRESULT hr) {
        char text[16];
        std::snprintf(text, sizeof text, "0x%08lX", static_cast<unsigned long>(hr));
        return text;
    }

    void wait_for(uint64_t value) {
        if (fence_->GetCompletedValue() >= value) return;
        if (SUCCEEDED(fence_->SetEventOnCompletion(value, event_)) && WaitForSingleObject(event_, kWaitMs) != WAIT_OBJECT_0)
            log("[amdnr] the network's answer for fence value %llu did not arrive within %lu ms", (unsigned long long)value,
                static_cast<unsigned long>(kWaitMs));
    }

    // A shared D3D12 buffer and its import into Vulkan.
    bool shared_buffer(uint64_t bytes, Owned<ID3D12Resource>* resource, LegBuffer* imported, std::string* why) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* made = nullptr;
        HRESULT hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON,
                                                      nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&made));
        if (FAILED(hr)) {
            *why = "could not create a shared D3D12 buffer of " + std::to_string(bytes) + " bytes (" + hex(hr) + ")";
            return false;
        }
        resource->reset(made);
        HANDLE handle = nullptr;
        hr = device_->CreateSharedHandle(made, nullptr, GENERIC_ALL, nullptr, &handle);
        if (FAILED(hr)) {
            *why = "could not share a D3D12 buffer (" + hex(hr) + ")";
            return false;
        }
        const bool ok = leg_->make_buffer(bytes, handle, imported, why);
        CloseHandle(handle);
        return ok;
    }

    std::unique_ptr<Exchange> make_exchange(const FrameRequest& r, std::string* why) {
        auto x = std::make_unique<Exchange>();
        x->leg = leg_.get();
        x->width = r.width;
        x->height = r.height;
        x->motion_width = r.motion ? r.motion_width : 0;
        x->motion_height = r.motion ? r.motion_height : 0;
        // CopyTextureRegion into a buffer wants rows a multiple of 256 bytes apart.
        x->colour_pitch = align_up(uint64_t(r.width) * 8, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
        x->motion_pitch = r.motion ? align_up(uint64_t(r.motion_width) * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) : 0;
        if (!shared_buffer(x->colour_pitch * r.height, &x->colour_buffer, &x->colour_import, why)) return nullptr;
        if (r.motion && !shared_buffer(x->motion_pitch * r.motion_height, &x->motion_buffer, &x->motion_import, why))
            return nullptr;
        if (!shared_buffer(x->colour_pitch * r.height, &x->out_buffer, &x->out_import, why)) return nullptr;
        x->leg_exchange = leg_->make_exchange(r.width, r.height, x->motion_width, x->motion_height, x->colour_pitch,
                                              x->motion_pitch, x->colour_pitch, x->colour_import, x->motion_import,
                                              x->out_import, why);
        if (!x->leg_exchange) return nullptr;
        log("[amdnr] native exchange for %ux%u%s", r.width, r.height, r.motion ? " with motion" : "");
        return x;
    }

    std::unique_ptr<OutputTexture> make_output(uint32_t width, uint32_t height) {
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
        auto out = std::make_unique<OutputTexture>();
        out->resource.reset(resource);
        out->state = kUav;
        return out;
    }

    // texture -> buffer, in the host's list; the buffer is in COMMON between lists, from which the
    // copy promotes it and to which it decays.
    static void copy_to_buffer(ID3D12GraphicsCommandList* list, ID3D12Resource* texture, D3D12_RESOURCE_STATES state,
                               ID3D12Resource* buffer, DXGI_FORMAT format, uint32_t width, uint32_t height, uint64_t pitch) {
        transition(list, texture, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = buffer;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = {format, width, height, 1, UINT(pitch)};
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = texture;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(list, texture, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    }

    Owned<ID3D12Device> device_;
    Owned<ID3D12CommandQueue> queue_, launch_queue_;
    std::unique_ptr<VulkanLeg> leg_;
    Owned<ID3D12Fence> fence_;
    VkSemaphore timeline_{};
    HANDLE event_ = nullptr;
    std::map<Key, std::unique_ptr<Exchange>> exchanges_;
    std::deque<std::pair<std::unique_ptr<Exchange>, uint64_t>> retired_;
    uint64_t counter_ = 0, last_value_ = 0, frames_ = 0;
    bool reset_pending_ = true;
};

}  // namespace

std::unique_ptr<Transport> make_native_transport(ID3D12Device* device, ID3D12CommandQueue* queue, std::string* why) {
    auto transport = std::make_unique<NativeTransport>();
    if (!transport->init(device, queue, why)) return nullptr;
    return transport;
}

}  // namespace nr::amdnr
