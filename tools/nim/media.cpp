#include "media.hpp"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <xaudio2.h>
#include <wrl/client.h>

#include <algorithm>
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

void Voice::submit(std::shared_ptr<const std::vector<int16_t>> samples, bool end_of_stream) {
    if (!voice_ || !samples || samples->empty()) return;
    std::lock_guard lock(mutex_);
    release_finished();
    XAUDIO2_BUFFER b{};
    b.AudioBytes = UINT32(samples->size() * sizeof(int16_t));
    b.pAudioData = reinterpret_cast<const BYTE*>(samples->data());
    b.Flags = end_of_stream ? XAUDIO2_END_OF_STREAM : 0;
    buffers_.push_back(std::move(samples));
    if (FAILED(voice_->SubmitSourceBuffer(&b))) buffers_.pop_back();
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

void nv12_to_rgba(const uint8_t* y, const uint8_t* uv, size_t pitch, uint32_t w, uint32_t h, uint8_t* out) {
    auto clip = [](int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); };
    for (uint32_t row = 0; row < h; ++row) {
        const uint8_t* yr = y + row * pitch;
        const uint8_t* cr = uv + (row / 2) * pitch;
        uint8_t* o = out + size_t(row) * w * 4;
        for (uint32_t x = 0; x < w; ++x) {
            const int C = 298 * (int(yr[x]) - 16), D = int(cr[x & ~1u]) - 128, E = int(cr[(x & ~1u) + 1]) - 128;
            o[0] = clip((C + 409 * E + 128) >> 8);
            o[1] = clip((C - 100 * D - 208 * E + 128) >> 8);
            o[2] = clip((C + 516 * D + 128) >> 8);
            o[3] = 255;
            o += 4;
        }
    }
}

struct Mpeg1Decoder {
    ComPtr<IMFTransform> mft;
    uint32_t out_w = 0, out_h = 0;
    LONG stride = 0;
    bool provides_samples = false;

    std::string init(uint32_t w, uint32_t h) {
        MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_MPG1};
        IMFActivate** acts = nullptr;
        UINT32 count = 0;
        HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in,
                               nullptr, &acts, &count);
        if (FAILED(hr) || !count) return "MPEG-1 デコーダー (Media Foundation) が見つかりません";
        hr = acts[0]->ActivateObject(IID_PPV_ARGS(&mft));
        for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
        CoTaskMemFree(acts);
        if (FAILED(hr)) return std::format("MPEG-1 デコーダーを開けません ({:08X})", uint32_t(hr));
        ComPtr<IMFMediaType> it;
        MFCreateMediaType(&it);
        it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        it->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_MPG1);
        MFSetAttributeSize(it.Get(), MF_MT_FRAME_SIZE, w, h);
        if (FAILED(hr = mft->SetInputType(0, it.Get(), 0))) return std::format("入力形式を設定できません ({:08X})", uint32_t(hr));
        if (!select_output()) return "NV12 出力に対応していません";
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return {};
    }

    bool select_output() {
        for (DWORD i = 0;; ++i) {
            ComPtr<IMFMediaType> t;
            if (FAILED(mft->GetOutputAvailableType(0, i, &t))) return false;
            GUID sub{};
            t->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub != MFVideoFormat_NV12) continue;
            if (FAILED(mft->SetOutputType(0, t.Get(), 0))) return false;
            UINT32 w = 0, h = 0;
            MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
            out_w = w, out_h = h;
            UINT32 s = 0;
            stride = SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &s)) ? LONG(s) : LONG(w);
            MFT_OUTPUT_STREAM_INFO si{};
            mft->GetOutputStreamInfo(0, &si);
            provides_samples = si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES);
            return true;
        }
    }

    // Pulls every ready picture.
    template <typename F> void drain(F&& on_frame) {
        for (;;) {
            MFT_OUTPUT_STREAM_INFO si{};
            mft->GetOutputStreamInfo(0, &si);
            ComPtr<IMFSample> sample;
            ComPtr<IMFMediaBuffer> buffer;
            MFT_OUTPUT_DATA_BUFFER ob{};
            if (!provides_samples) {
                MFCreateSample(&sample);
                MFCreateMemoryBuffer(si.cbSize ? si.cbSize : out_w * out_h * 2, &buffer);
                sample->AddBuffer(buffer.Get());
                ob.pSample = sample.Get();
            }
            DWORD status = 0;
            const HRESULT hr = mft->ProcessOutput(0, 1, &ob, &status);
            if (ob.pEvents) ob.pEvents->Release();
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (provides_samples && ob.pSample) ob.pSample->Release();
                if (!select_output()) return;
                continue;
            }
            if (FAILED(hr)) {
                if (hr != MF_E_TRANSFORM_NEED_MORE_INPUT && std::getenv("NIM_MEDIA_DEBUG"))
                    std::fprintf(stderr, "ProcessOutput %08lX\n", hr);
                return;
            }
            IMFSample* out = ob.pSample;
            ComPtr<IMFMediaBuffer> buf;
            if (out && SUCCEEDED(out->ConvertToContiguousBuffer(&buf))) {
                ComPtr<IMF2DBuffer> b2;
                BYTE* scan0 = nullptr;
                LONG pitch = 0;
                if (SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->Lock2D(&scan0, &pitch))) {
                    on_frame(scan0, size_t(pitch), out_w, out_h);
                    b2->Unlock2D();
                } else {
                    BYTE* p = nullptr;
                    DWORD len = 0;
                    if (SUCCEEDED(buf->Lock(&p, nullptr, &len))) {
                        on_frame(p, size_t(stride > 0 ? stride : LONG(out_w)), out_w, out_h);
                        buf->Unlock();
                    }
                }
            }
            if (provides_samples && out) out->Release();
        }
    }

    HRESULT feed(std::span<const uint8_t> es) {
        ComPtr<IMFMediaBuffer> b;
        if (es.empty()) return S_OK;
        if (HRESULT hr = MFCreateMemoryBuffer(DWORD(es.size()), &b); FAILED(hr)) return hr;
        BYTE* p = nullptr;
        b->Lock(&p, nullptr, nullptr);
        std::memcpy(p, es.data(), es.size());
        b->Unlock();
        b->SetCurrentLength(DWORD(es.size()));
        ComPtr<IMFSample> s;
        MFCreateSample(&s);
        s->AddBuffer(b.Get());
        return mft->ProcessInput(0, s.Get(), 0);
    }
};

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
        if (a.channel == channel_ && a.codec == 2 && engine_ && engine_->ok() && info_.audio_headers.contains(a.channel)) {
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
    cv_.wait_for(lock, std::chrono::seconds(5), [&] {
        if (stop_) return true;
        if (frames_.size() < 8) return true;
        return voice_ && started_ && !paused_ && voice_->queued() == 0 && frames_.size() < 48;
    });
    if (stop_) return;
    frames_.push_back(std::move(f));
    cv_.notify_all();
    if (!started_) {
        started_ = true;
        wall_start_ = std::chrono::steady_clock::now();
        if (voice_ && !paused_) voice_->start();
    }
}

