#include <SDL3/SDL.h>
#include <anima/audio_output.hpp>
#include <array>
#include <string>

namespace anima {
namespace {
constexpr double minimum_queue_seconds = .005, maximum_queue_seconds = .25;
// The mixer renders interleaved stereo, queued in blocks of this many samples.
constexpr int output_channels = 2;
constexpr std::size_t block_samples = 1024;
void check(bool ok, const char *operation) {
    if (!ok)
        throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
}
} // namespace
struct AudioOutput::State {
    Audio mixer;
    SDL_AudioStream *stream{};
    std::size_t target_frames{};
    bool initialized{};
    explicit State(Audio &audio) : mixer(audio.state_) {}
    ~State() {
        if (stream)
            SDL_DestroyAudioStream(stream);
        if (initialized)
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
};
AudioOutput::AudioOutput(Audio &audio, double seconds) : state_(std::make_unique<State>(audio)) {
    if (!std::isfinite(seconds) || seconds < minimum_queue_seconds || seconds > maximum_queue_seconds)
        throw std::invalid_argument("Audio queue duration must be between 5 and 250 milliseconds");
    auto &s = *state_;
    const auto rate = s.mixer.sample_rate();
    s.target_frames = static_cast<std::size_t>(std::ceil(seconds * rate));
    check(SDL_InitSubSystem(SDL_INIT_AUDIO), "Initialize SDL audio");
    s.initialized = true;
    SDL_AudioSpec format{};
    format.format = SDL_AUDIO_F32;
    format.channels = output_channels;
    format.freq = static_cast<int>(rate);
    s.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &format, nullptr, nullptr);
    check(s.stream != nullptr, "Open SDL audio output");
    check(SDL_ResumeAudioStreamDevice(s.stream), "Resume SDL audio output");
}
AudioOutput::~AudioOutput() = default;
AudioOutput::AudioOutput(AudioOutput &&) noexcept = default;
AudioOutput &AudioOutput::operator=(AudioOutput &&) noexcept = default;
AudioOutput::State &AudioOutput::state() const {
    if (!state_)
        throw std::logic_error("Moved-from audio output");
    return *state_;
}
double AudioOutput::queued_seconds() const {
    auto &s = state();
    const auto queued = SDL_GetAudioStreamQueued(s.stream);
    check(queued >= 0, "Read SDL audio queue");
    return double(queued) / (2 * sizeof(float) * s.mixer.sample_rate());
}
void AudioOutput::pump() {
    auto &s = state();
    const auto queued = SDL_GetAudioStreamQueued(s.stream);
    check(queued >= 0, "Read SDL audio queue");
    const auto queued_frames = static_cast<std::size_t>(queued) / (output_channels * sizeof(float));
    auto remaining = s.target_frames > queued_frames ? s.target_frames - queued_frames : 0;
    std::array<float, block_samples> samples{};
    while (remaining) {
        const auto frames = std::min(remaining, samples.size() / output_channels);
        const auto block = std::span<float>(samples).first(frames * output_channels);
        s.mixer.render(block);
        check(SDL_PutAudioStreamData(s.stream, block.data(), static_cast<int>(block.size_bytes())),
              "Queue mixed audio");
        remaining -= frames;
    }
}
void AudioOutput::paused(bool value) {
    auto &s = state();
    check(value ? SDL_PauseAudioStreamDevice(s.stream) : SDL_ResumeAudioStreamDevice(s.stream),
          "Change SDL audio pause state");
}
void AudioOutput::clear() { check(SDL_ClearAudioStream(state().stream), "Clear SDL audio queue"); }
} // namespace anima
