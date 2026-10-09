// Copyright (c) 2026 Mauri de Souza Meneguzzo (mauri870). MIT, see LICENSE.
// amdnr_leg_test <in.rgba8> <width> <height> <out.rgba8> [frames]
//
// The Vulkan half of the native transport (nr_amdnr_vkleg.cpp) on its own: plain host-visible buffers
// stand in for the imported D3D12 ones and the CPU stands in for the D3D12 fence, so it runs wherever
// Vulkan does. The dlssnr-amd/ folder (shaders and model) is expected beside the executable. Frames
// are paced so the network can finish building; the last answer is written to <out.rgba8> and the
// text printed goes to <out.rgba8>.log too (a Windows process under Proton has no terminal).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../src/pe/nr_amdnr_vkleg.hpp"
#include "../src/pe/nr_pe_log.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

extern "C" void nr_vk_load(void);

static uint16_t to_half(float f) {
    uint32_t u; std::memcpy(&u, &f, 4);
    if (f <= 0.0f) return 0;
    const int exponent = int((u >> 23) & 0xFF) - 127 + 15;
    if (exponent <= 0) {
        const uint32_t m = (u & 0x7FFFFFu) | 0x800000u;
        const int shift = 14 - exponent;
        if (shift > 24) return 0;
        uint32_t h = m >> shift, rest = m & ((1u << shift) - 1u), half = 1u << (shift - 1);
        if (rest > half || (rest == half && (h & 1u))) ++h;
        return uint16_t(h);
    }
    uint32_t h = (uint32_t(exponent) << 10) | ((u & 0x7FFFFFu) >> 13);
    const uint32_t rest = u & 0x1FFFu;
    if (rest > 0x1000u || (rest == 0x1000u && (h & 1u))) ++h;
    return uint16_t(h);
}
static float from_half(uint16_t h) {
    const int e = (h >> 10) & 0x1F, m = h & 0x3FF;
    if (e == 0) return std::ldexp(float(m), -24);
    return std::ldexp(float(m | 0x400), e - 25);
}

int main(int argc, char** argv) {
    if (argc < 5) return 2;
    const std::string log_path = std::string(argv[4]) + ".log";
    std::freopen(log_path.c_str(), "w", stdout);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const uint32_t width = uint32_t(atoi(argv[2])), height = uint32_t(atoi(argv[3]));
    const int frames = argc > 5 ? atoi(argv[5]) : 3;
    std::ifstream file(argv[1], std::ios::binary);
    const std::vector<uint8_t> input((std::istreambuf_iterator<char>(file)), {});
    if (input.size() != size_t(width) * height * 4) { std::printf("input is not %ux%u RGBA8\n", width, height); return 2; }

    nr_vk_load();
    nr::pe::set_module(GetModuleHandleA(nullptr));
    std::string why;
    nr::amdnr::VulkanLeg::Options options;
    auto leg = nr::amdnr::VulkanLeg::create(options, &why);
    if (!leg) { std::printf("leg create failed: %s\n", why.c_str()); return 1; }
    std::printf("leg on %s\n", leg->gpu_name().c_str());

    const uint64_t pitch = (uint64_t(width) * 8 + 255) / 256 * 256;
    nr::amdnr::LegBuffer colour_in, out, no_motion;
    if (!leg->make_buffer(pitch * height, nullptr, &colour_in, &why) || !leg->make_buffer(pitch * height, nullptr, &out, &why)) {
        std::printf("buffer failed: %s\n", why.c_str()); return 1;
    }
    for (uint32_t y = 0; y < height; ++y) {
        uint16_t* row = reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(colour_in.mapped) + y * pitch);
        for (uint32_t x = 0; x < width * 4; ++x) row[x] = to_half(float(input[size_t(y) * width * 4 + x]) / 255.0f);
    }
    nr::amdnr::LegExchange* exchange = leg->make_exchange(width, height, 0, 0, pitch, 0, pitch, colour_in, no_motion, out, &why);
    if (!exchange) { std::printf("exchange failed: %s\n", why.c_str()); return 1; }
    VkSemaphore timeline = leg->make_timeline(nullptr, &why);
    if (!timeline) { std::printf("timeline failed: %s\n", why.c_str()); return 1; }

    for (int k = 0; k < frames; ++k) {
        nr::amdnr::LegJob job;
        job.exchange = exchange;
        job.timeline = timeline;
        job.wait_value = uint64_t(2 * k);
        job.signal_value = uint64_t(2 * k + 1);
        job.reset = true;
        // The CPU is the D3D12 queue: its signal is what the job waits for.
        if (k > 0 && !leg->signal_timeline(timeline, job.wait_value)) { std::printf("signal failed\n"); return 1; }
        if (!leg->submit(job, &why)) { std::printf("frame %d submit failed: %s\n", k, why.c_str()); return 1; }
        if (!leg->wait_timeline(timeline, job.signal_value, 60'000'000'000ull)) { std::printf("frame %d: the timeline never reached %llu\n", k, (unsigned long long)job.signal_value); return 1; }
        std::printf("frame %d: network ran=%d, gpu_ms=%.2f, session: %s\n", k, leg->network_ran() ? 1 : 0, leg->gpu_ms(), leg->session_status().c_str());
        if (k < 3) Sleep(k == 0 ? 25000 : 5000);
    }
    std::vector<uint8_t> result(input.size());
    size_t changed = 0;
    double sq = 0;
    for (uint32_t y = 0; y < height; ++y) {
        const uint16_t* row = reinterpret_cast<const uint16_t*>(static_cast<uint8_t*>(out.mapped) + y * pitch);
        for (uint32_t x = 0; x < width * 4; ++x) {
            const float v = std::fmin(std::fmax(from_half(row[x]), 0.0f), 1.0f);
            const uint8_t b = uint8_t(std::lround(v * 255.0f));
            result[size_t(y) * width * 4 + x] = b;
            if ((x & 3) != 3) {
                const double d = double(b) - double(input[size_t(y) * width * 4 + x]);
                sq += d * d; changed += b != input[size_t(y) * width * 4 + x];
            }
        }
    }
    const double n = double(width) * height * 3;
    std::printf("output differs from the input in %.1f%% of channels, rms %.3f /255\n", 100.0 * double(changed) / n, std::sqrt(sq / n));
    FILE* f = std::fopen(argv[4], "wb");
    std::fwrite(result.data(), 1, result.size(), f);
    std::fclose(f);
    leg->wait_idle();
    leg->destroy_exchange(exchange);
    leg->destroy_timeline(timeline);
    leg->destroy_buffer(colour_in);
    leg->destroy_buffer(out);
    std::printf("done\n");
    return 0;
}
