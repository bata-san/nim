#pragma once
// NIM_SAMPLE=1: a minimal sampling profiler for nimshade's benchmark. A background thread
// suspends the measured thread every millisecond, records its instruction pointer and, at the
// end, prints the hottest functions (needs a PDB: build RelWithDebInfo).
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace nim {

class Sampler {
public:
    void start(HANDLE thread_to_sample) {
        target_ = thread_to_sample;
        running_ = true;
        worker_ = std::thread([this] {
            timeBeginPeriod(1);
            while (running_) {
                if (SuspendThread(target_) != DWORD(-1)) {
                    CONTEXT c{};
                    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                    uint64_t frames[32];
                    int depth = 0;
                    if (GetThreadContext(target_, &c)) {
                        // Unwind with the image's unwind data only: no allocation while the
                        // target (which may hold the heap lock) is suspended.
                        for (; depth < 32 && c.Rip; ++depth) {
                            frames[depth] = c.Rip;
                            DWORD64 base = 0;
                            auto* fn = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
                            if (!fn) break;
                            void* handler = nullptr;
                            DWORD64 establisher = 0;
                            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fn, &c, &handler, &establisher, nullptr);
                        }
                    }
                    ResumeThread(target_);
                    if (depth) stacks_.emplace_back(frames, frames + depth);
                    Sleep(1);
                    continue;
                }
                Sleep(1);
            }
            timeEndPeriod(1);
        });
    }
    void stop_and_print() {
        running_ = false;
        worker_.join();
        auto name = [](uint64_t a) {
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512]{};
            auto* s = reinterpret_cast<SYMBOL_INFO*>(buffer);
            s->SizeOfStruct = sizeof(SYMBOL_INFO);
            s->MaxNameLen = 500;
            DWORD64 off = 0;
            if (SymFromAddr(GetCurrentProcess(), a, &off, s)) return std::string(s->Name);
            return std::string("?");
        };
        std::map<std::string, size_t> self, total;
        size_t n = 0;
        // NIM_SAMPLE_FOCUS=name: only the stacks through that function.
        const char* focus = std::getenv("NIM_SAMPLE_FOCUS");
        for (const auto& st : stacks_) {
            if (st.empty()) continue;
            if (focus && std::none_of(st.begin(), st.end(), [&](uint64_t a) { return name(a) == focus; })) continue;
            ++n;
            ++self[name(st[0])];
            std::vector<std::string> seen;
            for (auto a : st) {
                auto f = name(a);
                if (std::find(seen.begin(), seen.end(), f) != seen.end()) continue;
                seen.push_back(f);
                ++total[f];
            }
        }
        auto print = [&](const char* what, const std::map<std::string, size_t>& m) {
            std::vector<std::pair<size_t, std::string>> v;
            for (auto& [k, c] : m) v.push_back({c, k});
            std::sort(v.rbegin(), v.rend());
            std::printf("sampler %s (%zu samples):\n", what, n);
            for (size_t i = 0; i < v.size() && i < 45; ++i)
                std::printf("  %5.1f%%  %s\n", 100.0 * double(v[i].first) / double(std::max<size_t>(n, 1)), v[i].second.c_str());
        };
        print("self", self);
        print("inclusive", total);
    }
    static void init_symbols() {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    }

private:
    HANDLE target_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::unordered_map<uint64_t, size_t> hits_;
    std::vector<std::vector<uint64_t>> stacks_;
};

} // namespace nim
