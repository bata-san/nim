#include "media.hpp"

#include <windows.h>
#include <objbase.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <xaudio2.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <format>

using Microsoft::WRL::ComPtr;

namespace nim::media {

// ---- XAudio2 --------------------------------------------------------------------------------------
AudioEngine::AudioEngine() {
    HRESULT hr = XAudio2Create(&xaudio_, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        error_ = std::format("XAudio2 を初期化できません ({:08X})", uint32_t(hr));
        return;
    }
    hr = xaudio_->CreateMasteringVoice(&master_);
    if (FAILED(hr)) {
        error_ = std::format("音声出力デバイスを開けません ({:08X})", uint32_t(hr));
        master_ = nullptr;
    } else {
        XAUDIO2_VOICE_DETAILS details{};
        master_->GetVoiceDetails(&details);
        if (details.InputChannels > 0) master_channels_ = details.InputChannels;
    }
}

AudioEngine::~AudioEngine() {
    if (master_) master_->DestroyVoice();
    if (xaudio_) xaudio_->Release();
}

Voice::Voice(AudioEngine& engine, uint32_t sample_rate, uint16_t channels) : rate_(sample_rate), channels_(channels) {
    if (!engine.ok() || !sample_rate || !channels) return;
    WAVEFORMATEXTENSIBLE fmt{};
    fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt.Format.nChannels = channels;
    fmt.Format.nSamplesPerSec = sample_rate;
    fmt.Format.wBitsPerSample = 16;
    fmt.Format.nBlockAlign = WORD(channels * 2);
    fmt.Format.nAvgBytesPerSec = sample_rate * fmt.Format.nBlockAlign;
    fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    fmt.Samples.wValidBitsPerSample = 16;
    fmt.SubFormat = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};  // PCM
    switch (channels) {
    case 1: fmt.dwChannelMask = KSAUDIO_SPEAKER_MONO; break;
    case 2: fmt.dwChannelMask = KSAUDIO_SPEAKER_STEREO; break;
    case 4: fmt.dwChannelMask = KSAUDIO_SPEAKER_QUAD; break;
    case 6: fmt.dwChannelMask = KSAUDIO_SPEAKER_5POINT1; break;
    case 8: fmt.dwChannelMask = KSAUDIO_SPEAKER_7POINT1_SURROUND; break;
    default: fmt.dwChannelMask = 0; break;
    }
    if (FAILED(engine.xaudio()->CreateSourceVoice(&voice_, &fmt.Format))) voice_ = nullptr;
}

Voice::~Voice() {
    if (voice_) {
        voice_->Stop();
        voice_->FlushSourceBuffers();
        voice_->DestroyVoice();  // blocks until the voice no longer reads our buffers
    }
}

void Voice::release_finished() const {
    if (!voice_) return;
    XAUDIO2_VOICE_STATE st{};
    voice_->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    while (buffers_.size() > st.BuffersQueued) buffers_.pop_front();
}

bool Voice::submit(std::shared_ptr<const std::vector<int16_t>> samples, bool end_of_stream) {
    if (!voice_ || !samples || samples->empty()) return false;
    std::lock_guard lock(mutex_);
    release_finished();
    XAUDIO2_BUFFER b{};
    b.AudioBytes = UINT32(samples->size() * sizeof(int16_t));
    b.pAudioData = reinterpret_cast<const BYTE*>(samples->data());
    b.Flags = end_of_stream ? XAUDIO2_END_OF_STREAM : 0;
    buffers_.push_back(std::move(samples));
    if (SUCCEEDED(voice_->SubmitSourceBuffer(&b))) return true;
    buffers_.pop_back();
    return false;
}

bool Voice::submit_looping(std::shared_ptr<const std::vector<int16_t>> samples) {
    if (!voice_ || !samples || samples->empty()) return false;
    std::lock_guard lock(mutex_);
    release_finished();
    XAUDIO2_BUFFER b{};
    b.AudioBytes = UINT32(samples->size() * sizeof(int16_t));
    b.pAudioData = reinterpret_cast<const BYTE*>(samples->data());
    b.LoopCount = XAUDIO2_LOOP_INFINITE;
    buffers_.push_back(std::move(samples));
    if (SUCCEEDED(voice_->SubmitSourceBuffer(&b))) return true;
    buffers_.pop_back();
    return false;
}

