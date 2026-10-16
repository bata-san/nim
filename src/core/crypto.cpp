#include "nim/core/crypto.hpp"

#include <stdexcept>
#include <windows.h>
#include <bcrypt.h>

namespace nim {
namespace {

// Tiger engine package keys (shared by Destiny 2 and Marathon; see tiger-pkg src/version.rs).
constexpr uint8_t kKey0[16] = {0xD6, 0x2A, 0xB2, 0xC1, 0x0C, 0xC0, 0x1B, 0xC5,
                               0x35, 0xDB, 0x7B, 0x86, 0x55, 0xC7, 0xDC, 0x3B};
constexpr uint8_t kKey1[16] = {0x3A, 0x4A, 0x5D, 0x36, 0x73, 0xA6, 0x60, 0x58,
                               0x7E, 0x63, 0xE6, 0x76, 0xE4, 0x08, 0x92, 0xB5};
constexpr uint8_t kNonceBase[12] = {0x84, 0xDF, 0x11, 0xC0, 0xAC, 0xAB, 0xFA, 0x20,
                                    0x33, 0x11, 0x26, 0x99};

void* make_key(void* alg, const uint8_t (&key)[16]) {
    BCRYPT_KEY_HANDLE h = nullptr;
    if (BCryptGenerateSymmetricKey(alg, &h, nullptr, 0, const_cast<PUCHAR>(key), 16, 0) != 0)
        throw std::runtime_error("BCryptGenerateSymmetricKey failed");
    return h;
}

} // namespace

PackageCipher::PackageCipher(uint16_t pkg_id) {
    for (int i = 0; i < 12; ++i) nonce_[i] = kNonceBase[i];
    nonce_[0] ^= uint8_t(pkg_id >> 8);
    nonce_[1] = 0xEA;
    nonce_[11] ^= uint8_t(pkg_id & 0xFF);

    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0)
        throw std::runtime_error("BCryptOpenAlgorithmProvider failed");
    BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                      sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
    alg_ = alg;
    key0_ = make_key(alg_, kKey0);
    key1_ = make_key(alg_, kKey1);
}

PackageCipher::~PackageCipher() {
    if (key0_) BCryptDestroyKey(key0_);
    if (key1_) BCryptDestroyKey(key1_);
    if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
}

bool PackageCipher::decrypt(bool alt_key, std::span<const uint8_t, 16> tag,
                            std::span<uint8_t> data) const {
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(nonce_.data());
    info.cbNonce = ULONG(nonce_.size());
    info.pbTag = const_cast<PUCHAR>(tag.data());
    info.cbTag = 16;
    ULONG out = 0;
    NTSTATUS st = BCryptDecrypt(alt_key ? key1_ : key0_, data.data(), ULONG(data.size()), &info,
                                nullptr, 0, data.data(), ULONG(data.size()), &out, 0);
    return st == 0;
}

} // namespace nim
