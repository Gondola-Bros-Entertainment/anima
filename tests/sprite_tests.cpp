// Sprite sheets, sprites and sprite shaders without a GPU: the validation that SpriteSheet's and SpriteShader's
// constructors and validate_sprites() apply, which VulkanRenderer::set_sprites() relies on. Sprite shaders are
// assembled word by word (spirv_module.hpp), so each test states exactly the interface it checks.
#include "spirv_module.hpp"
#include <anima/sprite.hpp>
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace anima;
using namespace spirv_test;
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
    sprite.parameters = {-1e30F, 0, 1e30F, -0.F};
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
    for (std::size_t i = 0; i < sprite.parameters.size(); ++i)
        for (const float value : {nan, infinity, -infinity}) {
            sprite = {};
            sprite.parameters[i] = value;
            rejected(sprite, "Sprite parameters must be finite");
        }
}

namespace {
// A sprite shader that writes a color and reads nothing.
Module sprite_module() {
    Module shader(fragment_model);
    (void)shader.output_at(0, shader.vector(4));
    return shader;
}
void rejects(const Module &shader, const std::string &message) {
    CHECK_THROWS_WITH_AS((void)SpriteShader(shader.words()), message.c_str(), std::invalid_argument);
}
} // namespace

TEST_CASE("A sprite shader may read every input of the sprite shader interface") {
    auto shader = sprite_module();
    (void)shader.input_at(0, shader.vector(2));
    (void)shader.input_at(1, shader.vector(4));
    (void)shader.input_at(2, shader.vector(2));
    (void)shader.input_at(3, shader.vector(4));
    (void)shader.sampler(1, 0);
    // The frame block's time and viewport, at the offsets that custom materials read them.
    (void)shader.resource(uniform, shader.structure({{shader.vector(4), 272}, {shader.scalar(), 288}}), 0, 0);
    const auto words = shader.words();
    const SpriteShader result(words);
    CHECK(result.fragment_shader() == words);
    // Or any subset of them, or none.
    CHECK_NOTHROW((void)SpriteShader(sprite_module().words()));
    auto parameters = sprite_module();
    (void)parameters.input_at(3, parameters.vector(4));
    CHECK_NOTHROW((void)SpriteShader(parameters.words()));
}

TEST_CASE("Sprite shaders outside the sprite shader interface are rejected") {
    CHECK_THROWS_WITH_AS((void)SpriteShader({}),
                         "The sprite shader is not SPIR-V: it is shorter than the five-word module header",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS((void)SpriteShader({0xDEADBEEF, 0x00010300, 0, 1, 0}),
                         "The sprite shader is not SPIR-V: its first word is not the SPIR-V magic number",
                         std::invalid_argument);
    auto later = sprite_module();
    later.version(0x00010500);
    rejects(later, "The sprite shader is SPIR-V 1.5, but sprite shaders accept SPIR-V 1.0 to 1.3");
    auto doubles = sprite_module();
    doubles.capability(10);
    rejects(doubles, "The sprite shader requires SPIR-V capability 10, which sprite shaders do not support");
    Module vertex(vertex_model);
    rejects(vertex, "The sprite shader has no fragment entry point named main");

    // Inputs: only what the sprite vertex shader writes, with its types.
    auto shader = sprite_module();
    (void)shader.input_at(0, shader.vector(3));
    rejects(shader, "The sprite shader reads location 0 as vec3, but the sprite vertex shader writes a vec2 there");
    shader = sprite_module();
    (void)shader.input_at(3, shader.vector(2));
    rejects(shader, "The sprite shader reads location 3 as vec2, but the sprite vertex shader writes a vec4 there");
    shader = sprite_module();
    (void)shader.input_at(4, shader.vector(4));
    rejects(shader, "The sprite shader reads location 4 as vec4, which the sprite vertex shader does not write");

    // Resources: only the frame block and the sheet.
    shader = sprite_module();
    (void)shader.resource(uniform, shader.structure({{shader.vector(4), 0}}), 2, 0);
    rejects(shader, "The sprite shader declares set 2 binding 0, which the sprite shader interface does not provide");
    shader = sprite_module();
    (void)shader.sampler(0, 1);
    rejects(shader, "The sprite shader declares set 0 binding 1, which the sprite shader interface does not provide");
    shader = sprite_module();
    (void)shader.sampler(1, 1);
    rejects(shader, "The sprite shader declares set 1 binding 1, which the sprite shader interface does not provide");
    shader = sprite_module();
    (void)shader.poses();
    rejects(shader, "The sprite shader declares set 1 binding 0 as storage buffer, but the sprite shader interface "
                    "provides the sheet's sampler2D there");
    shader = sprite_module();
    (void)shader.sampler(1, 0, true);
    rejects(shader, "The sprite shader declares set 1 binding 0 as sampler2DShadow, but the sprite shader interface "
                    "provides the sheet's sampler2D there");
    shader = sprite_module();
    (void)shader.resource(uniform, shader.structure({{shader.vector(4), 288}}), 0, 0);
    rejects(shader, "The sprite shader declares the frame block member at offset 288 as vec4, but the sprite shader "
                    "interface has a float there");
    shader = sprite_module();
    (void)shader.variable(push_constant, shader.structure({{shader.mat4(), 0}}, {0}));
    rejects(shader, "The sprite shader declares push constants, which the sprite shader interface does not provide");

    // Output: one vec4 at location 0.
    rejects(Module(fragment_model), "The sprite shader writes no color at location 0");
    shader = Module(fragment_model);
    (void)shader.output_at(0, shader.vector(3));
    rejects(shader, "The sprite shader writes location 0 as vec3, but sprite shaders write a vec4 there");
    shader = sprite_module();
    (void)shader.output_at(1, shader.vector(4));
    rejects(shader, "The sprite shader writes location 1, but sprite shaders have one color output at location 0");
}