void Voice::start() {
    if (voice_) voice_->Start();
}
void Voice::pause() {
    if (voice_) voice_->Stop();
}
void Voice::set_volume(float v) {
    if (voice_) voice_->SetVolume(v);
}
void Voice::set_pan(float pan, uint32_t master_channels) {
    if (!voice_ || master_channels < 2 || channels_ == 0 || channels_ > 8 || master_channels > 8) return;
    const float clamped = std::clamp(pan, -1.0f, 1.0f);
    const float angle = (clamped * 0.5f + 0.5f) * 1.57079632679f;
    const float l = std::cos(angle), r = std::sin(angle);
    float matrix[64]{};
    if (channels_ == 1) {
        matrix[0] = l;
        matrix[1] = r;
    } else if (channels_ == 2) {
        // Preserve stereo width while shifting balance toward the emitter's side.
        matrix[0 * channels_ + 0] = l * 1.15f;
        matrix[0 * channels_ + 1] = l * 0.35f;
        matrix[1 * channels_ + 0] = r * 0.35f;
        matrix[1 * channels_ + 1] = r * 1.15f;
    } else {
        for (uint32_t c = 0; c < channels_; ++c) {
            matrix[0 * channels_ + c] = l / float(channels_);
            matrix[1 * channels_ + c] = r / float(channels_);
        }
    }
    voice_->SetOutputMatrix(nullptr, channels_, master_channels, matrix);
}

uint64_t Voice::frames_played() const {
    if (!voice_) return 0;
    XAUDIO2_VOICE_STATE st{};
    voice_->GetState(&st, 0);
    return st.SamplesPlayed;
}

uint32_t Voice::queued() const {
    if (!voice_) return 0;
    std::lock_guard lock(mutex_);
    release_finished();
    return uint32_t(buffers_.size());
}

void ClipPlayer::play(std::shared_ptr<const PcmAudio> pcm, uint64_t id) {
    stop();
    if (!pcm || pcm->samples.empty() || !engine_.ok()) return;
    pcm_ = std::move(pcm);
    id_ = id;
    voice_ = std::make_unique<Voice>(engine_, pcm_->sample_rate, pcm_->channels);
    // The buffer aliases pcm_'s samples; the shared_ptr keeps them alive for the voice.
    auto alias = std::shared_ptr<const std::vector<int16_t>>(pcm_, &pcm_->samples);
    voice_->submit(alias, true);
    voice_->start();
}

void ClipPlayer::stop() {
    voice_.reset();
    pcm_.reset();
    id_ = 0;
}

bool ClipPlayer::playing() const { return voice_ && voice_->queued() > 0; }

double ClipPlayer::position() const {
    if (!voice_ || !pcm_ || !pcm_->sample_rate) return 0;
    return std::min(double(voice_->frames_played()) / pcm_->sample_rate, pcm_->seconds());
}

