#include <SDL3/SDL.h>
#include <anima/audio_output.hpp>
#include <doctest/doctest.h>

#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
constexpr double queue_target = .025;
// A pump to the target queues, and advances a voice by, more than queue_floor and at most queue_ceiling seconds.
constexpr double queue_floor = .024, queue_ceiling = .026;
constexpr auto moved_from = "Moved-from audio output";

// Pauses @p output so that the device consumes nothing, then checks that a pump fills the queue to its target.
void verify_queue(anima::AudioOutput &output) {
    output.paused(true);
    output.pump();
    const auto queued = output.queued_seconds();
    CHECK(queued > queue_floor);
    CHECK(queued <= queue_ceiling);
}
} // namespace

TEST_CASE("Queue targets outside [5, 250] ms are rejected before SDL is initialized") {
    REQUIRE(SDL_WasInit(0) == 0u);
    anima::Audio audio(8000);
    for (const double seconds :
         {0., .004, .251, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        CAPTURE(seconds);
        CHECK_THROWS_WITH_AS(anima::AudioOutput(audio, seconds),
                             "Audio queue duration must be between 5 and 250 milliseconds", std::invalid_argument);
        CHECK(SDL_WasInit(0) == 0u);
    }
}

TEST_CASE("An output queues its mixer on SDL audio alone, and a move transfers it") {
    REQUIRE(SDL_WasInit(0) == 0u);
    anima::Audio audio(8000);
    auto sound = audio.sound(anima::AudioClip::pcm(std::vector<float>(8000, .1F), 1, 8000));
    sound.play();
    {
        anima::AudioOutput device(audio, queue_target);
        CHECK(SDL_WasInit(SDL_INIT_AUDIO) == SDL_INIT_AUDIO);
        CHECK(SDL_WasInit(SDL_INIT_VIDEO) == 0u);
        verify_queue(device);
        // The pump rendered one queue target of the retained mixer.
        const auto cursor = sound.cursor();
        CHECK(cursor > queue_floor);
        CHECK(cursor <= queue_ceiling);
        device.pump();
        CHECK(sound.cursor() == cursor); // A full queue leaves the mixer untouched.
        device.clear();
        CHECK(device.queued_seconds() == 0);
        CHECK(sound.cursor() == cursor);
        anima::AudioOutput moved(std::move(device));
        CHECK_THROWS_WITH_AS(device.pump(), moved_from, std::logic_error);
        CHECK_THROWS_WITH_AS(device.clear(), moved_from, std::logic_error);
        CHECK_THROWS_WITH_AS(device.paused(true), moved_from, std::logic_error);
        CHECK_THROWS_WITH_AS(device.queued_seconds(), moved_from, std::logic_error);
        verify_queue(moved);
        CHECK(sound.cursor() > cursor);
        {
            anima::AudioOutput replacement(audio, queue_target);
            replacement = std::move(moved);
            CHECK_THROWS_WITH_AS(moved.pump(), moved_from, std::logic_error);
            replacement.clear();
            verify_queue(replacement);
        }
        CHECK(SDL_WasInit(SDL_INIT_AUDIO) == 0u); // Move assignment released the replaced output's reference.
    }
    CHECK(SDL_WasInit(0) == 0u);
}

TEST_CASE("An output and its voices keep the mixer state after the Audio wrapper is gone") {
    REQUIRE(SDL_WasInit(0) == 0u);
    std::optional<anima::AudioOutput> retained;
    anima::Sound retained_sound;
    {
        anima::Audio temporary(8000);
        retained_sound = temporary.sound(anima::AudioClip::pcm(std::vector<float>(8000, .2F), 1, 8000));
        retained_sound.play();
        retained.emplace(temporary, queue_target);
        retained->paused(true);
        anima::Audio moved(std::move(temporary));
    }
    verify_queue(*retained);
    CHECK(retained_sound.cursor() > 0);
    retained.reset();
    CHECK(SDL_WasInit(0) == 0u);
}

TEST_CASE("Each output releases only its own SDL audio subsystem reference") {
    REQUIRE(SDL_WasInit(0) == 0u);
    anima::Audio audio(8000);
    REQUIRE(SDL_InitSubSystem(SDL_INIT_AUDIO)); // The caller's own reference.
    {
        anima::AudioOutput caller_owned(audio, queue_target);
        verify_queue(caller_owned);
        {
            anima::AudioOutput second(audio, queue_target);
            second.paused(true);
        }
        caller_owned.clear();
        verify_queue(caller_owned);
    }
    CHECK(SDL_WasInit(SDL_INIT_AUDIO) == SDL_INIT_AUDIO);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    CHECK(SDL_WasInit(0) == 0u);
}
