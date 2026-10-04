#pragma once
// Pixel checks of renderer captures, shared by the desktop and UI consumers and by anima_check. Captures are
// compared in memory. A failing comparison writes only the images it compared, as PNG files in the check's
// output directory, and a passing check writes no image. A check that finds no display or Vulkan device exits
// with `skipped`, which CTest reports as skipped, unless ANIMA_REQUIRE_GPU is set to a value other than 0.
#include <SDL3/SDL.h>
#include <algorithm>
#include <anima/desktop/vulkan_renderer.hpp>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gpu_check {
/// Exit status of a check that found no display or Vulkan device; the gpu tests' SKIP_RETURN_CODE.
constexpr int skipped = 77;
/// Longest a check waits for its frames. Software drivers such as lavapipe need far longer than GPUs, and
/// CTest's own limit still ends a check that hangs.
constexpr std::chrono::seconds watchdog{300};

/// SDL found no display, or could not load a Vulkan driver for a window.
class Unavailable : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/// Reports that no display or Vulkan device is available and returns the exit status: `skipped`, or 1 when
/// ANIMA_REQUIRE_GPU requires a device.
inline int unavailable(const std::exception &reason) {
    // SDL reads its own copy of the environment, portably, instead of the getenv that MSVC's runtime deprecates.
    const char *required = SDL_getenv("ANIMA_REQUIRE_GPU");
    if (required && *required && std::string_view(required) != "0") {
        std::cerr << "GPU check failed: ANIMA_REQUIRE_GPU is set, but no display or Vulkan device is available: "
                  << reason.what() << '\n';
        return 1;
    }
    std::cout << "GPU check skipped: no display or Vulkan device is available: " << reason.what() << '\n';
    return skipped;
}

/// SDL's video subsystem for the duration of a check.
class Video {
  public:
    /// Throws Unavailable when SDL has no video device, as without a display.
    Video() {
        if (!SDL_Init(SDL_INIT_VIDEO))
            throw Unavailable(std::string("SDL video: ") + SDL_GetError());
    }
    ~Video() { SDL_Quit(); }
    Video(const Video &) = delete;
    Video &operator=(const Video &) = delete;
};
using Window = std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)>;
/// Creates a Vulkan window, adding `SDL_WINDOW_VULKAN` to @p flags. Throws Unavailable when SDL cannot load a
/// Vulkan driver for it.
inline Window window(const char *title, int width, int height, SDL_WindowFlags flags = 0) {
    Window created{SDL_CreateWindow(title, width, height, flags | SDL_WINDOW_VULKAN), SDL_DestroyWindow};
    if (!created)
        throw Unavailable(std::string("Vulkan window: ") + SDL_GetError());
    return created;
}
/// A RendererOptions::log that prints each message on its own line, information to standard output and warnings and
/// errors to standard error, where CTest's failure expressions for the checks find swapchain creations, validation
/// messages and the renderer's final counters.
inline void log(anima::RendererLogLevel level, std::string_view message) {
    (level == anima::RendererLogLevel::info ? std::cout : std::cerr) << message << '\n';
}

using Image = anima::CapturedImage;
using Rgb = std::array<int, 3>;

/// Hands over the image of the renderer's latest request_capture(); throws when that frame was not read back.
inline Image take(anima::VulkanRenderer &renderer) {
    auto image = renderer.take_capture();
    if (!image)
        throw std::runtime_error("The frame requested for capture was not read back");
    return std::move(*image);
}

/// One draw() call timed on the caller's clock, with the FrameProfile it left.
struct TimedDraw {
    bool presented{};
    std::chrono::steady_clock::time_point began, ended;
    anima::FrameProfile profile;
    /// The call's duration, in milliseconds.
    [[nodiscard]] double wall_ms() const { return std::chrono::duration<double, std::milli>(ended - began).count(); }
    /// The sum of the profile's CPU fields, which follow one another through the call.
    [[nodiscard]] double cpu_ms() const {
        return profile.fence_wait_ms + profile.prepare_ms + profile.upload_ms + profile.acquire_ms +
               profile.record_submit_ms + profile.present_ms;
    }
};
/// Calls @p renderer's draw() once and times the call.
inline TimedDraw timed_draw(anima::VulkanRenderer &renderer) {
    TimedDraw timed;
    timed.began = std::chrono::steady_clock::now();
    timed.presented = renderer.draw();
    timed.ended = std::chrono::steady_clock::now();
    timed.profile = renderer.frame_profile();
    return timed;
}

