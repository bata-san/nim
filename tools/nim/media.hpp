#pragma once
// Audio output (XAudio2) and movie playback (USM demux, Media Foundation MPEG-1 decoding, ADX audio)
// for the nim GUI. Nothing here touches the GPU: the player hands out RGBA frames to upload.
#include "nim/content/audio.hpp"
#include "nim/content/map_audio.hpp"
#include "nim/content/movies.hpp"
#include "nim/content/mpeg1.hpp"
#include "nim/content/hca.hpp"
#include "nim/core/package_manager.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
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
    };
    void decode_loop();
    void request_decode(TagHash wem);

    AudioEngine& engine_;
    std::shared_ptr<const MapAudio> audio_;
    std::vector<ActiveVoice> voices_;
    size_t active_voices_ = 0;

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

} // namespace nim::media
