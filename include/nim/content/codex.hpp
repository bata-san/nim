#pragma once
#include "nim/content/dialogue.hpp"
#include "nim/core/package_manager.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace nim {

// The in-game codex (intel archive), verified on Marathon data:
//
//   Definition table (class 808029D1, one tag): Vec<808029AA> at 0x08, elements 0x18 bytes
//     0x00 u32 definition hash   0x04 u32 kind   0x08 Vec<pointer> components
//   Each component pointer targets an object preceded by its u32 class id:
//     808028E9  display      0x00 u32 image index, 0x04 u32 string list index, 0x08 u32 title hash
//     808028E3  body         0x28 u32 string list index, 0x2C u32 body string hash
//     808028E8  unlock       0x00 Vec<808023C7 {u32 string list index, u32 hint hash}>
//                            ("Complete objective to unlock.")
//   String hashes are only unique inside their string container: 911 hashes in the install name
//   different texts in different containers (e.g. one hash is "Acceptance Notice" in 80A4111D and
//   "Mentoring Intelligence" in 80A51DCA). Every string reference therefore carries an index into
//   the investment string list (class 808071C8, one tag): Vec at 0x08 of 0x18-byte elements
//   {u32 key, 0, -1, 0, u64 hash64 of the 8080B9B8 container}. 0xFFFF = no string.
//   Kinds: 3049EB2C collection (a codex folder), 9F9F7DE7 entry (one intel record).
//   The table is stored in codex order: a collection is followed by its entries.
//
//   Recordings: item table (class 80806CAC) Vec at 0x08 of 0x48-byte elements
//     {u32 item hash = definition hash, u32 0, u32 asset key}. Asset index tags (class 8080B61C)
//     Vec at 0x08 of 0x18-byte elements {u64 key, u64 -1, u64 hash64 of the asset (8080BAAD)}.
//     The 8080BAAD asset holds its component tag (8080BADB) at 0xC0; that component names a
//     dialogue table by hash64 and the conversation key of the recording. The spoken lines are
//     the dialogue lines of that table whose conversation key matches.
//
//   Illustrations: the display component's u32 at 0x00 indexes the image list (class 808071C0,
//     one tag) Vec at 0x08 of 0x20-byte elements {u32 name hash, ..., 0x10 tag of an 80805335}.
//     0xFFFF = no image. The 80805335 entry {0x10 u32 name string hash, 0x18 tag of an 80805350};
//     the 80805350 nests Vec at 0x20 -> Vec -> texture header tags, largest first. Example: the
//     record "DIRE MARSH: ALGAE PONDS" has index 0xDF -> 80A664E0 ("Algae Ponds") -> 80A664DF
//     -> 880x910 RGBA8 texture 80A664DE, the green point-cloud art.
constexpr uint32_t kClassCodexDefinitions = 0x808029D1;
constexpr uint32_t kClassCodexDefinition = 0x808029AA;
constexpr uint32_t kCodexKindCollection = 0x3049EB2C;
constexpr uint32_t kCodexKindEntry = 0x9F9F7DE7;
constexpr uint32_t kClassCodexDisplay = 0x808028E9;
constexpr uint32_t kClassCodexBody = 0x808028E3;
constexpr uint32_t kClassCodexUnlock = 0x808028E8;
constexpr uint32_t kClassCodexImageList = 0x808071C0;
constexpr uint32_t kClassInvestmentStrings = 0x808071C8;
constexpr uint32_t kClassItemTable = 0x80806CAC;
constexpr uint32_t kClassAssetIndex = 0x8080B61C;
constexpr uint32_t kClassAssetRecord = 0x8080BAAD;
constexpr uint32_t kClassDialogueTable = 0x8080B4D1;

struct CodexDefinition {
    uint32_t hash = 0;
    uint32_t kind = 0;
    struct Component {
        uint32_t cls = 0;
        size_t offset = 0;  // absolute offset of the object (after its class id)
    };
    std::vector<Component> components;
};

// Parses the definition table tag. Throws on a malformed table.
std::vector<CodexDefinition> parse_codex_definitions(std::span<const uint8_t> tag);

// A localized string reference: the hash is only meaningful inside its container.
struct CodexString {
    uint32_t hash = 0;  // 0 = none
    TagHash container;  // the string container (8080B9B8); none when the list index did not resolve
    explicit operator bool() const { return hash != 0; }
};

struct CodexEntry {
    uint32_t hash = 0;
    CodexString title, body, unlock;
    TagHash illustration;                      // texture header (type 32), none if the record has no image
    uint32_t image_index = 0xFFFF;             // display component +0x00, index into the image list
    uint32_t asset_key = 0;                    // item table key, 0 = no recording asset
    TagHash asset;                             // 8080BAAD record, if any
    TagHash dialogue_table;
    uint32_t conversation = 0;
    std::vector<size_t> lines;                 // indices into the dialogue list, in table order
};

struct CodexCollection {
    uint32_t hash = 0;
    CodexString title, unlock;
    TagHash illustration;
    uint32_t image_index = 0xFFFF;
    std::vector<CodexEntry> entries;
};

struct Codex {
    TagHash definitions;
    std::vector<CodexCollection> collections;  // in codex order; [0] holds entries before any collection
    size_t entries = 0;
    size_t with_asset = 0;      // entries whose item resolves to a recording asset
    size_t with_audio = 0;      // entries with at least one spoken line
    size_t with_illustration = 0;  // entries whose display component resolves to a texture
};

// Builds the codex from the install. `dialogue` is the list from load_all_dialogue.
Codex load_codex(PackageManager& pm, const std::vector<DialogueLine>& dialogue);

} // namespace nim