inline Rgb pixel(const Image &image, std::size_t x, std::size_t y) {
    if (x >= image.width || y >= image.height)
        throw std::out_of_range("Pixel " + std::to_string(x) + ", " + std::to_string(y) + " is outside a " +
                                std::to_string(image.width) + "x" + std::to_string(image.height) + " capture");
    const auto *p = &image.rgb[(y * image.width + x) * 3];
    return {p[0], p[1], p[2]};
}
/// Largest channel difference.
inline int difference(const Rgb &a, const Rgb &b) {
    return std::max({std::abs(a[0] - b[0]), std::abs(a[1] - b[1]), std::abs(a[2] - b[2])});
}
inline std::string text(const Rgb &c) {
    return std::to_string(c[0]) + " " + std::to_string(c[1]) + " " + std::to_string(c[2]);
}
inline bool same(const Image &a, const Image &b) {
    return a.width == b.width && a.height == b.height && a.rgb == b.rgb;
}
inline bool same_size(const Image &a, const Image &b) { return a.width == b.width && a.height == b.height; }
/// Fraction of pixels whose largest channel difference exceeds @p threshold; throws for different sizes.
inline double changed(const Image &a, const Image &b, int threshold = 3) {
    if (!same_size(a, b))
        throw std::invalid_argument("Compared captures differ in size");
    std::size_t count = 0;
    for (std::size_t i = 0; i < a.rgb.size(); i += 3)
        if (difference({a.rgb[i], a.rgb[i + 1], a.rgb[i + 2]}, {b.rgb[i], b.rgb[i + 1], b.rgb[i + 2]}) > threshold)
            ++count;
    return double(count) / double(std::size_t{a.width} * a.height);
}
/// Pixels whose largest channel difference from the first pixel, the background, exceeds @p threshold.
inline std::size_t foreground(const Image &image, int threshold = 20) {
    const auto background = pixel(image, 0, 0);
    std::size_t count = 0;
    for (std::size_t i = 0; i < image.rgb.size(); i += 3)
        if (difference({image.rgb[i], image.rgb[i + 1], image.rgb[i + 2]}, background) > threshold)
            ++count;
    return count;
}
/// Whether every pixel equals the first.
inline bool uniform(const Image &image) {
    for (std::size_t i = 3; i < image.rgb.size(); ++i)
        if (image.rgb[i] != image.rgb[i % 3])
            return false;
    return true;
}
/// Mean absolute channel difference over every byte, and the fraction of pixels whose largest channel
/// difference exceeds 16.
struct Parity {
    double mean{};
    double large{};
};
inline Parity parity(const Image &reference, const Image &actual) {
    constexpr int large_difference = 16;
    if (!same_size(reference, actual))
        throw std::invalid_argument("Compared captures differ in size");
    std::uint64_t sum = 0;
    std::size_t large = 0;
    for (std::size_t i = 0; i < reference.rgb.size(); i += 3) {
        const Rgb a{reference.rgb[i], reference.rgb[i + 1], reference.rgb[i + 2]};
        const Rgb b{actual.rgb[i], actual.rgb[i + 1], actual.rgb[i + 2]};
        for (std::size_t c = 0; c < 3; ++c)
            sum += std::uint64_t(std::abs(a[c] - b[c]));
        if (difference(a, b) > large_difference)
            ++large;
    }
    return {double(sum) / double(reference.rgb.size()),
            double(large) / double(std::size_t{reference.width} * reference.height)};
}

namespace png {
inline std::uint32_t crc32(std::string_view type, const std::vector<std::uint8_t> &data) {
    std::uint32_t crc = 0xFFFFFFFFU;
    const auto add = [&](std::uint8_t byte) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    };
    for (const char c : type)
        add(static_cast<std::uint8_t>(c));
    for (const auto byte : data)
        add(byte);
    return ~crc;
}
inline void big_endian(std::vector<std::uint8_t> &out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}
/// Encodes 8-bit RGB (@p channels 3) or RGBA (4) rows, top to bottom, as a PNG whose zlib stream uses stored
/// deflate blocks: larger than a compressed PNG, but exact and free of dependencies.
inline std::vector<std::uint8_t> encode(std::uint32_t width, std::uint32_t height, unsigned channels,
                                        const std::vector<std::uint8_t> &pixels) {
    if ((channels != 3 && channels != 4) || pixels.size() != std::size_t{width} * height * channels)
        throw std::invalid_argument("PNG pixels do not match their size");
    std::vector<std::uint8_t> raw;
    const std::size_t row = std::size_t{width} * channels;
    for (std::size_t y = 0; y < height; ++y) {
        raw.push_back(std::uint8_t{0}); // Filter type None.
        raw.insert(raw.end(), pixels.begin() + static_cast<std::ptrdiff_t>(y * row),
                   pixels.begin() + static_cast<std::ptrdiff_t>((y + 1) * row));
    }
    std::vector<std::uint8_t> zlib{0x78, 0x01};
    constexpr std::size_t stored_block = 65535;
    std::size_t offset = 0;
    do {
        const auto length = std::min(stored_block, raw.size() - offset);
        const bool last = offset + length == raw.size();
        zlib.push_back(static_cast<std::uint8_t>(last ? 1 : 0));
        for (const auto half : {std::uint16_t(length), std::uint16_t(~length)}) {
            zlib.push_back(static_cast<std::uint8_t>(half & 0xFFU));
            zlib.push_back(static_cast<std::uint8_t>(half >> 8));
        }
        zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
                    raw.begin() + static_cast<std::ptrdiff_t>(offset + length));
        offset += length;
    } while (offset < raw.size());
    constexpr std::uint32_t adler_modulus = 65521;
    std::uint32_t low = 1, high = 0;
    for (const auto byte : raw) {
        low = (low + byte) % adler_modulus;
        high = (high + low) % adler_modulus;
    }
    big_endian(zlib, (high << 16) | low);
    std::vector<std::uint8_t> file{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    const auto chunk = [&](std::string_view type, const std::vector<std::uint8_t> &data) {
        big_endian(file, static_cast<std::uint32_t>(data.size()));
        file.insert(file.end(), type.begin(), type.end());
        file.insert(file.end(), data.begin(), data.end());
        big_endian(file, crc32(type, data));
    };
    std::vector<std::uint8_t> header;
    big_endian(header, width);
    big_endian(header, height);
    header.insert(header.end(), {8, static_cast<std::uint8_t>(channels == 4 ? 6 : 2), 0, 0, 0});
    chunk("IHDR", header);
    chunk("IDAT", zlib);
    chunk("IEND", {});
    return file;
}
} // namespace png

