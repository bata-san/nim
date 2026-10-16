#pragma once
#include "nim/core/binary.hpp"
#include "nim/core/tag_hash.hpp"

#include <format>
#include <span>
#include <stdexcept>

namespace nim {

// Reader for Tiger structured tags (type 8). Conventions (verified on Marathon data):
//   - offset 0: u64 total size
//   - Vec<T>:   u64 count, u64 rel_offset   (relative to the rel_offset field itself)
//               target: u64 count, u32 element class hash, u32 0; elements follow at +0x10
//   - Pointer:  u64 rel_offset (relative to the field itself)
class TagReader : public BinaryReader {
public:
    using BinaryReader::BinaryReader;

    struct Vec {
        uint64_t count = 0;
        size_t data = 0;          // absolute offset of element 0
        uint32_t element_class = 0;
    };

    Vec vec_at(size_t off) const {
        Vec v;
        v.count = u64_at(off);
        if (v.count == 0) return v;
        size_t header = off + 8 + size_t(u64_at(off + 8));
        if (u64_at(header) != v.count)
            throw std::runtime_error(std::format("Vec at 0x{:X}: header count mismatch", off));
        v.element_class = u32_at(header + 8);
        v.data = header + 0x10;
        return v;
    }

    size_t pointer_at(size_t off) const { return off + size_t(u64_at(off)); }

    TagHash tag_at(size_t off) const { return TagHash(u32_at(off)); }
};

} // namespace nim
