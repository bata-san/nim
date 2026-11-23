// Frame-time measurement for the viewer (docs/PROGRESS.md 3.111).
//
// `NIM_BENCH=FRAMES` runs nim.exe as an unattended benchmark: it loads the map given on the command
// line, places the camera, optionally starts the AI, lets the scene settle and then records FRAMES
// frames of CPU time per subsystem and GPU time per pass (timestamp queries), writes a report and
// quits. Nothing is clicked; the window only exists to own the swap chain.
//
//   NIM_BENCH=FRAMES            frames measured (0 / unset = off)
//   NIM_BENCH_EYE=x,y,z         camera position (default: the map's own framing)
//   NIM_BENCH_LOOK=x,y,z        camera target
//   NIM_BENCH_RADIUS=M          map radius in metres (0 = whole map, the GUI default)
//   NIM_BENCH_AI=1              start the AI simulation (estimated, 3.69) before measuring
//   NIM_BENCH_SETTLE=FRAMES     frames run before measuring (default 240)
//   NIM_BENCH_VSYNC=1           keep Present(1) (default: uncapped, to see the real cost)
//   NIM_BENCH_OUT=PATH          report file (default: nim_bench.txt next to the exe)
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace nimview {

class FrameProfile {
public:
    enum Cpu : uint8_t {
        Wait,         // swap chain waitable + frame fence
        Pending,      // apply_pending without the parts below
        Stream,       // map streaming uploads
        AiSim,        // AiSimulation::step
        AiActors,     // AiActors::update (poses, scene instances, sounds)
        Ui,           // ImGui frame (pages, overlay, media tick)
        Anim,         // live model animation
        Prepare,      // GameLighting::prepare_frame and the scene's channel values
        Audio,        // ambient + entity audio update
        SunShadow,    // sun shadow culling and recording
        LightShadow,  // local light shadow maps
        Rain,         // rain occlusion
        GBuffer,      // opaque geometry
        Lighting,     // lighting, transparents, FX, post
        Submit,       // ImGui draw data, close, execute
        Present,
        CpuCount
    };
    static constexpr const char* kCpuNames[CpuCount] = {
        "wait (swap chain + fence)", "pending (other)", "map streaming", "ai simulation", "ai actors", "ui (imgui)",
        "animation", "lighting prepare", "audio", "sun shadow", "light shadows", "rain occlusion", "gbuffer",
        "lighting + transparents + post", "submit", "present"};

    enum Gpu : uint8_t { GStart, GSunShadow, GLightShadow, GGBuffer, GLighting, GUi, GpuMarks };
    static constexpr const char* kGpuNames[GpuMarks - 1] = {"sun shadow", "light shadows + rain", "gbuffer",
                                                            "lighting + transparents + post", "ui + present copy"};
    static constexpr uint32_t kSlots = 2;  // Gpu::kFramesInFlight

    bool collecting = false;  // timings are kept only while true

    // ---- CPU ----
    using clock = std::chrono::steady_clock;
    class Scope {
    public:
        Scope(FrameProfile& p, Cpu c) : p_(p), c_(c), t0_(clock::now()) {}
        ~Scope() { p_.cur_[c_] += std::chrono::duration<double, std::milli>(clock::now() - t0_).count(); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        FrameProfile& p_;
        Cpu c_;
        clock::time_point t0_;
    };
    Scope scope(Cpu c) { return Scope(*this, c); }
    void subtract(Cpu c, double ms) { cur_[c] -= ms; }
    double current(Cpu c) const { return cur_[c]; }

    // ---- GPU ----
    void init(ID3D12Device* device) {
        D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kSlots * GpuMarks, 0};
        device->CreateQueryHeap(&qd, IID_PPV_ARGS(&heap_));
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = kSlots * GpuMarks * sizeof(uint64_t);
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(&readback_));
    }
    void set_frequency(uint64_t f) { frequency_ = f; }
    void set_list(ID3D12GraphicsCommandList* cl, uint32_t slot) { cl_ = cl, slot_ = slot; }
    void gpu_mark(Gpu m) {
        if (!collecting || !heap_ || !cl_) return;
        cl_->EndQuery(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot_ * GpuMarks + m);
        written_[slot_] |= 1u << m;
    }
    void resolve() {
        if (!collecting || !heap_ || !cl_) return;
        cl_->ResolveQueryData(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot_ * GpuMarks, GpuMarks, readback_.Get(),
                              slot_ * GpuMarks * sizeof(uint64_t));
        pending_[slot_] = written_[slot_] == (1u << GpuMarks) - 1;
        written_[slot_] = 0;
    }
    // After the slot's fence: the GPU times of the frame that last used it.
    void read_slot(uint32_t slot) {
        if (!pending_[slot] || !readback_) return;
        pending_[slot] = false;
        uint64_t* t = nullptr;
        const D3D12_RANGE r{slot * GpuMarks * sizeof(uint64_t), (slot + 1) * GpuMarks * sizeof(uint64_t)};
        if (FAILED(readback_->Map(0, &r, reinterpret_cast<void**>(&t)))) return;
        std::array<double, GpuMarks - 1> g{};
        const uint64_t* s = t + slot * GpuMarks;
        for (uint32_t i = 0; i + 1 < GpuMarks; ++i)
            g[i] = s[i + 1] >= s[i] ? double(s[i + 1] - s[i]) * 1000.0 / double(frequency_) : 0;
        const D3D12_RANGE none{0, 0};
        readback_->Unmap(0, &none);
        gpu_.push_back(g);
    }

    // ---- frames ----
    void begin_frame() { cur_ = {}, t_frame_ = clock::now(); }
    void end_frame() {
        if (!collecting) return;
        cpu_.push_back(cur_);
        if (have_prev_) interval_.push_back(std::chrono::duration<double, std::milli>(clock::now() - prev_end_).count());
        prev_end_ = clock::now(), have_prev_ = true;
    }
    size_t frames() const { return cpu_.size(); }

    std::string report() const {
        std::string out;
        auto pct = [](std::vector<double> v, double q) {
            if (v.empty()) return 0.0;
            std::ranges::sort(v);
            return v[size_t(double(v.size() - 1) * q)];
        };
        auto mean = [](const std::vector<double>& v) {
            double s = 0;
            for (double x : v) s += x;
            return v.empty() ? 0.0 : s / double(v.size());
        };
        std::vector<double> cpu_work, gpu_total;
        for (const auto& f : cpu_) {
            double s = 0;
            for (uint32_t i = 0; i < CpuCount; ++i)
                if (i != Wait && i != Present) s += f[i];
            cpu_work.push_back(s);
        }
        for (const auto& g : gpu_) {
            double s = 0;
            for (double x : g) s += x;
            gpu_total.push_back(s);
        }
        const size_t within = size_t(std::ranges::count_if(interval_, [](double x) { return x < 1000.0 / 60.0; }));
        out += std::format("frames {}: interval mean {:.2f} ms ({:.1f} fps), p50 {:.2f}, p95 {:.2f}, p99 {:.2f}, worst {:.2f}; "
                           "{} / {} under 16.67 ms\n",
                           cpu_.size(), mean(interval_), interval_.empty() ? 0.0 : 1000.0 / mean(interval_), pct(interval_, 0.5),
                           pct(interval_, 0.95), pct(interval_, 0.99), pct(interval_, 1.0), within, interval_.size());
        out += std::format("CPU work (without wait / present): mean {:.2f} ms, p95 {:.2f}, worst {:.2f}\n", mean(cpu_work),
                           pct(cpu_work, 0.95), pct(cpu_work, 1.0));
        out += std::format("GPU (timestamps): mean {:.2f} ms, p95 {:.2f}, worst {:.2f}\n", mean(gpu_total), pct(gpu_total, 0.95),
                           pct(gpu_total, 1.0));
        out += "CPU by subsystem (mean / p95 / worst ms):\n";
        for (uint32_t i = 0; i < CpuCount; ++i) {
            std::vector<double> v;
            for (const auto& f : cpu_) v.push_back(f[i]);
            out += std::format("  {:<32} {:7.3f} {:7.3f} {:7.3f}\n", kCpuNames[i], mean(v), pct(v, 0.95), pct(v, 1.0));
        }
        out += "GPU by pass (mean / p95 / worst ms):\n";
        for (uint32_t i = 0; i + 1 < GpuMarks; ++i) {
            std::vector<double> v;
            for (const auto& g : gpu_) v.push_back(g[i]);
            out += std::format("  {:<32} {:7.3f} {:7.3f} {:7.3f}\n", kGpuNames[i], mean(v), pct(v, 0.95), pct(v, 1.0));
        }
        return out;
    }

private:
    std::array<double, CpuCount> cur_{};
    std::vector<std::array<double, CpuCount>> cpu_;
    std::vector<std::array<double, GpuMarks - 1>> gpu_;
    std::vector<double> interval_;
    clock::time_point t_frame_{}, prev_end_{};
    bool have_prev_ = false;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
    uint64_t frequency_ = 1;
    ID3D12GraphicsCommandList* cl_ = nullptr;
    uint32_t slot_ = 0;
    std::array<uint32_t, kSlots> written_{};
    std::array<bool, kSlots> pending_{};
};

}  // namespace nimview
