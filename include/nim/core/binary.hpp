#pragma once
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>

namespace nim {

// Bounds-checked little-endian reads over a byte span.
class BinaryReader {
public:
    explicit BinaryReader(std::span<const uint8_t> data) : data_(data) {}

    template <typename T>
    T read_at(size_t off) const {
        if (off + sizeof(T) > data_.size()) throw std::out_of_range("BinaryReader: read past end");
        T v;
        std::memcpy(&v, data_.data() + off, sizeof(T));
        return v;
    }
    uint8_t u8_at(size_t off) const { return read_at<uint8_t>(off); }
    uint16_t u16_at(size_t off) const { return read_at<uint16_t>(off); }
    uint32_t u32_at(size_t off) const { return read_at<uint32_t>(off); }
    uint64_t u64_at(size_t off) const { return read_at<uint64_t>(off); }
    float f32_at(size_t off) const { return read_at<float>(off); }

    template <typename Arr>
    void copy_at(size_t off, Arr& out) const {
        if (off + sizeof(out) > data_.size()) throw std::out_of_range("BinaryReader: copy past end");
        std::memcpy(out.data(), data_.data() + off, sizeof(out));
    }

    std::string cstr_at(size_t off) const {
        std::string s;
        while (off < data_.size() && data_[off]) s.push_back(char(data_[off++]));
        return s;
    }

    size_t size() const { return data_.size(); }
    std::span<const uint8_t> span() const { return data_; }

private:
    std::span<const uint8_t> data_;
};

} // namespace nim
