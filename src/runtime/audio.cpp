#include "../detail/audio.hpp"
#include "../detail/audio_backend.hpp"
#include <algorithm>
#include <anima/audio.hpp>
#include <array>
#include <cmath>
#include <limits>
#include <mutex>
#include <tuple>

// Mixing and output on miniaudio's engine. Each voice is an ma_sound, reading an ma_audio_buffer_ref over a
// decompressed clip or a StreamSource decoding a streamed one, which feeds its own GainNode; that node mixes into
// its bus's GainNode, and so on up to the master GainNode on the engine's endpoint. The engine never owns a device:
// an Audio with a device runs its own ma_device, whose callback mixes through the same AudioState::mix as
// Audio::render. AudioState::mutex guards every miniaudio object of an engine and every field below that the mixing
// thread reads, and mix() holds it for each block, so miniaudio's own cross-thread mechanisms are never relied on.
namespace anima {
namespace detail {
namespace {
constexpr unsigned minimum_sample_rate = 8000, maximum_sample_rate = 192000;
constexpr unsigned maximum_voice_count = 4096, maximum_bus_depth = 16;
constexpr double maximum_fade_seconds = 24 * 60 * 60;
constexpr int default_priority = 128;
// Engine output is interleaved stereo.
constexpr ma_uint32 output_channels = 2;
constexpr std::size_t not_playing = std::numeric_limits<std::size_t>::max();

void validate_sample_rate(unsigned value) {
    if (value < minimum_sample_rate || value > maximum_sample_rate)
        throw std::invalid_argument("Audio sample rate must be between 8000 and 192000 Hz");
}
void validate_voice_limit(unsigned value) {
    if (!value || value > maximum_voice_count)
        throw std::invalid_argument("Audio voice limit must be in [1, 4096]");
}
// Channel factors that pan a nonspatial voice: equal power for a mono clip, which the engine has copied to both
// channels, and a balance that attenuates the opposite channel of a stereo clip.
std::array<float, 2> pan_factors(float pan, unsigned channels) {
    if (channels == 1)
        return {std::sqrt((1 - pan) * .5F), std::sqrt((1 + pan) * .5F)};
    return {pan > 0 ? std::sqrt(1 - pan) : 1.F, pan < 0 ? std::sqrt(1 + pan) : 1.F};
}
} // namespace

// A stereo gain stage that a voice or bus mixes through into its parent's stage, or the master through into the
// engine's endpoint. Each output sample is the input times a gain and a per-channel factor, each of which moves
// linearly to a new value over a given number of frames. The gain carries a voice's volume and fades or a bus's or
// the master's gain; the factors carry a voice's pan.
//
// miniaudio processes a node with an attached input in every block, with silence when nothing plays
// (ma_node_input_bus_read_pcm_frames reports every frame as read). A voice's stage therefore skips the blocks that
// begin with the voice stopped, which holds its ramps, and marks itself and every stage it mixes into as passing
// audio in the others; it cannot ask the sound, which miniaudio stops partway through the block in which it ends. The
// ramps of bus and master stages advance with output time.
struct GainNode {
    ma_node_base base; // First, so that miniaudio's ma_node pointer to this node also points to base.
    const std::uint64_t *blocks = nullptr; // The engine's count of mixed blocks.
    GainNode *parent = nullptr;            // The stage this one mixes into; null for the master.
    bool voice = false;                    // Whether a voice mixes through this stage, rather than buses.
    bool playing = false;     // For a voice's stage: whether the voice plays, as AudioState::playing lists.
    std::uint64_t passed = 0; // One more than the latest block in which a playing voice passed; or 0.
    double gain_from = 1, gain_to = 1;
    std::uint64_t gain_elapsed = 0, gain_frames = 0;
    bool fading = false; // Whether the gain ramp is a Sound::fade.
    std::array<float, 2> factor_from{1, 1}, factor_to{1, 1};
    std::uint64_t factor_elapsed = 0, factor_frames = 0;

