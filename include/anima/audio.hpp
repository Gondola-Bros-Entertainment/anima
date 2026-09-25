#pragma once
#include <anima/core/math.hpp>
#include <cstddef>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace anima {
class AudioClip {
  public:
    static std::shared_ptr<const AudioClip> pcm(std::vector<float> samples, unsigned channels, unsigned sample_rate);
    // Little-endian RIFF/WAVE: mono/stereo PCM16 or IEEE float32, fully buffered.
    static std::shared_ptr<const AudioClip> wav(std::span<const std::byte> bytes);
    std::span<const float> samples() const { return samples_; }
    unsigned channels() const { return channels_; }
    unsigned sample_rate() const { return sample_rate_; }
    double duration() const { return double(samples_.size() / channels_) / sample_rate_; }

  private:
    AudioClip(std::vector<float>, unsigned channels, unsigned sample_rate);
    std::vector<float> samples_;
    unsigned channels_, sample_rate_;
};
namespace detail {
struct AudioState;
struct AudioBusState;
struct VoiceState;
struct AudioSceneAccess;
} // namespace detail
class AudioBus {
  public:
    AudioBus() = default;
    void volume(float gain);
    void muted(bool value);

  private:
    friend class Audio;
    std::shared_ptr<detail::AudioBusState> state_;
};
// Move-only voice ownership. Destruction stops this voice. Voices retain their
// clips, bus hierarchy and mixer state; they never reference a destroyed Audio.
class Sound {
  public:
    Sound() = default;
    Sound(Sound &&) noexcept = default;
    Sound &operator=(Sound &&) noexcept = default;
    Sound(const Sound &) = delete;
    Sound &operator=(const Sound &) = delete;
    void play();
    void pause();
    void stop();
    void seek(double seconds);
    bool playing() const;
    double cursor() const;
    void looping(bool value);
    void volume(float gain);
    void pitch(float rate);
    void pan(float value);
    void spatial(bool value);
    void position(Vec3 value);
    void attenuation(float minimum_distance, float maximum_distance);
    void fade(float target_gain, double seconds);

  private:
    friend class Audio;
    detail::VoiceState &state() const;
    std::shared_ptr<detail::VoiceState> state_;
};
// Anima's single-threaded stereo float mixer. Device IO is separate. Mutations
// and render run on the caller's thread; the mixer starts no threads or devices.
class Audio {
  public:
    explicit Audio(unsigned sample_rate = 48000, unsigned maximum_voices = 256);
    Audio(const Audio &) = delete;
    Audio &operator=(const Audio &) = delete;
    Audio(Audio &&) noexcept = default;
    Audio &operator=(Audio &&) noexcept = default;
    AudioBus bus(const AudioBus &parent = {});
    Sound sound(std::shared_ptr<const AudioClip> clip, const AudioBus &bus = {});
    [[nodiscard]] bool owns(const Sound &sound) const noexcept;
    void volume(float gain);
    void listener(Vec3 position, Vec3 forward = {0, 0, 1}, Vec3 up = {0, 1, 0});
    unsigned sample_rate() const;
    // Overwrites interleaved stereo samples, hard-limited to [-1, 1]. Linear
    // resampling, looping and fades advance by rendered samples, not wall time.
    void render(std::span<float> output);

  private:
    friend class AudioOutput;
    friend struct detail::AudioSceneAccess;
    explicit Audio(std::shared_ptr<detail::AudioState> state) : state_(std::move(state)) {}
    detail::AudioState &state() const;
    std::shared_ptr<detail::AudioState> state_;
};
} // namespace anima
