// Vector art without a GPU: render_vector_art()'s pixel sizes and texels, checked against Linea's documented output,
// and the sheets that VectorAtlas packs from them: borders between frames, the color spread into them, frame lookup
// by name across scales, and rejected input. GPU checks cover how the renderer and the UI draw them.
#include <anima/vector.hpp>
#include <doctest/doctest.h>
#include <linea/Shapes.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace anima;
namespace {
using Texel = std::array<std::uint8_t, 4>;
// A layer that fills @p path with @p color.
linea::Layer filled(linea::Path path, linea::Color color) {
    linea::Layer layer;
    layer.path = std::move(path);
    layer.fill = color;
    return layer;
}
// Art named @p name that covers @p size units, its canvas the same size, drawn with @p layers.
VectorArt art(std::string name, Vec2 size, std::vector<linea::Node> layers) {
    return {std::move(name), linea::Drawing{size.x, size.y, std::move(layers)}, size};
}
// A disc of opaque @p color filling a canvas of @p size, so that every texel it touches holds that color.
VectorArt disc(std::string name, Vec2 size, linea::Color color) {
    const double radius = std::min(size.x, size.y) / 2;
    return art(std::move(name), size, {filled(linea::circle({size.x / 2., size.y / 2.}, radius), color)});
}
Texel texel_of(const Image &image, std::uint32_t x, std::uint32_t y) {
    const auto *p = &image.rgba[(std::size_t{y} * image.width + x) * 4];
    return {p[0], p[1], p[2], p[3]};
}
const Image &image_of(const VectorAtlas &atlas) { return *atlas.sheet()->definition().texture.image; }
const std::vector<SpriteFrame> &frames_of(const VectorAtlas &atlas) { return atlas.sheet()->definition().frames; }
// Whether @p a and @p b, each grown by @p margin texels on every side, overlap: along each axis, each starts before the
// other ends.
bool overlap(const SpriteFrame &a, const SpriteFrame &b, std::uint32_t margin) {
    const auto before = [&](std::uint32_t first, std::uint32_t other, std::uint32_t length) {
        return std::uint64_t{first} < std::uint64_t{other} + length + 2 * margin;
    };
    return before(a.x, b.x, b.width) && before(b.x, a.x, a.width) && before(a.y, b.y, b.height) &&
           before(b.y, a.y, a.height);
}
// Art of assorted sizes and colors, in no order of size.
std::vector<VectorArt> assorted() {
    std::vector<VectorArt> result;
    std::uint32_t seed = 7;
    const auto next = [&] {
        seed = seed * 1103515245U + 12345U;
        return (seed >> 16) % 23;
    };
    const std::array<linea::Color, 4> colors{
        {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}}};
    for (int i = 0; i < 37; ++i)
        result.push_back(disc("assorted/" + std::to_string(i), {float(next() + 1) * .75F, float(next() + 1) * .5F},
                              colors[std::size_t(i) % colors.size()]));
    return result;
}
} // namespace

TEST_CASE("Art renders at its size times the scale, each dimension rounded half away from zero") {
    struct Case {
        Vec2 size;
        float scale;
        std::uint32_t width, height;
    };
    // 12.5 and 2.5 round up, unlike to even, and a size under half a pixel still renders one.
    for (const auto &[size, scale, width, height] :
         {Case{{10, 3}, 1.25F, 13, 4}, Case{{2, 1}, 1.75F, 4, 2}, Case{{3, 5}, 1.5F, 5, 8}, Case{{2.5F, .5F}, 1, 3, 1},
          Case{{.2F, 1}, 1, 1, 1}, Case{{1, 1}, .4F, 1, 1}, Case{{8192.4F, 1}, 1, 8192, 1}, Case{{7, 9}, 2, 14, 18}}) {
        CAPTURE(size.x);
        CAPTURE(size.y);
        CAPTURE(scale);
        const auto drawn = disc("sized", size, {255, 0, 0, 255});
        const auto image = render_vector_art(drawn, scale);
        CHECK(image.width == width);
        CHECK(image.height == height);
        CHECK(image.format == ImageFormat::rgba8);
        CHECK(image.levels == 1);
        CHECK(image.rgba.size() == std::size_t{width} * height * 4);
        if (width < VectorAtlas::max_sheet_edge - 2) {
            const VectorAtlas atlas({drawn}, scale);
            REQUIRE(frames_of(atlas).size() == 1);
            CHECK(frames_of(atlas)[0].width == width);
            CHECK(frames_of(atlas)[0].height == height);
        }
    }
}