    // Whether a playing voice's audio passed through the stage in the latest mixed block.
    [[nodiscard]] bool active() const { return passed && passed == *blocks; }
    [[nodiscard]] double gain() const {
        return gain_frames ? gain_from + (gain_to - gain_from) * (double(gain_elapsed) / double(gain_frames)) : gain_to;
    }
    [[nodiscard]] float factor(std::size_t channel) const {
        return factor_frames
                   ? factor_from[channel] + (factor_to[channel] - factor_from[channel]) *
                                                static_cast<float>(double(factor_elapsed) / double(factor_frames))
                   : factor_to[channel];
    }
    void ramp_gain(double target, std::uint64_t frames, bool fade) {
        gain_from = frames ? gain() : target;
        gain_to = target;
        gain_elapsed = 0;
        gain_frames = frames;
        fading = fade && frames;
    }
    // Ends the gain ramp: a fade stops at its current gain, and any other ramp completes.
    void settle_gain() {
        if (fading)
            gain_to = gain();
        gain_from = gain_to;
        gain_elapsed = gain_frames = 0;
        fading = false;
    }
    void ramp_factors(std::array<float, 2> target, std::uint64_t frames) {
        factor_from = frames ? std::array{factor(0), factor(1)} : target;
        factor_to = target;
        factor_elapsed = 0;
        factor_frames = frames;
    }
    void settle_factors() {
        factor_from = factor_to;
        factor_elapsed = factor_frames = 0;
    }
    void advance() {
        if (gain_frames && ++gain_elapsed == gain_frames) {
            gain_from = gain_to;
            gain_elapsed = gain_frames = 0;
            fading = false;
        }
        if (factor_frames && ++factor_elapsed == factor_frames) {
            factor_from = factor_to;
            factor_elapsed = factor_frames = 0;
        }
    }
};

namespace {
void process_gain(ma_node *node, const float **input, ma_uint32 *input_frames, float **output,
                  ma_uint32 *output_frames) {
    auto &stage = *static_cast<GainNode *>(node);
    const auto frames = std::min(*input_frames, *output_frames);
    *input_frames = *output_frames = frames;
    float *out = output[0];
    if (stage.voice) {
        if (!stage.playing) {
            std::fill_n(out, std::size_t(frames) * output_channels, 0.F);
            return;
        }
        for (auto *passing = &stage; passing; passing = passing->parent)
            passing->passed = *stage.blocks + 1;
    }
    const float *in = input[0];
    for (ma_uint32 frame = 0; frame < frames; ++frame) {
        const auto gain = stage.gain();
        for (std::size_t channel = 0; channel < output_channels; ++channel) {
            const auto at = std::size_t(frame) * output_channels + channel;
            out[at] = static_cast<float>(in[at] * gain * stage.factor(channel));
        }
        stage.advance();
    }
}
const ma_node_vtable gain_vtable{process_gain, nullptr, 1, 1, 0};

// Opens @p stage in @p engine's graph, mixing into @p parent, or into the engine's endpoint when @p parent is null.
// On failure nothing stays open.
void open_gain(GainNode &stage, ma_engine &engine, const std::uint64_t &blocks, GainNode *parent) {
    stage.blocks = &blocks;
    stage.parent = parent;
    const ma_uint32 channels[] = {output_channels};
    auto config = ma_node_config_init();
    config.vtable = &gain_vtable;
    config.pInputChannels = channels;
    config.pOutputChannels = channels;
    check_audio(ma_node_init(ma_engine_get_node_graph(&engine), &config, nullptr, &stage), "Create an audio mix node");
    const auto result = ma_node_attach_output_bus(
        &stage, 0, parent ? static_cast<ma_node *>(parent) : ma_engine_get_endpoint(&engine), 0);
    if (result != MA_SUCCESS) {
        ma_node_uninit(&stage, nullptr);
        check_audio(result, "Connect an audio mix node");
    }
}
} // namespace

struct CloseDecoder {
    void operator()(ma_decoder *decoder) const {
        ma_decoder_uninit(decoder);
        delete decoder;
    }
};
// A decoder on the heap: its backend keeps the decoder's address, so it must not move, but a seek can replace it.
using Decoder = std::unique_ptr<ma_decoder, CloseDecoder>;

// Opens a decoder at the start of the streamed @p clip. dr_mp3's seek tables would make its seeks shorter but not
// sample-exact, so none is built and a seek decodes from the start.
Decoder open_stream(const AudioClip &clip) {
    auto decoder = std::make_unique<ma_decoder>();
    check_audio(open_audio_decoder(AudioClipAccess::encoded(clip), AudioClipAccess::encoding(clip), 0, *decoder),
                "Open an audio stream");
    return Decoder(decoder.release());
}

// A data source that decodes a streamed clip for one voice. Decoding errors end the stream at that point, and
// non-finite samples are replaced by silence, so that malformed data cannot leave a voice reading without end or
// reach the mix.
struct StreamSource {
    ma_data_source_base base; // First, so that miniaudio's ma_data_source pointer also points to base.
    Decoder decoder;
    std::uint64_t frames = 0;
    ma_uint32 channels = 0, rate = 0;
    bool failed = false;
};

namespace {
ma_result stream_read(ma_data_source *source, void *output, ma_uint64 count, ma_uint64 *read) {
    auto &stream = *static_cast<StreamSource *>(source);
    *read = 0;
    if (stream.failed)
        return MA_AT_END;
    const auto result = ma_decoder_read_pcm_frames(stream.decoder.get(), output, count, read);
    const auto samples = std::span(static_cast<float *>(output), static_cast<std::size_t>(*read) * stream.channels);
    for (auto &sample : samples)
        if (!std::isfinite(sample))
            sample = 0;
    if (result != MA_SUCCESS && result != MA_AT_END)
        stream.failed = true;
    return *read ? MA_SUCCESS : MA_AT_END;
}
// A failed seek ends the stream instead of failing the read that loops it, which would leave the voice reading
// nothing without reaching its end.
ma_result stream_seek(ma_data_source *source, ma_uint64 frame) {
    auto &stream = *static_cast<StreamSource *>(source);
    stream.failed = ma_decoder_seek_to_pcm_frame(stream.decoder.get(), frame) != MA_SUCCESS;
    return MA_SUCCESS;
}
ma_result stream_format(ma_data_source *source, ma_format *format, ma_uint32 *channels, ma_uint32 *rate,
                        ma_channel *map, size_t capacity) {
    const auto &stream = *static_cast<StreamSource *>(source);
    if (format)
        *format = ma_format_f32;
    if (channels)
        *channels = stream.channels;
    if (rate)
        *rate = stream.rate;
    if (map && capacity)
        ma_channel_map_init_standard(ma_standard_channel_map_default, map, capacity, stream.channels);
    return MA_SUCCESS;
}
ma_result stream_cursor(ma_data_source *source, ma_uint64 *cursor) {
    return ma_decoder_get_cursor_in_pcm_frames(static_cast<StreamSource *>(source)->decoder.get(), cursor);
}
ma_result stream_length(ma_data_source *source, ma_uint64 *length) {
    *length = static_cast<StreamSource *>(source)->frames;
    return MA_SUCCESS;
}
const ma_data_source_vtable stream_vtable{
    stream_read, stream_seek, stream_format, stream_cursor, stream_length, nullptr, 0};
} // namespace

struct AudioState {
    std::mutex mutex;
    ma_device device{};
    ma_engine engine{};
    GainNode master{};
    bool device_open = false, engine_open = false, master_open = false;
    unsigned rate = 0, limit = 0;
    std::uint64_t smoothing_frames = 0;
    std::uint64_t blocks = 0, frames = 0; // Blocks and frames mixed so far.
    std::uint64_t admissions = 0;         // Voices that play() has admitted so far.
    std::size_t voices = 0;
    float volume = 1;
    std::vector<VoiceState *> playing;  // Voices admitted and not yet stopped; its capacity is the voice limit.
    std::vector<float> silence, primed; // Spatializer input and output for prime().

