#include "near.hpp"
#include <anima/audio.hpp>
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace anima;
namespace {
constexpr double tolerance = 1e-6;
constexpr unsigned rate = 8000;
// Frames over which a playing voice's changes ramp at 8000 Hz.
constexpr std::size_t smoothing = 160;
constexpr auto unsupported = "Unsupported or malformed audio data";

std::vector<std::byte> read_asset(std::string_view name) {
    std::ifstream file(std::string(ANIMA_AUDIO_ASSETS) + "/" + std::string(name), std::ios::binary);
    REQUIRE(file);
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<std::byte> result(bytes.size());
    std::ranges::transform(bytes, result.begin(), [](char value) { return std::byte(value); });
    return result;
}
// The next @p frames stereo frames of @p audio.
std::vector<float> render(Audio &audio, std::size_t frames) {
    std::vector<float> output(frames * 2);
    audio.render(output);
    return output;
}
// The last frame after 0.25 s of output, long after every ramp has settled.
std::array<float, 2> settle(Audio &audio) {
    const auto output = render(audio, rate / 4);
    return {output[output.size() - 2], output.back()};
}
void word(std::vector<std::byte> &bytes, std::uint32_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i)
        bytes.push_back(std::byte((value >> (8 * i)) & 255));
}
// A canonical RIFF/WAVE file of @p samples, interleaved over @p channels: PCM16 unless @p floating.
std::vector<std::byte> wave(const std::vector<float> &samples, unsigned channels, unsigned sample_rate = rate,
                            bool floating = false) {
    const unsigned width = floating ? 4 : 2;
    std::vector<std::byte> body;
    for (auto tag : {"WAVE", "fmt "})
        for (int i = 0; i < 4; ++i)
            body.push_back(std::byte(tag[i]));
    word(body, 16, 4);
    word(body, floating ? 3 : 1, 2);
    word(body, channels, 2);
    word(body, sample_rate, 4);
    word(body, sample_rate * channels * width, 4);
    word(body, channels * width, 2);
    word(body, width * 8, 2);
    for (int i = 0; i < 4; ++i)
        body.push_back(std::byte("data"[i]));
    word(body, static_cast<std::uint32_t>(samples.size() * width), 4);
    for (auto sample : samples)
        word(body,
             floating ? std::bit_cast<std::uint32_t>(sample)
                      : static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(sample * 32767))),
             width);
    std::vector<std::byte> bytes;
    for (int i = 0; i < 4; ++i)
        bytes.push_back(std::byte("RIFF"[i]));
    word(bytes, static_cast<std::uint32_t>(body.size()), 4);
    bytes.insert(bytes.end(), body.begin(), body.end());
    return bytes;
}
// The fixtures' reference signal: sample @p n of a 440 Hz sine at half scale and 8000 Hz.
double tone(std::size_t n) { return .5 * std::sin(2 * std::numbers::pi * 440 * double(n) / rate); }
} // namespace

TEST_CASE("A voice plays, finishes, loops, pauses and restarts") {
    Audio audio(rate, 2);
    const auto clip = AudioClip::pcm(std::vector<float>(16, 1), 1, rate);
    auto sound = audio.sound(clip);
    sound.pan(-1);
    CHECK(sound.play());
    auto output = render(audio, 32);
    // The resampler delays the clip by one frame; the left channel then carries it at full gain.
    CHECK(output[0] == 0);
    for (std::size_t frame = 1; frame < 15; ++frame) {
        CAPTURE(frame);
        CHECK(output[2 * frame] == Near{1, tolerance});
        CHECK(output[2 * frame + 1] == 0);
    }
    CHECK(output.back() == 0);
    // The voice stopped at the end of the clip, with its cursor there.
    CHECK_FALSE(sound.playing());
    CHECK(sound.cursor() == Near{clip->duration(), tolerance});
    sound.looping(true);
    CHECK(sound.play()); // Playing from the end restarts the clip.
    output = render(audio, 40);
    CHECK(sound.playing());
    CHECK(
        std::ranges::all_of(std::span(output).subspan(2, 76), [](float sample) { return sample == 1 || sample == 0; }));
    CHECK(output[2 * 17] == Near{1, tolerance}); // The loop continues without a gap.
    sound.pause();
    const auto paused = sound.cursor();
    output = render(audio, 8);
    CHECK(std::ranges::all_of(output, [](float sample) { return sample == 0; }));
    CHECK(sound.cursor() == paused);
    sound.stop();
    CHECK(sound.cursor() == 0);
    sound.seek(clip->duration());
    CHECK(sound.cursor() == Near{clip->duration(), tolerance});
    sound.looping(false);
    CHECK(sound.play());
    CHECK(sound.cursor() == 0);
    output = render(audio, 2);
    CHECK(output[2] == Near{1, tolerance});
    sound.seek(8. / rate);
    CHECK(sound.cursor() == Near{8. / rate, tolerance});
    CHECK(sound.playing()); // Seeking keeps a playing voice playing.
}

