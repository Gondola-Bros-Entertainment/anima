#pragma once
#include "rejection.hpp"
#include <algorithm>
#include <anima/audio.hpp>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

// Plays clips through the public audio API alone: no miniaudio header, device library or engine asset code.
namespace audio_consumer {
inline void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
// A mono PCM16 WAV file of @p frames samples at 8000 Hz, each 8192 (a quarter of full scale).
inline std::vector<std::byte> wave(std::uint32_t frames) {
    std::vector<std::byte> bytes;
    const auto word = [&](std::uint32_t value, unsigned width) {
        for (unsigned i = 0; i < width; ++i)
            bytes.push_back(std::byte((value >> (8 * i)) & 255));
    };
    const auto tag = [&](const char *text) {
        for (int i = 0; i < 4; ++i)
            bytes.push_back(std::byte(text[i]));
    };
    tag("RIFF");
    word(36 + frames * 2, 4);
    tag("WAVE");
    tag("fmt ");
    word(16, 4);
    word(1, 2);    // PCM
    word(1, 2);    // mono
    word(8000, 4); // frames per second
    word(16000, 4);
    word(2, 2);
    word(16, 2);
    tag("data");
    word(frames * 2, 4);
    for (std::uint32_t i = 0; i < frames; ++i)
        word(8192, 2);
    return bytes;
}
inline float peak(std::span<const float> samples) {
    float result = 0;
    for (auto sample : samples)
        result = std::max(result, std::abs(sample));
    return result;
}
inline void run() {
    const auto file = wave(800);
    const auto decoded = anima::AudioClip::decode(file);
    const auto streamed = anima::AudioClip::decode(file, anima::AudioLoadMode::stream);
    check(decoded->frames() == 800 && streamed->frames() == 800 && decoded->channels() == 1 &&
              streamed->load_mode() == anima::AudioLoadMode::stream && decoded->memory_bytes() == 800 * sizeof(float) &&
              streamed->memory_bytes() == file.size(),
          "Independent audio consumer decoded a WAV file wrongly");

    // Offline: a decoded and a streamed voice of the same file mix identically.
    anima::Audio offline(8000, 1);
    std::vector<float> first(512), second(512);
    auto voice = offline.sound(decoded);
    voice.set_pan(-1);
    check(voice.play(), "Independent offline voice did not start");
    offline.render(first);
    auto stream = offline.sound(streamed);
    stream.set_pan(-1);
    stream.set_priority(200); // Outranks the playing voice for the engine's only voice.
    voice.play();
    check(stream.play() && !voice.playing(), "Independent voice limit did not steal the lower priority");
    offline.render(second);
    // Panned fully left, the mono clip plays at its own level on the left channel only.
    check(first == second && std::abs(peak(first) - .25F) < 1e-6F && first[3] == 0,
          "Independent decoded and streamed voices mixed differently");
    check(offline.voice_count() == 2, "Independent engine miscounted its voices");

    // A one-shot plays in a voice the engine owns, which the render that reaches the clip's end releases.
    anima::Audio fired(8000, 1);
    fired.play_one_shot(decoded, {.pan = -1});
    std::vector<float> third(2048);
    fired.render(third);
    check(std::abs(peak(third) - .25F) < 1e-6F && third[3] == 0 && fired.voice_count() == 0,
          "Independent one-shot did not play once in a released voice");

    // A device on the null backend mixes on its own thread while this one sleeps.
    auto device = anima::Audio::open_device(anima::AudioBackend::null);
    auto bus = device.bus();
    bus.set_volume(.5F);
    auto looping = device.sound(streamed, bus);
    looping.set_looping(true);
    check(looping.play(), "Independent device voice did not start");
    auto observed = looping.cursor();
    for (int wait = 0; wait < 200 && looping.cursor() == observed; ++wait)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check(looping.playing() && looping.cursor() != observed, "Independent device thread did not mix");
    std::array<float, 2> frame{};
    rejection::rejects<std::logic_error>([&] { device.render(frame); },
                                         "Audio with a device mixes on its device thread");
}
} // namespace audio_consumer
