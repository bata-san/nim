#include "movie_export.hpp"

#include "media.hpp"
#include "nim/content/hca.hpp"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace nim::media {
namespace {

using namespace nim;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

void check(HRESULT hr, std::string_view operation) {
    if (FAILED(hr)) throw std::runtime_error(std::format("{} (0x{:08X})", operation, uint32_t(hr)));
}

struct ComSession {
    ComSession() { check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Initialize COM"); }
    ~ComSession() { CoUninitialize(); }
};

struct MfSession {
    MfSession() { check(MFStartup(MF_VERSION, MFSTARTUP_LITE), "Start Windows Media Foundation"); }
    ~MfSession() { MFShutdown(); }
};

struct TemporaryExport {
    fs::path mp4, srt;
    ~TemporaryExport() {
        std::error_code ec;
        fs::remove(mp4, ec);
        fs::remove(srt, ec);
    }
};

PcmAudio read_audio(PackageManager& pm, const MovieEntry& movie, const UsmInfo::Audio& track,
                    const std::atomic<bool>& cancel) {
    AdxDecoder adx;
    HcaDecoder hca;
    PcmAudio pcm;
    std::vector<uint8_t> buffer;
    uint64_t offset = 0;
    while (offset < movie.size && !cancel) {
        auto bytes = pm.read_range(movie.usm, offset, std::min<uint64_t>(0x200000, movie.size - offset));
        if (bytes.empty()) throw std::runtime_error("Unexpected end of movie audio data");
        offset += bytes.size();
        buffer.insert(buffer.end(), bytes.begin(), bytes.end());
        const auto used = for_each_usm_chunk(buffer, [&](const UsmChunk& chunk) {
            if (cancel || chunk.stream != UsmStream::Audio || chunk.data_type != 0 || chunk.channel != track.channel) return;
            auto payload = chunk.payload;
            if (track.codec == 2) {
                adx.feed(payload, pcm.samples);
            } else {
                if (payload.size() >= 8 && (payload[0] & 0x7F) == 'H' &&
                    (payload[1] & 0x7F) == 'C' && (payload[2] & 0x7F) == 'A') {
                    if (!hca.ready()) hca.parse_header(payload);
                    if (payload.size() < hca.header_size()) throw std::runtime_error("Incomplete HCA header");
                    payload = payload.subspan(hca.header_size());
                }
                if (!hca.ready()) throw std::runtime_error("Missing HCA header");
                hca.feed(payload, pcm.samples);
            }
        });
        buffer.erase(buffer.begin(), buffer.begin() + ptrdiff_t(used));
    }
    if (cancel) return {};
    if (adx.encrypted) throw std::runtime_error("Encrypted ADX audio is unsupported");
    if (hca.frame_errors()) throw std::runtime_error("Cannot decode all frames of the HCA audio track");
    pcm.channels = uint16_t(track.codec == 2 ? adx.channels : hca.channels());
    pcm.sample_rate = track.codec == 2 ? adx.sample_rate : hca.sample_rate();
    if (pcm.samples.empty() || (pcm.channels != 1 && pcm.channels != 2 && pcm.channels != 6) ||
        (pcm.sample_rate != 48000 && pcm.sample_rate != 44100))
        throw std::runtime_error("Selected track has no PCM supported by the Windows AAC encoder");
    return pcm;
}

ComPtr<IMFMediaType> media_type(const GUID& major, const GUID& subtype) {
    ComPtr<IMFMediaType> type;
    check(MFCreateMediaType(&type), "Create media type");
    check(type->SetGUID(MF_MT_MAJOR_TYPE, major), "Set media type");
    check(type->SetGUID(MF_MT_SUBTYPE, subtype), "Set media codec");
    return type;
}

ComPtr<IMFMediaType> video_type(const GUID& codec, UINT32 w, UINT32 h, const UsmInfo& info) {
    auto type = media_type(MFMediaType_Video, codec);
    check(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, w, h), "Set video resolution");
    check(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, info.frame_rate_numerator, info.frame_rate_denominator), "Set frame rate");
    check(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "Set pixel aspect ratio");
    check(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "Set progressive video");
    return type;
}

DWORD add_video(IMFSinkWriter& writer, UINT32 w, UINT32 h, const UsmInfo& info) {
    DWORD stream = 0;
    auto output = video_type(MFVideoFormat_H264, w, h, info);
    const auto bitrate = UINT32(std::clamp(double(w) * h * info.frame_rate * 0.20, 1000000.0, 40000000.0));
    check(output->SetUINT32(MF_MT_AVG_BITRATE, bitrate), "Set H.264 bitrate");
    check(writer.AddStream(output.Get(), &stream), "Add H.264 video");
    auto input = video_type(MFVideoFormat_RGB32, w, h, info);
    check(input->SetUINT32(MF_MT_DEFAULT_STRIDE, UINT32(-int32_t(w * 4))), "Set packed RGB stride");
    check(writer.SetInputMediaType(stream, input.Get(), nullptr), "Configure H.264 encoder");
    return stream;
}

