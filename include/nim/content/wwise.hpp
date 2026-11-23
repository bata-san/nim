#pragma once
#include "nim/content/audio.hpp"
#include "nim/core/package_manager.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nim {

// Wwise SoundBanks (docs/PROGRESS.md 3.90). Every audio event tag (8080B128) has its own bank
// (package entry 26/6, reference = bank id) one or two entries before it. Marathon's banks are
// version 150 (BKHD +0x08); the layout read here is that version's (wwiser's CAkBankMgr readers):
//   BKHD; DIDX {u32 source id, u32 offset, u32 size} into DATA (embedded media and the prefetch
//   start of streamed files); HIRC {u32 count; {u8 type, u32 size, u32 id, body}}.
// The event tag: +0x08 u32 play event id, +0x0C u32 stop event id (0: none), +0x14 Tag 8080B12E
// {u32 4, f32 max distance, f32 longest stream s}, +0x20 Vec<Tag> streams (26/7, reference =
// Wwise source id).
struct WwiseCurvePoint {
    float x = 0, y = 0;
    uint32_t interp = 4;  // AkCurveInterpolation: 0 Log3 1 Sine 2 Log1 3 InvSCurve 4 Linear 5 SCurve 6 Exp1 7 SineRecip 8 Exp3 9 Constant
};
struct WwiseCurve {
    uint8_t scaling = 0;  // 0 none, 2 dB (values are linear gain - 1), 3 log, 4 dB to linear
    std::vector<WwiseCurvePoint> points;
    float eval(float x) const;
};

// Property ids (AkPropID, bank version 150) nim uses.
namespace wwise_prop {
constexpr uint8_t Volume = 0x00, Pitch = 0x01, LPF = 0x02, HPF = 0x03, MakeUpGain = 0x05, OutputBusVolume = 0x0D,
                  PlaybackSpeed = 0x21, InitialDelay = 0x22, DelayTime = 0x39, TransitionTime = 0x3A,
                  Probability = 0x3B, Loop = 0x54, AttenuationID = 0x55;
}

struct WwiseProps {
    std::vector<std::pair<uint8_t, uint32_t>> values;            // raw AkPropValue (float or int)
    std::vector<std::tuple<uint8_t, float, float>> ranges;       // randomizer (min, max) added to the value
    bool has(uint8_t id) const;
    float f(uint8_t id, float fallback = 0) const;
    uint32_t u(uint8_t id, uint32_t fallback = 0) const;
    std::pair<float, float> range(uint8_t id) const;
};

// The node kinds nim plays (HIRC types 2 Sound, 5 Random/Sequence, 6 Switch, 7 Actor-Mixer, 9 Layer).
struct WwiseNode {
    uint8_t type = 0;
    uint32_t id = 0, parent = 0, bus = 0;
    WwiseProps props;
    bool positioning_override = false, spatial = false, attenuation_on = false;
    std::vector<uint32_t> children;
    // Sound
    uint32_t plugin = 0, source = 0, media_size = 0;
    uint8_t stream_type = 0;  // 0 embedded in the bank, 1 prefetch + stream, 2 stream
    // Random/Sequence container
    uint16_t loop_count = 1, avoid_repeat = 0;
    float transition_ms = 0;
    uint8_t transition_mode = 0, random_mode = 0, mode = 0;  // mode 0 random, 1 sequence
    bool continuous = false, reset_playlist = false, using_weight = false;
    std::vector<std::pair<uint32_t, int32_t>> playlist;  // node, weight
    // Switch container
    uint8_t group_type = 0;  // 0 switch, 1 state
    uint32_t group = 0, default_switch = 0;
    std::vector<std::pair<uint32_t, std::vector<uint32_t>>> switches;
};

struct WwiseAction {
    uint32_t id = 0;
    uint16_t type = 0;  // 0x0403 play, 0x01xx stop, 0x1Exx seek, ... (AkActionType)
    uint32_t target = 0;
    bool bus = false;
    WwiseProps props;
    uint8_t fade_curve = 4;
    uint32_t bank = 0;  // play: the bank that holds the target
    bool seek_relative = false;
    float seek = 0, seek_min = 0, seek_max = 0;  // seek: value + random [min, max]
};

struct WwiseAttenuation {
    uint32_t id = 0;
    std::vector<int8_t> curve_to_use;  // per AkAttenuationCurveType; [0] = dry volume
    std::vector<WwiseCurve> curves;
    // Linear gain of the dry path at `distance` m (1 without a volume curve).
    float gain(float distance) const;
    float max_distance() const;  // last point of the volume curve (0 without one)
};

