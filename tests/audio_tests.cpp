#include "near.hpp"
#include <anima/audio.hpp>
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace anima;
namespace {
constexpr double tolerance = 1e-6;
// wave() writes a RIFF header (identifier, size of the rest, form type), then chunks at these offsets, each an
// 8-byte header (identifier, body size) before its body: JUNK (1 byte and a pad byte), fmt (16 bytes) and data.
constexpr std::size_t junk_chunk = 12, format_chunk = 22, data_chunk = 46, chunk_header = 8;
constexpr std::size_t size_field = 4; // In the RIFF header and in every chunk header.
constexpr std::size_t sample_rate_field = format_chunk + chunk_header + 4;
constexpr auto invalid_container = "Invalid or unsupported RIFF/WAVE container";
constexpr auto truncated_chunk = "Truncated WAVE chunk or padding";
constexpr auto unnormalized = "PCM samples must be finite and normalized to [-1, 1]";

void word(std::vector<std::byte> &bytes, std::uint32_t value, unsigned width = 4) {
    for (unsigned i = 0; i < width; ++i)
        bytes.push_back(std::byte((value >> (8 * i)) & 255));
}
// Overwrites the little-endian 32-bit word at @p at.
void overwrite(std::vector<std::byte> &bytes, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        bytes[at + i] = std::byte((value >> (8 * i)) & 255);
}
std::vector<std::byte> wave(bool floating) {
    std::vector<std::byte> body;
    word(body, 0x45564157); // WAVE
    word(body, 0x4b4e554a);
    word(body, 1);
    word(body, 0, 2); // odd JUNK plus padding
    word(body, 0x20746d66);
    word(body, 16);
    word(body, floating ? 3 : 1, 2);
    word(body, 1, 2);
    word(body, 8000);
    word(body, floating ? 32000 : 16000);
    word(body, floating ? 4 : 2, 2);
    word(body, floating ? 32 : 16, 2);
    word(body, 0x61746164);
    word(body, floating ? 8 : 4);
    if (floating) {
        word(body, std::bit_cast<std::uint32_t>(-.5F));
        word(body, std::bit_cast<std::uint32_t>(.5F));
    } else {
        word(body, 0x8000, 2);
        word(body, 0x7fff, 2);
    }
    std::vector<std::byte> bytes;
    word(bytes, 0x46464952);
    word(bytes, static_cast<std::uint32_t>(body.size()));
    bytes.insert(bytes.end(), body.begin(), body.end());
    return bytes;
}
// The error for wave(false) cut to @p size bytes, at least its RIFF header, with the RIFF size corrected: a cut
// between chunks leaves the format or data missing, one inside a chunk header truncates a field, and one inside a
// chunk body or before its pad byte truncates the chunk.
const char *truncation_error(std::size_t size) {
    const auto chunk = size < format_chunk ? junk_chunk : size < data_chunk ? format_chunk : data_chunk;
    if (size == chunk)
        return "Missing WAVE format or data";
    return size < chunk + chunk_header ? "Truncated WAVE field" : truncated_chunk;
}
} // namespace

TEST_CASE("A voice pans, finishes, loops, pauses and restarts at exact sample positions") {
    Audio audio(8000, 2);
    const auto clip = AudioClip::pcm({1, 1, 1, 1}, 1, 8000);
    auto sound = audio.sound(clip);
    sound.pan(-1);
    sound.play();
    std::array<float, 8> output{};
    audio.render(output);
    for (std::size_t i = 0; i < output.size(); i += 2) {
        CAPTURE(i);
        CHECK(output[i] == Near{1, tolerance});
        CHECK(output[i + 1] == Near{0, tolerance});
    }
    // The voice stops at the clip boundary, with its cursor there.
    CHECK_FALSE(sound.playing());
    CHECK(sound.cursor() == Near{clip->duration(), tolerance});
    sound.looping(true);
    sound.play();
    std::array<float, 18> long_output{};
    audio.render(long_output);
    CHECK(sound.cursor() == Near{1.0 / 8000, tolerance}); // Nine frames of a four-frame loop.
    sound.pause();
    audio.render(output);
    CHECK(std::ranges::all_of(output, [](float sample) { return sample == 0; }));
    CHECK(sound.cursor() == Near{1.0 / 8000, tolerance});
    sound.stop();
    CHECK(sound.cursor() == Near{0, tolerance});
    sound.seek(clip->duration());
    sound.play();
    audio.render(output);
    CHECK(output[0] == Near{1, tolerance}); // Playing from the end restarts the clip.
}

