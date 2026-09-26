#include <anima/assets/material_textures.hpp>
#include <anima/assets/mesh_preparation.hpp>
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace anima;
namespace {
constexpr float cutout_cutoff = .75F; // The cutout's alpha test.
constexpr auto invalid_texture = "Invalid texture dimensions or byte count";

std::size_t covered(const MipLevel &level, float cutoff) {
    std::size_t count = 0;
    for (std::size_t i = 3; i < level.rgba.size(); i += 4)
        count += float(level.rgba[i]) / 255 >= cutoff;
    return count;
}
// An 8x8 cutout of opaque white leaves on transparent magenta; its 2x2 blocks hold 0 to 4 leaf texels, 22 in all.
Texture cutout() {
    Texture texture{8, 8, std::vector<std::uint8_t>(8 * 8 * 4), {}};
    constexpr unsigned opaque_per_block[]{0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 2, 2, 3, 4, 4};
    for (unsigned block = 0; block < 16; ++block)
        for (unsigned texel = 0; texel < 4; ++texel) {
            const auto x = block % 4 * 2 + texel % 2, y = block / 4 * 2 + texel / 2;
            const auto offset = (y * 8 + x) * 4;
            const bool visible = texel < opaque_per_block[block];
            texture.rgba[offset] = texture.rgba[offset + 2] = 255;
            texture.rgba[offset + 1] = visible ? 255 : 0;
            texture.rgba[offset + 3] = visible ? 255 : 0;
        }
    return texture;
}
Texture black_and_white() { return {2, 1, {0, 0, 0, 255, 255, 255, 255, 255}, {}}; }
// Masks on the cutout: one, an equivalent one at half alpha with half the cutoff, one with another cutoff, and
// an opaque material that also emits the cutout.
std::array<Material, 4> materials() {
    Material mask;
    mask.texture = 0;
    mask.alpha_mode = AlphaMode::mask;
    mask.alpha_cutoff = .5F;
    auto same = mask;
    same.alpha = .5F;
    same.alpha_cutoff = .25F;
    auto different = mask;
    different.alpha_cutoff = .75F;
    auto opaque = mask;
    opaque.alpha_mode = AlphaMode::opaque;
    opaque.emissive_texture = 0;
    return {mask, same, different, opaque};
}
// The cutout, and a data image that no material uses and that should not upload.
std::array<Texture, 2> textures() {
    auto data = black_and_white();
    data.encoding = TextureEncoding::linear;
    return {cutout(), data};
}
} // namespace

TEST_CASE("Alpha coverage correction keeps the base level and restores each mip's cutout") {
    const auto source = cutout();
    const auto original = source.rgba;
    const auto plain = texture_mips(source);
    const auto masked = texture_mips(source, {.alpha_coverage_cutoff = cutout_cutoff});
    CHECK(source.rgba == original);
    CHECK(masked.front().rgba == original);
    // Filtering alone thins the cutout; correction restores the nearest attainable coverage.
    CHECK(covered(masked.front(), cutout_cutoff) == 22);
    CHECK(covered(plain[1], cutout_cutoff) == 2);
    CHECK(covered(masked[1], cutout_cutoff) == 5);
    // Transparent colour does not reach a visible leaf.
    for (std::size_t i = 0; i < masked[1].rgba.size(); i += 4)
        if (masked[1].rgba[i + 3]) {
            CAPTURE(i);
            CHECK(masked[1].rgba[i] == 255);
            CHECK(masked[1].rgba[i + 1] == 255);
            CHECK(masked[1].rgba[i + 2] == 255);
        }
    for (std::size_t level = 1; level < masked.size(); ++level) {
        CAPTURE(level);
        const auto desired = 22.0 / 64 * (masked[level].width * masked[level].height);
        CHECK(std::abs(double(covered(masked[level], cutout_cutoff)) - desired) <=
              std::abs(double(covered(plain[level], cutout_cutoff)) - desired));
    }
    CHECK(texture_mips(source, {.alpha_coverage_cutoff = cutout_cutoff})[1].rgba == masked[1].rgba);
}

TEST_CASE("Coverage correction keeps as much alpha as the cutoff allows") {
    // One visible texel out of four: the 1x1 mip must be below the cutoff,
    // but retain as much alpha as possible for interpolation with its parent.
    const Texture sparse{2, 2, {255, 255, 255, 127, 255, 255, 255, 127, 255, 255, 255, 127, 255, 255, 255, 255}, {}};
    CHECK(texture_mips(sparse, {.alpha_coverage_cutoff = .5F}).back().rgba[3] == 127);
}

TEST_CASE("Uniform alpha keeps its coverage at every level") {
    for (const auto alpha : {std::uint8_t{0}, std::uint8_t{255}}) {
        CAPTURE(alpha);
        auto solid = cutout();
        for (std::size_t i = 3; i < solid.rgba.size(); i += 4)
            solid.rgba[i] = alpha;
        for (const auto &mip : texture_mips(solid, {.alpha_coverage_cutoff = 1.F})) {
            CAPTURE(mip.width);
            CHECK(covered(mip, 1.F) == (alpha ? mip.width * mip.height : 0U));
        }
    }
}