DWORD add_audio(IMFSinkWriter& writer, const PcmAudio& audio) {
    DWORD stream = 0;
    auto type = [&](const GUID& subtype) {
        auto mt = media_type(MFMediaType_Audio, subtype);
        check(mt->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, audio.channels), "Set audio channels");
        check(mt->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, audio.sample_rate), "Set audio sample rate");
        check(mt->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16), "Set audio sample depth");
        return mt;
    };
    auto output = type(MFAudioFormat_AAC);
    check(output->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, audio.channels == 6 ? 72000 : 24000), "Set AAC bitrate");
    check(writer.AddStream(output.Get(), &stream), "Add AAC audio");
    auto input = type(MFAudioFormat_PCM);
    const UINT32 step = audio.channels * sizeof(int16_t);
    check(input->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, step), "Set audio block alignment");
    check(input->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, audio.sample_rate * step), "Set PCM bitrate");
    check(writer.SetInputMediaType(stream, input.Get(), nullptr), "Configure AAC encoder");
    return stream;
}

void write_sample(IMFSinkWriter& writer, DWORD stream, std::span<const uint8_t> bytes,
                  LONGLONG start, LONGLONG end) {
    const DWORD size = DWORD(bytes.size());
    ComPtr<IMFMediaBuffer> buffer;
    check(MFCreateMemoryBuffer(size, &buffer), "Allocate encoder buffer");
    BYTE* target = nullptr;
    check(buffer->Lock(&target, nullptr, nullptr), "Lock encoder buffer");
    std::memcpy(target, bytes.data(), size);
    check(buffer->Unlock(), "Unlock encoder buffer");
    check(buffer->SetCurrentLength(size), "Set encoder buffer length");
    ComPtr<IMFSample> sample;
    check(MFCreateSample(&sample), "Create media sample");
    check(sample->AddBuffer(buffer.Get()), "Attach media sample buffer");
    check(sample->SetSampleTime(start), "Set media sample time");
    check(sample->SetSampleDuration(end - start), "Set media sample duration");
    check(writer.WriteSample(stream, sample.Get()), "Write MP4 sample");
}

std::string srt_time(double time) {
    const auto ms = std::max(0LL, std::llround(time * 1000));
    return std::format("{:02}:{:02}:{:02},{:03}", ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, ms % 1000);
}

bool write_subtitles(const fs::path& file, std::vector<MovieSubtitle> subs, const StringTable& strings, size_t language) {
    std::ranges::stable_sort(subs, {}, &MovieSubtitle::start);
    std::ofstream out;
    size_t index = 0;
    for (const auto& sub : subs) {
        const auto* entry = strings.find(sub.string_hash);
        if (!entry || entry->text[language].empty() || sub.duration <= 0) continue;
        if (!out.is_open()) {
            out.open(file, std::ios::binary);
            if (!out) throw std::runtime_error("Cannot open subtitle output");
        }
        out << ++index << "\r\n" << srt_time(sub.start) << " --> " << srt_time(sub.start + sub.duration)
            << "\r\n" << entry->text[language] << "\r\n\r\n";
    }
    if (out.is_open()) {
        out.close();
        if (!out) throw std::runtime_error("Cannot write subtitles");
    }
    return index != 0;
}

void publish(const fs::path& from, const fs::path& to, bool replace) {
    if (!MoveFileExW(from.c_str(), to.c_str(), replace ? MOVEFILE_REPLACE_EXISTING : 0))
        throw std::system_error(int(GetLastError()), std::system_category(), "Save exported file");
}

} // namespace