struct WwiseBank {
    uint32_t version = 0, id = 0;
    std::unordered_map<uint32_t, WwiseNode> nodes;
    std::unordered_map<uint32_t, WwiseAction> actions;
    std::unordered_map<uint32_t, std::vector<uint32_t>> events;  // event id -> action ids
    std::unordered_map<uint32_t, WwiseAttenuation> attenuations;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> media;  // DIDX source -> (offset, size) in data
    std::vector<uint8_t> data;
    size_t other_objects = 0;  // HIRC objects of kinds nim does not read
};
// Throws std::runtime_error for other versions or malformed objects.
WwiseBank parse_wwise_bank(std::span<const uint8_t> bytes);

// One sound an event starts.
struct WwiseVoice {
    uint32_t sound = 0, source = 0;
    uint8_t stream_type = 0;
    float delay = 0;        // s before it starts (action delay + initial delays)
    float volume_db = 0;    // Volume + MakeUpGain + OutputBusVolume (with randomizers) up to the bank's root
    float pitch_cents = 0;  // Pitch (with randomizers) up the hierarchy
    uint32_t loops = 1;     // plays of the sound; 0 = endless
    float fade_in = 0;      // s (the play action's transition time)
    float seek = 0;         // start point as a fraction of the duration (seek actions, relative)
    uint32_t attenuation = 0;  // the attenuation in effect; 0 = none
    bool spatial = false;      // 3D positioned (else 2D, centred)
    int after = -1;         // continuous containers: starts when voice `after` ends
    float overlap = 0;      // s it starts before voice `after` ends (cross-fade transitions)
    uint32_t repeat = 0;    // endless continuous container: when this voice ends, play the container again
};

// Container memory between plays (sequence positions, recently played children).
struct WwisePlayState {
    std::unordered_map<uint32_t, size_t> sequence;
    std::unordered_map<uint32_t, std::vector<uint32_t>> recent;
    uint32_t rng = 0x9E3779B9u;
    float unit();  // uniform [0, 1)
};

// The voices event `event_id`'s play actions start (others are ignored). Switch containers take
// their default switch (推定: the game's switch values are not known).
std::vector<WwiseVoice> wwise_play_event(const WwiseBank& bank, uint32_t event_id, WwisePlayState& state);
// The voices node `node` starts (an endless continuous container's next round).
std::vector<WwiseVoice> wwise_play_node(const WwiseBank& bank, uint32_t node, WwisePlayState& state);
// The nodes event `event_id`'s stop actions name.
std::vector<uint32_t> wwise_stop_targets(const WwiseBank& bank, uint32_t event_id);
// Whether `node` is `ancestor` or below it.
bool wwise_is_under(const WwiseBank& bank, uint32_t node, uint32_t ancestor);

// An audio event tag with its bank, ready to play.
struct WwiseEventData {
    TagHash tag;
    uint32_t play_id = 0, stop_id = 0;
    float max_distance = 0, longest = 0;  // the tag's 8080B12E (0 when absent)
    std::shared_ptr<const WwiseBank> bank;
    std::unordered_map<uint32_t, TagHash> streams;  // Wwise source id -> 26/7 entry
};
// Every Wwise stream in the install (26/7 entries with data) by source id.
std::unordered_map<uint32_t, TagHash> index_wwise_streams(PackageManager& pm);
// Reads event tag `event` and its bank; throws when either is missing or unreadable. Sources the
// event does not list are looked up in `all_streams` (index_wwise_streams) when given.
// Authored maximum distance in metres and longest stream duration in seconds.
std::pair<float, float> wwise_event_range(PackageManager& pm, TagHash event_info);
WwiseEventData load_wwise_event(PackageManager& pm, TagHash event,
                                const std::unordered_map<uint32_t, TagHash>* all_streams = nullptr);

// The .wem bytes of `source`: its stream entry, else its media embedded in the bank (empty if neither).
std::vector<uint8_t> wwise_source_bytes(PackageManager& pm, const WwiseEventData& event, uint32_t source);
// At most two channels: square counts (4, 9, 16) are ambisonic beds (ACN, SN3D) heard as a
// front-facing cardioid pair, L/R = (W +- Y) / 2 (推定); 6 channels a 5.1 downmix (L R C LFE Ls Rs
// order, 推定); other counts the mean of their channels.
PcmAudio wwise_downmix(PcmAudio pcm);

}  // namespace nim
