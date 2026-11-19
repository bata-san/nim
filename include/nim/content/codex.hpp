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
//     808028E9  display      0x08 u32 title string hash
//     808028E3  body         0x2C u32 body string hash
//     808028E8  unlock       0x6C u32 unlock-hint string hash ("Complete objective to unlock.")
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
//   No texture is referenced from any of these structures, so the codex has no illustration
//   that nim can show (listed as unresolved in PROGRESS.md, not substituted).
constexpr uint32_t kClassCodexDefinitions = 0x808029D1;
constexpr uint32_t kClassCodexDefinition = 0x808029AA;
constexpr uint32_t kCodexKindCollection = 0x3049EB2C;
constexpr uint32_t kCodexKindEntry = 0x9F9F7DE7;
constexpr uint32_t kClassCodexDisplay = 0x808028E9;
constexpr uint32_t kClassCodexBody = 0x808028E3;
constexpr uint32_t kClassCodexUnlock = 0x808028E8;
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

struct CodexEntry {
    uint32_t hash = 0;
    uint32_t title = 0, body = 0, unlock = 0;  // string hashes (0 = none)
    uint32_t asset_key = 0;                    // item table key, 0 = no recording asset
    TagHash asset;                             // 8080BAAD record, if any
    TagHash dialogue_table;
    uint32_t conversation = 0;
    std::vector<size_t> lines;                 // indices into the dialogue list, in table order
};

struct CodexCollection {
    uint32_t hash = 0;
    uint32_t title = 0, unlock = 0;
    std::vector<CodexEntry> entries;
};

struct Codex {
    TagHash definitions;
    std::vector<CodexCollection> collections;  // in codex order; [0] holds entries before any collection
    size_t entries = 0;
    size_t with_asset = 0;      // entries whose item resolves to a recording asset
    size_t with_audio = 0;      // entries with at least one spoken line
};

// Builds the codex from the install. `dialogue` is the list from load_all_dialogue.
Codex load_codex(PackageManager& pm, const std::vector<DialogueLine>& dialogue);

} // namespace nim