uint32_t subtitle_string_hash(const std::string& key) {
    uint32_t h = 0x811C9DC5;
    for (char c : key) {
        h *= 0x01000193;
        h ^= uint8_t(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    return h;
}

// ---- Movie player ---------------------------------------------------------------------------------
namespace {

uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

} // namespace

MoviePlayer::MoviePlayer(PackageManager& pm, AudioEngine* engine, const MovieEntry& movie, int audio_channel, double start)
    : pm_(pm.fork()), engine_(engine), usm_(movie.usm), size_(movie.size), info_(movie.info), channel_(audio_channel) {
    // Start at the last keyframe at or before `start`.
    if (start > 0 && info_.frame_rate > 0)
        for (const auto& s : info_.seek) {
            const double t = s.frame / info_.frame_rate;
            if (t > start + 1e-6) break;
            start_offset_ = s.offset, start_frame_ = s.frame, start_ = t;
        }
    for (const auto& a : info_.audio)
        if (a.channel == channel_ && (a.codec == 2 || a.codec == 4) && engine_ && engine_->ok()) {
            voice_ = std::make_unique<Voice>(*engine_, a.sample_rate, uint16_t(a.channels));
            audio_ok_ = voice_->ok();
            if (!audio_ok_) voice_.reset();
        }
    thread_ = std::thread([this] { run(); });
}

MoviePlayer::~MoviePlayer() {
    stop_ = true;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    voice_.reset();
}

bool MoviePlayer::frames_empty() const {
    std::lock_guard lock(mutex_);
    return frames_.empty();
}

void MoviePlayer::push_frame(std::unique_ptr<Frame> f) {
    std::unique_lock lock(mutex_);
    // Keep a few frames ahead; allow more when the audio clock is starving (audio muxed late).
    cv_.wait(lock, [&] {
        if (stop_) return true;
        if (frames_.size() < 8) return true;
        return voice_ && started_ && !paused_ && voice_->queued() == 0 && frames_.size() < 48;
    });
    if (stop_) return;
    frames_.push_back(std::move(f));
    cv_.notify_all();
    if (!started_) {
        wall_start_ = std::chrono::steady_clock::now();
        last_audio_progress_ = wall_start_;
        wall_paused_at_ = clock_position_ = start_;
        started_ = true;
        if (voice_ && !paused_) voice_->start();
    }
}

double MoviePlayer::position() const {
    std::lock_guard lock(mutex_);
    return position_locked();
}

double MoviePlayer::position_locked() const {
    if (!started_) return start_;
    if (paused_) return wall_paused_at_;
    const auto now = std::chrono::steady_clock::now();
    if (!voice_) return start_ + std::chrono::duration<double>(now - wall_start_).count();
    const uint64_t played = voice_->frames_played();
    const double audio = start_ + double(played) / voice_->sample_rate();
    if (played != clock_samples_) {
        clock_samples_ = played;
        last_audio_progress_ = now;
        clock_position_ = std::max(clock_position_, audio);
    } else if (voice_->queued() == 0) {
        // Late blocks or a shorter audio track must not block the video reader and clock.
        const double idle = std::chrono::duration<double>(now - last_audio_progress_).count();
        clock_position_ = std::max(clock_position_, audio + std::max(0.0, idle - 0.12));
    }
    return clock_position_;
}

void MoviePlayer::set_paused(bool p) {
    std::lock_guard lock(mutex_);
    if (p == paused_) return;
    if (started_) {
        if (p) wall_paused_at_ = position_locked();
        else {
            last_audio_progress_ = std::chrono::steady_clock::now();
            wall_start_ = std::chrono::steady_clock::now() -
                          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double>(wall_paused_at_ - start_));
        }
    }
    paused_ = p;
    if (voice_ && started_) p ? voice_->pause() : voice_->start();
    cv_.notify_all();
}

std::unique_ptr<MoviePlayer::Frame> MoviePlayer::take_due_frame() {
    const double now = position();
    std::unique_ptr<Frame> due;
    {
        std::lock_guard lock(mutex_);
        while (!frames_.empty() && frames_.front()->pts <= now + 0.004) {
            due = std::move(frames_.front());
            frames_.pop_front();
        }
    }
    if (due) cv_.notify_all();
    return due;
}

std::unique_ptr<MoviePlayer::Frame> MoviePlayer::take_next_frame(std::chrono::milliseconds timeout) {
    std::unique_ptr<Frame> f;
    {
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, timeout, [&] { return !frames_.empty() || finished_; });
        if (frames_.empty()) return nullptr;
        f = std::move(frames_.front());
        frames_.pop_front();
    }
    cv_.notify_all();
    return f;
}

std::string MoviePlayer::error() const {
    std::lock_guard lock(mutex_);
    return error_;
}

std::vector<MovieSubtitle> MoviePlayer::subtitles() const {
    std::lock_guard lock(mutex_);
    return subtitles_;
}

