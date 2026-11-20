#pragma once
#include "nim/core/package_manager.hpp"
#include "nim/core/tag_hash.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nim {

// Class hashes of Marathon's localized-string tags.
constexpr uint32_t kClassLocalizedStrings = 0x8080B9B8;  // container: string hashes + per-language data
constexpr uint32_t kClassStringData = 0x8080B9BA;        // one language's text
constexpr uint32_t kClassStringPart = 0x8080B9C0;

// Language slot order inside a container (same as Destiny 2).
enum class TextLanguage : uint8_t {
    English, Japanese, German, French, Spanish, SpanishLatAm, Italian,
    Korean, ChineseTraditional, ChineseSimplified, Portuguese, Polish, Russian,
    Count
};
constexpr size_t kTextLanguageCount = size_t(TextLanguage::Count);
std::string_view language_code(TextLanguage l);  // "en", "ja", ...

struct LocalizedString {
    uint32_t hash = 0;  // FNV-1 hash used by other tags to reference this string
    TagHash container;
    std::array<std::string, kTextLanguageCount> text;
};

// SStringPart (0x20 bytes):
//   0x00 u64  rel pointer to formatting data, or 0
//   0x08 u64  rel pointer to UTF-8 bytes
//   0x10 u32  FNV hash (0x811C9DC5 = none); for placeholders, what to substitute
//   0x14 u16  byte length, 0x16 u16 character length
//   0x18 u16  kind. Destiny tools call this "cipher_shift", but in Marathon it is a part kind
//             (high byte = kind, low byte = index), verified against the text around it:
//               0x0000       literal text
//               0x01nn       positional argument n          -> "{n}"      ("{0} Wins, {1} Losses")
//               0x0200       named variable (by hash)       -> "{var:H}"
//               0x0300       input-action glyph (by hash)   -> "{input:H}" ("Press {input:H} to ...")
//               0x0500       game value lookup (by hash)    -> "{value:H}" ("worth {value:H} credits")
constexpr uint16_t kStringPartText = 0;

struct StringDecodeStats {
    size_t parts = 0;
    size_t arguments = 0, variables = 0, input_glyphs = 0, values = 0;
    size_t unknown_kinds = 0;  // emitted as {kind_XXXX:H}
};

// Decodes one SStringData tag into strings (one per combination, in container order).
// Placeholders are kept inline as {input:HASH} / {kind_XXXX:HASH} so no text is silently dropped.
std::vector<std::string> decode_string_data(std::span<const uint8_t> tag, StringDecodeStats* stats = nullptr);

// Every localized string in the install, found by enumerating all container tags in all
// packages (not by following references, so unreferenced strings are included).
class StringTable {
public:
    struct Stats {
        size_t containers = 0;
        size_t containers_failed = 0;
        size_t strings = 0;
        size_t unique_hashes = 0;
        StringDecodeStats decode;
        size_t data_tags_total = 0;       // string-data tags present in the install
        size_t data_tags_referenced = 0;  // language slots resolved from containers
        std::vector<TagHash> orphan_data_tags;  // present but not reachable from any container
        std::array<size_t, kTextLanguageCount> non_empty{};
    };

    void load_all(PackageManager& pm);

    const std::vector<LocalizedString>& strings() const { return strings_; }
    const LocalizedString* find(uint32_t hash, TagHash container = {}) const;
    const Stats& stats() const { return stats_; }

private:
    std::vector<LocalizedString> strings_;
    std::unordered_map<uint32_t, size_t> by_hash_;
    std::unordered_map<TagHash, std::pair<size_t, size_t>> by_container_;
    Stats stats_;
};

} // namespace nim