TEST_CASE("Linea's straight sRGB texels reach the image and the sheet as they are") {
    // Row 0, the top: 2.5 units of an opaque color, so that texel 2 is half covered and texel 3 transparent. Row 1:
    // a color at alpha 128 across the whole row. Linea composites in linear light and divides the premultiplied color
    // by alpha again, so every covered texel keeps its color's sRGB bytes, and coverage scales only alpha: round(0.5 *
    // 255) is 128.
    const auto drawing = art("conversion", {4, 2},
                             {filled(linea::rectangle({0, 0}, {2.5, 1}), {200, 100, 50, 255}),
                              filled(linea::rectangle({0, 1}, {4, 2}), {30, 60, 90, 128})});
    const std::array<std::array<Texel, 4>, 2> expected{{
        {{{200, 100, 50, 255}, {200, 100, 50, 255}, {200, 100, 50, 128}, {0, 0, 0, 0}}},
        {{{30, 60, 90, 128}, {30, 60, 90, 128}, {30, 60, 90, 128}, {30, 60, 90, 128}}},
    }};
    const auto image = render_vector_art(drawing, 1);
    REQUIRE(image.width == 4);
    REQUIRE(image.height == 2);
    for (std::uint32_t y = 0; y < 2; ++y)
        for (std::uint32_t x = 0; x < 4; ++x) {
            CAPTURE(x);
            CAPTURE(y);
            CHECK(texel_of(image, x, y) == expected[y][x]);
        }
    // In the sheet, the transparent texel takes the color of a nearest texel that is not transparent, one step away to
    // its left or below, and keeps alpha 0.
    const VectorAtlas atlas({drawing}, 1);
    const auto &frame = frames_of(atlas)[0];
    for (std::uint32_t y = 0; y < 2; ++y)
        for (std::uint32_t x = 0; x < 3 + y; ++x) {
            CAPTURE(x);
            CAPTURE(y);
            CHECK(texel_of(image_of(atlas), frame.x + x, frame.y + y) == expected[y][x]);
        }
    const auto spread = texel_of(image_of(atlas), frame.x + 3, frame.y);
    CHECK(spread[3] == 0);
    const Texel left{200, 100, 50, 0}, below{30, 60, 90, 0};
    CHECK((spread == left || spread == below));
    const auto &texture = atlas.sheet()->definition().texture;
    CHECK(texture.encoding == TextureEncoding::srgb);
    CHECK(texture.image->format == ImageFormat::rgba8);
}

TEST_CASE("The sheet filters linearly without mips and clamps at its edges") {
    const VectorAtlas atlas({disc("one", {4, 4}, {255, 0, 0, 255})}, 1);
    const auto &sampler = atlas.sheet()->definition().texture.sampler;
    CHECK(sampler.mag == Filter::linear);
    CHECK(sampler.min == Filter::linear);
    CHECK(sampler.u == Wrap::clamp);
    CHECK(sampler.v == Wrap::clamp);
    CHECK_FALSE(sampler.mipmapped);
}

TEST_CASE("Frames never overlap and keep a border from each other and the sheet's edges") {
    for (const float scale : {1.F, 1.25F, 1.5F, 2.F, 3.F}) {
        CAPTURE(scale);
        const VectorAtlas atlas(assorted(), scale);
        const auto &frames = frames_of(atlas);
        const auto &image = image_of(atlas);
        REQUIRE(frames.size() == atlas.art().size());
        CHECK(image.width <= VectorAtlas::max_sheet_edge);
        CHECK(image.height <= VectorAtlas::max_sheet_edge);
        for (std::size_t i = 0; i < frames.size(); ++i) {
            const auto &frame = frames[i];
            CAPTURE(i);
            const auto expected = render_vector_art(atlas.art()[i], scale);
            CHECK(frame.width == expected.width);
            CHECK(frame.height == expected.height);
            CHECK(frame.x >= VectorAtlas::padding);
            CHECK(frame.y >= VectorAtlas::padding);
            CHECK(std::uint64_t{frame.x} + frame.width + VectorAtlas::padding <= image.width);
            CHECK(std::uint64_t{frame.y} + frame.height + VectorAtlas::padding <= image.height);
            for (std::size_t j = 0; j < i; ++j) {
                CAPTURE(j);
                CHECK_FALSE(overlap(frame, frames[j], VectorAtlas::padding));
            }
        }
    }
}