    AudioState() = default;
    AudioState(const AudioState &) = delete;
    AudioState &operator=(const AudioState &) = delete;
    ~AudioState();
    void open(unsigned sample_rate, unsigned maximum_voices);
    void mix(float *output, ma_uint64 count) noexcept;
    // The frames a change of @p stage ramps over: the smoothing time when a playing voice's audio passed through it
    // in the latest block, so that the change is heard, and none otherwise, so that no later voice hears it ramp.
    [[nodiscard]] std::uint64_t ramp(const GainNode &stage) const { return stage.active() ? smoothing_frames : 0; }
    void step_pitches();
    void retire_finished();
    void withdraw(VoiceState &voice);
    void halt(VoiceState &voice);
    void prime(VoiceState &voice);
};

struct AudioBusState {
    std::shared_ptr<AudioState> audio;
    std::shared_ptr<AudioBusState> parent;
    unsigned depth{};
    GainNode node{};
    bool node_open = false;
    float volume = 1;
    bool muted = false;

    AudioBusState() = default;
    AudioBusState(const AudioBusState &) = delete;
    AudioBusState &operator=(const AudioBusState &) = delete;
    ~AudioBusState() {
        if (node_open) {
            std::lock_guard lock(audio->mutex);
            ma_node_uninit(&node, nullptr);
        }
    }
};

struct VoiceState {
    std::shared_ptr<AudioState> audio;
    std::shared_ptr<AudioBusState> bus;
    std::shared_ptr<const AudioClip> clip;
    ma_audio_buffer_ref buffer{};
    StreamSource stream{};
    ma_sound sound{};
    GainNode output{};
    bool buffer_open = false, stream_open = false, sound_open = false, output_open = false;
    bool counted = false;
    float pitch_from = 1, pitch_to = 1;
    std::uint64_t pitch_elapsed = 0, pitch_frames = 0;
    float pan = 0;
    int priority = default_priority;
    bool spatial = false;
    std::uint64_t admitted = 0;     // The admission order of play(), for the voice limit's tie rule.
    std::size_t slot = not_playing; // Index in AudioState::playing.