void MoviePlayer::run() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto set_error = [&](std::string e) {
        std::lock_guard lock(mutex_);
        error_ = std::move(e);
    };
    try {
        if (info_.video_codec != 1) throw std::runtime_error(std::format("映像コーデック {} は未対応です", usm_video_codec_name(info_.video_codec)));
        const uint32_t w = std::min(info_.display_width ? info_.display_width : info_.width, info_.width);
        const uint32_t h = std::min(info_.display_height ? info_.display_height : info_.height, info_.height);

        AdxDecoder adx;
        HcaDecoder hca;
        bool is_hca = false;
        std::vector<int16_t> pcm;
        if (voice_ && start_offset_) {
            std::span<const uint8_t> head;
            std::vector<uint8_t> fetched_header;
            if (const auto header = info_.audio_headers.find(uint8_t(channel_)); header != info_.audio_headers.end()) head = header->second;
            if (head.empty()) {
                // Catalog metadata is read from a bounded prefix. Some muxes put audio after it.
                for (uint64_t limit = 0x80000; limit <= 0x800000 && fetched_header.empty() && !stop_; limit *= 2) {
                    const auto prefix = pm_->read_range(usm_, 0, std::min(limit, size_));
                    for_each_usm_chunk(prefix, [&](const UsmChunk& c) {
                        if (fetched_header.empty() && c.data_type == 0 && c.stream == UsmStream::Audio && c.channel == channel_)
                            fetched_header.assign(c.payload.begin(), c.payload.end());
                    });
                    if (prefix.size() == size_) break;
                }
                head = fetched_header;
            }
            if (head.empty()) throw std::runtime_error("movie audio header not available for this seek");
            if (head.size() >= 8 && (head[0] & 0x7F) == 'H' && (head[1] & 0x7F) == 'C' && (head[2] & 0x7F) == 'A') {
                hca.parse_header(head);
                is_hca = true;
            } else {
                // Only the header part: the stream restarts at a keyframe with fresh history.
                size_t n = head.size() >= 4 ? std::min<size_t>(head.size(), be16(head.data() + 2) + 4u) : 0;
                adx.feed(std::span(head).first(n), pcm);
            }
            pcm.clear();
        }
        uint64_t frame_no = start_frame_;
        auto on_frame = [&](const Mpeg1Picture& p) {
            auto f = std::make_unique<Frame>();
            f->width = std::min(w, p.width);
            f->height = std::min(h, p.height);
            f->pts = info_.frame_rate > 0 ? double(frame_no) / info_.frame_rate : 0;
            ++frame_no;
            f->rgba.resize(size_t(f->width) * f->height * 4);
            mpeg1_to_rgba(p, f->width, f->height, f->rgba.data());
            push_frame(std::move(f));
        };
        Mpeg1Decoder dec(on_frame);
        auto flush_audio = [&](bool end) {
            if (!voice_ || pcm.empty()) return;
            uint32_t peak = audio_peak_;
            for (int16_t value : pcm) peak = std::max(peak, uint32_t(std::abs(int(value))));
            auto buf = std::make_shared<const std::vector<int16_t>>(std::move(pcm));
            pcm.clear();
            if (!voice_->submit(buf, end)) throw std::runtime_error("XAudio2 rejected the decoded movie audio");
            audio_peak_ = peak;
        };

        auto feed_video = [&](std::span<const uint8_t> es) { dec.feed(es); };
        // Starting mid-stream: the decoder still needs the sequence header from the first picture chunk.
        if (start_offset_ && !info_.video_head.empty()) {
            const auto& vh = info_.video_head;
            size_t end = vh.size();
            for (size_t i = 4; i + 4 <= vh.size(); ++i)
                if (vh[i] == 0 && vh[i + 1] == 0 && vh[i + 2] == 1 && (vh[i + 3] == 0xB8 || vh[i + 3] == 0x00)) {
                    end = i;
                    break;
                }
            if (vh.size() >= 4 && vh[3] == 0xB3) feed_video(std::span(vh).first(end));
        }

        std::vector<uint8_t> buf;
        uint64_t pos = start_offset_;
        bool first_read = true;
        while (!stop_) {
            if (pos < size_) {
                auto more = pm_->read_range(usm_, pos, 0x200000);
                pos += more.size();
                buf.insert(buf.end(), more.begin(), more.end());
            }
            if (first_read && start_offset_ && (buf.size() < 4 || (std::memcmp(buf.data(), "@SFV", 4) != 0 &&
                                                                   std::memcmp(buf.data(), "@SFA", 4) != 0)))
                throw std::runtime_error("シーク位置がチャンク境界ではありません");
            first_read = false;
            // Submit the segment's audio before picture decoding can block on the frame queue.
            if (voice_) for_each_usm_chunk(buf, [&](const UsmChunk& c) {
                    if (stop_ || c.data_type != 0 || c.stream != UsmStream::Audio ||
                        c.channel != channel_ || c.seconds() + 0.02 < start_) return;
                    auto payload = c.payload;
                    if (payload.size() >= 8 && (payload[0] & 0x7F) == 'H' &&
                        (payload[1] & 0x7F) == 'C' && (payload[2] & 0x7F) == 'A') {
                        if (!hca.ready()) hca.parse_header(payload);
                        is_hca = true;
                        if (payload.size() < hca.header_size()) throw std::runtime_error("truncated USM HCA header");
                        payload = payload.subspan(hca.header_size());
                    }
                    if (is_hca) hca.feed(payload, pcm);
                    else adx.feed(payload, pcm);
            });
            flush_audio(pos >= size_);
            const size_t used = for_each_usm_chunk(buf, [&](const UsmChunk& c) {
                if (stop_ || c.data_type != 0) return;
                if (c.stream == UsmStream::Video && c.channel == 0) feed_video(c.payload);
                else if (c.stream == UsmStream::Subtitle) {
                    auto s = parse_usm_subtitle(c.payload);
                    if (!s) return;
                    MovieSubtitle m;
                    const double scale = s->interval ? 1.0 / s->interval : 0.001;
                    m.start = s->start * scale;
                    m.duration = s->duration * scale;
                    std::string text = s->text;
                    // Marathon's subtitle text is "<duration ms> <string key>".
                    if (auto sp = text.find(' '); sp != std::string::npos &&
                                                  std::all_of(text.begin(), text.begin() + std::ptrdiff_t(sp), [](char ch) { return ch >= '0' && ch <= '9'; }) && sp) {
                        m.duration = std::stod(text.substr(0, sp)) / 1000.0;
                        text = text.substr(sp + 1);
                    }
                    m.key = text;
                    m.string_hash = subtitle_string_hash(text);
                    std::lock_guard lock(mutex_);
                    const bool dup = std::any_of(subtitles_.begin(), subtitles_.end(),
                                                 [&](const MovieSubtitle& o) { return o.key == m.key && o.start == m.start; });
                    if (!dup) subtitles_.push_back(std::move(m));
                }
            });
            buf.erase(buf.begin(), buf.begin() + std::ptrdiff_t(used));
            if (pos >= size_ && (used == 0 || buf.size() < 0x20)) break;
        }
        if (!stop_) {
            dec.flush();
            flush_audio(true);
        }
        if (!started_ && !stop_) {  // no picture at all: still let the clock run for audio
            std::lock_guard lock(mutex_);
            wall_start_ = std::chrono::steady_clock::now();
            last_audio_progress_ = wall_start_;
            wall_paused_at_ = clock_position_ = start_;
            started_ = true;
            if (voice_ && !paused_) voice_->start();
        }
    } catch (const std::exception& ex) {
        set_error(ex.what());
    }
    finished_ = true;
    if (SUCCEEDED(co)) CoUninitialize();
}