TEST_CASE("Each frame's color spreads into its transparent texels and border, and nowhere else") {
    // Discs of one color each, whose covered texels all hold that color, and a frame of two halves whose border takes
    // the color of the nearer half.
    auto drawings = assorted();
    drawings.push_back(art("halves", {6, 4},
                           {filled(linea::rectangle({1, 1}, {3, 3}), {255, 0, 0, 255}),
                            filled(linea::rectangle({3, 1}, {5, 3}), {0, 0, 255, 255})}));
    for (const float scale : {1.F, 1.5F, 2.F}) {
        CAPTURE(scale);
        const VectorAtlas atlas(drawings, scale);
        const auto &image = image_of(atlas);
        const auto &frames = frames_of(atlas);
        std::vector<std::uint8_t> bordered(std::size_t{image.width} * image.height, 0);
        for (std::size_t i = 0; i + 1 < frames.size(); ++i) {
            const auto &frame = frames[i];
            const auto &color =
                std::get<linea::Color>(*std::get<linea::Layer>(atlas.art()[i].drawing.nodes[0].content).fill);
            for (auto y = frame.y - VectorAtlas::padding; y < frame.y + frame.height + VectorAtlas::padding; ++y)
                for (auto x = frame.x - VectorAtlas::padding; x < frame.x + frame.width + VectorAtlas::padding; ++x) {
                    bordered[std::size_t{y} * image.width + x] = 1;
                    const auto texel = texel_of(image, x, y);
                    const bool inside =
                        x >= frame.x && y >= frame.y && x < frame.x + frame.width && y < frame.y + frame.height;
                    CAPTURE(i);
                    CAPTURE(x);
                    CAPTURE(y);
                    CHECK(texel[0] == color.r);
                    CHECK(texel[1] == color.g);
                    CHECK(texel[2] == color.b);
                    if (!inside)
                        CHECK(texel[3] == 0);
                }
        }
        const auto &halves = frames.back();
        for (auto y = halves.y - 1; y < halves.y + halves.height + 1; ++y) {
            bordered[std::size_t{y} * image.width + halves.x - 1] = 1;
            bordered[std::size_t{y} * image.width + halves.x + halves.width] = 1;
            CHECK(texel_of(image, halves.x - 1, y) == Texel{255, 0, 0, 0});
            CHECK(texel_of(image, halves.x + halves.width, y) == Texel{0, 0, 255, 0});
        }
        for (auto y = halves.y - 1; y < halves.y + halves.height + 1; ++y)
            for (auto x = halves.x; x < halves.x + halves.width; ++x)
                bordered[std::size_t{y} * image.width + x] = 1;
        // Texels outside every border, which no frame's filtering reads, stay transparent black.
        for (std::uint32_t y = 0; y < image.height; ++y)
            for (std::uint32_t x = 0; x < image.width; ++x)
                if (!bordered[std::size_t{y} * image.width + x])
                    CHECK(texel_of(image, x, y) == Texel{0, 0, 0, 0});
    }
}