TEST_CASE("A full voice limit stops the lowest priority, then the voice admitted longest ago") {
    Audio audio(rate, 2);
    CHECK(audio.maximum_voices() == 2u);
    const auto clip = AudioClip::pcm(std::vector<float>(rate, .5F), 1, rate);
    auto first = audio.sound(clip), second = audio.sound(clip), low = audio.sound(clip), high = audio.sound(clip);
    CHECK(audio.voice_count() == 4u);
    low.priority(100);
    high.priority(200);
    CHECK(first.play());
    CHECK(second.play());
    (void)render(audio, 64);
    CHECK_FALSE(low.play()); // Every playing voice outranks it, so nothing stops.
    CHECK_FALSE(low.playing());
    CHECK(first.playing());
    CHECK(second.playing());
    CHECK(high.play()); // The playing voices tie for lowest, so the one admitted first stops, rewound by stop().
    CHECK_FALSE(first.playing());
    CHECK(first.cursor() == 0);
    CHECK(second.playing());
    CHECK(first.play()); // The second voice now ranks lowest, and an equal priority stops it.
    CHECK_FALSE(second.playing());
    CHECK(high.playing());
    second.priority(255);
    CHECK(second.play()); // The first voice, admitted last, now ranks lowest and stops.
    CHECK_FALSE(first.playing());
    first.pause();
    high.pause(); // Paused voices hold no place in the limit.
    CHECK(low.play());
    CHECK(first.play());
    CHECK_FALSE(high.playing());
    {
        const auto one_shot = AudioClip::pcm(std::vector<float>(4, .5F), 1, rate);
        auto brief = audio.sound(one_shot);
        brief.priority(0);
        first.stop();
        CHECK(brief.play());
        (void)render(audio, 64); // A voice that reached its end holds no place either.
        CHECK_FALSE(brief.playing());
        CHECK(first.play());
        CHECK(audio.voice_count() == 5u);
    }
    CHECK(audio.voice_count() == 4u);
}

