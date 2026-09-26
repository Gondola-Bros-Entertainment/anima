#include "../detail/audio.hpp"
#include <anima/audio.hpp>
#include <bit>
#include <climits>
#include <cstdint>
#include <limits>
#include <utility>

namespace anima {
namespace {
constexpr std::size_t maximum_samples = 32 * 1024 * 1024;
constexpr std::size_t maximum_wav_bytes = 128 * 1024 * 1024;
constexpr unsigned minimum_sample_rate = 8000, maximum_sample_rate = 192000;
constexpr unsigned maximum_voice_count = 4096, maximum_bus_depth = 16;
constexpr double maximum_fade_seconds = 24 * 60 * 60;
// Little-endian RIFF chunk identifiers and WAVE format tags.
constexpr std::uint32_t riff_id = 0x46464952, wave_id = 0x45564157;
constexpr std::uint32_t format_id = 0x20746d66, data_id = 0x61746164;
constexpr unsigned pcm_encoding = 1, float_encoding = 3;
// A RIFF file is one chunk: an 8-byte header (identifier, then the size of what follows), the WAVE form type and
// the WAVE chunks, each with the same header.
constexpr std::size_t chunk_header_bytes = 8, chunk_size_offset = 4, form_type_bytes = 4;
constexpr std::size_t riff_header_bytes = chunk_header_bytes + form_type_bytes;
// Little-endian fields of the WAVE format chunk, and its size up to the last of them.
namespace format_chunk {
struct Field {
    std::size_t offset;
    unsigned bytes;
};
constexpr Field tag{0, 2}, channels{2, 2}, sample_rate{4, 4}, byte_rate{8, 4}, block_align{12, 2},
    bits_per_sample{14, 2};
constexpr std::size_t bytes = bits_per_sample.offset + bits_per_sample.bytes;
} // namespace format_chunk
constexpr unsigned pcm16_bits = 16, float32_bits = 32;
// A PCM16 sample divided by this lies in [-1, 1).
constexpr float pcm16_full_scale = -float(std::numeric_limits<std::int16_t>::min());
void validate_sample_rate(unsigned value) {
    if (value < minimum_sample_rate || value > maximum_sample_rate)
        throw std::invalid_argument("Audio sample rate must be between 8000 and 192000 Hz");
}
std::uint32_t word(std::span<const std::byte> bytes, std::size_t at, unsigned width = 4) {
    if (at > bytes.size() || width > bytes.size() - at)
        throw std::invalid_argument("Truncated WAVE field");
    std::uint32_t value{};
    for (unsigned i = 0; i < width; ++i)
        value |= std::uint32_t(std::to_integer<unsigned char>(bytes[at + i])) << (CHAR_BIT * i);
    return value;
}
std::uint32_t word(std::span<const std::byte> bytes, format_chunk::Field field) {
    return word(bytes, field.offset, field.bytes);
}
} // namespace
AudioClip::AudioClip(std::vector<float> samples, unsigned channels, unsigned rate)
    : samples_(std::move(samples)), channels_(channels), sample_rate_(rate) {}
std::shared_ptr<const AudioClip> AudioClip::pcm(std::vector<float> samples, unsigned channels, unsigned rate) {
    validate_sample_rate(rate);
    if ((channels != 1 && channels != 2) || samples.empty() || samples.size() > maximum_samples ||
        samples.size() % channels)
        throw std::invalid_argument("Invalid PCM channels or sample count");
    for (auto value : samples)
        if (!std::isfinite(value) || std::abs(value) > 1)
            throw std::invalid_argument("PCM samples must be finite and normalized to [-1, 1]");
    return std::shared_ptr<const AudioClip>(new AudioClip(std::move(samples), channels, rate));
}
std::shared_ptr<const AudioClip> AudioClip::wav(std::span<const std::byte> bytes) {
    if (bytes.size() < riff_header_bytes || bytes.size() > maximum_wav_bytes || word(bytes, 0) != riff_id ||
        word(bytes, chunk_header_bytes) != wave_id ||
        word(bytes, chunk_size_offset) != bytes.size() - chunk_header_bytes)
        throw std::invalid_argument("Invalid or unsupported RIFF/WAVE container");
    std::span<const std::byte> format, data;
    bool have_format = false, have_data = false;
    for (std::size_t at = riff_header_bytes; at < bytes.size();) {
        const auto kind = word(bytes, at), size = word(bytes, at + chunk_size_offset);
        at += chunk_header_bytes;
        if (size > bytes.size() - at || (size % 2 && size == bytes.size() - at))
            throw std::invalid_argument("Truncated WAVE chunk or padding");
        if (kind == format_id) {
            if (have_format)
                throw std::invalid_argument("Duplicate WAVE format");
            format = bytes.subspan(at, size);
            have_format = true;
        } else if (kind == data_id) {
            if (have_data)
                throw std::invalid_argument("Duplicate WAVE data");
            data = bytes.subspan(at, size);
            have_data = true;
        }
        at += size + size % 2;
    }
    if (!have_format || !have_data || format.size() < format_chunk::bytes)
        throw std::invalid_argument("Missing WAVE format or data");
    const auto encoding = word(format, format_chunk::tag), channels = word(format, format_chunk::channels),
               rate = word(format, format_chunk::sample_rate);
    const auto bits = word(format, format_chunk::bits_per_sample);
    validate_sample_rate(rate);
    if ((encoding != pcm_encoding || bits != pcm16_bits) && (encoding != float_encoding || bits != float32_bits))
        throw std::invalid_argument("Only PCM16 and float32 WAVE are supported");
    const auto width = bits / CHAR_BIT;
    if ((channels != 1 && channels != 2) || word(format, format_chunk::block_align) != channels * width ||
        word(format, format_chunk::byte_rate) != rate * channels * width || data.empty() ||
        data.size() % (channels * width) || data.size() / width > maximum_samples)
        throw std::invalid_argument("Invalid WAVE layout or decoded sample count");
    std::vector<float> samples;
    samples.reserve(data.size() / width);
    for (std::size_t at = 0; at < data.size(); at += width) {
        const auto raw = word(data, at, width);
        samples.push_back(encoding == float_encoding
                              ? std::bit_cast<float>(raw)
                              : std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(raw)) / pcm16_full_scale);
    }
    return pcm(std::move(samples), channels, rate);
}
namespace detail {
struct AudioState {
    unsigned rate{}, limit{};
    float volume = 1;
    Vec3 position{}, right{1, 0, 0};
    std::vector<std::weak_ptr<VoiceState>> voices;
};
struct AudioBusState {
    std::shared_ptr<AudioState> audio;
    std::shared_ptr<AudioBusState> parent;
    unsigned depth{};
    float volume = 1;
    bool muted{};
};
struct VoiceState {
    std::shared_ptr<AudioState> audio;
    std::shared_ptr<AudioBusState> bus;
    std::shared_ptr<const AudioClip> clip;
    double frame{}, fade_from{}, fade_to{}, fade_elapsed{}, fade_duration{};
    float volume = 1, pitch = 1, pan{}, minimum_distance = 1, maximum_distance = 100;
    bool playing{}, looping{}, spatial{};
    Vec3 position{};
};
} // namespace detail
detail::AudioState &Audio::state() const {
    if (!state_)
        throw std::logic_error("Moved-from audio mixer");
    return *state_;
}
detail::VoiceState &Sound::state() const {
    if (!state_)
        throw std::logic_error("Empty audio voice");
    return *state_;
}
Audio::Audio(unsigned rate, unsigned maximum_voices) : state_(std::make_shared<detail::AudioState>()) {
    validate_sample_rate(rate);
    if (!maximum_voices || maximum_voices > maximum_voice_count)
        throw std::invalid_argument("Audio voice limit must be in [1, 4096]");
    state_->rate = rate;
    state_->limit = maximum_voices;
    state_->voices.reserve(maximum_voices);
}
unsigned Audio::sample_rate() const { return state().rate; }
void Audio::volume(float value) {
    detail::audio_gain(value);
    state().volume = value;
}
void Audio::listener(Vec3 position, Vec3 forward, Vec3 up) {
    detail::audio_location(position);
    const auto right = detail::audio_right(forward, up);
    auto &s = state();
    s.position = position;
    s.right = right;
}
AudioBus Audio::bus(const AudioBus &parent) {
    (void)state();
    if (!owns(parent) || (parent.state_ && parent.state_->depth >= maximum_bus_depth))
        throw std::invalid_argument("Foreign audio bus or bus depth exceeded");
    AudioBus result;
    result.state_ = std::make_shared<detail::AudioBusState>();
    result.state_->audio = state_;
    result.state_->parent = parent.state_;
    result.state_->depth = parent.state_ ? parent.state_->depth + 1 : 1;
    return result;
}
void AudioBus::volume(float value) {
    detail::audio_gain(value);
    if (!state_)
        throw std::logic_error("Empty audio bus");
    state_->volume = value;
}
void AudioBus::muted(bool value) {
    if (!state_)
        throw std::logic_error("Empty audio bus");
    state_->muted = value;
}
Sound Audio::sound(std::shared_ptr<const AudioClip> clip, const AudioBus &bus) {
    auto &s = state();
    if (!clip || !owns(bus))
        throw std::invalid_argument("Missing audio clip or foreign bus");
    std::erase_if(s.voices, [](const auto &voice) { return voice.expired(); });
    if (s.voices.size() >= s.limit)
        throw std::length_error("Audio voice limit exceeded");
    Sound result;
    result.state_ = std::make_shared<detail::VoiceState>();
    result.state_->audio = state_;
    result.state_->bus = bus.state_;
    result.state_->clip = std::move(clip);
    s.voices.push_back(result.state_);
    return result;
}
bool Audio::owns(const Sound &sound) const noexcept { return state_ && sound.state_ && sound.state_->audio == state_; }
bool Audio::owns(const AudioBus &bus) const noexcept { return state_ && (!bus.state_ || bus.state_->audio == state_); }
void Sound::play() {
    auto &s = state();
    if (s.frame >= double(s.clip->samples().size() / s.clip->channels()))
        s.frame = 0;
    s.playing = true;
}
void Sound::pause() { state().playing = false; }
void Sound::stop() {
    auto &s = state();
    s.playing = false;
    s.frame = 0;
    s.fade_duration = 0;
}
bool Sound::playing() const { return state().playing; }
double Sound::cursor() const {
    const auto &s = state();
    return s.frame / s.clip->sample_rate();
}
void Sound::seek(double seconds) {
    auto &s = state();
    if (!std::isfinite(seconds) || seconds < 0 || seconds > s.clip->duration())
        throw std::invalid_argument("Audio seek lies outside the clip");
    s.frame = seconds * s.clip->sample_rate();
}
void Sound::looping(bool value) { state().looping = value; }
void Sound::volume(float value) {
    detail::audio_gain(value);
    auto &s = state();
    s.volume = value;
    s.fade_duration = 0;
}
void Sound::pitch(float value) {
    detail::audio_pitch(value);
    state().pitch = value;
}
void Sound::pan(float value) {
    detail::audio_pan(value);
    state().pan = value;
}
void Sound::spatial(bool value) { state().spatial = value; }
void Sound::position(Vec3 value) {
    detail::audio_location(value);
    state().position = value;
}
void Sound::attenuation(float minimum, float maximum) {
    detail::audio_attenuation(minimum, maximum);
    auto &s = state();
    s.minimum_distance = minimum;
    s.maximum_distance = maximum;
}
void Sound::fade(float target, double seconds) {
    detail::audio_gain(target);
    if (!std::isfinite(seconds) || seconds < 0 || seconds > maximum_fade_seconds)
        throw std::invalid_argument("Invalid audio fade duration");
    auto &s = state();
    s.fade_from = s.volume;
    s.fade_to = target;
    s.fade_elapsed = 0;
    s.fade_duration = seconds;
    if (seconds == 0)
        s.volume = target;
}
void Audio::render(std::span<float> output) {
    auto &audio = state();
    if (output.size() % 2)
        throw std::invalid_argument("Audio output must contain whole stereo frames");
    std::fill(output.begin(), output.end(), 0.F);
    for (auto &entry : audio.voices) {
        auto owner = entry.lock();
        if (!owner || !owner->playing)
            continue;
        auto &voice = *owner;
        double bus_gain = audio.volume;
        for (auto bus = voice.bus; bus; bus = bus->parent)
            bus_gain *= bus->muted ? 0 : bus->volume;
        float pan = voice.pan;
        if (voice.spatial) {
            const auto offset = voice.position - audio.position;
            const auto distance = length(offset);
            pan = distance > detail::audio_direction_epsilon
                      ? std::clamp(dot(offset * (1 / distance), audio.right), -1.F, 1.F)
                      : 0;
            bus_gain *=
                1 - std::clamp((distance - voice.minimum_distance) / (voice.maximum_distance - voice.minimum_distance),
                               0.F, 1.F);
        }
        const bool mono = voice.spatial || voice.clip->channels() == 1;
        const float left = mono ? std::sqrt((1 - pan) * .5F) : (pan > 0 ? std::sqrt(1 - pan) : 1);
        const float right = mono ? std::sqrt((1 + pan) * .5F) : (pan < 0 ? std::sqrt(1 + pan) : 1);
        const auto samples = voice.clip->samples();
        const auto channels = voice.clip->channels();
        const auto frames = samples.size() / channels;
        const double advance = double(voice.clip->sample_rate()) / audio.rate * voice.pitch;
        for (std::size_t out = 0; out < output.size(); out += 2) {
            if (voice.frame >= double(frames)) {
                if (voice.looping)
                    voice.frame = std::fmod(voice.frame, double(frames));
                else {
                    voice.playing = false;
                    voice.frame = double(frames);
                    break;
                }
            }
            const auto a = static_cast<std::size_t>(voice.frame);
            const auto b = a + 1 < frames ? a + 1 : (voice.looping ? 0 : a);
            const auto alpha = static_cast<float>(voice.frame - double(a));
            const auto sample = [&](unsigned channel) {
                return samples[a * channels + channel] * (1 - alpha) + samples[b * channels + channel] * alpha;
            };
            const float l = sample(0), r = channels == 1 ? l : sample(1);
            const double amplitude = bus_gain * voice.volume;
            output[out] += static_cast<float>((mono ? (l + r) * .5 : l) * left * amplitude);
            output[out + 1] += static_cast<float>((mono ? (l + r) * .5 : r) * right * amplitude);
            voice.frame += advance;
            if (voice.fade_duration > 0) {
                voice.fade_elapsed = std::min(voice.fade_elapsed + 1.0 / audio.rate, voice.fade_duration);
                const double t = voice.fade_elapsed / voice.fade_duration;
                voice.volume = static_cast<float>(voice.fade_from * (1 - t) + voice.fade_to * t);
                if (voice.fade_elapsed == voice.fade_duration)
                    voice.fade_duration = 0;
            }
        }
        if (voice.frame >= double(frames)) {
            if (voice.looping)
                voice.frame = std::fmod(voice.frame, double(frames));
            else {
                voice.playing = false;
                voice.frame = double(frames);
            }
        }
    }
    for (auto &sample : output)
        sample = std::clamp(sample, -1.F, 1.F);
}
} // namespace anima
