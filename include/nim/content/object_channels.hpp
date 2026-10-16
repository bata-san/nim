#pragma once
// Object channels: per-object values a material's TFX reads with push_object_channel (states such
// as frozen, inside a heat cascade, damage, dissolve ...). The bytecode names them by FNV-1 hash.

#include <cstdint>
#include <string_view>

namespace nim {

uint32_t fnv1(std::string_view s);
// A known name for the hash, or nullptr.
const char* object_channel_name(uint32_t hash);

}  // namespace nim
