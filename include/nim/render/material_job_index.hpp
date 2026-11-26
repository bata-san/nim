#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace nim {

// Pass-local lookup for deferred material jobs. The caller visits requests in draw-item
// order, so the first request owns the job and later users get that same index.
class MaterialJobIndex {
public:
    void clear() {
        size_ = 0;
        if (++epoch_ == 0) {
            for (auto& slot : slots_) slot.epoch = 0;
            epoch_ = 1;
        }
    }

    std::pair<size_t, bool> find_or_insert(uint64_t key, size_t job) {
        if (slots_.empty()) slots_.resize(64);
        const size_t hash = hash_key(key);
        Slot* slot = &lookup(slots_, key, hash);
        if (slot->epoch == epoch_) return {slot->job, false};
        // Keep one quarter of the power-of-two table empty, including across probe chains.
        if (size_ >= slots_.size() - slots_.size() / 4) {
            if (slots_.size() > slots_.max_size() / 2) throw std::length_error("material job index capacity");
            std::vector<Slot> next(slots_.size() * 2);
            for (const auto& old : slots_)
                if (old.epoch == epoch_) lookup(next, old.key, hash_key(old.key)) = old;
            slots_.swap(next);
            slot = &lookup(slots_, key, hash);
        }
        *slot = {key, job, epoch_};
        ++size_;
        return {job, true};
    }

private:
    struct Slot {
        uint64_t key = 0;
        size_t job = 0;
        uint32_t epoch = 0;
    };
    static size_t hash_key(uint64_t key) {
        // SplitMix64 finalizer: technique and object IDs often share many low bits.
        key = (key ^ (key >> 30)) * 0xbf58476d1ce4e5b9ull;
        key = (key ^ (key >> 27)) * 0x94d049bb133111ebull;
        return size_t(key ^ (key >> 31));
    }
    Slot& lookup(std::vector<Slot>& slots, uint64_t key, size_t hash) {
        const size_t mask = slots.size() - 1;
        auto pos = hash & mask;
        while (slots[pos].epoch == epoch_ && slots[pos].key != key)
            pos = (pos + 1) & mask;
        return slots[pos];
    }

    std::vector<Slot> slots_;
    size_t size_ = 0;
    uint32_t epoch_ = 1;
};

} // namespace nim