// ---- Spatialized map ambient audio ----------------------------------------------------------------
AmbientAudioPlayer::AmbientAudioPlayer(AudioEngine& engine) : engine_(engine) {
    worker_ = std::thread([this] { decode_loop(); });
}

AmbientAudioPlayer::~AmbientAudioPlayer() {
    clear();
    {
        std::lock_guard lk(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void AmbientAudioPlayer::set_map(PackageManager& pm, std::shared_ptr<const MapAudio> audio) {
    voices_.clear();
    active_voices_ = 0;
    audio_ = std::move(audio);
    {
        std::lock_guard lk(mutex_);
        pm_ = pm.fork();
        decode_queue_.clear();
        decode_pending_.clear();
        decoded_.clear();
        failed_.clear();
    }
}

void AmbientAudioPlayer::clear() {
    voices_.clear();
    active_voices_ = 0;
    audio_.reset();
    {
        std::lock_guard lk(mutex_);
        decode_queue_.clear();
        decode_pending_.clear();
        decoded_.clear();
        failed_.clear();
    }
}

size_t AmbientAudioPlayer::decoded_count() const {
    std::lock_guard lk(mutex_);
    return decoded_.size();
}

void AmbientAudioPlayer::request_decode(TagHash wem) {
    if (wem.is_none()) return;
    std::lock_guard lk(mutex_);
    if (!pm_) return;
    for (const auto& [k, _] : decoded_) if (k == wem.value) return;
    for (uint32_t f : failed_) if (f == wem.value) return;
    for (uint32_t p : decode_pending_) if (p == wem.value) return;
    decode_pending_.push_back(wem.value);
    decode_queue_.push_back(wem);
    cv_.notify_one();
}

void AmbientAudioPlayer::decode_loop() {
    for (;;) {
        TagHash wem;
        PackageManager* pm = nullptr;
        {
            std::unique_lock lk(mutex_);
            cv_.wait(lk, [&] { return stop_ || !decode_queue_.empty(); });
            if (stop_) return;
            wem = decode_queue_.front();
            decode_queue_.pop_front();
            pm = pm_.get();
        }
        std::shared_ptr<const PcmAudio> pcm;
        if (pm) {
            try {
                auto raw = pm->read(wem);
                auto dec = decode_wem(raw);
                if (!dec.samples.empty() && dec.sample_rate > 0 && dec.channels > 0)
                    pcm = std::make_shared<const PcmAudio>(std::move(dec));
            } catch (...) {
            }
        }
        std::lock_guard lk(mutex_);
        std::erase(decode_pending_, wem.value);
        if (pcm) {
            // Bound decoded cache to ~48 entries (~40-60 MB) so memory stays lean on huge maps.
            if (decoded_.size() >= 48) decoded_.erase(decoded_.begin());
            decoded_.emplace_back(wem.value, std::move(pcm));
        } else {
            failed_.push_back(wem.value);
        }
    }
}

void AmbientAudioPlayer::update(std::array<float, 3> eye, std::array<float, 3> right, float dt,
                                float master_volume, bool enabled) {
    if (!engine_.ok() || !audio_ || !enabled || master_volume <= 1e-4f) {
        for (auto& v : voices_)
            if (v.voice) v.voice->set_volume(0.0f);
        active_voices_ = 0;
        return;
    }
    constexpr size_t kMaxVoices = 12;
    struct Candidate {
        uint32_t event_tag = 0;
        TagHash wem;
        float weight = 0.0f;
        float pan = 0.0f;
    };
    // Deduplicate by event tag: keep the loudest/closest emitter for each distinct AudioEvent.
    std::vector<Candidate> best;
    best.reserve(64);
    for (const auto& em : audio_->emitters) {
        if (em.wems.empty()) continue;
        const auto pt = em.closest_point(eye);
        const float dx = pt[0] - eye[0], dy = pt[1] - eye[1], dz = pt[2] - eye[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        const float max_dist = em.global ? 1e9f : std::max(52.0f, em.radius + 40.0f);
        if (!em.global && d2 > max_dist * max_dist) continue;
        const float d = std::sqrt(d2);
        const float ref_dist = std::max(7.0f, em.radius + 5.0f);
        const float edge_fade = em.global ? 1.0f : std::clamp(1.0f - (d / max_dist) * (d / max_dist), 0.0f, 1.0f);
        const float att = em.global ? 0.55f : (edge_fade / (1.0f + (d / ref_dist) * (d / ref_dist)));
        const float w = std::clamp(em.gain, 0.1f, 3.0f) * att;
        if (w < 0.015f) continue;
        float pan = 0.0f;
        if (!em.global && d > 0.25f) {
            pan = std::clamp((dx * right[0] + dy * right[1] + dz * right[2]) / d, -1.0f, 1.0f);
        }
        bool merged = false;
        for (auto& c : best) {
            if (c.event_tag == em.event.value) {
                if (w > c.weight) {
                    c.weight = w;
                    c.pan = pan;
                }
                merged = true;
                break;
            }
        }
        if (!merged) {
            // Deterministically pick one WEM stream from the event's variations.
            const TagHash wem = em.wems[em.event_id % em.wems.size()];
            best.push_back({em.event.value, wem, w, pan});
        }
    }
    if (best.size() > kMaxVoices) {
        std::partial_sort(best.begin(), best.begin() + kMaxVoices, best.end(),
                          [](const Candidate& a, const Candidate& b) { return a.weight > b.weight; });
        best.resize(kMaxVoices);
    }

    // Lookup decoded PCMs under lock.
    auto find_pcm = [&](TagHash wem) -> std::shared_ptr<const PcmAudio> {
        std::lock_guard lk(mutex_);
        for (const auto& [k, p] : decoded_)
            if (k == wem.value) return p;
        return nullptr;
    };

    const float smooth = 1.0f - std::exp(-std::clamp(dt, 0.001f, 0.1f) * 5.0f);
    // Fade out voices no longer in `best`.
    for (auto& v : voices_) {
        const auto it = std::find_if(best.begin(), best.end(), [&](const Candidate& c) { return c.event_tag == v.event_tag; });
        if (it == best.end()) {
            v.volume += (0.0f - v.volume) * smooth;
            if (v.voice) v.voice->set_volume(v.volume);
        }
    }
    std::erase_if(voices_, [&](const ActiveVoice& v) {
        const bool wanted = std::any_of(best.begin(), best.end(), [&](const Candidate& c) { return c.event_tag == v.event_tag; });
        return !wanted && v.volume < 0.004f;
    });

    // Update or start wanted voices.
    const float mix_scale = 0.28f * master_volume;  // keep 12 concurrent ambient beds balanced without clipping
    size_t playing = 0;
    for (const auto& c : best) {
        const float target_vol = std::min(c.weight * mix_scale, 0.85f * master_volume);
        auto it = std::find_if(voices_.begin(), voices_.end(), [&](const ActiveVoice& v) { return v.event_tag == c.event_tag; });
        if (it != voices_.end()) {
            it->volume += (target_vol - it->volume) * smooth;
            it->pan += (c.pan - it->pan) * smooth;
            if (it->voice) {
                it->voice->set_volume(it->volume);
                it->voice->set_pan(it->pan, engine_.master_channels());
                if (it->volume > 0.005f) ++playing;
            }
            continue;
        }
        auto pcm = find_pcm(c.wem);
        if (!pcm) {
            request_decode(c.wem);
            continue;
        }
        if (voices_.size() >= kMaxVoices + 4) continue;
        ActiveVoice av;
        av.event_tag = c.event_tag;
        av.wem = c.wem;
        av.pcm = std::move(pcm);
        av.voice = std::make_unique<Voice>(engine_, av.pcm->sample_rate, av.pcm->channels);
        if (av.voice->ok()) {
            av.volume = target_vol * smooth;
            av.pan = c.pan;
            av.voice->set_volume(av.volume);
            av.voice->set_pan(av.pan, engine_.master_channels());
            auto alias = std::shared_ptr<const std::vector<int16_t>>(av.pcm, &av.pcm->samples);
            av.voice->submit_looping(alias);
            av.voice->start();
            voices_.push_back(std::move(av));
            ++playing;
        }
    }
    active_voices_ = playing;
}

} // namespace nim::media
