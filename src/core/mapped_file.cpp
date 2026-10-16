#include "nim/core/mapped_file.hpp"

#include <stdexcept>
#include <utility>
#include <windows.h>

namespace nim {

MappedFile::MappedFile(const std::filesystem::path& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (f == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open " + path.string());
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    file_ = f;
    size_ = size_t(sz.QuadPart);
    if (size_ == 0) return;
    mapping_ = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping_) { close(); throw std::runtime_error("cannot map " + path.string()); }
    data_ = static_cast<const uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
    if (!data_) { close(); throw std::runtime_error("cannot view " + path.string()); }
}

MappedFile::~MappedFile() { close(); }

MappedFile::MappedFile(MappedFile&& o) noexcept { *this = std::move(o); }

MappedFile& MappedFile::operator=(MappedFile&& o) noexcept {
    if (this != &o) {
        close();
        file_ = std::exchange(o.file_, nullptr);
        mapping_ = std::exchange(o.mapping_, nullptr);
        data_ = std::exchange(o.data_, nullptr);
        size_ = std::exchange(o.size_, 0);
    }
    return *this;
}

void MappedFile::close() {
    if (data_) UnmapViewOfFile(data_);
    if (mapping_) CloseHandle(mapping_);
    if (file_) CloseHandle(file_);
    data_ = nullptr; mapping_ = nullptr; file_ = nullptr; size_ = 0;
}

} // namespace nim