TEST_CASE("Gain, pan and fades ramp linearly while a voice plays, and apply at once while it does not") {
    Audio audio(rate);
    const auto clip = AudioClip::pcm(std::vector<float>(16, 1), 1, rate);
    auto sound = audio.sound(clip);
    sound.looping(true);
    sound.pan(-1);
    sound.volume(.5F); // Stopped: at once.
    CHECK(sound.play());
    auto output = render(audio, 64);
    CHECK(output[2] == Near{.5, tolerance});
    sound.volume(1);
    output = render(audio, 2 * smoothing);
    for (std::size_t frame = 0; frame <= smoothing; frame += 40) {
        CAPTURE(frame);
        CHECK(output[2 * frame] == Near{.5 + .5 * double(frame) / smoothing, 1e-5});
    }
    CHECK(output[2 * (2 * smoothing - 1)] == Near{1, tolerance});
    sound.pan(1); // The equal-power gains move linearly between the two sides.
    output = render(audio, 2 * smoothing);
    CHECK(output[0] == Near{1, tolerance});
    CHECK(output[1] == Near{0, tolerance});
    CHECK(output[smoothing] == Near{.5, 1e-5});
    CHECK(output[smoothing + 1] == Near{.5, 1e-5});
    CHECK(output[2 * smoothing] == Near{0, tolerance});
    CHECK(output[2 * smoothing + 1] == Near{1, tolerance});
    sound.fade(0, 80. / rate);
    output = render(audio, 100);
    CHECK(output[1] == Near{1, tolerance});
    CHECK(output[2 * 40 + 1] == Near{.5, 1e-5});
    CHECK(output[2 * 80 + 1] == Near{0, tolerance});
    sound.fade(1, 80. / rate);
    output = render(audio, 20);
    sound.pause(); // Pausing keeps the fade where it is, and it resumes with the voice.
    const auto held = output.back();
    CHECK(held == Near{19. / 80, 1e-5});
    sound.play();
    output = render(audio, 2);
    CHECK(output[1] == Near{20. / 80, 1e-5});
    sound.stop(); // Stop cancels the fade at its current gain.
    sound.play();
    output = render(audio, 100);
    CHECK(output[2 * 99 + 1] == Near{output[3], tolerance});
    CHECK(output[3] == Near{22. / 80, 1e-5}); // Where the fade had reached after its 22nd frame.
    sound.fade(1, 0);
    output = render(audio, 2);
    CHECK(output[1] == Near{1, tolerance});
    sound.stop();
    sound.pan(-1); // Stopped: at once.
    sound.play();
    output = render(audio, 2);
    CHECK(output[2] == Near{1, tolerance});
    CHECK(output[3] == 0);
}

TEST_CASE("Pitch ramps in steps at the start of each block while a voice plays") {
    Audio audio(rate);
    const auto clip = AudioClip::pcm(std::vector<float>(rate, .5F), 1, rate);
    auto sound = audio.sound(clip);
    sound.pitch(2); // Stopped: at once.
    CHECK(sound.play());
    (void)render(audio, audio_block_frames);
    CHECK(sound.cursor() == Near{2. * audio_block_frames / rate, 2. / rate});
    sound.stop();
    sound.pitch(1);
    CHECK(sound.play());
    (void)render(audio, audio_block_frames);
    sound.pitch(2);
    // The ramp covers 160 frames: blocks starting 64 and 128 frames into it play at 1.4 and 1.8, and then 2.
    for (const double pitch : {1.4, 1.8, 2., 2.}) {
        CAPTURE(pitch);
        const auto before = sound.cursor();
        (void)render(audio, audio_block_frames);
        CHECK((sound.cursor() - before) * rate == Near{pitch * audio_block_frames, 2});
    }
}

TEST_CASE("Bus and master gains compose, ramp while audio passes and apply at once while idle") {
    Audio audio(rate);
    auto parent = audio.bus();
    parent.volume(.5F);
    auto child = audio.bus(parent);
    child.volume(.5F);
    auto sound = audio.sound(AudioClip::pcm({1, 1}, 1, rate), child);
    sound.pan(-1);
    sound.looping(true);
    sound.play();
    auto output = render(audio, 64);
    CHECK(output[2] == Near{.25, tolerance});
    parent.muted(true); // The parent's mute reaches the child, over the smoothing time.
    output = render(audio, 2 * smoothing);
    CHECK(output[smoothing] == Near{.125, 1e-5});
    CHECK(output.back() == 0);
    CHECK(output[2 * (2 * smoothing - 1)] == 0);
    parent.muted(false);
    parent.volume(1);
    child.volume(1);
    audio.volume(.5F);
    (void)settle(audio);
    CHECK(settle(audio)[0] == Near{.5, tolerance});
    sound.pause();
    (void)render(audio, 1);
    audio.volume(1); // Nothing passes through the master now, so the gain applies at once.
    child.volume(.25F);
    sound.play();
    output = render(audio, 2);
    CHECK(output[2] == Near{.25, tolerance});
    child.volume(.5F); // Ramping, since the voice plays through the bus.
    sound.pause();
    (void)render(audio, smoothing); // Bus ramps advance with output time, even with nothing passing.
    sound.play();
    output = render(audio, 2);
    CHECK(output[0] == Near{.5, tolerance});
    child = {};
    parent = {};
    CHECK(settle(audio)[0] == Near{.5, tolerance}); // The voice retains its bus chain.
}

