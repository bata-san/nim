#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace nim {
// Per-package decoded blocks. A stripe serializes misses for the same block without
// holding the cache lock over I/O/decryption/decompression. Active readers own their bytes.
class DecodedBlockCache {
public:
    using Data = std::shared_ptr<const std::vector<uint8_t>>;
    explicit DecodedBlockCache(size_t capacity = 1024) : capacity_(capacity) {}
    template <class Decode>
    Data get(uint32_t index, Decode&& decode, bool* hit = nullptr) const {
        {
            std::shared_lock lock(mutex_);
            if (auto it = blocks_.find(index); it != blocks_.end()) {
                if (hit) *hit = true;
                return it->second;
            }
        }
        std::lock_guard stripe(stripes_[index % stripes_.size()]);
        uint64_t generation;
        {
            std::shared_lock lock(mutex_);
            if (auto it = blocks_.find(index); it != blocks_.end()) {
                if (hit) *hit = true;
                return it->second;
            }
            generation = generation_;
        }
        if (hit) *hit = false;
        auto data = std::make_shared<const std::vector<uint8_t>>(decode(index));
        std::unique_lock lock(mutex_);
        // A concurrent clear must not repopulate the cache with a stale in-flight decode.
        if (generation != generation_ || !capacity_) return data;
        if (blocks_.size() >= capacity_) blocks_.erase(blocks_.begin());
        blocks_.emplace(index, data);
        return data;
    }
    void clear() const {
        std::unique_lock lock(mutex_);
        ++generation_;
        blocks_.clear();
    }
private:
    const size_t capacity_;
    mutable std::shared_mutex mutex_;
    mutable std::array<std::mutex, 16> stripes_;
    mutable std::unordered_map<uint32_t, Data> blocks_;
    mutable uint64_t generation_ = 0;
};
} // namespace nim
