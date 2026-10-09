// Copyright (c) 2026 Mauri de Souza Meneguzzo (mauri870). MIT, see LICENSE.
// amdnr_abi_test <DlssnrAmdRuntime.dll> <in.rgba8> <width> <height> <out.rgba8> [frames [temporal [reference.rgba8 [settle seconds]]]]
//
// Drives the runtime DLL through the C ABI the way AMDNR's OptiScaler does (LmxxfBackend.cpp), on a
// D3D12 device: an FP16 copy of the frame is the "fed" texture, a job is prepared and its inputs
// recorded into a command list, the list is executed, the job is launched, and the answer is read
// back from the job's output texture after RecordOutputs. With a reference (NVIDIA's output for the same
// frame, 8-bit RGBA) it prints the PSNR of the answer against it, and of the input against it. Frame k's answer is consumed on frame k+1's
// list, as the host does. The last frame's answer is written to <out.rgba8> (8-bit RGBA).
//
// Not a game and not the host: it checks that the DLL honours the ABI and that its picture is the
// network's (compare out.rgba8 with NVIDIA's output for the same frame).
//
// The text this prints goes to <out.rgba8>.log as well, because a Windows process under Proton has
// no terminal to print to.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include "../src/pe/nr_amdnr_abi.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { std::printf("FAIL %s hr=0x%08lx line %d\n", #x, (unsigned long)hr_, __LINE__); return 1; } } while (0)
#define CHECKH(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { std::printf("FAIL %s hr=0x%08lx line %d\n", #x, (unsigned long)hr_, __LINE__); return hr_; } } while (0)
#define ABI(x) do { int32_t rc_ = (x); if (rc_ != NR_ABI_OK) { char e_[512] = {}; api.GetLastError(e_, sizeof e_); std::printf("FAIL %s rc=%d (%s) line %d\n", #x, (int)rc_, e_, __LINE__); return 1; } } while (0)

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

static UINT64 align(UINT64 v, UINT64 a) { return (v + a - 1) / a * a; }