TEST_CASE("Spatial voices attenuate with distance and weight channels by direction from the listener") {
    Audio audio(rate);
    auto sound = audio.sound(AudioClip::pcm({1, 1}, 1, rate));
    sound.looping(true);
    sound.spatial(true);
    sound.attenuation(0, 4);
    sound.position({2, 0, 0});
    sound.play();
    // Half gain at half the linear range; the right side takes it all and the left its floor of 0.2.
    auto frame = settle(audio);
    CHECK(frame[0] == Near{.1, 1e-4});
    CHECK(frame[1] == Near{.5, 1e-4});
    sound.position({0, 0, -2}); // Straight ahead, each side takes half.
    frame = settle(audio);
    CHECK(frame[0] == Near{.25, 1e-4});
    CHECK(frame[1] == Near{.25, 1e-4});
    // Silent from the maximum distance, once the block the change lands in and one whole block have mixed.
    sound.position({4, 0, 0});
    auto moved = render(audio, 2 * audio_block_frames);
    CHECK(moved[moved.size() - 2] == Near{0, 1e-6});
    CHECK(moved.back() == Near{0, 1e-6});
    sound.position({1, 0, 0});
    audio.listener({}, {0, 0, 1}); // Facing +Z puts +X on the listener's left.
    frame = settle(audio);
    CHECK(frame[0] == Near{.75, 1e-4});
    CHECK(frame[1] == Near{.15, 1e-4});
    audio.listener({}, {0, 0, -1}, {0, -1, 0}); // Upside down, facing -Z, also puts +X on the left.
    frame = settle(audio);
    CHECK(frame[0] == Near{.75, 1e-4});
    audio.listener({2, 0, 0}); // Within 0.001 of the listener, neither side is weighted.
    sound.position({2, 0, 0});
    frame = settle(audio);
    CHECK(frame[0] == Near{1, 1e-4});
    CHECK(frame[1] == Near{1, 1e-4});
    audio.listener({});
    sound.attenuation(1, 10, AudioRolloff::inverse);
    sound.position({4, 0, 0}); // A quarter at four times the minimum distance.
    frame = settle(audio);
    CHECK(frame[1] == Near{.25, 1e-4});
    sound.position({40, 0, 0}); // Held at minimum / maximum beyond the maximum distance.
    frame = settle(audio);
    CHECK(frame[1] == Near{.1, 1e-4});
    // A voice that starts takes its current gains at once rather than ramping from its last output.
    sound.pause();
    sound.position({-2, 0, 0});
    sound.play();
    const auto output = render(audio, 2);
    CHECK(output[2] == Near{.5, 1e-4});
    CHECK(output[3] == Near{.1, 1e-4});
    sound.spatial(false); // Nonspatial: equal-power center pan, at once.
    const auto centered = render(audio, 1);
    CHECK(centered[0] == Near{std::sqrt(.5), 1e-5});
}

TEST_CASE("Spatial changes ramp linearly across the block they land in") {
    constexpr std::size_t block = audio_block_frames;
    Audio audio(rate);
    auto sound = audio.sound(AudioClip::pcm({1, 1}, 1, rate));
    sound.looping(true);
    sound.spatial(true);
    sound.attenuation(0, 4);
    sound.position({0, 0, -2}); // A quarter on each side.
    sound.play();
    (void)render(audio, 4 * block); // Whole blocks, so that the next change lands at the start of one.
    // Frame k of the block has moved k / audio_block_frames of the way to the new gains.
    const auto check = [](const std::vector<float> &output, std::size_t frame, double from, double to) {
        CAPTURE(frame);
        const auto expected = from + (to - from) * static_cast<double>(frame) / block;
        CHECK(output[2 * frame] == Near{expected, tolerance});
        CHECK(output[2 * frame + 1] == Near{expected, tolerance});
    };
    sound.position({4, 0, 0}); // Silent.
    auto output = render(audio, block);
    for (const std::size_t frame : {std::size_t{1}, block / 2, block - 1})
        check(output, frame, .25, 0);
    // A render that ends partway through a block ends the block early, here after an odd number of frames.
    sound.position({0, 0, -2});
    output = render(audio, block / 2 + 1);
    for (const std::size_t frame : {block / 2 - 1, block / 2})
        check(output, frame, 0, .25);
}

