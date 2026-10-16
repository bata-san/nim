#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace nim {

// AES-128-GCM block cipher state for one package (keys are per-engine, nonce is per-package).
class PackageCipher {
public:
    PackageCipher(uint16_t pkg_id);
    ~PackageCipher();
    PackageCipher(const PackageCipher&) = delete;
    PackageCipher& operator=(const PackageCipher&) = delete;

    // Decrypts in place; returns false if the GCM tag does not authenticate.
    bool decrypt(bool alt_key, std::span<const uint8_t, 16> tag, std::span<uint8_t> data) const;

private:
    std::array<uint8_t, 12> nonce_{};
    void* alg_ = nullptr;
    void* key0_ = nullptr;
    void* key1_ = nullptr;
};

} // namespace nim