TEST_CASE("An odd edge texel is filtered, and colour is weighted by alpha") {
    const Texture odd{3, 1, {255, 0, 255, 0, 255, 0, 255, 0, 0, 255, 0, 255}, {}};
    const auto tail = texture_mips(odd, {.alpha_coverage_cutoff = .5F}).back();
    REQUIRE(tail.width == 1);
    REQUIRE(tail.height == 1);
    CHECK(tail.rgba[0] == 0);
    CHECK(tail.rgba[1] == 255);
    CHECK(tail.rgba[2] == 0);
}

TEST_CASE("Colour filters in linear light by default, and linear data filters directly") {
    auto colour = black_and_white();
    CHECK(texture_mips(colour).back().rgba[0] == 188);
    colour.encoding = TextureEncoding::linear;
    CHECK(texture_mips(colour).back().rgba[0] == 128);
}

TEST_CASE("Invalid mip inputs are rejected") {
    const auto source = cutout();
    for (const auto cutoff : {0.F, -1.F, 1.01F, std::numeric_limits<float>::quiet_NaN()}) {
        CAPTURE(cutoff);
        CHECK_THROWS_WITH_AS(texture_mips(source, {.alpha_coverage_cutoff = cutoff}),
                             "Alpha coverage cutoff must be finite and in (0, 1]", std::invalid_argument);
    }
    CHECK_THROWS_WITH_AS(texture_mips(Texture{}), invalid_texture, std::invalid_argument);
    // 2^31 x 2^31 texels of 4 bytes wrap size_t to 0, the empty byte count, so only the overflow check rejects them.
    Texture oversized;
    oversized.width = oversized.height = std::uint32_t{1} << 31;
    CHECK_THROWS_WITH_AS(texture_mips(oversized), invalid_texture, std::invalid_argument);
}

TEST_CASE("A material texture plan shares mips between equivalent cutoffs and isolates conflicting ones") {
    const auto sources = textures();
    const auto plan = material_texture_plan(materials(), sources);
    REQUIRE(plan.images.size() == 4);
    REQUIRE(plan.bindings.size() == 5);
    CHECK(plan.bindings[1][0] == plan.bindings[2][0]); // Equivalent alpha factors.
    // Conflicting cutoffs and ordinary colour do not share corrected pixels.
    CHECK(plan.bindings[1][0] != plan.bindings[3][0]);
    CHECK(plan.bindings[1][0] != plan.bindings[4][0]);
    CHECK(plan.bindings[4][0] == plan.bindings[4][3]); // Ordinary colour and emission use one image.
    for (const auto &image : plan.images)
        CHECK(image.source != 1); // The unused data image.
    // Every texel passes a cutoff of 0 and none passes one above the alpha, so neither gets coverage correction.
    for (const auto cutoff : {0.F, 2.F}) {
        CAPTURE(cutoff);
        auto never_correct = materials()[0];
        never_correct.alpha_cutoff = cutoff;
        const auto special = material_texture_plan(std::array{never_correct}, sources);
        CHECK_FALSE(special.images.at(special.bindings.at(1)[0]).mips.alpha_coverage_cutoff);
    }
    // Without mips, the cutoffs need no separate images.
    auto no_mips = sources[0];
    no_mips.sampler.mipmapped = false;
    CHECK(material_texture_plan(materials(), std::array{no_mips}).images.size() == 2);
}

TEST_CASE("Mesh preparation filters each planned image as the plan asks") {
    const auto sources = textures();
    const auto plan = material_texture_plan(materials(), sources);
    const auto authored = materials();
    Asset upload_source;
    upload_source.nodes.resize(1);
    upload_source.materials.assign(authored.begin(), authored.end());
    upload_source.textures.assign(sources.begin(), sources.end());
    const auto compiled = Mesh::compile(upload_source);
    const MeshPreparation prepared(compiled);
    CHECK(prepared.asset() == compiled);
    CHECK(prepared.plan().bindings == plan.bindings);
    REQUIRE(prepared.images().size() == plan.images.size());
    CHECK(prepared.images().front().front().rgba == std::vector<std::uint8_t>{255, 255, 255, 255}); // The fallback.
    for (std::size_t i = 1; i < plan.images.size(); ++i) {
        CAPTURE(i);
        const auto expected = texture_mips(sources.at(plan.images[i].source), plan.images[i].mips);
        const auto &actual = prepared.images().at(i);
        REQUIRE(actual.size() == expected.size());
        for (std::size_t level = 0; level < expected.size(); ++level) {
            CAPTURE(level);
            CHECK(actual[level].width == expected[level].width);
            CHECK(actual[level].height == expected[level].height);
            CHECK(actual[level].rgba == expected[level].rgba);
        }
    }
    // An unmipmapped sampler uploads its source bytes as one level.
    auto no_mips = sources[0];
    no_mips.sampler.mipmapped = false;
    upload_source.textures.front() = no_mips;
    const MeshPreparation unfiltered(Mesh::compile(upload_source));
    REQUIRE(unfiltered.images().size() == 2);
    REQUIRE(unfiltered.images()[1].size() == 1);
    CHECK(unfiltered.images()[1][0].rgba == no_mips.rgba);
    CHECK_THROWS_WITH_AS(MeshPreparation(nullptr), "Cannot prepare a null render asset", std::invalid_argument);
}