TEST_CASE("Clips resample linearly and the mix is limited") {
    Audio audio(16000);
    auto sound = audio.sound(AudioClip::pcm({0, 1, 0, -1}, 1, rate));
    sound.pan(-1);
    sound.play();
    auto output = render(audio, 8);
    // Doubling the rate interpolates halfway between the clip's samples, two output frames late.
    const std::array<float, 8> expected{0, 0, 0, .5F, 1, .5F, 0, -.5F};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CAPTURE(i);
        CHECK(output[2 * i] == Near{expected[i], tolerance});
    }
    auto stereo = audio.sound(AudioClip::pcm({.25F, -.5F, .25F, -.5F}, 2, 16000));
    stereo.volume(16);
    stereo.play();
    output = render(audio, 2);
    CHECK(output[2] == 1);
    CHECK(output[3] == -1);
}

TEST_CASE("Rendering in several calls matches one call, and voices outlive their engine's wrapper") {
    Audio a(11025), b(11025);
    const auto clip = AudioClip::pcm({0, .2F, -.4F, .7F, -.9F}, 1, rate);
    auto x = a.sound(clip), y = b.sound(clip);
    for (auto *sound : {&x, &y}) {
        sound->looping(true);
        sound->pitch(1.3F);
        sound->fade(.2F, .01);
        sound->play();
    }
    std::vector<float> whole(2048), split(2048);
    a.render(whole);
    b.render(std::span<float>(split).first(26));
    b.render(std::span<float>(split).subspan(26, 110));
    b.render(std::span<float>(split).subspan(136));
    CHECK(whole == split);
    Audio moved(std::move(a));
    moved.render(whole);
    CHECK_THROWS_WITH_AS(a.render(whole), "Moved-from audio mixer", std::logic_error);
    Sound survivor;
    {
        Audio temporary;
        survivor = temporary.sound(clip);
    }
    CHECK(survivor.play());
    CHECK(survivor.playing());
}

