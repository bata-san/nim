#pragma once
// Audio output (XAudio2) and movie playback (USM demux, Media Foundation MPEG-1 decoding, ADX audio)
// for the nim GUI. Nothing here touches the GPU: the player hands out RGBA frames to upload.
#include "nim/content/audio.hpp"
#include "nim/content/map_audio.hpp"
#include "nim/content/movies.hpp"
#include "nim/content/mpeg1.hpp"
#include "nim/content/hca.hpp"
#include "nim/content/wwise.hpp"
#include "nim/core/package_manager.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <span>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SourceVoice;

namespace nim::media {

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();
    bool ok() const { return master_ != nullptr; }
    const std::string& error() const { return error_; }
    IXAudio2* xaudio() const { return xaudio_; }
    uint32_t master_channels() const { return master_channels_; }

private:
    IXAudio2* xaudio_ = nullptr;
    IXAudio2MasteringVoice* master_ = nullptr;
    uint32_t master_channels_ = 2;
    std::string error_;
};

// One source voice. Buffers stay alive until XAudio2 is done with them.
class Voice {
public:
    Voice(AudioEngine& engine, uint32_t sample_rate, uint16_t channels);
    ~Voice();
    Voice(const Voice&) = delete;
    Voice& operator=(const Voice&) = delete;
    bool ok() const { return voice_ != nullptr; }
    bool submit(std::shared_ptr<const std::vector<int16_t>> samples, bool end_of_stream = false);
    bool submit_looping(std::shared_ptr<const std::vector<int16_t>> samples);
    // From frame `begin` to the end, then the whole buffer `repeats` more times (~0u: endlessly).
    bool submit_from(std::shared_ptr<const std::vector<int16_t>> samples, uint32_t begin, uint32_t repeats);
    void set_ratio(float r);  // playback rate (pitch), up to 4
    void start();
    void pause();
    void set_volume(float v);
    void set_pan(float pan, uint32_t master_channels);  // pan in [-1, 1] (-1 = left, +1 = right)
    uint64_t frames_played() const;  // since the voice was created
    uint32_t queued() const;         // buffers not yet finished
    uint32_t sample_rate() const { return rate_; }
    uint16_t channels() const { return channels_; }

private:
    void release_finished() const;
    IXAudio2SourceVoice* voice_ = nullptr;
    uint32_t rate_ = 0;
    uint16_t channels_ = 0;
    mutable std::mutex mutex_;
    mutable std::deque<std::shared_ptr<const std::vector<int16_t>>> buffers_;
};

// Plays one decoded clip at a time (dialogue lines).
class ClipPlayer {
public:
    explicit ClipPlayer(AudioEngine& engine) : engine_(engine) {}
    void play(std::shared_ptr<const PcmAudio> pcm, uint64_t id);
    void stop();
    bool playing() const;
    uint64_t id() const { return playing() ? id_ : 0; }
    double position() const;  // seconds
    double length() const { return pcm_ ? pcm_->seconds() : 0; }

private:
    AudioEngine& engine_;
    std::unique_ptr<Voice> voice_;
    std::shared_ptr<const PcmAudio> pcm_;
    uint64_t id_ = 0;
};

struct MovieSubtitle {
    double start = 0, duration = 0;  // seconds
    std::string key;                 // e.g. "WWL_NUX_010_rep"; text lives in the string table
    uint32_t string_hash = 0;        // FNV-1 of the lower-case key
};
uint32_t subtitle_string_hash(const std::string& key);

// Plays one USM: a reader thread demuxes MPEG-1 pictures and the selected ADX/HCA track.
class MoviePlayer {
public:
    MoviePlayer(PackageManager& pm, AudioEngine* engine, const MovieEntry& movie, int audio_channel, double start = 0);
    ~MoviePlayer();
    MoviePlayer(const MoviePlayer&) = delete;
    MoviePlayer& operator=(const MoviePlayer&) = delete;

