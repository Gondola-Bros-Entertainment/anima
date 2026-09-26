#include <anima/audio.hpp>
#include <bit>
#include <cstdint>
#include <iostream>
#include <limits>

namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
void near(double a, double b, const char *message) { require(std::abs(a - b) < 1e-6, message); }
template <class F> void rejects(F f) {
    try {
        f();
    } catch (const std::exception &) {
        return;
    }
    throw std::runtime_error("Invalid audio operation accepted");
}
void word(std::vector<std::byte> &bytes, std::uint32_t value, unsigned width = 4) {
    for (unsigned i = 0; i < width; ++i)
        bytes.push_back(std::byte((value >> (8 * i)) & 255));
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
void playback() {
    using namespace anima;
    Audio audio(8000, 2);
    const auto clip = AudioClip::pcm({1, 1, 1, 1}, 1, 8000);
    auto sound = audio.sound(clip);
    sound.pan(-1);
    sound.play();
    std::array<float, 8> output{};
    audio.render(output);
    for (std::size_t i = 0; i < output.size(); i += 2) {
        near(output[i], 1, "Mono left pan failed");
        near(output[i + 1], 0, "Mono leaked right");
    }
    require(!sound.playing(), "Voice did not finish at the exact clip boundary");
    near(sound.cursor(), clip->duration(), "Finished cursor was not clamped");
    sound.looping(true);
    sound.play();
    std::array<float, 18> long_output{};
    audio.render(long_output);
    near(sound.cursor(), 1.0 / 8000, "Loop did not preserve fractional playback");
    sound.pause();
    audio.render(output);
    require(std::all_of(output.begin(), output.end(), [](float sample) { return sample == 0; }), "Paused voice played");
    near(sound.cursor(), 1.0 / 8000, "Paused cursor advanced");
    sound.stop();
    near(sound.cursor(), 0, "Stop did not rewind");
    sound.seek(clip->duration());
    sound.play();
    audio.render(output);
    near(output[0], 1, "Play at end did not restart");
    auto other = audio.sound(clip);
    rejects([&] { (void)audio.sound(clip); });
    other = {}; // destruction frees a capacity slot
    other = audio.sound(clip);
    Sound moved(std::move(other));
    rejects([&] { other.play(); });
    Audio foreign;
    rejects([&] { (void)foreign.sound(clip, audio.bus()); });
    rejects([&] { sound.seek(-1); });
    rejects([&] { sound.volume(std::numeric_limits<float>::quiet_NaN()); });
    rejects([&] { sound.pitch(0); });
    rejects([&] { sound.attenuation(5, 2); });
    rejects([&] { audio.listener({}, {}, {0, 1, 0}); });
}
void resample_and_mix() {
    using namespace anima;
    Audio audio(16000);
    auto sound = audio.sound(AudioClip::pcm({0, 1, 0, -1}, 1, 8000));
    sound.pan(-1);
    sound.play();
    std::array<float, 12> output{};
    audio.render(output);
    const std::array<float, 6> expected{0, .5F, 1, .5F, 0, -.5F};
    for (std::size_t i = 0; i < expected.size(); ++i)
        near(output[2 * i], expected[i], "Linear resampling failed");
    sound.stop();
    sound.pitch(2);
    sound.play();
    audio.render(output);
    near(output[2], 1, "Pitch did not change source advance");
    require(!sound.playing(), "Pitched voice failed to finish");
    auto stereo = audio.sound(AudioClip::pcm({.25F, -.5F, .25F, -.5F}, 2, 16000));
    stereo.play();
    audio.render(output);
    near(output[0], .25, "Stereo left changed");
    near(output[1], -.5, "Stereo right changed");
    stereo.volume(16);
    stereo.play();
    audio.render(output);
    near(output[0], 1, "Positive mixed sample did not limit");
    near(output[1], -1, "Negative sample did not limit");
}
void buses_fades_and_space() {
    using namespace anima;
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
    near(output[0], .25, "Bus gains did not compose");
    parent.muted(true);
    audio.render(output);
    near(output[0], 0, "Parent mute did not reach child");
    parent.muted(false);
    parent.volume(1);
    child.volume(1);
    sound.fade(0, 2.0 / 8000);
    audio.render(output);
    near(output[0], 1, "Fade start changed");
    near(output[2], .5, "Fade midpoint incorrect");
    near(output[4], 0, "Fade did not reach target");
    sound.volume(1);
    sound.spatial(true);
    sound.position({1, 0, 0});
    sound.attenuation(1, 3);
    audio.render(output);
    near(output[0], 0, "Right source leaked left");
    near(output[1], 1, "Right source lost gain");
    sound.position({2, 0, 0});
    audio.render(output);
    near(output[1], .5, "Distance attenuation failed");
    sound.position({3, 0, 0});
    audio.render(output);
    near(output[1], 0, "Source beyond radius remained audible");
    sound.position({1, 0, 0});
    audio.listener({});
    audio.render(output);
    near(output[1], 1, "The default listener did not face -Z");
    audio.listener({}, {0, 0, -1});
    audio.render(output);
    near(output[1], 1, "A listener facing -Z did not hear +X on its right");
    audio.listener({}, {0, 0, 1});
    audio.render(output);
    near(output[0], 1, "Listener orientation did not reverse panning");
    child = {};
    parent = {};
    audio.render(output);
    near(output[0], 1, "Voice lost retained bus hierarchy");
}
void partition_and_lifetime() {
    using namespace anima;
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
    require(whole == split, "Mix changed across output block sizes");
    Audio moved(std::move(a));
    moved.render(whole);
    rejects([&] { a.render(whole); });
    Sound survivor;
    {
        Audio temporary;
        survivor = temporary.sound(clip);
    }
    survivor.play();
    require(survivor.playing(), "Voice lost retained mixer context");
}
void decoding() {
    using namespace anima;
    const auto integer = AudioClip::wav(wave(false)), floating = AudioClip::wav(wave(true));
    near(integer->samples()[0], -1, "PCM16 negative decoding failed");
    near(integer->samples()[1], 32767.0 / 32768, "PCM16 positive decoding failed");
    near(floating->samples()[0], -.5, "Float32 WAVE decoding failed");
    auto bytes = wave(false);
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        rejects([&] { (void)AudioClip::wav(std::span<const std::byte>(bytes).first(n)); });
        if (n >= 12) {
            auto truncated = std::vector<std::byte>(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n));
            const auto declared = static_cast<std::uint32_t>(n - 8);
            for (unsigned i = 0; i < 4; ++i)
                truncated[4 + i] = std::byte((declared >> (8 * i)) & 255);
            rejects([&] { (void)AudioClip::wav(truncated); });
        }
    }
    auto malformed = bytes;
    malformed[16] = malformed[17] = malformed[18] = malformed[19] = std::byte{0xff};
    rejects([&] { (void)AudioClip::wav(malformed); });
    malformed = wave(true);
    const auto nan = std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN());
    for (unsigned i = 0; i < 4; ++i)
        malformed[malformed.size() - 4 + i] = std::byte((nan >> (8 * i)) & 255);
    rejects([&] { (void)AudioClip::wav(malformed); });
    bytes[20] = std::byte{0xff}; // JUNK body/padding is ignored
    (void)AudioClip::wav(bytes);
    bytes[34] = std::byte{0};
    bytes[35] = std::byte{0}; // sample rate becomes unsupported
    rejects([&] { (void)AudioClip::wav(bytes); });
    rejects([&] { (void)AudioClip::pcm({2}, 1, 8000); });
    rejects([&] { (void)AudioClip::pcm({}, 1, 8000); });
}
} // namespace
int main() {
    try {
        playback();
        resample_and_mix();
        buses_fades_and_space();
        partition_and_lifetime();
        decoding();
        std::cout << "PASS native mixing, WAV parsing, resampling, looping, fades, buses and spatial audio\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
