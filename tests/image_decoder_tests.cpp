#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <memory>

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
// Engine-authored two-pixel PNGs exercise the 8-bit row copy and the 16-bit
// grayscale-to-RGBA conversion through stb's public in-memory decoding API.
constexpr unsigned char rgb8_png[]{
    137, 80, 78, 71, 13, 10,  26,  10,  0,   0,  0, 13, 73, 72, 68, 82, 0,  0,   0,   2,  0,   0,   0, 1,
    8,   2,  0,  0,  0,  123, 64,  232, 221, 0,  0, 0,  13, 73, 68, 65, 84, 120, 156, 99, 248, 207, 0, 4,
    255, 1,  7,  0,  1,  255, 226, 35,  158, 89, 0, 0,  0,  0,  73, 69, 78, 68,  174, 66, 96,  130,
};
constexpr unsigned char gray16_png[]{
    137, 80, 78, 71, 13, 10,  26,  10,  0,   0,  0, 13, 73, 72, 68, 82, 0,  0,   0,   2,  0,  0,   0,  1,
    16,  0,  0,  0,  0,  129, 217, 252, 21,  0,  0, 0,  13, 73, 68, 65, 84, 120, 156, 99, 16, 50,  89, 125,
    22,  0,  3,  12, 1,  191, 110, 185, 198, 93, 0, 0,  0,  0,  73, 69, 78, 68,  174, 66, 96, 130,
};
} // namespace

TEST_CASE("An 8-bit PNG decodes its row bytes unchanged") {
    int width = 0, height = 0, channels = 0;
    const std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> rgb{
        stbi_load_from_memory(rgb8_png, static_cast<int>(sizeof(rgb8_png)), &width, &height, &channels, 3),
        stbi_image_free};
    REQUIRE(rgb.get() != nullptr);
    REQUIRE(width == 2);
    REQUIRE(height == 1);
    CHECK(channels == 3);
    std::array<unsigned char, 6> actual{};
    std::copy_n(rgb.get(), actual.size(), actual.begin());
    CHECK(actual == std::array<unsigned char, 6>{255, 0, 0, 0, 0, 255});
}

TEST_CASE("A 16-bit grayscale PNG converts to RGBA with opaque alpha") {
    int width = 0, height = 0, channels = 0;
    const std::unique_ptr<stbi_us, decltype(&stbi_image_free)> rgba{
        stbi_load_16_from_memory(gray16_png, static_cast<int>(sizeof(gray16_png)), &width, &height, &channels, 4),
        stbi_image_free};
    REQUIRE(rgba.get() != nullptr);
    REQUIRE(width == 2);
    REQUIRE(height == 1);
    CHECK(channels == 1);
    std::array<unsigned short, 8> actual{};
    std::copy_n(rgba.get(), actual.size(), actual.begin());
    CHECK(actual == std::array<unsigned short, 8>{0x1234, 0x1234, 0x1234, 0xffff, 0xabcd, 0xabcd, 0xabcd, 0xffff});
}

TEST_CASE("An oversized channel conversion fails without reaching the allocator") {
    // Exercise the internal boundary directly: production rejects these image
    // dimensions before conversion. Expanded RGBA16 sizes are 2 GiB and 4 GiB;
    // the latter wrapped to zero in the original unsigned multiplication.
    for (const auto width : {16384U, 32768U}) {
        CAPTURE(width);
        auto *source = static_cast<stbi__uint16 *>(std::malloc(sizeof(stbi__uint16)));
        REQUIRE(source != nullptr);
        allocation_calls = 0;
        free_calls = 0;
        const auto *result = stbi__convert_format16(source, 1, 4, width, 16384U);
        CHECK(result == nullptr);
        CHECK(allocation_calls == 0);
        CHECK(free_calls == 1); // The rejected conversion released its input.
    }
}