    struct Frame {
        double pts = 0;
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> rgba;
    };
    // The newest frame due at the current clock (older due frames are dropped), if any.
    std::unique_ptr<Frame> take_due_frame();
    // The next decoded frame regardless of the clock (headless checks), waiting up to `timeout`.
    std::unique_ptr<Frame> take_next_frame(std::chrono::milliseconds timeout);
    double position() const;
    double duration() const { return info_.duration(); }
    bool paused() const { return paused_; }
    void set_paused(bool p);
    bool finished() const { return finished_ && frames_empty(); }
    std::string error() const;
    std::vector<MovieSubtitle> subtitles() const;  // seen so far (they are interleaved with the video)
    const UsmInfo& info() const { return info_; }
    int audio_channel() const { return channel_; }
    bool audio_supported() const { return audio_ok_; }
    uint64_t audio_frames_played() const { return voice_ ? voice_->frames_played() : 0; }
    uint32_t audio_peak() const { return audio_peak_; }
    void set_volume(float v) { if (voice_) voice_->set_volume(v); }

private:
    void run();
    bool frames_empty() const;
    void push_frame(std::unique_ptr<Frame> f);
    double position_locked() const;

    std::unique_ptr<PackageManager> pm_;
    AudioEngine* engine_ = nullptr;
    TagHash usm_;
    uint64_t size_ = 0;
    UsmInfo info_;
    int channel_ = 0;
    double start_ = 0;  // keyframe time playback starts at
    uint64_t start_offset_ = 0;
    uint32_t start_frame_ = 0;
    bool audio_ok_ = false;

    std::unique_ptr<Voice> voice_;
    std::atomic<bool> stop_{false}, finished_{false}, paused_{false}, started_{false};
    std::atomic<uint32_t> audio_peak_{0};
    std::chrono::steady_clock::time_point wall_start_{};
    double wall_paused_at_ = 0;
    mutable uint64_t clock_samples_ = 0;
    mutable double clock_position_ = 0;
    mutable std::chrono::steady_clock::time_point last_audio_progress_{};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::unique_ptr<Frame>> frames_;
    std::vector<MovieSubtitle> subtitles_;
    std::string error_;
    std::thread thread_;
};

// Spatialized multi-voice ambient sound player for maps (XAudio2).
// Selects the loudest distinct AudioEvents around the camera, decodes their .wem streams
// asynchronously on a worker thread, and loops them with smooth distance attenuation & stereo pan.
class AmbientAudioPlayer {
public:
    explicit AmbientAudioPlayer(AudioEngine& engine);
    ~AmbientAudioPlayer();
    AmbientAudioPlayer(const AmbientAudioPlayer&) = delete;
    AmbientAudioPlayer& operator=(const AmbientAudioPlayer&) = delete;

    void set_map(PackageManager& pm, std::shared_ptr<const MapAudio> audio);
    void clear();
    void update(std::array<float, 3> eye, std::array<float, 3> right, float dt, float master_volume, bool enabled);

    size_t emitter_count() const { return audio_ ? audio_->emitters.size() : 0; }
    size_t active_voices() const { return active_voices_; }
    size_t decoded_count() const;

private:
    struct ActiveVoice {
        uint32_t event_tag = 0;
        TagHash wem;
        std::shared_ptr<const PcmAudio> pcm;
        std::unique_ptr<Voice> voice;
        float volume = 0.0f;
        float pan = 0.0f;
        bool looping = false;
        float cooldown = 0.0f;  // remaining wait seconds before next one-shot trigger
        uint32_t next_index = 0;
    };
    void decode_loop();
    void request_decode(TagHash wem);
    float next_random_unit();

    AudioEngine& engine_;
    std::shared_ptr<const MapAudio> audio_;
    std::vector<ActiveVoice> voices_;
    size_t active_voices_ = 0;
    uint32_t rng_state_ = 0x12345678u;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unique_ptr<PackageManager> pm_;
    std::deque<TagHash> decode_queue_;
    std::vector<uint32_t> decode_pending_;
    std::vector<std::pair<uint32_t, std::shared_ptr<const PcmAudio>>> decoded_;
    std::vector<uint32_t> failed_;
    bool stop_ = false;
    std::thread worker_;
};

