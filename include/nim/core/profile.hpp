#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace nim {

class Profile {
public:
    static Profile& instance() {
        static Profile p;
        return p;
    }

    bool enabled() {
        static const bool on = [] {
            char* value = nullptr;
            size_t size = 0;
            if (_dupenv_s(&value, &size, "NIM_PROFILE") != 0 || !value) return false;
            const bool enabled = value[0] == '1' && value[1] == '\0';
            std::free(value);
            return enabled;
        }();
        return on;
    }

    void add(const char* name, std::chrono::steady_clock::duration elapsed) {
        std::lock_guard lock(mutex_);
        auto& value = values_[name];
        value.first += std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
        ++value.second;
    }

    void count(const char* name, uint64_t n = 1) {
        if (!enabled()) return;
        std::lock_guard lock(mutex_);
        auto& value = counts_[name];
        value += n;
    }

    ~Profile() {
        if (!enabled()) return;
        std::lock_guard lock(mutex_);
        for (const auto& [name, value] : values_)
            std::fprintf(stderr, "NIM_PROFILE %s: %.3fs (%llu calls)\n", name.c_str(), value.first / 1e9,
                         static_cast<unsigned long long>(value.second));
        for (const auto& [name, value] : counts_)
            std::fprintf(stderr, "NIM_PROFILE %s: %llu\n", name.c_str(), static_cast<unsigned long long>(value));
    }

private:
    std::mutex mutex_;
    std::unordered_map<std::string, std::pair<int64_t, uint64_t>> values_;
    std::unordered_map<std::string, uint64_t> counts_;
};

class ProfileScope {
public:
    explicit ProfileScope(const char* name) : name_(name), active_(Profile::instance().enabled()), start_(active_ ? Clock::now() : Clock::time_point{}) {}
    ~ProfileScope() {
        stop();
    }
    void stop() {
        if (!active_) return;
        Profile::instance().add(name_, Clock::now() - start_);
        active_ = false;
    }

private:
    using Clock = std::chrono::steady_clock;
    const char* name_;
    bool active_;
    Clock::time_point start_;
};

} // namespace nim