TEST_CASE("Voice capacity, empty voices and invalid voice arguments are rejected") {
    Audio audio(8000, 2);
    const auto clip = AudioClip::pcm({1, 1, 1, 1}, 1, 8000);
    auto sound = audio.sound(clip);
    auto other = audio.sound(clip);
    CHECK_THROWS_WITH_AS(audio.sound(clip), "Audio voice limit exceeded", std::length_error);
    other = {}; // Destruction frees a capacity slot.
    other = audio.sound(clip);
    Sound moved(std::move(other));
    CHECK_THROWS_WITH_AS(other.play(), "Empty audio voice", std::logic_error);
    Audio foreign;
    CHECK_THROWS_WITH_AS(foreign.sound(clip, audio.bus()), "Missing audio clip or foreign bus", std::invalid_argument);
    CHECK_THROWS_WITH_AS(sound.seek(-1), "Audio seek lies outside the clip", std::invalid_argument);
    CHECK_THROWS_WITH_AS(sound.volume(std::numeric_limits<float>::quiet_NaN()), "Audio gain must be in [0, 16]",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(sound.pitch(0), "Audio pitch must be in [0.01, 8]", std::invalid_argument);
    CHECK_THROWS_WITH_AS(sound.attenuation(5, 2), "Invalid audio attenuation distances", std::invalid_argument);
    CHECK_THROWS_WITH_AS(audio.listener({}, {}, {0, 1, 0}), "Invalid audio listener orientation",
                         std::invalid_argument);
}

TEST_CASE("Clips resample linearly, pitch scales their advance and the mix is limited") {
    Audio audio(16000);
    auto sound = audio.sound(AudioClip::pcm({0, 1, 0, -1}, 1, 8000));
    sound.pan(-1);
    sound.play();
    std::array<float, 12> output{};
    audio.render(output);
    const std::array<float, 6> expected{0, .5F, 1, .5F, 0, -.5F};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CAPTURE(i);
        CHECK(output[2 * i] == Near{expected[i], tolerance});
    }
    sound.stop();
    sound.pitch(2);
    sound.play();
    audio.render(output);
    CHECK(output[2] == Near{1, tolerance});
    CHECK_FALSE(sound.playing());
    auto stereo = audio.sound(AudioClip::pcm({.25F, -.5F, .25F, -.5F}, 2, 16000));
    stereo.play();
    audio.render(output);
    CHECK(output[0] == Near{.25, tolerance});
    CHECK(output[1] == Near{-.5, tolerance});
    stereo.volume(16);
    stereo.play();
    audio.render(output);
    CHECK(output[0] == Near{1, tolerance});
    CHECK(output[1] == Near{-1, tolerance});
}

