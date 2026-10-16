#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

namespace nim {

// Read-only memory mapping. Packages are read through mappings so that
// reading an entry costs a page fault, not a syscall + copy.
class MappedFile {
public:
    MappedFile() = default;
    explicit MappedFile(const std::filesystem::path& path);
    ~MappedFile();
    MappedFile(MappedFile&& o) noexcept;
    MappedFile& operator=(MappedFile&& o) noexcept;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    bool is_open() const { return data_ != nullptr; }
    std::span<const uint8_t> bytes() const { return {data_, size_}; }
    size_t size() const { return size_; }

private:
    void close();
    void* file_ = nullptr;
    void* mapping_ = nullptr;
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
};

} // namespace nim
