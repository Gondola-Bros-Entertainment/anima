#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
unsigned allocation_calls = 0;
unsigned free_calls = 0;

void *bounded_malloc(std::size_t size) {
    ++allocation_calls;
    return size && size <= 4096 ? std::malloc(size) : nullptr;
}
void *bounded_realloc(void *pointer, std::size_t size) {
    ++allocation_calls;
    return size && size <= 4096 ? std::realloc(pointer, size) : nullptr;
}
void counted_free(void *pointer) {
    if (pointer)
        ++free_calls;
    std::free(pointer);
}
} // namespace

// Match the production decoder's formats and dimension cap. The allocator cap
// makes overflow regressions fail without reserving or touching large buffers.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 8192
#define STBI_MALLOC(size) bounded_malloc(size)
#define STBI_REALLOC(pointer, size) bounded_realloc(pointer, size)
#define STBI_FREE(pointer) counted_free(pointer)
#include <stb_image.h>

namespace {
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

// Engine-authored two-pixel PNGs exercise the 8-bit row copy and the 16-bit
// grayscale-to-RGBA conversion through stb's public in-memory decoding API.
constexpr unsigned char rgb8_png[]{
    137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82,
    0, 0, 0, 2, 0, 0, 0, 1, 8, 2, 0, 0, 0, 123, 64, 232,
    221, 0, 0, 0, 13, 73, 68, 65, 84, 120, 156, 99, 248, 207, 0, 4,
    255, 1, 7, 0, 1, 255, 226, 35, 158, 89, 0, 0, 0, 0, 73, 69,
    78, 68, 174, 66, 96, 130,
};
constexpr unsigned char gray16_png[]{
    137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82,
    0, 0, 0, 2, 0, 0, 0, 1, 16, 0, 0, 0, 0, 129, 217, 252,
    21, 0, 0, 0, 13, 73, 68, 65, 84, 120, 156, 99, 16, 50, 89, 125,
    22, 0, 3, 12, 1, 191, 110, 185, 198, 93, 0, 0, 0, 0, 73, 69,
    78, 68, 174, 66, 96, 130,
};

void decode_small_pngs() {
    int width = 0, height = 0, channels = 0;
    const std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> rgb{
        stbi_load_from_memory(rgb8_png, static_cast<int>(sizeof(rgb8_png)), &width, &height, &channels, 3),
        stbi_image_free};
    require(rgb && width == 2 && height == 1 && channels == 3, "8-bit PNG decoding failed");
    const std::array<unsigned char, 6> expected_rgb{255, 0, 0, 0, 0, 255};
    for (std::size_t i = 0; i < expected_rgb.size(); ++i)
        require(rgb.get()[i] == expected_rgb[i], "8-bit PNG row bytes changed");

    const std::unique_ptr<stbi_us, decltype(&stbi_image_free)> rgba{
        stbi_load_16_from_memory(gray16_png, static_cast<int>(sizeof(gray16_png)), &width, &height, &channels, 4),
        stbi_image_free};
    require(rgba && width == 2 && height == 1 && channels == 1, "16-bit PNG decoding failed");
    const std::array<unsigned short, 8> expected_rgba{0x1234, 0x1234, 0x1234, 0xffff,
                                                   0xabcd, 0xabcd, 0xabcd, 0xffff};
    for (std::size_t i = 0; i < expected_rgba.size(); ++i)
        require(rgba.get()[i] == expected_rgba[i], "16-bit PNG channel conversion changed");
}

void reject_oversized_conversion() {
    // Exercise the internal boundary directly: production rejects these image
    // dimensions before conversion. Expanded RGBA16 sizes are 2 GiB and 4 GiB;
    // the latter wrapped to zero in the original unsigned multiplication.
    for (const auto width : {16384U, 32768U}) {
        auto *source = static_cast<stbi__uint16 *>(std::malloc(sizeof(stbi__uint16)));
        require(source != nullptr, "Test source allocation failed");
        allocation_calls = 0;
        free_calls = 0;
        const auto *result = stbi__convert_format16(source, 1, 4, width, 16384U);
        require(result == nullptr, "Oversized channel conversion was accepted");
        require(allocation_calls == 0, "Oversized channel conversion reached the allocator");
        require(free_calls == 1, "Rejected channel conversion did not release its input");
    }
}
} // namespace

int main() {
    try {
        decode_small_pngs();
        reject_oversized_conversion();
        std::cout << "PASS PNG row copying, 16-bit channel conversion and checked allocation rejection\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
