// Sprite sheets and sprites without a GPU: the validation that SpriteSheet's constructor and validate_sprites() apply,
// which VulkanRenderer::set_sprites() relies on.
#include <anima/sprite.hpp>
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace anima;
namespace {
// A texture of @p width by @p height opaque white RGBA8 texels.
Texture white(std::uint32_t width, std::uint32_t height) {
    auto image = std::make_shared<Image>();
    image->width = width;
    image->height = height;
    image->rgba.assign(std::size_t{width} * height * 4, 255);
    return {std::move(image), {}, TextureEncoding::srgb};
}
std::shared_ptr<const SpriteSheet> sheet(std::vector<SpriteFrame> frames) {
    return std::make_shared<const SpriteSheet>(SpriteSheetDefinition{white(8, 4), std::move(frames)});
}
// One batch of @p sprite from a sheet of two frames.
std::vector<SpriteBatch> batch_of(const Sprite &sprite) {
    return {{sheet({{0, 0, 4, 4}, {4, 0, 4, 4}}), SpriteBlend::blended, {sprite}}};
}
} // namespace

TEST_CASE("A sprite sheet keeps its texture and frames") {
    const auto texture = white(8, 4);
    const SpriteSheet result({texture, {{0, 0, 8, 4}, {7, 3, 1, 1}}});
    CHECK(result.definition().texture.image == texture.image);
    REQUIRE(result.definition().frames.size() == 2);
    CHECK(result.definition().frames[1].x == 7);
    CHECK(result.definition().frames[1].height == 1);
}

TEST_CASE("Sprite sheets outside the documented limits are rejected") {
    CHECK_THROWS_WITH_AS(SpriteSheet({white(8, 4), {}}), "Sprite sheet needs at least one frame",
                         std::invalid_argument);
    constexpr auto most = std::numeric_limits<std::uint32_t>::max();
    for (const SpriteFrame frame : {SpriteFrame{0, 0, 0, 4}, SpriteFrame{0, 0, 8, 0}, SpriteFrame{1, 0, 8, 4},
                                    SpriteFrame{0, 1, 8, 4}, SpriteFrame{most, 0, 2, 1}, SpriteFrame{0, most, 1, 2}})
        CHECK_THROWS_WITH_AS(SpriteSheet({white(8, 4), {frame}}), "Sprite frame must lie inside its sheet's image",
                             std::invalid_argument);
    // The texture rules are validate_scene()'s, including that a sheet's image has texels.
    CHECK_THROWS_AS(SpriteSheet({Texture{}, {{0, 0, 1, 1}}}), std::invalid_argument);
    auto described = std::make_shared<Image>();
    described->width = described->height = 4;
    CHECK_THROWS_WITH_AS(SpriteSheet({Texture{described, {}, TextureEncoding::srgb}, {{0, 0, 1, 1}}}),
                         "Texture image has no texels", std::invalid_argument);
    auto short_image = std::make_shared<Image>();
    short_image->width = short_image->height = 4;
    short_image->rgba.assign(4, 255);
    CHECK_THROWS_AS(SpriteSheet({Texture{short_image, {}, TextureEncoding::srgb}, {{0, 0, 1, 1}}}),
                    std::invalid_argument);
}

TEST_CASE("Sprites within the documented ranges are accepted") {
    CHECK_NOTHROW(validate_sprites({}));
    CHECK_NOTHROW(validate_sprites(batch_of({})));
    Sprite sprite;
    sprite.position = {-1e6F, 2, -3};
    sprite.size = {-4, 0};
    sprite.pivot = {-1, 2};
    sprite.rotation = 100;
    sprite.frame = 1;
    sprite.color = {0, 4, 1};
    sprite.alpha = 0;
    CHECK_NOTHROW(validate_sprites(batch_of(sprite)));
    std::vector<SpriteBatch> additive_empty{{sheet({{0, 0, 1, 1}}), SpriteBlend::additive, {}}};
    CHECK_NOTHROW(validate_sprites(additive_empty));
}

TEST_CASE("Sprites outside the documented ranges are rejected") {
    std::vector<SpriteBatch> sheetless{{nullptr, SpriteBlend::blended, {}}};
    CHECK_THROWS_WITH_AS(validate_sprites(sheetless), "Sprite batch has no sheet", std::invalid_argument);
    auto unknown = batch_of({});
    unknown[0].blend = static_cast<SpriteBlend>(2);
    CHECK_THROWS_WITH_AS(validate_sprites(unknown), "Unknown sprite blend", std::invalid_argument);

    constexpr auto infinity = std::numeric_limits<float>::infinity();
    constexpr auto nan = std::numeric_limits<float>::quiet_NaN();
    const auto rejected = [](const Sprite &sprite, const char *message) {
        CHECK_THROWS_WITH_AS(validate_sprites(batch_of(sprite)), message, std::invalid_argument);
    };
    Sprite sprite;
    sprite.position.z = nan;
    rejected(sprite, "Sprite placement must be finite");
    sprite = {};
    sprite.size.x = infinity;
    rejected(sprite, "Sprite placement must be finite");
    sprite = {};
    sprite.pivot.y = -infinity;
    rejected(sprite, "Sprite placement must be finite");
    sprite = {};
    sprite.rotation = nan;
    rejected(sprite, "Sprite placement must be finite");
    sprite = {};
    sprite.frame = 2;
    rejected(sprite, "Sprite frame is not in its sheet");
    for (const Vec3 color : {Vec3{-1, 0, 0}, Vec3{0, nan, 0}, Vec3{0, 0, infinity}}) {
        sprite = {};
        sprite.color = color;
        rejected(sprite, "Sprite color must be finite and non-negative");
    }
    for (const float alpha : {-.1F, 1.1F, nan}) {
        sprite = {};
        sprite.alpha = alpha;
        rejected(sprite, "Sprite alpha must be in [0, 1]");
    }
}