    VoiceState() = default;
    VoiceState(const VoiceState &) = delete;
    VoiceState &operator=(const VoiceState &) = delete;
    ~VoiceState() {
        if (audio) {
            std::lock_guard lock(audio->mutex);
            audio->withdraw(*this);
            // Detaching the sound, then its gain stage, leaves nothing in the graph that refers to this voice.
            if (sound_open)
                ma_sound_uninit(&sound);
            if (output_open)
                ma_node_uninit(&output, nullptr);
            if (counted)
                --audio->voices;
        }
        if (stream_open)
            ma_data_source_uninit(&stream.base);
        if (buffer_open)
            ma_audio_buffer_ref_uninit(&buffer);
    }
    [[nodiscard]] ma_data_source *source() {
        return clip->load_mode() == AudioLoadMode::stream ? static_cast<ma_data_source *>(&stream)
                                                          : static_cast<ma_data_source *>(&buffer);
    }
    [[nodiscard]] bool playing() const { return ma_sound_is_playing(&sound) && !ma_sound_at_end(&sound); }
    // The frames a change ramps over: the smoothing time while the voice plays and played in the latest block.
    [[nodiscard]] std::uint64_t ramp() const { return slot != not_playing ? audio->ramp(output) : 0; }
    [[nodiscard]] float pitch() const {
        return pitch_frames ? pitch_from + (pitch_to - pitch_from) *
                                               static_cast<float>(double(pitch_elapsed) / double(pitch_frames))
                            : pitch_to;
    }
    void apply_pitch(float value) {
        pitch_from = pitch_to = value;
        pitch_elapsed = pitch_frames = 0;
        ma_sound_set_pitch(&sound, value);
    }
    // Frames of the clip the voice has taken for mixing: those its data source has delivered, less those its sound
    // still caches, modulo the clip when that crosses a loop.
    [[nodiscard]] std::uint64_t cursor() {
        const auto length = clip->frames();
        if (ma_sound_at_end(&sound))
            return length;
        ma_uint64 delivered = 0;
        if (ma_data_source_get_cursor_in_pcm_frames(source(), &delivered) != MA_SUCCESS)
            delivered = 0;
        const std::uint64_t cached = sound.processingCacheFramesRemaining;
        if (delivered >= cached)
            return std::min<std::uint64_t>(delivered - cached, length);
        return (delivered % length + length - cached % length) % length;
    }
    // Clears the end marker of a sound that finished, which ma_sound_start would otherwise answer by seeking to the
    // start. The voice finished, so it stays stopped.
    void clear_end() {
        if (!ma_sound_at_end(&sound))
            return;
        ma_sound_stop(&sound);
        ma_sound_start(&sound);
        ma_sound_stop(&sound);
    }
    // Drops what the sound took from its source before a seek: ma_engine_node_process_pcm_frames__sound plays the
    // frames in its processing cache before those it reads next, and the resampler holds the frames it last
    // interpolated between. The voice then continues as if it had just been created at its source's position.
    void forget_buffered() {
        sound.processingCacheFramesRemaining = 0;
        ma_resampler_reset(&sound.engineNode.resampler);
    }
    // Moves the voice to @p frame of its clip, keeping it playing or not. For a streamed clip only the start is
    // cheap to seek to; Sound::seek replaces the decoder for other positions.
    void seek(std::uint64_t frame) {
        clear_end();
        check_audio(ma_data_source_seek_to_pcm_frame(source(), frame), "Seek an audio voice");
        forget_buffered();
    }
};

AudioState::~AudioState() {
    // Stopping the device first ends the callback that uses the rest.
    if (device_open)
        ma_device_uninit(&device);
    if (master_open)
        ma_node_uninit(&master, nullptr);
    if (engine_open)
        ma_engine_uninit(&engine);
}
void AudioState::open(unsigned sample_rate, unsigned maximum_voices) {
    rate = sample_rate;
    limit = maximum_voices;
    smoothing_frames = static_cast<std::uint64_t>(std::llround(audio_smoothing_seconds * rate));
    playing.reserve(limit);
    silence.assign(smoothing_frames * output_channels, 0.F);
    primed.assign(smoothing_frames * output_channels, 0.F);
    auto config = ma_engine_config_init();
    config.noDevice = MA_TRUE;
    config.channels = output_channels;
    config.sampleRate = rate;
    // The spatializer sets new gains every block, which restarts its ramp from the current gain. A ramp longer
    // than a block would never finish, so gains would approach a new position geometrically; one block's ramp
    // reaches it by the end of each block.
    config.gainSmoothTimeInFrames = audio_block_frames;
    check_audio(ma_engine_init(&config, &engine), "Create the audio engine");
    engine_open = true;
    open_gain(master, engine, blocks, nullptr);
    master_open = true;
}
void AudioState::mix(float *output, ma_uint64 count) noexcept {
    while (count) {
        const std::lock_guard lock(mutex);
        const auto offset = frames % audio_block_frames;
        if (!offset)
            step_pitches();
        const auto block = std::min<ma_uint64>(count, audio_block_frames - offset);
        ma_uint64 read = 0;
        if (ma_engine_read_pcm_frames(&engine, output, block, &read) != MA_SUCCESS)
            read = 0;
        const auto samples = std::span(output, static_cast<std::size_t>(block) * output_channels);
        std::fill(samples.begin() + static_cast<std::ptrdiff_t>(read * output_channels), samples.end(), 0.F);
        for (auto &sample : samples)
            sample = std::isfinite(sample) ? std::clamp(sample, -1.F, 1.F) : 0.F;
        ++blocks;
        frames += block;
        retire_finished();
        output += samples.size();
        count -= block;
    }
}
// Advances the pitch ramps of playing voices by one block, at the start of a block.
void AudioState::step_pitches() {
    for (auto *voice : playing)
        if (voice->pitch_frames) {
            voice->pitch_elapsed = std::min(voice->pitch_elapsed + audio_block_frames, voice->pitch_frames);
            const auto value = voice->pitch();
            if (voice->pitch_elapsed == voice->pitch_frames)
                voice->apply_pitch(value);
            else
                ma_sound_set_pitch(&voice->sound, value);
        }
}
void AudioState::retire_finished() {
    for (std::size_t i = 0; i < playing.size();)
        if (playing[i]->playing())
            ++i;
        else
            withdraw(*playing[i]);
}
void AudioState::withdraw(VoiceState &voice) {
    if (voice.slot == not_playing)
        return;
    playing[voice.slot] = playing.back();
    playing[voice.slot]->slot = voice.slot;
    playing.pop_back();
    voice.slot = not_playing;
    voice.output.playing = false;
}
// Stops @p voice as Sound::stop does.
void AudioState::halt(VoiceState &voice) {
    ma_sound_stop(&voice.sound);
    voice.seek(0);
    voice.output.settle_gain();
    voice.output.settle_factors();
    voice.apply_pitch(voice.pitch_to);
    withdraw(voice);
}
// Runs a spatial voice's spatializer over silence for the smoothing time, so that its channel gains start from
// the current listener and position rather than ramping from those of its last output.
void AudioState::prime(VoiceState &voice) {
    ma_spatializer_process_pcm_frames(&voice.sound.engineNode.spatializer, &engine.listeners[0], primed.data(),
                                      silence.data(), smoothing_frames);
}
} // namespace detail

namespace {
void play_device(ma_device *device, void *output, const void *, ma_uint32 frames) {
    static_cast<detail::AudioState *>(device->pUserData)->mix(static_cast<float *>(output), frames);
}
// Backends to try for @p backend, in miniaudio's priority order.
std::vector<ma_backend> backends_for(AudioBackend backend) {
    if (backend == AudioBackend::null)
        return {ma_backend_null};
    if (backend != AudioBackend::platform)
        throw std::invalid_argument("Invalid audio backend");
    std::array<ma_backend, MA_BACKEND_COUNT> enabled{};
    std::size_t count = 0;
    detail::check_audio(ma_get_enabled_backends(enabled.data(), enabled.size(), &count), "List audio backends");
    std::vector<ma_backend> result;
    // The null backend would stand in silently for a missing device, and custom backends need callbacks.
    for (std::size_t i = 0; i < count; ++i)
        if (enabled[i] != ma_backend_null && enabled[i] != ma_backend_custom)
            result.push_back(enabled[i]);
    return result;
}
} // namespace

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
Audio::Audio(unsigned rate, unsigned maximum_voices) {
    detail::validate_sample_rate(rate);
    detail::validate_voice_limit(maximum_voices);
    auto engine = std::make_shared<detail::AudioState>();
    engine->open(rate, maximum_voices);
    state_ = std::move(engine);
}
Audio Audio::open_device(AudioBackend backend, unsigned maximum_voices) {
    detail::validate_voice_limit(maximum_voices);
    const auto backends = backends_for(backend);
    auto engine = std::make_shared<detail::AudioState>();
    auto config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = detail::output_channels;
    config.sampleRate = 0; // The device's own rate, so that miniaudio resamples nothing on the way out.
    config.dataCallback = play_device;
    config.pUserData = engine.get();
    config.noPreSilencedOutputBuffer = MA_TRUE; // mix() writes every frame.
    config.noClip = MA_TRUE;                    // mix() limits the output itself.
    detail::check_audio(
        ma_device_init_ex(backends.data(), static_cast<ma_uint32>(backends.size()), nullptr, &config, &engine->device),
        "Open the audio playback device");
    engine->device_open = true;
    engine->open(engine->device.sampleRate, maximum_voices);
    detail::check_audio(ma_device_start(&engine->device), "Start the audio playback device");
    return Audio(std::move(engine));
}
unsigned Audio::sample_rate() const { return state().rate; }
unsigned Audio::maximum_voices() const { return state().limit; }
std::size_t Audio::voice_count() const {
    auto &s = state();
    const std::lock_guard lock(s.mutex);
    return s.voices;
}
void Audio::volume(float value) {
    detail::audio_gain(value);
    auto &s = state();
    const std::lock_guard lock(s.mutex);
    s.volume = value;
    s.master.ramp_gain(value, s.ramp(s.master), false);
}
void Audio::listener(Vec3 position, Vec3 forward, Vec3 up) {
    detail::audio_location(position);
    (void)detail::audio_right(forward, up);
    auto &s = state();
    const std::lock_guard lock(s.mutex);
    ma_engine_listener_set_position(&s.engine, 0, position.x, position.y, position.z);
    ma_engine_listener_set_direction(&s.engine, 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(&s.engine, 0, up.x, up.y, up.z);
}
AudioBus Audio::bus(const AudioBus &parent) {
    auto &s = state();
    if (!owns(parent) || (parent.state_ && parent.state_->depth >= detail::maximum_bus_depth))
        throw std::invalid_argument("Foreign audio bus or bus depth exceeded");
    auto created = std::make_shared<detail::AudioBusState>();
    created->audio = state_;
    created->parent = parent.state_;
    created->depth = parent.state_ ? parent.state_->depth + 1 : 1;
    const std::lock_guard lock(s.mutex);
    detail::open_gain(created->node, s.engine, s.blocks, parent.state_ ? &parent.state_->node : &s.master);
    created->node_open = true;
    AudioBus result;
    result.state_ = std::move(created);
    return result;
}
void AudioBus::volume(float value) {
    detail::audio_gain(value);
    if (!state_)
        throw std::logic_error("Empty audio bus");
    auto &audio = *state_->audio;
    const std::lock_guard lock(audio.mutex);
    state_->volume = value;
    state_->node.ramp_gain(state_->muted ? 0 : value, audio.ramp(state_->node), false);
}
void AudioBus::muted(bool value) {
    if (!state_)
        throw std::logic_error("Empty audio bus");
    auto &audio = *state_->audio;
    const std::lock_guard lock(audio.mutex);
    state_->muted = value;
    state_->node.ramp_gain(value ? 0 : state_->volume, audio.ramp(state_->node), false);
}
Sound Audio::sound(std::shared_ptr<const AudioClip> clip, const AudioBus &bus) {
    auto &s = state();
    if (!clip || !owns(bus))
        throw std::invalid_argument("Missing audio clip or foreign bus");
    auto voice = std::make_shared<detail::VoiceState>();
    voice->audio = state_;
    voice->bus = bus.state_;
    voice->clip = std::move(clip);
    const auto &data = *voice->clip;
    // Opening a streamed clip's decoder parses its headers, so it happens before the engine is locked; nothing
    // refers to the source until the sound attaches below.
    if (data.load_mode() == AudioLoadMode::stream) {
        auto &stream = voice->stream;
        stream.frames = data.frames();
        stream.channels = data.channels();
        stream.rate = data.sample_rate();
        auto config = ma_data_source_config_init();
        config.vtable = &detail::stream_vtable;
        detail::check_audio(ma_data_source_init(&config, &stream.base), "Create an audio stream");
        voice->stream_open = true;
        stream.decoder = detail::open_stream(data);
    } else {
        const auto samples = detail::AudioClipAccess::samples(data);
        detail::check_audio(
            ma_audio_buffer_ref_init(ma_format_f32, data.channels(), samples.data(), data.frames(), &voice->buffer),
            "Create an audio buffer");
        voice->buffer_open = true;
        // ma_audio_buffer_ref_init leaves the rate at 0, which the sound would take for the engine's own.
        voice->buffer.sampleRate = data.sample_rate();
    }
    const std::lock_guard lock(s.mutex);
    auto config = ma_sound_config_init_2(&s.engine);
    config.pDataSource = voice->source();
    config.flags = MA_SOUND_FLAG_NO_DEFAULT_ATTACHMENT;
    detail::check_audio(ma_sound_init_ex(&s.engine, &config, &voice->sound), "Create an audio voice");
    voice->sound_open = true;
    detail::open_gain(voice->output, s.engine, s.blocks, voice->bus ? &voice->bus->node : &s.master);
    voice->output_open = true;
    voice->output.voice = true;
    detail::check_audio(ma_node_attach_output_bus(&voice->sound, 0, &voice->output, 0), "Connect an audio voice");
    ma_sound_set_spatialization_enabled(&voice->sound, MA_FALSE);
    ma_sound_set_doppler_factor(&voice->sound, 0);
    ma_sound_set_attenuation_model(&voice->sound, ma_attenuation_model_linear);
    ma_sound_set_min_distance(&voice->sound, 1);
    ma_sound_set_max_distance(&voice->sound, 100);
    voice->output.ramp_factors(detail::pan_factors(0, data.channels()), 0);
    ++s.voices;
    voice->counted = true;
    Sound result;
    result.state_ = std::move(voice);
    return result;
}
bool Audio::owns(const Sound &sound) const noexcept { return state_ && sound.state_ && sound.state_->audio == state_; }
bool Audio::owns(const AudioBus &bus) const noexcept { return state_ && (!bus.state_ || bus.state_->audio == state_); }
void Audio::render(std::span<float> output) {
    auto &s = state();
    if (s.device_open)
        throw std::logic_error("Audio with a device mixes on its device thread");
    if (output.size() % detail::output_channels)
        throw std::invalid_argument("Audio output must contain whole stereo frames");
    s.mix(output.data(), output.size() / detail::output_channels);
}

bool Sound::play() {
    auto &voice = state();
    auto &audio = *voice.audio;
    const std::lock_guard lock(audio.mutex);
    audio.retire_finished();
    if (voice.slot != detail::not_playing)
        return true;
    detail::VoiceState *victim = nullptr;
    if (audio.playing.size() >= audio.limit) {
        // The lowest priority stops, and of those the voice admitted longest ago, unless this one ranks lower.
        victim = *std::ranges::min_element(audio.playing, [](const auto *a, const auto *b) {
            return std::tie(a->priority, a->admitted) < std::tie(b->priority, b->admitted);
        });
        if (victim->priority > voice.priority)
            return false;
    }
    if (!ma_sound_is_looping(&voice.sound) && voice.cursor() >= voice.clip->frames())
        voice.seek(0);
    if (victim)
        audio.halt(*victim);
    if (voice.spatial)
        audio.prime(voice);
    detail::check_audio(ma_sound_start(&voice.sound), "Start an audio voice");
    voice.admitted = ++audio.admissions;
    voice.slot = audio.playing.size();
    audio.playing.push_back(&voice);
    voice.output.playing = true;
    return true;
}
void Sound::pause() {
    auto &voice = state();
    const std::lock_guard lock(voice.audio->mutex);
    ma_sound_stop(&voice.sound);
    voice.audio->withdraw(voice);
}
void Sound::stop() {
    auto &voice = state();
    const std::lock_guard lock(voice.audio->mutex);
    voice.audio->halt(voice);
}
bool Sound::playing() const {
    auto &voice = state();
    const std::lock_guard lock(voice.audio->mutex);
    return voice.playing();
}
double Sound::cursor() const {
    auto &voice = state();
    const std::lock_guard lock(voice.audio->mutex);
    return double(voice.cursor()) / voice.clip->sample_rate();
}
void Sound::seek(double seconds) {
    auto &voice = state();
    const auto &clip = *voice.clip;
    if (!std::isfinite(seconds) || seconds < 0 || seconds > clip.duration())
        throw std::invalid_argument("Audio seek lies outside the clip");
    const auto frame =
        std::min<std::uint64_t>(static_cast<std::uint64_t>(std::llround(seconds * clip.sample_rate())), clip.frames());
    if (clip.load_mode() == AudioLoadMode::stream) {
        // Seeking a decoder can mean decoding up to the new position, so a new decoder does it before the engine is
        // locked; the replaced one closes after the lock is released.
        auto decoder = detail::open_stream(clip);
        const bool failed = ma_decoder_seek_to_pcm_frame(decoder.get(), frame) != MA_SUCCESS;
        const std::lock_guard lock(voice.audio->mutex);
        voice.clear_end();
        std::swap(voice.stream.decoder, decoder);
        voice.stream.failed = failed;
        voice.forget_buffered();
        return;
    }
    const std::lock_guard lock(voice.audio->mutex);
    voice.seek(frame);
}
void Sound::looping(bool value) {
    auto &voice = state();
    const std::lock_guard lock(voice.audio->mutex);
    ma_sound_set_looping(&voice.sound, value ? MA_TRUE : MA_FALSE);
}
void Sound::volume(float value) {
    detail::audio_gain(value);
    auto &voice = state();
    const std::lock_guard lock(voice.audio->mutex);
    voice.output.ramp_gain(value, voice.ramp(), false);
}
void Sound::pitch(float value) {
    detail::audio_pitch(value);
    auto &voice = state();
    const std::lock_guard lock(voice.audio->mutex);
    if (const auto frames = voice.ramp()) {
        voice.pitch_from = voice.pitch();
        voice.pitch_to = value;
        voice.pitch_elapsed = 0;
        voice.pitch_frames = frames;
    } else {
        voice.apply_pitch(value);
    }
}
void Sound::pan(float value) {
    detail::audio_pan(value);
    auto &voice = state();
    const std::lock_guard lock(voice.audio->mutex);
    voice.pan = value;
    if (!voice.spatial)
        voice.output.ramp_factors(detail::pan_factors(value, voice.clip->channels()), voice.ramp());
}
void Sound::priority(int value) {
    detail::audio_priority(value);
    auto &voice = state();
    const std::lock_guard lock(voice.audio->mutex);
    voice.priority = value;
}
void Sound::spatial(bool value) {
    auto &voice = state();
    const std::lock_guard lock(voice.audio->mutex);
    if (voice.spatial == value)
        return;
    voice.spatial = value;
    ma_sound_set_spatialization_enabled(&voice.sound, value ? MA_TRUE : MA_FALSE);
    voice.output.ramp_factors(value ? std::array{1.F, 1.F} : detail::pan_factors(voice.pan, voice.clip->channels()), 0);
    if (value)
        voice.audio->prime(voice);
}
void Sound::position(Vec3 value) {
    detail::audio_location(value);
    auto &voice = state();
    const std::lock_guard lock(voice.audio->mutex);
    ma_sound_set_position(&voice.sound, value.x, value.y, value.z);
}
void Sound::attenuation(float minimum, float maximum, AudioRolloff rolloff) {
    detail::audio_attenuation(minimum, maximum, rolloff);
    auto &voice = state();
    const std::lock_guard lock(voice.audio->mutex);
    ma_sound_set_attenuation_model(&voice.sound, rolloff == AudioRolloff::inverse ? ma_attenuation_model_inverse
                                                                                  : ma_attenuation_model_linear);
    ma_sound_set_min_distance(&voice.sound, minimum);
    ma_sound_set_max_distance(&voice.sound, maximum);
}
void Sound::fade(float target, double seconds) {
    detail::audio_gain(target);
    if (!std::isfinite(seconds) || seconds < 0 || seconds > detail::maximum_fade_seconds)
        throw std::invalid_argument("Invalid audio fade duration");
    auto &voice = state();
    const std::lock_guard lock(voice.audio->mutex);
    voice.output.ramp_gain(target, static_cast<std::uint64_t>(std::llround(seconds * voice.audio->rate)), true);
}
} // namespace anima