int main(int argc, char** argv) {
    if (argc < 6) return 2;
    const std::string log_path = std::string(argv[5]) + ".log";
    std::freopen(log_path.c_str(), "w", stdout);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const UINT width = UINT(atoi(argv[3])), height = UINT(atoi(argv[4]));
    const int frames = argc > 6 ? atoi(argv[6]) : 1;
    const bool temporal = argc > 7 && atoi(argv[7]) != 0;
    // How long to give the network to build after the first frame (it compiles its pipelines on a
    // thread of its own, a minute on a cold cache with the Windows driver).
    const int settle_seconds = argc > 9 ? atoi(argv[9]) : 25;
    std::ifstream file(argv[2], std::ios::binary);
    const std::vector<uint8_t> input((std::istreambuf_iterator<char>(file)), {});
    if (input.size() != size_t(width) * height * 4) { std::printf("input is not %ux%u RGBA8\n", width, height); return 2; }

    HMODULE dll = LoadLibraryA(argv[1]);
    if (!dll) { std::printf("cannot load %s (%lu)\n", argv[1], GetLastError()); return 1; }
    auto get_api = reinterpret_cast<int32_t (*)(uint32_t, NrAbiApi*)>(GetProcAddress(dll, "LmxxfNrGetApi"));
    if (!get_api) { std::printf("no LmxxfNrGetApi\n"); return 1; }
    NrAbiApi api{};
    api.struct_size = sizeof api;
    if (get_api(NR_ABI_VERSION, &api) != NR_ABI_OK || !api.OutputReady || !api.AbandonJob || !api.EnqueueHipAsync) {
        std::printf("LmxxfNrGetApi failed\n"); return 1;
    }

    // ---- D3D12 ----
    IDXGIFactory4* factory{};
    CHECK(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&factory));
    // The adapter with the most video memory that is not the software one: the discrete GPU.
    IDXGIAdapter1* adapter{};
    {
        SIZE_T best = 0;
        for (UINT i = 0;; ++i) {
            IDXGIAdapter1* a{};
            if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            std::printf("adapter %u: %ls (%u MB)%s\n", i, d.Description, unsigned(d.DedicatedVideoMemory >> 20),
                        (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " software" : "");
            if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.DedicatedVideoMemory >= best) {
                if (adapter) adapter->Release();
                adapter = a;
                best = d.DedicatedVideoMemory;
            } else {
                a->Release();
            }
        }
        if (!adapter) { std::printf("no adapter\n"); return 1; }
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        std::printf("using %ls\n", d.Description);
    }
    ID3D12Device* dev{};
    CHECK(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), (void**)&dev));
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue{};
    CHECK(dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&queue));
    ID3D12CommandAllocator* alloc{};
    CHECK(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&alloc));
    ID3D12GraphicsCommandList* list{};
    CHECK(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&list));
    CHECK(list->Close());   // created recording; every use below starts with a Reset
    ID3D12Fence* fence{};
    CHECK(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&fence));
    HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    UINT64 fence_value = 0;
    auto run_list = [&]() -> HRESULT {
        CHECKH(list->Close());
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        CHECKH(queue->Signal(fence, ++fence_value));
        return S_OK;
    };
    auto wait = [&]() -> HRESULT {
        CHECKH(fence->SetEventOnCompletion(fence_value, event));
        if (WaitForSingleObject(event, 120000) != WAIT_OBJECT_0) { std::printf("FAIL: GPU wait timed out\n"); return E_FAIL; }
        return S_OK;
    };
    auto reset_list = [&]() -> HRESULT {
        CHECKH(alloc->Reset());
        CHECKH(list->Reset(alloc, nullptr));
        return S_OK;
    };

    D3D12_HEAP_PROPERTIES defaultHeap{}; defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES uploadHeap{}; uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES readbackHeap{}; readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    auto texture = [&](DXGI_FORMAT format, ID3D12Resource** out) {
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = width; d.Height = height;
        d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = format; d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return dev->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            __uuidof(ID3D12Resource), (void**)out);
    };
    auto buffer = [&](const D3D12_HEAP_PROPERTIES& heap, UINT64 bytes, D3D12_RESOURCE_STATES state, ID3D12Resource** out) {
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = bytes; d.Height = 1;
        d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, __uuidof(ID3D12Resource), (void**)out);
    };
    auto barrier = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER x{}; x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; x.Transition.pResource = r;
        x.Transition.StateBefore = a; x.Transition.StateAfter = b; x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &x);
    };

    const UINT rowBytesFp16 = width * 8, pitchFp16 = UINT(align(rowBytesFp16, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
    const UINT rowBytesMotion = width * 4, pitchMotion = UINT(align(rowBytesMotion, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
    ID3D12Resource *fed{}, *motion{}, *upload{}, *motionUpload{}, *readback{};
    CHECK(texture(DXGI_FORMAT_R16G16B16A16_FLOAT, &fed));
    CHECK(texture(DXGI_FORMAT_R16G16_FLOAT, &motion));
    CHECK(buffer(uploadHeap, UINT64(pitchFp16) * height, D3D12_RESOURCE_STATE_GENERIC_READ, &upload));
    CHECK(buffer(uploadHeap, UINT64(pitchMotion) * height, D3D12_RESOURCE_STATE_GENERIC_READ, &motionUpload));
    CHECK(buffer(readbackHeap, UINT64(pitchFp16) * height, D3D12_RESOURCE_STATE_COPY_DEST, &readback));
    {
        uint8_t* p{}; CHECK(upload->Map(0, nullptr, (void**)&p));
        for (UINT y = 0; y < height; ++y) {
            uint16_t* row = reinterpret_cast<uint16_t*>(p + size_t(y) * pitchFp16);
            for (UINT x = 0; x < width * 4; ++x) row[x] = to_half(float(input[size_t(y) * width * 4 + x]) / 255.0f);
        }
        upload->Unmap(0, nullptr);
        uint8_t* m{}; CHECK(motionUpload->Map(0, nullptr, (void**)&m));
        std::memset(m, 0, size_t(pitchMotion) * height);
        motionUpload->Unmap(0, nullptr);
    }
    auto copy_to_texture = [&](ID3D12Resource* tex, ID3D12Resource* src, DXGI_FORMAT format, UINT pitch) {
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION s{}; s.pResource = src; s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        s.PlacedFootprint.Footprint = {format, width, height, 1, pitch};
        list->CopyTextureRegion(&dst, 0, 0, 0, &s, nullptr);
    };
    CHECK(reset_list());
    copy_to_texture(fed, upload, DXGI_FORMAT_R16G16B16A16_FLOAT, pitchFp16);
    copy_to_texture(motion, motionUpload, DXGI_FORMAT_R16G16_FLOAT, pitchMotion);
    barrier(fed, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(motion, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    CHECK(run_list()); CHECK(wait());

    // ---- the runtime ----
    NrAbiCapabilities caps{}; caps.struct_size = sizeof caps;
    ABI(api.QueryCapabilities(&caps));
    std::printf("capabilities: abi %u, max %ux%u, history %u\n", caps.abi_version, caps.max_input_width, caps.max_input_height, caps.history_supported);
    NrAbiCreateInfo ci{}; ci.struct_size = sizeof ci; ci.device = dev; ci.queue = queue; ci.assets_directory = L"";
    void* ctx{};
    int32_t rc = api.Create(&ci, &ctx);
    if (rc != NR_ABI_OK) { char e[512] = {}; api.GetLastError(e, sizeof e); std::printf("Create failed rc=%d: %s\n", (int)rc, e); return 1; }
    ABI(api.PrepareSession(ctx));

    NrAbiJob previous{};
    bool have_previous = false;
    ID3D12Resource* last_output = nullptr;
    int answered = 0;
    // The network builds in the background for the first seconds (a minute on a cold cache): until it
    // has, the answer is the colour itself. Frames are paced so that it can finish.
    for (int k = 0; k < frames; ++k) {
        CHECK(reset_list());
        if (have_previous) {
            ABI(api.RecordOutputs(ctx, previous.handle, list));
            last_output = static_cast<ID3D12Resource*>(previous.private_output);
            ++answered;
        }
        NrAbiFrameInfo fi{};
        fi.struct_size = 128;
        fi.session_id = 1; fi.frame_id = k + 1; fi.list_generation = k + 1;
        fi.command_list = list;
        fi.color_width = width; fi.color_height = height;
        fi.color = fed; fi.color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        fi.flags = NR_ABI_FRAME_STRENGTH | (temporal ? NR_ABI_FRAME_TEMPORAL : 0u);
        fi.transfer_strength = 1.0f; fi.color_strength = 1.0f; fi.model_scale = 1.0f;
        fi.motion = motion; fi.motion_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        fi.motion_width = width; fi.motion_height = height; fi.motion_scale_x = 1.0f; fi.motion_scale_y = 1.0f;
        fi.history_reset = k == 0 ? 1u : 0u;
        fi.passes = 1;
        NrAbiJob job{}; job.struct_size = sizeof job;
        ABI(api.PrepareFrame(ctx, &fi, &job));
        ABI(api.RecordInputs(ctx, job.handle, list));
        CHECK(run_list());
        ABI(api.EnqueueHipAsync(ctx, job.handle, queue));
        uint32_t ready = 0;
        ABI(api.OutputReady(ctx, job.handle, &ready));
        CHECK(wait());
        if (have_previous) ABI(api.Retire(ctx, previous.handle));
        previous = job; have_previous = true;
        char status[512] = {};
        api.GetStatus(ctx, status, sizeof status);
        std::printf("frame %d: ready=%u status: %s\n", k, ready, status);
        if (k < 3) Sleep(k == 0 ? DWORD(settle_seconds) * 1000 : 5000);   // let the background build run on
    }
    // The last answer: consume it and read it back.
    CHECK(reset_list());
    ABI(api.RecordOutputs(ctx, previous.handle, list));
    ID3D12Resource* out = static_cast<ID3D12Resource*>(previous.private_output);
    barrier(out, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    {
        D3D12_TEXTURE_COPY_LOCATION s{}; s.pResource = out; s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION d{}; d.pResource = readback; d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        d.PlacedFootprint.Footprint = {DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, 1, pitchFp16};
        list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
    }
    CHECK(run_list()); CHECK(wait());
    ABI(api.Retire(ctx, previous.handle));
    (void)last_output;

    uint8_t* p{}; CHECK(readback->Map(0, nullptr, (void**)&p));
    std::vector<uint8_t> result(input.size());
    double sq_in = 0, sq_out = 0; size_t changed = 0;
    for (UINT y = 0; y < height; ++y) {
        const uint16_t* row = reinterpret_cast<const uint16_t*>(p + size_t(y) * pitchFp16);
        for (UINT x = 0; x < width * 4; ++x) {
            const float v = std::fmin(std::fmax(from_half(row[x]), 0.0f), 1.0f);
            const uint8_t b = uint8_t(std::lround(v * 255.0f));
            result[size_t(y) * width * 4 + x] = b;
            if ((x & 3) != 3) {
                const double d = double(b) - double(input[size_t(y) * width * 4 + x]);
                sq_out += d * d; if (b != input[size_t(y) * width * 4 + x]) ++changed;
            }
        }
    }
    readback->Unmap(0, nullptr);
    const double n = double(width) * height * 3;
    if (argc > 8) {
        std::ifstream ref_file(argv[8], std::ios::binary);
        const std::vector<uint8_t> ref((std::istreambuf_iterator<char>(ref_file)), {});
        if (ref.size() == input.size()) {
            double sq_answer = 0, sq_input = 0;
            for (size_t i = 0; i < ref.size(); ++i) {
                if ((i & 3) == 3) continue;
                const double a = double(result[i]) - ref[i], b = double(input[i]) - ref[i];
                sq_answer += a * a; sq_input += b * b;
            }
            std::printf("PSNR against the reference: answer %.2f dB, unprocessed input %.2f dB (a working network scores about 45 dB; the unprocessed input about 26)\n",
                        10.0 * std::log10(255.0 * 255.0 / std::fmax(sq_answer / n, 1e-12)),
                        10.0 * std::log10(255.0 * 255.0 / std::fmax(sq_input / n, 1e-12)));
        } else {
            std::printf("reference has the wrong size\n");
        }
    }
    std::printf("answered %d, output differs from the input in %.1f%% of channels, rms difference %.3f /255\n", answered + 1,
                100.0 * double(changed) / n, std::sqrt(sq_out / n));
    (void)sq_in;
    FILE* f = std::fopen(argv[5], "wb");
    std::fwrite(result.data(), 1, result.size(), f);
    std::fclose(f);
    ABI(api.Drain(ctx));
    ABI(api.Destroy(ctx));
    std::printf("done\n");
    return 0;
}
