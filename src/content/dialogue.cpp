#include "nim/content/dialogue.hpp"
#include "nim/core/tag_reader.hpp"

#include <cstring>

namespace nim {

std::vector<DialogueLine> load_all_dialogue(PackageManager& pm, DialogueStats* stats) {
    DialogueStats local;
    DialogueStats& st = stats ? *stats : local;
    st = {};
    pm.build_hash64_index();

    std::vector<DialogueLine> lines;
    for (const auto& info : pm.packages()) {
        Package& pkg = pm.open(info);
        for (uint32_t i = 0; i < pkg.entries().size(); ++i) {
            const Entry& e = pkg.entries()[i];
            if (e.type != 8 || e.file_size < 0x40) continue;
            st.tags_scanned++;
            std::vector<uint8_t> bytes;
            try { bytes = pkg.read_entry(i); } catch (...) { continue; }
            TagReader r(bytes);
            const TagHash table = TagHash::make(info.pkg_id, uint16_t(i));
            bool is_table = false;

            // Array headers are {u64 count, u32 class, u32 0}, 16-byte aligned within the tag.
            for (size_t h = 0x10; h + 0x10 <= bytes.size(); h += 0x10) {
                if (r.u32_at(h + 8) != kClassDialogueEntry || r.u32_at(h + 12) != 0) continue;
                uint64_t count = r.u64_at(h);
                if (count == 0 || h + 0x10 + count * 0x28 > bytes.size()) continue;
                is_table = true;
                for (uint64_t k = 0; k < count; ++k) {
                    size_t entry = h + 0x10 + size_t(k) * 0x28;
                    st.entries++;
                    if (r.u64_at(entry + 0x20) == 0) { st.other_variants++; continue; }
                    size_t target = r.pointer_at(entry + 0x20);
                    if (target < 4 || target + 0xA0 > bytes.size() || r.u32_at(target - 4) != kClassDialogueLine) {
                        st.other_variants++;
                        continue;
                    }
                    DialogueLine line;
                    line.table = table;
                    line.line_id = r.u32_at(target + 0x00);
                    line.conversation = r.u32_at(target + 0x08);
                    line.audio_event64 = r.u64_at(target + 0x28);
                    line.subtitle_hash = r.u32_at(target + 0x40);
                    line.caption_hash = r.u32_at(target + 0x78);
                    line.speaker_hash = r.u32_at(target + 0x94);
                    if (auto ev = pm.resolve64(line.audio_event64)) {
                        line.audio_event = *ev;
                        st.audio_resolved++;
                        try {
                            auto evb = pm.read(*ev);
                            TagReader er(evb);
                            auto streams = er.vec_at(0x20);
                            for (uint64_t s = 0; s < streams.count; ++s)
                                line.wem.push_back(er.tag_at(streams.data + size_t(s) * 4));
                        } catch (...) {
                        }
                    }
                    lines.push_back(std::move(line));
                    st.lines++;
                }
            }
            if (is_table) st.tables++;
        }
    }
    return lines;
}

} // namespace nim