double MoviePlayer::position() const {
    if (!started_) return start_;
    if (voice_) return start_ + double(voice_->frames_played()) / voice_->sample_rate();
    if (paused_) return wall_paused_at_;
    return start_ + std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start_).count();
}

void MoviePlayer::set_paused(bool p) {
    if (p == paused_) return;
    if (!voice_ && started_) {
        if (p) {
            wall_paused_at_ = position();
        } else {
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
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    auto set_error = [&](std::string e) {
        std::lock_guard lock(mutex_);
        error_ = std::move(e);
    };
    try {
        Mpeg1Decoder dec;
        if (info_.video_codec != 1) throw std::runtime_error(std::format("映像コーデック {} は未対応です", usm_video_codec_name(info_.video_codec)));
        if (auto e = dec.init(info_.width, info_.height); !e.empty()) throw std::runtime_error(e);
        const uint32_t w = std::min(info_.display_width ? info_.display_width : info_.width, info_.width);
        const uint32_t h = std::min(info_.display_height ? info_.display_height : info_.height, info_.height);

        AdxDecoder adx;
        std::vector<int16_t> pcm;
        if (voice_) {
            const auto& head = info_.audio_headers.at(uint8_t(channel_));
            // Only the header part: the stream restarts at a keyframe with fresh history.
            size_t n = head.size() >= 4 ? std::min<size_t>(head.size(), be16(head.data() + 2) + 4u) : 0;
            adx.feed(std::span(head).first(n), pcm);
            pcm.clear();
        }
        uint64_t frame_no = start_frame_;
        auto on_frame = [&](const uint8_t* base, size_t pitch, uint32_t ow, uint32_t oh) {
            auto f = std::make_unique<Frame>();
            f->width = std::min(w, ow);
            f->height = std::min(h, oh);
            f->pts = info_.frame_rate > 0 ? double(frame_no) / info_.frame_rate : 0;
            ++frame_no;
            f->rgba.resize(size_t(f->width) * f->height * 4);
            nv12_to_rgba(base, base + pitch * oh, pitch, f->width, f->height, f->rgba.data());
            push_frame(std::move(f));
        };
        auto flush_audio = [&](bool end) {
            if (!voice_ || (pcm.empty() && !end)) return;
            auto buf = std::make_shared<const std::vector<int16_t>>(std::move(pcm));
            pcm.clear();
            if (!buf->empty() || end) voice_->submit(buf, end);
        };

        auto feed_video = [&](std::span<const uint8_t> es) {
            HRESULT hr;
            int spins = 0;
            while (!stop_ && (hr = dec.feed(es)) == MF_E_NOTACCEPTING) {
                dec.drain(on_frame);
                if (++spins > 1000) throw std::runtime_error("MPEG-1 デコーダーが入力を受け付けません");
            }
            if (FAILED(hr) && std::getenv("NIM_MEDIA_DEBUG")) std::fprintf(stderr, "ProcessInput %08lX\n", hr);
            dec.drain(on_frame);
        };
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
            const size_t used = for_each_usm_chunk(buf, [&](const UsmChunk& c) {
                if (stop_ || c.data_type != 0) return;
                if (c.stream == UsmStream::Video && c.channel == 0) {
                    feed_video(c.payload);
                } else if (c.stream == UsmStream::Audio && voice_ && c.channel == channel_) {
                    if (c.seconds() + 0.02 < start_) return;
                    // the header was fed up front; data frames never start with 0x8000
                    if (c.payload.size() >= 2 && c.payload[0] == 0x80 && c.payload[1] == 0x00) return;
                    adx.feed(c.payload, pcm);
                    if (pcm.size() >= size_t(voice_->sample_rate()) * voice_->channels() / 10) flush_audio(false);
                } else if (c.stream == UsmStream::Subtitle) {
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
            dec.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            dec.mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
            dec.drain(on_frame);
            flush_audio(true);
        }
        if (!started_ && !stop_) {  // no picture at all: still let the clock run for audio
            std::lock_guard lock(mutex_);
            started_ = true;
            wall_start_ = std::chrono::steady_clock::now();
            if (voice_ && !paused_) voice_->start();
        }
    } catch (const std::exception& ex) {
        set_error(ex.what());
    }
    finished_ = true;
    MFShutdown();
    if (SUCCEEDED(co)) CoUninitialize();
}

} // namespace nim::media