TEST_CASE("Frames are found by name and keep their indices across scales") {
    VectorAtlas atlas({disc("icons/b", {4, 2}, {255, 0, 0, 255}), disc("icons/a", {3, 3}, {0, 255, 0, 255}),
                       disc("c", {1, 5}, {0, 0, 255, 255})},
                      1);
    CHECK(atlas.scale() == 1);
    CHECK(atlas.frame("icons/b") == 0);
    CHECK(atlas.frame("icons/a") == 1);
    CHECK(atlas.frame("c") == 2);
    CHECK_THROWS_WITH_AS((void)atlas.frame("icons"), "Missing vector art: icons", std::out_of_range);
    CHECK_THROWS_WITH_AS((void)atlas.frame("ICONS/A"), "Missing vector art: ICONS/A", std::out_of_range);
    const auto before = atlas.sheet();
    atlas.set_scale(1);
    CHECK(atlas.sheet() == before);
    atlas.set_scale(2.5F);
    CHECK(atlas.scale() == 2.5F);
    CHECK(atlas.sheet() != before);
    CHECK(atlas.frame("icons/b") == 0);
    CHECK(atlas.frame("icons/a") == 1);
    CHECK(atlas.frame("c") == 2);
    const std::array<std::array<std::uint32_t, 2>, 3> sizes{{{10, 5}, {8, 8}, {3, 13}}};
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        CHECK(frames_of(atlas)[i].width == sizes[i][0]);
        CHECK(frames_of(atlas)[i].height == sizes[i][1]);
    }
    // The previous sheet stays whole for the batches that still hold it.
    CHECK(before->definition().frames[0].width == 4);
    CHECK(before->definition().texture.image->rgba.size() ==
          std::size_t{before->definition().texture.image->width} * before->definition().texture.image->height * 4);
}

TEST_CASE("The same art at the same scale packs the same sheet") {
    const VectorAtlas first(assorted(), 1.5F), second(assorted(), 1.5F);
    CHECK(image_of(first).width == image_of(second).width);
    CHECK(image_of(first).height == image_of(second).height);
    CHECK(image_of(first).rgba == image_of(second).rgba);
    for (std::size_t i = 0; i < frames_of(first).size(); ++i) {
        CHECK(frames_of(first)[i].x == frames_of(second)[i].x);
        CHECK(frames_of(first)[i].y == frames_of(second)[i].y);
    }
}