/// Named captures of one check and the directory that receives the images of a failed comparison.
class Captures {
  public:
    explicit Captures(std::filesystem::path failures) : failures_(std::move(failures)) {}
    /// Keeps @p image as @p name, replacing an earlier image of that name.
    void add(const std::string &name, Image image) { images_.insert_or_assign(name, std::move(image)); }
    /// Releases images that no later comparison needs.
    void discard(std::initializer_list<std::string_view> names) {
        for (const auto name : names)
            if (const auto found = images_.find(name); found != images_.end())
                images_.erase(found);
    }
    [[nodiscard]] const Image &operator[](const std::string &name) const {
        const auto found = images_.find(name);
        if (found == images_.end())
            throw std::out_of_range("Capture " + name + " was not taken");
        return found->second;
    }
    [[nodiscard]] std::size_t size() const noexcept { return images_.size(); }
    /// Writes @p names as PNG files in the output directory, then throws `std::runtime_error` with @p message
    /// and the paths written.
    [[noreturn]] void fail(const std::string &message, const std::vector<std::string> &names) const {
        std::ostringstream report;
        report << message;
        std::filesystem::create_directories(failures_);
        const char *separator = "; compared ";
        for (const auto &name : names) {
            const auto &image = (*this)[name];
            const auto path = failures_ / (name + ".png");
            const auto bytes = png::encode(image.width, image.height, 3, image.rgb);
            std::ofstream file(path, std::ios::binary);
            file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!file)
                throw std::runtime_error(message + "; could not write " + path.string());
            report << separator << path.string();
            separator = ", ";
        }
        throw std::runtime_error(report.str());
    }
    void require(bool condition, const std::string &message, const std::vector<std::string> &names) const {
        if (!condition)
            fail(message, names);
    }
    /// Requires identical size and pixels.
    void require_same(const std::string &a, const std::string &b, const std::string &message) const {
        require(same((*this)[a], (*this)[b]), message + ": " + a + " differs from " + b, {a, b});
    }
    /// Requires more than @p minimum of the pixels to change by more than 3 levels between @p a and @p b.
    void require_changed(const std::string &a, const std::string &b, double minimum, const std::string &message) const {
        const auto fraction = changed((*this)[a], (*this)[b]);
        require(fraction > minimum,
                message + ": " + std::to_string(fraction) + " of the pixels changed between " + a + " and " + b +
                    ", not more than " + std::to_string(minimum),
                {a, b});
    }
    /// Requires more than 1 percent of the pixels to differ from the background by more than 20 levels.
    void require_foreground(const std::string &name, const std::string &message) const {
        constexpr double minimum = .01;
        const auto &image = (*this)[name];
        const auto count = foreground(image);
        require(double(count) > double(std::size_t{image.width} * image.height) * minimum,
                message + ": only " + std::to_string(count) + " pixels of " + name + " differ from its background",
                {name});
    }
    /// Requires one color everywhere, darker than 80 in every channel.
    void require_clear(const std::string &name, const std::string &message) const {
        constexpr int darkest_background = 80;
        const auto &image = (*this)[name];
        const auto background = pixel(image, 0, 0);
        require(uniform(image), message + ": " + name + " is not one color", {name});
        require(std::max({background[0], background[1], background[2]}) < darkest_background,
                message + ": " + name + " has the background " + text(background), {name});
    }
    /// Requires a mean channel difference below 0.5 and at most 0.2 percent of the pixels differing by more than
    /// 16 levels, which allows floating-point skinning or rasterization to move an edge by a pixel.
    void require_parity(const std::string &reference, const std::string &actual) const {
        constexpr double mean_limit = .5, large_limit = .002;
        const auto result = parity((*this)[reference], (*this)[actual]);
        require(result.mean < mean_limit && result.large < large_limit,
                actual + " differs from " + reference + ": mean channel difference " + std::to_string(result.mean) +
                    ", pixels beyond 16 levels " + std::to_string(result.large),
                {reference, actual});
    }

  private:
    std::filesystem::path failures_;
    std::map<std::string, Image, std::less<>> images_;
};
} // namespace gpu_check