MovieExportResult export_movie(PackageManager& pm, const MovieEntry& movie, const MovieExport& settings,
                               const StringTable* strings, std::atomic<unsigned>& progress, const std::atomic<bool>& cancel) {
    if (!movie.error.empty()) throw std::runtime_error(movie.error);
    if (CompareStringOrdinal(settings.output.extension().c_str(), -1, L".mp4", -1, TRUE) != CSTR_EQUAL)
        throw std::runtime_error("The output name must end in .mp4");
    if (!settings.replace_mp4 && fs::exists(settings.output))
        throw std::runtime_error("The MP4 output already exists");
    const auto& info = movie.info;
    if (info.video_codec != 1 || !info.frame_rate_numerator || !info.frame_rate_denominator)
        throw std::runtime_error("Unsupported movie video format");
    if (settings.subtitle_language >= 0 && (!strings || settings.subtitle_language >= int(kTextLanguageCount)))
        throw std::runtime_error("Invalid subtitle language");
    auto srt_destination = settings.output;
    srt_destination.replace_extension(L".srt");
    ComSession com;
    MfSession mf;
    GUID id{};
    check(CoCreateGuid(&id), "Reserve temporary export name");
    wchar_t guid[39]{};
    if (!StringFromGUID2(id, guid, 39)) throw std::runtime_error("Cannot format temporary export name");
    const auto stem = settings.output.parent_path() / (std::wstring(L"nim-export-") + guid);
    TemporaryExport temp{stem.wstring() + L".mp4", stem.wstring() + L".srt"};

    PcmAudio audio;
    if (settings.channel >= 0) {
        const auto track = std::ranges::find(info.audio, settings.channel, &UsmInfo::Audio::channel);
        if (track == info.audio.end() || (track->codec != 2 && track->codec != 4))
            throw std::runtime_error("Unsupported movie audio track");
        audio = read_audio(pm, movie, *track, cancel);
    }
    if (cancel) return {.cancelled = true};
    progress = 8;
    MoviePlayer decoder(pm, nullptr, movie, -1);
    ComPtr<IMFSinkWriter> writer;
    UINT32 width = 0, height = 0, vm = 0, am = 0;
    size_t sound_frame = 0;
    const size_t total_audio = audio.channels ? audio.samples.size() / audio.channels : 0;
    std::vector<uint8_t> bgra;
    MovieExportResult result;
    constexpr LONGLONG ticks = 10'000'000;
    auto video_time = [&](uint64_t frame) {
        return LONGLONG(std::llround(double(frame) * ticks * info.frame_rate_denominator / info.frame_rate_numerator));
    };
    auto write_audio_until = [&](size_t stop) {
        stop = std::min(total_audio, stop);
        while (sound_frame < stop && !cancel) {
            const size_t last = std::min(stop, sound_frame + 2048);
            const auto* start = reinterpret_cast<const uint8_t*>(audio.samples.data() + sound_frame * audio.channels);
            write_sample(*writer.Get(), am, {start, (last - sound_frame) * audio.channels * sizeof(int16_t)},
                         LONGLONG(sound_frame * ticks / audio.sample_rate), LONGLONG(last * ticks / audio.sample_rate));
            sound_frame = last;
        }
    };

    while (!cancel) {
        auto frame = decoder.take_next_frame(std::chrono::milliseconds(250));
        if (!frame) {
            if (decoder.finished()) break;
            continue;
        }
        if (!writer) {
            width = (frame->width + 1u) & ~1u;
            height = (frame->height + 1u) & ~1u;
            if (!width || !height) throw std::runtime_error("Empty video frame");
            bgra.resize(size_t(width) * height * 4);
            check(MFCreateSinkWriterFromURL(temp.mp4.c_str(), nullptr, nullptr, &writer), "Open MP4 output");
            vm = add_video(*writer.Get(), width, height, info);
            if (audio.channels) am = add_audio(*writer.Get(), audio);
            check(writer->BeginWriting(), "Start MP4 encoder");
        }
        if (((frame->width + 1u) & ~1u) != width || ((frame->height + 1u) & ~1u) != height)
            throw std::runtime_error("Video resolution changed during export");
        for (UINT32 y = 0; y < frame->height; ++y) {
            auto* out = bgra.data() + size_t(height - 1 - y) * width * 4;
            const auto* pixel = frame->rgba.data() + size_t(y) * frame->width * 4;
            for (UINT32 x = 0; x < frame->width; ++x, out += 4, pixel += 4) {
                out[0] = pixel[2];
                out[1] = pixel[1];
                out[2] = pixel[0];
                out[3] = 255;
            }
        }
        const auto end = video_time(result.frames + 1);
        write_sample(*writer.Get(), vm, bgra, video_time(result.frames), end);
        write_audio_until(size_t(end * audio.sample_rate / ticks));
        ++result.frames;
        progress = unsigned(8 + std::min<uint64_t>(90, result.frames * 90 / std::max(1u, info.total_frames)));
    }
    if (cancel) return {.cancelled = true};
    if (const auto message = decoder.error(); !message.empty()) throw std::runtime_error(message);
    if (!writer) throw std::runtime_error("No video frames decoded");
    write_audio_until(total_audio);
    check(writer->Finalize(), "Finalize MP4 output");
    writer.Reset();
    if (cancel) return {.cancelled = true};

    if (settings.subtitle_language >= 0)
        result.subtitles = write_subtitles(temp.srt, decoder.subtitles(), *strings, size_t(settings.subtitle_language));
    if (cancel) return {.cancelled = true};
    if (result.subtitles) {
        if (fs::exists(srt_destination)) throw std::runtime_error("A matching SRT file already exists. Choose another MP4 filename.");
        publish(temp.srt, srt_destination, false);
    }
    try {
        publish(temp.mp4, settings.output, settings.replace_mp4);
    } catch (...) {
        if (result.subtitles) {
            std::error_code ec;
            fs::remove(srt_destination, ec);
        }
        throw;
    }
    progress = 100;
    return result;
}

} // namespace media