TEST_CASE("Vector art outside the documented limits is rejected") {
    const auto valid = disc("valid", {4, 4}, {255, 0, 0, 255});
    CHECK_THROWS_WITH_AS(VectorAtlas({}, 1), "Vector atlas needs at least one drawing", std::invalid_argument);
    for (const std::string name : {"", "memory:icons/coin", "/icons", "icons/", "icons//coin", "./coin",
                                   "icons/../coin", "icon coin", "icon(coin)", "icons\\coin", "caf\xc3\xa9"}) {
        auto renamed = valid;
        renamed.name = name;
        const auto message = "Invalid vector art name: " + name;
        CHECK_THROWS_WITH_AS(VectorAtlas({renamed}, 1), message.c_str(), std::invalid_argument);
    }
    auto copy = valid;
    CHECK_THROWS_WITH_AS(VectorAtlas({valid, disc("other", {1, 1}, {0, 0, 0, 255}), copy}, 1),
                         "Duplicate vector art name: valid", std::invalid_argument);
    constexpr auto infinity = std::numeric_limits<float>::infinity();
    constexpr auto nan = std::numeric_limits<float>::quiet_NaN();
    for (const Vec2 size : {Vec2{0, 1}, Vec2{1, -1}, Vec2{nan, 1}, Vec2{1, infinity}}) {
        auto sized = valid;
        sized.size = size;
        CHECK_THROWS_WITH_AS(VectorAtlas({sized}, 1), "Vector art size must be finite and positive",
                             std::invalid_argument);
        CHECK_THROWS_WITH_AS((void)render_vector_art(sized, 1), "Vector art size must be finite and positive",
                             std::invalid_argument);
    }
    for (const std::array<double, 2> canvas :
         {std::array{0., 1.}, std::array{1., -2.}, std::array{double(nan), 1.}, std::array{1., double(infinity)}}) {
        auto drawn = valid;
        drawn.drawing.width = canvas[0];
        drawn.drawing.height = canvas[1];
        CHECK_THROWS_WITH_AS(VectorAtlas({drawn}, 1), "Vector art canvas must be finite and positive",
                             std::invalid_argument);
        CHECK_THROWS_WITH_AS((void)render_vector_art(drawn, 1), "Vector art canvas must be finite and positive",
                             std::invalid_argument);
    }
    for (const float scale : {0.F, -1.F, nan, infinity}) {
        CHECK_THROWS_WITH_AS(VectorAtlas({valid}, scale), "Vector art scale must be finite and positive",
                             std::invalid_argument);
        CHECK_THROWS_WITH_AS((void)render_vector_art(valid, scale), "Vector art scale must be finite and positive",
                             std::invalid_argument);
    }
    // 8192.5 rounds to 8193 pixels, one past the limit.
    auto wide = valid;
    wide.size = {8192.5F, 1};
    CHECK_THROWS_WITH_AS((void)render_vector_art(wide, 1), "Vector art must render at most 8192 pixels along each axis",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(VectorAtlas({wide}, 1), "Vector art must render at most 8192 pixels along each axis",
                         std::invalid_argument);
    // A frame of 8191 texels and its border of one on each side are 8193 texels across.
    auto bordered = valid;
    bordered.size = {8191, 1};
    CHECK_THROWS_WITH_AS(VectorAtlas({bordered}, 1), "Vector art must fit one sheet of 8192 by 8192 texels",
                         std::invalid_argument);
    // Five frames of 4096 texels a side fit no square of 8192.
    std::vector<VectorArt> many;
    for (int i = 0; i < 5; ++i)
        many.push_back(disc("large/" + std::to_string(i), {4096, 4096}, {255, 0, 0, 255}));
    CHECK_THROWS_WITH_AS(VectorAtlas(many, 1), "Vector art must fit one sheet of 8192 by 8192 texels",
                         std::invalid_argument);
    // A rejected scale keeps the previous scale and sheet.
    VectorAtlas atlas({valid}, 1);
    const auto sheet = atlas.sheet();
    CHECK_THROWS_WITH_AS(atlas.set_scale(nan), "Vector art scale must be finite and positive", std::invalid_argument);
    CHECK_THROWS_WITH_AS(atlas.set_scale(4096), "Vector art must render at most 8192 pixels along each axis",
                         std::invalid_argument);
    CHECK(atlas.scale() == 1);
    CHECK(atlas.sheet() == sheet);
}

TEST_CASE("fits() says whether an atlas renders art at a scale, without rendering it") {
    constexpr auto infinity = std::numeric_limits<float>::infinity();
    constexpr auto nan = std::numeric_limits<float>::quiet_NaN();
    const auto valid = disc("valid", {2, 2}, {255, 0, 0, 255});
    CHECK(VectorAtlas::fits(std::vector{valid}, 1));
    // A frame wider than a sheet fits at a scale that narrows it enough, and an atlas renders it there.
    auto wide = valid;
    wide.size = {8192.5F, 1};
    CHECK_FALSE(VectorAtlas::fits(std::vector{wide}, 1));
    CHECK(VectorAtlas::fits(std::vector{wide}, .5F));
    CHECK_NOTHROW(VectorAtlas({wide}, .5F));
    // 8191 texels and a border of one on each side are 8193 across; at .99, 8109 and its border fit.
    auto bordered = valid;
    bordered.size = {8191, 1};
    CHECK_FALSE(VectorAtlas::fits(std::vector{bordered}, 1));
    CHECK(VectorAtlas::fits(std::vector{bordered}, .99F));
    CHECK_NOTHROW(VectorAtlas({bordered}, .99F));
    // Five square frames pack two to a shelf on three shelves: at .66 each is 2705 texels with its border, 8115 down,
    // and at .67 each is 2746, 8238 down, past a sheet even on shelves a whole sheet wide.
    std::vector<VectorArt> many;
    for (int i = 0; i < 5; ++i)
        many.push_back(disc("large/" + std::to_string(i), {4096, 4096}, {255, 0, 0, 255}));
    CHECK(VectorAtlas::fits(many, .66F));
    CHECK_FALSE(VectorAtlas::fits(many, .67F));
    CHECK_FALSE(VectorAtlas::fits(many, 1));
    // It rejects what an atlas rejects before packing.
    for (const float scale : {0.F, -1.F, nan, infinity})
        CHECK_THROWS_WITH_AS((void)VectorAtlas::fits(std::vector{valid}, scale),
                             "Vector art scale must be finite and positive", std::invalid_argument);
    auto unsized = valid;
    unsized.size = {0, 1};
    CHECK_THROWS_WITH_AS((void)VectorAtlas::fits(std::vector{unsized}, 1),
                         "Vector art size must be finite and positive", std::invalid_argument);
}