TEST_CASE("Bus gains compose, fades ramp and spatial voices pan and attenuate against the listener") {
    Audio audio(8000);
    auto parent = audio.bus();
    parent.volume(.5F);
    auto child = audio.bus(parent);
    child.volume(.5F);
    auto sound = audio.sound(AudioClip::pcm({1, 1}, 1, 8000), child);
    sound.pan(-1);
    sound.looping(true);
    sound.play();
    std::array<float, 8> output{};
    audio.render(output);
    CHECK(output[0] == Near{.25, tolerance});
    parent.muted(true);
    audio.render(output);
    CHECK(output[0] == Near{0, tolerance}); // The parent's mute reaches the child.
    parent.muted(false);
    parent.volume(1);
    child.volume(1);
    sound.fade(0, 2.0 / 8000);
    audio.render(output);
    CHECK(output[0] == Near{1, tolerance});
    CHECK(output[2] == Near{.5, tolerance});
    CHECK(output[4] == Near{0, tolerance});
    sound.volume(1);
    sound.spatial(true);
    sound.position({1, 0, 0});
    sound.attenuation(1, 3);
    audio.render(output);
    CHECK(output[0] == Near{0, tolerance});
    CHECK(output[1] == Near{1, tolerance});
    sound.position({2, 0, 0});
    audio.render(output);
    CHECK(output[1] == Near{.5, tolerance});
    sound.position({3, 0, 0});
    audio.render(output);
    CHECK(output[1] == Near{0, tolerance}); // Silent at the maximum distance.
    sound.position({1, 0, 0});
    audio.listener({});
    audio.render(output);
    CHECK(output[1] == Near{1, tolerance}); // The default listener faces -Z, with +X on its right.
    audio.listener({}, {0, 0, -1});
    audio.render(output);
    CHECK(output[1] == Near{1, tolerance});
    audio.listener({}, {0, 0, 1});
    audio.render(output);
    CHECK(output[0] == Near{1, tolerance});
    child = {};
    parent = {};
    audio.render(output);
    CHECK(output[0] == Near{1, tolerance}); // The voice retains its bus chain.
}

TEST_CASE("The mix does not depend on output block sizes, and voices outlive their mixer wrapper") {
    Audio a(11025), b(11025);
    const auto clip = AudioClip::pcm({0, .2F, -.4F, .7F, -.9F}, 1, 8000);
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
    survivor.play();
    CHECK(survivor.playing());
}

TEST_CASE("PCM16 and float32 WAVE files decode") {
    const auto integer = AudioClip::wav(wave(false)), floating = AudioClip::wav(wave(true));
    CHECK(integer->samples()[0] == Near{-1, tolerance});
    CHECK(integer->samples()[1] == Near{32767.0 / 32768, tolerance});
    CHECK(floating->samples()[0] == Near{-.5, tolerance});
}

TEST_CASE("Every truncation of a WAVE file is rejected") {
    const auto bytes = wave(false);
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        CAPTURE(size);
        const auto cut = std::span<const std::byte>(bytes).first(size);
        CHECK_THROWS_WITH_AS(AudioClip::wav(cut), invalid_container, std::invalid_argument);
        if (size < junk_chunk)
            continue;
        // With the RIFF size matching the cut, the chunks themselves must be rejected.
        auto resized = std::vector<std::byte>(cut.begin(), cut.end());
        overwrite(resized, size_field, static_cast<std::uint32_t>(size - chunk_header));
        CHECK_THROWS_WITH_AS(AudioClip::wav(resized), truncation_error(size), std::invalid_argument);
    }
}

TEST_CASE("Malformed WAVE files and PCM samples are rejected") {
    auto malformed = wave(false);
    overwrite(malformed, junk_chunk + size_field, UINT32_MAX);
    CHECK_THROWS_WITH_AS(AudioClip::wav(malformed), truncated_chunk, std::invalid_argument);
    malformed = wave(true);
    const auto last_sample = malformed.size() - 4;
    overwrite(malformed, last_sample, std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN()));
    CHECK_THROWS_WITH_AS(AudioClip::wav(malformed), unnormalized, std::invalid_argument);
    auto bytes = wave(false);
    bytes[junk_chunk + chunk_header] = std::byte{0xff}; // JUNK body/padding is ignored
    CHECK_NOTHROW(AudioClip::wav(bytes));
    overwrite(bytes, sample_rate_field, 0);
    CHECK_THROWS_WITH_AS(AudioClip::wav(bytes), "Audio sample rate must be between 8000 and 192000 Hz",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(AudioClip::pcm({2}, 1, 8000), unnormalized, std::invalid_argument);
    CHECK_THROWS_WITH_AS(AudioClip::pcm({}, 1, 8000), "Invalid PCM channels or sample count", std::invalid_argument);
}
