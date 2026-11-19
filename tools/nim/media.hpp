#pragma once
// Audio output (XAudio2) and movie playback (USM demux, Media Foundation MPEG-1 decoding, ADX audio)
// for the nim GUI. Nothing here touches the GPU: the player hands out RGBA frames to upload.
#include "nim/content/audio.hpp"
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

private:
    IXAudio2* xaudio_ = nullptr;
    IXAudio2MasteringVoice* master_ = nullptr;
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
    void submit(std::shared_ptr<const std::vector<int16_t>> samples, bool end_of_stream = false);
    void start();
    void pause();
    void set_volume(float v);
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

// Plays one USM: a reader thread demuxes, decodes MPEG-1 video through the Media Foundation
// decoder and the chosen ADX audio channel, and keeps a few RGBA frames queued. The clock is the
// audio voice when there is audio, otherwise wall time.
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

private:
    void run();
    bool frames_empty() const;
    void push_frame(std::unique_ptr<Frame> f);

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
    std::chrono::steady_clock::time_point wall_start_{};
    double wall_paused_at_ = 0;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::unique_ptr<Frame>> frames_;
    std::vector<MovieSubtitle> subtitles_;
    std::string error_;
    std::thread thread_;
};

} // namespace nim::media
