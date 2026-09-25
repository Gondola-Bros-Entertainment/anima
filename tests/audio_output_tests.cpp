#include <SDL3/SDL.h>
#include <anima/audio_output.hpp>
#include <iostream>
#include <limits>
#include <optional>

namespace {
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <class Exception, class F> void rejects(F action) {
    try {
        action();
    } catch (const Exception &) {
        return;
    }
    throw std::runtime_error("Audio output accepted an invalid operation");
}
void verify_queue(anima::AudioOutput &device) {
    device.paused(true);
    device.pump();
    require(device.queued_seconds() > .024 && device.queued_seconds() <= .026,
            "Audio output did not respect its queue target");
}
} // namespace

int main() {
    try {
        require(SDL_WasInit(0) == 0, "Test started with an SDL subsystem");
        anima::Audio audio(8000);
        for (const double seconds :
             {0., .004, .251, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
            rejects<std::invalid_argument>([&] { anima::AudioOutput invalid(audio, seconds); });
            require(SDL_WasInit(0) == 0, "Invalid queue duration initialized SDL");
        }
        auto sound = audio.sound(anima::AudioClip::pcm(std::vector<float>(8000, .1F), 1, 8000));
        sound.play();
        {
            anima::AudioOutput device(audio, .025);
            require(SDL_WasInit(SDL_INIT_AUDIO) == SDL_INIT_AUDIO && !SDL_WasInit(SDL_INIT_VIDEO),
                    "Audio output initialized video or failed to initialize audio");
            verify_queue(device);
            const auto cursor = sound.cursor();
            require(cursor > .024 && cursor <= .026, "Device pump did not advance the retained mixer");
            device.pump();
            require(sound.cursor() == cursor, "Full audio queue advanced the mixer");
            device.clear();
            require(device.queued_seconds() == 0 && sound.cursor() == cursor,
                    "Clearing the queue failed or rewound the voice");
            anima::AudioOutput moved(std::move(device));
            rejects<std::logic_error>([&] { device.pump(); });
            rejects<std::logic_error>([&] { device.clear(); });
            rejects<std::logic_error>([&] { device.paused(true); });
            rejects<std::logic_error>([&] { (void)device.queued_seconds(); });
            verify_queue(moved);
            require(sound.cursor() > cursor, "Moved output lost its mixer");
            {
                anima::AudioOutput replacement(audio, .025);
                replacement = std::move(moved);
                rejects<std::logic_error>([&] { moved.pump(); });
                replacement.clear();
                verify_queue(replacement);
            }
            require(SDL_WasInit(SDL_INIT_AUDIO) == 0, "Move assignment leaked SDL audio ownership");
        }
        require(SDL_WasInit(0) == 0, "Output destruction retained an SDL subsystem");

        // Output and voice own the mixer context independently of its wrapper.
        std::optional<anima::AudioOutput> retained;
        anima::Sound retained_sound;
        {
            anima::Audio temporary(8000);
            retained_sound = temporary.sound(anima::AudioClip::pcm(std::vector<float>(8000, .2F), 1, 8000));
            retained_sound.play();
            retained.emplace(temporary, .025);
            retained->paused(true);
            anima::Audio moved(std::move(temporary));
        }
        verify_queue(*retained);
        require(retained_sound.cursor() > 0, "Destroyed mixer wrapper invalidated output");
        retained.reset();
        require(SDL_WasInit(0) == 0, "Retained output leaked SDL ownership");

        // Each output releases only its own subsystem reference.
        require(SDL_InitSubSystem(SDL_INIT_AUDIO), "Could not acquire the caller's SDL audio reference");
        {
            anima::AudioOutput caller_owned(audio, .025);
            verify_queue(caller_owned);
            {
                anima::AudioOutput second(audio, .025);
                second.paused(true);
            }
            caller_owned.clear();
            verify_queue(caller_owned);
        }
        require(SDL_WasInit(SDL_INIT_AUDIO) == SDL_INIT_AUDIO, "Output destruction shut down caller-owned SDL audio");
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        require(SDL_WasInit(0) == 0, "Final SDL audio reference did not release");
        std::cout << "PASS SDL audio-only queues, moves, mixer lifetime and subsystem ownership\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