// Sounds started at moving emitters (docs/PROGRESS.md 3.90): the audio events AiActors cues
// (clip events, component loops), played as their Wwise banks lay out (wwise_play_event: delays,
// containers, loops, seeks, fades, pitch, volume, attenuation curves), panned and attenuated by
// each emitter's distance from the camera. Banks load and .wem streams decode on a worker thread.
class EntityAudioPlayer {
public:
    struct Cue {
        TagHash event;
        uint64_t emitter = 0;
    };
    explicit EntityAudioPlayer(AudioEngine& engine);
    ~EntityAudioPlayer();
    EntityAudioPlayer(const EntityAudioPlayer&) = delete;
    EntityAudioPlayer& operator=(const EntityAudioPlayer&) = delete;

    void set_package(PackageManager& pm);
    void clear();  // stops everything
    // `cues` start this frame, `stopped` emitters fade out, `emitters` are where each one is now.
    void update(std::span<const Cue> cues, std::span<const uint64_t> stopped,
                std::span<const std::pair<uint64_t, std::array<float, 3>>> emitters, std::array<float, 3> eye,
                std::array<float, 3> right, float dt, float master_volume, bool enabled);
    size_t active_voices() const { return active_; }
    size_t events_loaded() const;

private:
    struct Event {
        std::shared_ptr<const WwiseEventData> data;
        std::vector<uint32_t> stops;  // the nodes its own stop actions name
    };
    struct Playing {
        uint64_t emitter = 0;
        uint64_t group = 0;  // one play of an event (chained voices share it)
        int index = 0;       // in its group
        std::shared_ptr<const WwiseEventData> event;
        WwiseVoice v;
        std::shared_ptr<const PcmAudio> pcm;
        std::unique_ptr<Voice> voice;
        double start_at = -1;  // clock time it starts (< 0: waiting on the voice it follows)
        double end_at = -1;    // clock time it ends (< 0: endless or not started)
        bool started = false, repeated = false, stopping = false;
        float stop_gain = 1;
    };
    void work();
    std::shared_ptr<const Event> event(TagHash t);           // nullptr while loading
    bool event_failed(TagHash t) const;
    bool pcm_failed(uint32_t source) const;
    std::shared_ptr<const PcmAudio> pcm(const std::shared_ptr<const WwiseEventData>& ev, uint32_t source);
    void instantiate(const Cue& cue, const Event& ev, float age);
    void add_voices(uint64_t emitter, uint64_t group, const std::shared_ptr<const WwiseEventData>& ev,
                    std::vector<WwiseVoice> voices, int after);
    float gain(const Playing& p) const;  // without the master volume
    std::array<float, 3> eye_{};

    AudioEngine& engine_;
    std::vector<Playing> playing_;
    struct Pending {
        Cue cue;
        float age = 0;
    };
    std::vector<Pending> pending_;
    std::unordered_map<uint64_t, std::array<float, 3>> where_;
    WwisePlayState state_;
    double clock_ = 0;
    uint64_t next_group_ = 1;
    size_t active_ = 0;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unique_ptr<PackageManager> pm_;
    struct Job {
        TagHash event;
        std::shared_ptr<const WwiseEventData> source_event;
        uint32_t source = 0;
    };
    std::deque<Job> jobs_;
    std::unordered_map<uint32_t, std::shared_ptr<const Event>> events_;
    std::unordered_map<uint32_t, bool> event_requested_;  // true: failed
    std::unordered_map<uint32_t, std::shared_ptr<const PcmAudio>> pcm_;
    std::unordered_map<uint32_t, bool> pcm_requested_;  // true: failed
    std::deque<uint32_t> pcm_order_;
    size_t pcm_bytes_ = 0;
    bool stop_ = false;
    uint64_t generation_ = 0;
    std::thread worker_;
};

class MapObjectAudioPlayer {
public:
    explicit MapObjectAudioPlayer(AudioEngine& engine) : player_(engine) {}
    void set_map(PackageManager& pm, std::shared_ptr<const MapAudio> audio);
    void clear();
    void update(std::array<float, 3> eye, std::array<float, 3> right, float dt, float volume, bool enabled, float rain = 0);  // rain: the weather's rain_amount
    size_t source_count() const { return audio_ ? audio_->objects.size() : 0; }

private:
    EntityAudioPlayer player_;
    std::shared_ptr<const MapAudio> audio_;
    std::vector<std::pair<uint64_t, std::array<float, 3>>> active_;
};

} // namespace nim::media