TEST_CASE("Invalid engines, voices and arguments are rejected") {
    CHECK_THROWS_WITH_AS(Audio(7999), "Audio sample rate must be between 8000 and 192000 Hz", std::invalid_argument);
    CHECK_THROWS_WITH_AS(Audio(rate, 0), "Audio voice limit must be in [1, 4096]", std::invalid_argument);
    CHECK_THROWS_WITH_AS(Audio(rate, 4097), "Audio voice limit must be in [1, 4096]", std::invalid_argument);
    CHECK_THROWS_WITH_AS((void)Audio::open_device(AudioBackend::null, 0), "Audio voice limit must be in [1, 4096]",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS((void)Audio::open_device(static_cast<AudioBackend>(7)), "Invalid audio backend",
                         std::invalid_argument);
    Audio audio(rate);
    const auto clip = AudioClip::pcm({1, 1, 1, 1}, 1, rate);
    auto sound = audio.sound(clip);
    Sound moved(std::move(sound));
    CHECK_THROWS_WITH_AS(sound.play(), "Empty audio voice", std::logic_error);
    CHECK_THROWS_WITH_AS((void)sound.playing(), "Empty audio voice", std::logic_error);
    Audio foreign;
    CHECK_THROWS_WITH_AS(foreign.sound(clip, audio.bus()), "Missing audio clip or foreign bus", std::invalid_argument);
    CHECK_THROWS_WITH_AS(audio.sound(nullptr), "Missing audio clip or foreign bus", std::invalid_argument);
    CHECK_THROWS_WITH_AS(moved.seek(-1), "Audio seek lies outside the clip", std::invalid_argument);
    CHECK_THROWS_WITH_AS(moved.volume(std::numeric_limits<float>::quiet_NaN()), "Audio gain must be in [0, 16]",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(moved.pitch(0), "Audio pitch must be in [0.01, 8]", std::invalid_argument);
    CHECK_THROWS_WITH_AS(moved.pan(1.5F), "Audio pan must be in [-1, 1]", std::invalid_argument);
    CHECK_THROWS_WITH_AS(moved.priority(256), "Audio priority must be in [0, 255]", std::invalid_argument);
    CHECK_THROWS_WITH_AS(moved.priority(-1), "Audio priority must be in [0, 255]", std::invalid_argument);
    CHECK_THROWS_WITH_AS(moved.attenuation(5, 2), "Invalid audio attenuation distances", std::invalid_argument);
    CHECK_THROWS_WITH_AS(moved.attenuation(0, 2, AudioRolloff::inverse), "Invalid audio attenuation distances",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(moved.attenuation(1, 2, static_cast<AudioRolloff>(2)), "Invalid audio rolloff",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(moved.fade(1, -1), "Invalid audio fade duration", std::invalid_argument);
    CHECK_THROWS_WITH_AS(moved.position({2e9F, 0, 0}), "Audio coordinates must be finite and within one billion units",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(audio.listener({}, {}, {0, 1, 0}), "Invalid audio listener orientation",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(audio.listener({}, {0, 1, 0}, {0, 2, 0}), "Parallel audio listener orientation vectors",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(AudioBus().volume(1), "Empty audio bus", std::logic_error);
    std::array<float, 3> odd{};
    CHECK_THROWS_WITH_AS(audio.render(odd), "Audio output must contain whole stereo frames", std::invalid_argument);
}

TEST_CASE("WAV, FLAC, MP3 and Vorbis clips decode") {
    const auto wav = AudioClip::decode(read_asset("tone.wav"));
    const auto flac = AudioClip::decode(read_asset("tone.flac"));
    const auto mp3 = AudioClip::decode(read_asset("tone.mp3"));
    const auto vorbis = AudioClip::decode(read_asset("tone.ogg"));
    for (const auto *clip : {wav.get(), flac.get(), mp3.get(), vorbis.get()}) {
        CHECK(clip->channels() == 1u);
        CHECK(clip->sample_rate() == rate);
        CHECK(clip->load_mode() == AudioLoadMode::decompress);
        CHECK(clip->memory_bytes() == clip->frames() * sizeof(float));
    }
    CHECK(wav->frames() == 1600u);
    CHECK(flac->frames() == 1600u);
    CHECK(vorbis->frames() == 1600u);
    CHECK(mp3->frames() == 2880u); // With the encoder's delay and padding.
    CHECK(wav->duration() == Near{.2, tolerance});
    // Each decoded clip, played at full gain on the left, reproduces the tone: exactly for the lossless files.
    Audio audio(rate);
    const auto play = [&](const std::shared_ptr<const AudioClip> &clip) {
        auto sound = audio.sound(clip);
        sound.pan(-1);
        sound.play();
        const auto output = render(audio, clip->frames() + 1);
        std::vector<float> left;
        for (std::size_t frame = 1; frame < output.size() / 2; ++frame)
            left.push_back(output[2 * frame]);
        return left;
    };
    const auto lossless = play(wav);
    CHECK(play(flac) == lossless);
    double error = 0, peak = 0;
    for (std::size_t n = 0; n + 1 < lossless.size(); ++n)
        error = std::max(error, std::abs(lossless[n] - tone(n)));
    CHECK(error < 1. / 32768);
    error = 0;
    const auto lossy = play(vorbis);
    for (std::size_t n = 0; n + 1 < lossy.size(); ++n)
        error = std::max(error, std::abs(lossy[n] - tone(n)));
    CHECK(error < .05);
    for (const auto sample : play(mp3))
        peak = std::max(peak, double(std::abs(sample)));
    CHECK(peak == Near{.5, .05});
}

TEST_CASE("Streamed clips play without being decoded whole, and match their decompressed clips") {
    Audio audio(rate);
    for (const std::string_view name : {"tone.wav", "tone.flac", "tone.mp3", "tone.ogg"}) {
        CAPTURE(name);
        const auto bytes = read_asset(name);
        const auto decoded = AudioClip::decode(bytes);
        const auto streamed = AudioClip::decode(bytes, AudioLoadMode::stream);
        CHECK(streamed->load_mode() == AudioLoadMode::stream);
        CHECK(streamed->frames() == decoded->frames());
        CHECK(streamed->memory_bytes() == bytes.size()); // The encoded copy, not the samples.
        auto a = audio.sound(decoded), b = audio.sound(streamed);
        a.pan(-1);
        b.pan(1);
        a.looping(true);
        b.looping(true);
        const auto check_same = [&](std::size_t frames) {
            const auto output = render(audio, frames);
            bool same = true;
            for (std::size_t frame = 0; frame < frames; ++frame)
                same = same && output[2 * frame] == output[2 * frame + 1];
            CHECK(same);
        };
        a.play();
        b.play();
        check_same(decoded->frames() + 300); // Across the loop.
        a.seek(.1);
        b.seek(.1);
        CHECK(b.cursor() == Near{.1, tolerance});
        check_same(500);
        a.stop();
        b.stop();
    }
    // A streamed float WAV plays a non-finite sample as silence, which a decompressed one rejects.
    const auto bytes = wave({.5F, std::numeric_limits<float>::quiet_NaN(), .5F, .5F}, 1, rate, true);
    CHECK_THROWS_WITH_AS(AudioClip::decode(bytes), "Decoded audio samples must be finite", std::invalid_argument);
    auto sound = audio.sound(AudioClip::decode(bytes, AudioLoadMode::stream));
    sound.pan(-1);
    sound.play();
    const auto output = render(audio, 4);
    CHECK(output[2] == Near{.5, tolerance});
    CHECK(output[4] == 0);
    CHECK(output[6] == Near{.5, tolerance});
}

TEST_CASE("Malformed or unsupported audio data is rejected") {
    CHECK_THROWS_WITH_AS(AudioClip::decode({}), unsupported, std::invalid_argument);
    std::vector<std::byte> noise(4096);
    for (std::size_t i = 0; i < noise.size(); ++i)
        noise[i] = std::byte((i * 7919) >> 3);
    for (const auto mode : {AudioLoadMode::decompress, AudioLoadMode::stream}) {
        CAPTURE(static_cast<int>(mode));
        CHECK_THROWS_WITH_AS(AudioClip::decode(noise, mode), unsupported, std::invalid_argument);
        auto riff = wave({.5F}, 1);
        riff.resize(20); // Cut inside the format chunk.
        CHECK_THROWS_WITH_AS(AudioClip::decode(riff, mode), unsupported, std::invalid_argument);
        auto ogg = noise;
        std::ranges::copy(std::string_view("OggS"), reinterpret_cast<char *>(ogg.data()));
        CHECK_THROWS_WITH_AS(AudioClip::decode(ogg, mode), unsupported, std::invalid_argument);
        auto flac = noise;
        std::ranges::copy(std::string_view("fLaC"), reinterpret_cast<char *>(flac.data()));
        CHECK_THROWS_WITH_AS(AudioClip::decode(flac, mode), unsupported, std::invalid_argument);
        CHECK_THROWS_WITH_AS(AudioClip::decode(wave({.1F, .2F, .3F}, 3), mode), "Audio clips must have 1 or 2 channels",
                             std::invalid_argument);
        CHECK_THROWS_WITH_AS(AudioClip::decode(wave({.1F}, 1, 4000), mode),
                             "Audio sample rate must be between 8000 and 192000 Hz", std::invalid_argument);
        CHECK_THROWS_WITH_AS(AudioClip::decode(wave({}, 1), mode), "Audio data holds no samples",
                             std::invalid_argument);
    }
    CHECK_THROWS_WITH_AS(AudioClip::decode(read_asset("tone.wav"), static_cast<AudioLoadMode>(2)),
                         "Invalid audio load mode", std::invalid_argument);
    const std::vector<std::byte> oversized(128 * 1024 * 1024 + 1);
    CHECK_THROWS_WITH_AS(AudioClip::decode(oversized), "Encoded audio exceeds 128 MiB", std::invalid_argument);
    CHECK_THROWS_WITH_AS(AudioClip::pcm({2}, 1, rate), "PCM samples must be finite and normalized to [-1, 1]",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(AudioClip::pcm({}, 1, rate), "Invalid PCM channels or sample count", std::invalid_argument);
    CHECK_THROWS_WITH_AS(AudioClip::pcm({1, 1, 1}, 2, rate), "Invalid PCM channels or sample count",
                         std::invalid_argument);
}

TEST_CASE("A device on the null backend mixes on its own thread, so a stalled caller does not delay playback") {
    auto audio = Audio::open_device(AudioBackend::null);
    CHECK(audio.sample_rate() >= 8000u);
    std::array<float, 2> frame{};
    CHECK_THROWS_WITH_AS(audio.render(frame), "Audio with a device mixes on its device thread", std::logic_error);
    // A quarter-second one-shot finishes while this thread makes no calls at all.
    auto brief = audio.sound(AudioClip::pcm(std::vector<float>(rate / 4, .25F), 1, rate));
    auto music = audio.sound(AudioClip::decode(read_asset("tone.ogg"), AudioLoadMode::stream));
    music.looping(true);
    CHECK(brief.play());
    CHECK(music.play());
    std::this_thread::sleep_for(std::chrono::seconds(1));
    CHECK_FALSE(brief.playing());
    CHECK(music.playing());
    // The looping stream went round its 0.2 s clip about five times; its cursor lies inside the clip.
    CHECK(music.cursor() <= .2);
    // The engine outlives its wrapper while a voice retains it, and mixing continues.
    Sound survivor = audio.sound(AudioClip::pcm(std::vector<float>(rate, .25F), 1, rate));
    {
        Audio retired(std::move(audio));
    }
    CHECK(survivor.play());
    for (int wait = 0; wait < 200 && survivor.cursor() == 0; ++wait)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(survivor.cursor() > 0);
}

// Run under ThreadSanitizer, this also checks that every call is safe while the device thread mixes.
TEST_CASE("Voices, buses and the listener change safely while the device thread mixes") {
    auto audio = Audio::open_device(AudioBackend::null, 4);
    const auto streamed = AudioClip::decode(read_asset("tone.mp3"), AudioLoadMode::stream);
    const auto decoded = AudioClip::decode(read_asset("tone.flac"));
    auto bus = audio.bus();
    std::vector<Sound> voices;
    for (int step = 0; step < 300; ++step) {
        if (voices.size() < 6)
            voices.push_back(audio.sound(step % 2 ? streamed : decoded, step % 3 ? bus : AudioBus{}));
        auto &voice = voices[static_cast<std::size_t>(step) % voices.size()];
        voice.looping(step % 4 == 0);
        voice.priority(step % 256);
        voice.volume(float(step % 5) / 4);
        voice.pan(float(step % 3) - 1);
        voice.pitch(.5F + float(step % 4) / 2);
        voice.spatial(step % 7 == 0);
        voice.position({float(step % 9) - 4, 0, -1});
        voice.fade(.5F, .01);
        (void)voice.play();
        if (step % 5 == 0)
            voice.seek(.05);
        if (step % 11 == 0)
            voice.stop();
        (void)voice.cursor();
        bus.volume(float(step % 3) / 2);
        bus.muted(step % 17 == 0);
        audio.volume(1);
        audio.listener({float(step % 5), 0, 0}, {0, 0, -1});
        if (step % 13 == 0)
            voices.erase(voices.begin());
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    CHECK(audio.voice_count() == voices.size());
}
