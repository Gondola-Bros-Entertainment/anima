#include "../detail/audio_backend.hpp"
#include <algorithm>
#include <anima/audio.hpp>
#include <array>
#include <cmath>
#include <string_view>

namespace anima {
namespace {
constexpr std::size_t maximum_samples = 32 * 1024 * 1024;
constexpr std::size_t maximum_encoded_bytes = 128 * 1024 * 1024;
constexpr unsigned minimum_sample_rate = 8000, maximum_sample_rate = 192000;
// Frames per decoder read while a clip is decoded or counted.
constexpr ma_uint64 decode_chunk_frames = 4096;
constexpr auto unsupported = "Unsupported or malformed audio data";

void validate_sample_rate(unsigned value) {
    if (value < minimum_sample_rate || value > maximum_sample_rate)
        throw std::invalid_argument("Audio sample rate must be between 8000 and 192000 Hz");
}
// The decoder for @p bytes, chosen by the container's leading bytes rather than by trial decoding: `RIFF`, `RIFX`,
// `RF64` and Wave64's `riff` are WAV, `fLaC` is FLAC and `OggS` is Ogg Vorbis. MP3 has no fixed signature (an ID3
// tag or a frame header comes first), so it takes everything else.
ma_encoding_format encoding_of(std::span<const std::byte> bytes) {
    const auto starts = [&](std::string_view magic) {
        return bytes.size() >= magic.size() &&
               std::equal(magic.begin(), magic.end(), bytes.begin(),
                          [](char expected, std::byte actual) { return std::byte(expected) == actual; });
    };
    if (starts("RIFF") || starts("RIFX") || starts("RF64") || starts("riff"))
        return ma_encoding_format_wav;
    if (starts("fLaC"))
        return ma_encoding_format_flac;
    if (starts("OggS"))
        return ma_encoding_format_vorbis;
    return ma_encoding_format_mp3;
}
struct Decoder {
    ma_decoder decoder{};
    bool open = false;
    Decoder() = default;
    Decoder(const Decoder &) = delete;
    Decoder &operator=(const Decoder &) = delete;
    ~Decoder() {
        if (open)
            ma_decoder_uninit(&decoder);
    }
};
// Reads @p decoder to its end, passing each chunk of interleaved samples to @p consume. Throws "Malformed audio
// data" when the decoder fails before the end.
template <class Consume> void read_all(ma_decoder &decoder, unsigned channels, Consume consume) {
    std::array<float, decode_chunk_frames * 2> chunk{};
    for (;;) {
        ma_uint64 read = 0;
        const auto result = ma_decoder_read_pcm_frames(&decoder, chunk.data(), decode_chunk_frames, &read);
        if (result != MA_SUCCESS && result != MA_AT_END)
            throw std::invalid_argument("Malformed audio data");
        consume(std::span<const float>(chunk).first(static_cast<std::size_t>(read) * channels));
        if (result == MA_AT_END || read == 0)
            return;
    }
}
} // namespace

std::shared_ptr<const AudioClip> AudioClip::pcm(std::vector<float> samples, unsigned channels, unsigned rate) {
    validate_sample_rate(rate);
    if ((channels != 1 && channels != 2) || samples.empty() || samples.size() > maximum_samples ||
        samples.size() % channels)
        throw std::invalid_argument("Invalid PCM channels or sample count");
    for (auto value : samples)
        if (!std::isfinite(value) || std::abs(value) > 1)
            throw std::invalid_argument("PCM samples must be finite and normalized to [-1, 1]");
    const auto frames = samples.size() / channels;
    return detail::AudioClipAccess::create(std::move(samples), {}, frames, channels, rate, ma_encoding_format_unknown);
}

std::shared_ptr<const AudioClip> AudioClip::decode(std::span<const std::byte> encoded, AudioLoadMode mode) {
    if (mode != AudioLoadMode::decompress && mode != AudioLoadMode::stream)
        throw std::invalid_argument("Invalid audio load mode");
    if (encoded.size() > maximum_encoded_bytes)
        throw std::invalid_argument("Encoded audio exceeds 128 MiB");
    const auto encoding = encoding_of(encoded);
    Decoder input;
    if (encoded.empty() || detail::open_audio_decoder(encoded, encoding, 0, input.decoder) != MA_SUCCESS)
        throw std::invalid_argument(unsupported);
    input.open = true;
    ma_format format{};
    ma_uint32 channels{}, rate{};
    if (ma_decoder_get_data_format(&input.decoder, &format, &channels, &rate, nullptr, 0) != MA_SUCCESS)
        throw std::invalid_argument(unsupported);
    if (channels != 1 && channels != 2)
        throw std::invalid_argument("Audio clips must have 1 or 2 channels");
    validate_sample_rate(rate);
    ma_uint64 stated = 0;
    if (ma_decoder_get_length_in_pcm_frames(&input.decoder, &stated) != MA_SUCCESS)
        stated = 0;
    if (mode == AudioLoadMode::decompress) {
        std::vector<float> samples;
        // The stated length only sizes the first allocation; the decoded frames decide the clip's length.
        if (stated && stated <= maximum_samples / channels)
            samples.reserve(static_cast<std::size_t>(stated) * channels);
        read_all(input.decoder, channels, [&](std::span<const float> chunk) {
            if (chunk.size() > maximum_samples - samples.size())
                throw std::invalid_argument("Decoded audio exceeds 33554432 samples");
            samples.insert(samples.end(), chunk.begin(), chunk.end());
        });
        if (samples.empty())
            throw std::invalid_argument("Audio data holds no samples");
        if (!std::ranges::all_of(samples, [](float value) { return std::isfinite(value); }))
            throw std::invalid_argument("Decoded audio samples must be finite");
        const auto frames = samples.size() / channels;
        return detail::AudioClipAccess::create(std::move(samples), {}, frames, channels, rate, encoding);
    }
    auto frames = stated;
    if (!frames)
        read_all(input.decoder, channels,
                 [&](std::span<const float> chunk) { frames += static_cast<ma_uint64>(chunk.size() / channels); });
    if (!frames)
        throw std::invalid_argument("Audio data holds no samples");
    return detail::AudioClipAccess::create({}, std::vector<std::byte>(encoded.begin(), encoded.end()), frames, channels,
                                           rate, encoding);
}
} // namespace anima
