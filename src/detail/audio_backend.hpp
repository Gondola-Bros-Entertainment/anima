#pragma once
// Private miniaudio helpers shared by anima::core's clip decoder and engine. The configuration macros that shape
// miniaudio's structures come from the anima_audio_dependencies target, so this header and the one translation
// unit that compiles miniaudio agree on them.
#include <anima/audio.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <miniaudio.h>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace anima::detail {
// Throws for a failed miniaudio call: std::bad_alloc when it ran out of memory, otherwise std::runtime_error naming
// the operation and miniaudio's description of the result.
inline void check_audio(ma_result result, const char *operation) {
    if (result == MA_SUCCESS)
        return;
    if (result == MA_OUT_OF_MEMORY)
        throw std::bad_alloc();
    throw std::runtime_error(std::string(operation) + ": " + ma_result_description(result));
}

// Opens a decoder that reads @p bytes, which must outlive it, as 32-bit float at the file's own channel count and
// sample rate. @p seek_points asks the MP3 decoder for a seek table of that many entries; the others ignore it.
inline ma_result open_audio_decoder(std::span<const std::byte> bytes, int encoding, ma_uint32 seek_points,
                                    ma_decoder &decoder) {
    auto config = ma_decoder_config_init(ma_format_f32, 0, 0);
    config.encodingFormat = static_cast<ma_encoding_format>(encoding);
    config.seekPointCount = seek_points;
    return ma_decoder_init_memory(bytes.data(), bytes.size(), &config, &decoder);
}

struct AudioClipAccess {
    // A decompressed clip of @p samples, or a streamed one of @p encoded; the caller has validated both.
    static std::shared_ptr<const AudioClip> create(std::vector<float> samples, std::vector<std::byte> encoded,
                                                   std::uint64_t frames, unsigned channels, unsigned rate,
                                                   int encoding) {
        auto clip = std::shared_ptr<AudioClip>(new AudioClip);
        clip->mode_ = encoded.empty() ? AudioLoadMode::decompress : AudioLoadMode::stream;
        clip->samples_ = std::move(samples);
        clip->encoded_ = std::move(encoded);
        clip->frames_ = frames;
        clip->channels_ = channels;
        clip->sample_rate_ = rate;
        clip->encoding_ = encoding;
        return clip;
    }
    static std::span<const float> samples(const AudioClip &clip) { return clip.samples_; }
    static std::span<const std::byte> encoded(const AudioClip &clip) { return clip.encoded_; }
    static int encoding(const AudioClip &clip) { return clip.encoding_; }
};
} // namespace anima::detail
