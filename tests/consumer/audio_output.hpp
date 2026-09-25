#pragma once
#include <anima/audio_output.hpp>
#include <memory>
#include <stdexcept>

// Uses only Anima's public audio API: no SDL header, window or engine asset code.
inline void consume_audio_output() {
    const auto check = [](bool value, const char *message) {
        if (!value)
            throw std::runtime_error(message);
    };
    anima::Sound voice;
    std::unique_ptr<anima::AudioOutput> output;
    {
        anima::Audio mixer(8000);
        voice = mixer.sound(anima::AudioClip::pcm(std::vector<float>(8000, .2F), 1, 8000));
        voice.play();
        output = std::make_unique<anima::AudioOutput>(mixer, .02);
        output->paused(true);
    }
    output->pump();
    const double cursor = voice.cursor();
    check(cursor > .019 && cursor <= .021 && output->queued_seconds() > 0 && output->queued_seconds() <= .021,
          "Independent audio output did not retain and mix its destroyed owner");
    output->pump();
    check(voice.cursor() == cursor, "Independent full queue advanced the mixer");
    anima::AudioOutput moved(std::move(*output));
    output.reset();
    moved.clear();
    check(moved.queued_seconds() == 0 && voice.cursor() == cursor, "Independent output clear changed the voice cursor");
    moved.pump();
    check(voice.cursor() > cursor, "Independent moved output lost its mixer");
    moved.paused(false);
    moved.paused(true);
}
