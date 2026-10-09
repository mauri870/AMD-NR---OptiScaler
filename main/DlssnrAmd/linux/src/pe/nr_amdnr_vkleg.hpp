// Copyright (c) 2026 Mauri de Souza Meneguzzo (mauri870). MIT, see LICENSE.
#pragma once
// The Vulkan half of the native transport (nr_amdnr_native.cpp): a Vulkan device of our own on a
// given adapter, buffers that are D3D12 resources imported through their shared handles, a timeline
// semaphore that is a D3D12 fence imported the same way, and one function that turns a frame sitting
// in those buffers into the network's answer sitting in another.
//
// Nothing here knows about D3D12 beyond "an NT handle of a shared resource" and "an NT handle of a
// shared fence". With no handle, buffers and the semaphore are plain Vulkan objects, which is how the
// leg is tested away from a D3D12 device (linux/test/amdnr_leg_test.cpp).
//
// Frame layout in the buffers, for a frame of width x height:
//   colour_in   R16G16B16A16_FLOAT rows, colour_pitch bytes apart
//   motion_in   R16G16_FLOAT rows, motion_pitch bytes apart (optional)
//   out         R16G16B16A16_FLOAT rows, out_pitch bytes apart
// Pitches are whatever the D3D12 side's copies need (multiples of 256) and are passed in.

#include "nr_dlssnr_model.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>

namespace nr::pe { class Session; }

namespace nr::amdnr {

struct LegBuffer {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    VkDeviceSize size{};
    void* mapped{};        // the buffers the leg made itself (no shared handle)
    bool external{};       // imported from a D3D12 resource
};

struct LegExchange;   // the images and buffers of one frame size

struct LegJob {
    LegExchange* exchange = nullptr;
    VkSemaphore timeline{};
    uint64_t wait_value = 0, signal_value = 0;
    bool use_motion = false;
    bool reset = false;
    nr::dlssnr::Controls6 controls{};
    float motion_scale_x = 1.0f, motion_scale_y = 1.0f;   // the host's: motion times this is pixels
};

class VulkanLeg {
  public:
    struct Options {
        const uint8_t* luid = nullptr;   // 8 bytes: the adapter to run on; null picks an AMD GPU
        bool external_win32 = false;     // enable the extensions that import D3D12 handles
    };
    static std::unique_ptr<VulkanLeg> create(const Options&, std::string* why);
    ~VulkanLeg();
    VulkanLeg(const VulkanLeg&) = delete;
    VulkanLeg& operator=(const VulkanLeg&) = delete;

    const std::string& gpu_name() const;

    // `shared` is the NT handle of a D3D12 resource created with D3D12_HEAP_FLAG_SHARED to import, or
    // null for a plain host-visible buffer. The handle may be closed once this returns.
    bool make_buffer(VkDeviceSize size, void* shared, LegBuffer* out, std::string* why);
    void destroy_buffer(LegBuffer& buffer);
    // `shared` is the NT handle of a D3D12 fence to import as the timeline, or null for a local one.
    VkSemaphore make_timeline(void* shared, std::string* why);
    void destroy_timeline(VkSemaphore semaphore);
    bool signal_timeline(VkSemaphore semaphore, uint64_t value);
    bool wait_timeline(VkSemaphore semaphore, uint64_t value, uint64_t timeout_ns);

    // An exchange for one frame size over buffers the caller keeps. motion_in may be empty (no history
    // at this size). Null with the reason in *why when it could not be made; destroy_exchange frees it.
    LegExchange* make_exchange(uint32_t width, uint32_t height, uint32_t motion_width,
                                               uint32_t motion_height, VkDeviceSize colour_pitch,
                                               VkDeviceSize motion_pitch, VkDeviceSize out_pitch,
                                               const LegBuffer& colour_in, const LegBuffer& motion_in,
                                               const LegBuffer& out, std::string* why);
    void destroy_exchange(LegExchange* exchange);

    // Record the frame's trip into a command buffer and submit it: wait on the timeline for
    // wait_value, copy the buffers into images, run the network (or, while it is still building,
    // copy the colour straight to the output), copy the answer to the output buffer, signal the
    // timeline with signal_value. Returns false when Vulkan refused.
    bool submit(const LegJob&, std::string* why);
    // Whether the last submit ran the network (false: the output is a copy of the colour).
    bool network_ran() const;

    void wait_idle();
    float gpu_ms() const;
    bool session_failed() const;
    std::string session_status() const;

  private:
    VulkanLeg();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nr::amdnr
