#pragma once
#include "nim/content/strings.hpp"
#include "nim/core/package_manager.hpp"

#include <cstdint>
#include <vector>

namespace nim {

// Dialogue layout (verified on Marathon data):
//   Tables (class 8080B4D1, and others) hold Vec<S8080AA3B> (0x28 bytes, variant pointer at 0x20).
//   The variant target of class 8080AA3F (0xA0 bytes) is one spoken line:
//     0x00 u32   line id                      0x08 u32 conversation key
//     0x28 u64   TagHash64 of the audio event (class 8080B128)
//     0x30 Tag   string container (subtitle)      0x40 u32 string hash
//     0x48 f32   unknown (2.5 typical)
//     0x68 Tag   string container (caption)       0x78 u32 string hash
//     0x94 u32   speaker name string hash
//   Audio event (class 8080B128): 0x08 u32 event id, 0x14 Tag sound bank, 0x20 Vec<Tag> WEM streams.
constexpr uint32_t kClassDialogueEntry = 0x8080AA3B;
constexpr uint32_t kClassDialogueLine = 0x8080AA3F;
constexpr uint32_t kClassAudioEvent = 0x8080B128;

struct DialogueLine {
    TagHash table;      // tag that holds the line
    uint32_t line_id = 0;
    uint32_t conversation = 0;      // 0x08: key shared by the lines of one conversation
    uint32_t speaker_hash = 0;
    uint32_t subtitle_hash = 0;
    uint32_t caption_hash = 0;
    uint64_t audio_event64 = 0;
    TagHash audio_event;            // resolved through the hash64 table, if present
    std::vector<TagHash> wem;       // audio streams of the event
};

struct DialogueStats {
    size_t tags_scanned = 0;
    size_t tables = 0;
    size_t entries = 0;
    size_t lines = 0;
    size_t other_variants = 0;      // entries whose variant is not a spoken line
    size_t audio_resolved = 0;
};

// Scans every structured tag in the install for dialogue entry arrays.
std::vector<DialogueLine> load_all_dialogue(PackageManager& pm, DialogueStats* stats = nullptr);

} // namespace nim
