#include <anima/assets/material_textures.hpp>
#include <anima/assets/mesh_preparation.hpp>
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
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
// A texture over new pixels, with the default sampler and sRGB encoding.
Texture texture_of(Image image) { return {std::make_shared<Image>(std::move(image)), {}}; }
// An 8x8 cutout of opaque white leaves on transparent magenta; its 2x2 blocks hold 0 to 4 leaf texels, 22 in all.
Texture cutout() {
    Image image{8, 8, std::vector<std::uint8_t>(8 * 8 * 4)};
    constexpr unsigned opaque_per_block[]{0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 2, 2, 3, 4, 4};
    for (unsigned block = 0; block < 16; ++block)
        for (unsigned texel = 0; texel < 4; ++texel) {
            const auto x = block % 4 * 2 + texel % 2, y = block / 4 * 2 + texel / 2;
            const auto offset = (y * 8 + x) * 4;
            const bool visible = texel < opaque_per_block[block];
            image.rgba[offset] = image.rgba[offset + 2] = 255;
            image.rgba[offset + 1] = visible ? 255 : 0;
            image.rgba[offset + 3] = visible ? 255 : 0;
        }
    return texture_of(std::move(image));
}
Texture black_and_white() { return texture_of({2, 1, {0, 0, 0, 255, 255, 255, 255, 255}}); }
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
    const auto original = source.image->rgba;
    const auto plain = texture_mips(source);
    const auto masked = texture_mips(source, {.alpha_coverage_cutoff = cutout_cutoff});
    CHECK(source.image->rgba == original);
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
    const auto sparse =
        texture_of({2, 2, {255, 255, 255, 127, 255, 255, 255, 127, 255, 255, 255, 127, 255, 255, 255, 255}});
    CHECK(texture_mips(sparse, {.alpha_coverage_cutoff = .5F}).back().rgba[3] == 127);
}

TEST_CASE("Uniform alpha keeps its coverage at every level") {
    for (const auto alpha : {std::uint8_t{0}, std::uint8_t{255}}) {
        CAPTURE(alpha);
        auto pixels = *cutout().image;
        for (std::size_t i = 3; i < pixels.rgba.size(); i += 4)
            pixels.rgba[i] = alpha;
        for (const auto &mip : texture_mips(texture_of(std::move(pixels)), {.alpha_coverage_cutoff = 1.F})) {
            CAPTURE(mip.width);
            CHECK(covered(mip, 1.F) == (alpha ? mip.width * mip.height : 0U));
        }
    }
}

TEST_CASE("An odd edge texel is filtered, and colour is weighted by alpha") {
    const auto odd = texture_of({3, 1, {255, 0, 255, 0, 255, 0, 255, 0, 0, 255, 0, 255}});
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

TEST_CASE("Alpha-weighted color makes each level's premultiplied color the mean of its source texels'") {
    // Varied colors under alpha from 0 to 255 on a 7x5 image, whose odd edges merge three texels into one.
    constexpr std::uint32_t width = 7, height = 5;
    Image image{width, height, std::vector<std::uint8_t>(width * height * 4)};
    for (std::uint32_t i = 0; i < width * height; ++i) {
        image.rgba[i * 4] = std::uint8_t(i * 37 % 256);
        image.rgba[i * 4 + 1] = std::uint8_t(255 - i * 53 % 256);
        image.rgba[i * 4 + 2] = std::uint8_t(i * 91 % 256);
        image.rgba[i * 4 + 3] = std::uint8_t(i % 5 == 0 ? 0 : i * 71 % 256);
    }
    auto color = texture_of(std::move(image));
    const auto decode = [](double byte) {
        const auto s = byte / 255;
        return s <= .04045 ? s / 12.92 : std::pow((s + .055) / 1.055, 2.4);
    };
    for (const auto encoding : {TextureEncoding::srgb, TextureEncoding::linear}) {
        CAPTURE(static_cast<int>(encoding));
        color.encoding = encoding;
        const auto value = [&](double byte) { return encoding == TextureEncoding::srgb ? decode(byte) : byte / 255; };
        const auto weighted = texture_mips(color, {.alpha_weighted_color = true});
        const auto plain = texture_mips(color);
        REQUIRE(weighted.size() == plain.size());
        CHECK(weighted.front().rgba == color.image->rgba); // The base level is kept.
        bool plain_differs = false;
        for (std::size_t level = 1; level < weighted.size(); ++level) {
            CAPTURE(level);
            const auto &previous = weighted[level - 1], &next = weighted[level];
            REQUIRE(next.width == plain[level].width);
            REQUIRE(next.height == plain[level].height);
            for (std::uint32_t y = 0; y < next.height; ++y)
                for (std::uint32_t x = 0; x < next.width; ++x) {
                    CAPTURE(x);
                    CAPTURE(y);
                    // The previous level's texels that this one averages, as texture_mips documents.
                    const auto x0 = x * previous.width / next.width, x1 = (x + 1) * previous.width / next.width;
                    const auto y0 = y * previous.height / next.height, y1 = (y + 1) * previous.height / next.height;
                    std::array<double, 3> premultiplied{};
                    for (auto sy = y0; sy < y1; ++sy)
                        for (auto sx = x0; sx < x1; ++sx) {
                            const auto *texel = &previous.rgba[(std::size_t(sy) * previous.width + sx) * 4];
                            for (std::size_t c = 0; c < 3; ++c)
                                premultiplied[c] += texel[3] / 255. * value(texel[c]);
                        }
                    const auto count = double((x1 - x0) * (y1 - y0));
                    const auto offset = (std::size_t(y) * next.width + x) * 4;
                    const auto alpha = next.rgba[offset + 3] / 255.;
                    CHECK(next.rgba[offset + 3] == plain[level].rgba[offset + 3]); // Alpha is averaged as stored.
                    for (std::size_t c = 0; c < 3; ++c) {
                        CAPTURE(c);
                        const auto expected = premultiplied[c] / count;
                        const double byte = next.rgba[offset + c];
                        // Rounding to 8 bits moves the color by at most half a step either way and the alpha by
                        // half of 1/255.
                        const auto half_step = std::max(value(std::min(byte + .5, 255.)) - value(byte),
                                                        value(byte) - value(std::max(byte - .5, 0.)));
                        const auto tolerance = (alpha + .5 / 255) * half_step + .5 / 255 * value(byte) + 1e-9;
                        CHECK(std::abs(alpha * value(byte) - expected) <= tolerance);
                        const auto plain_error = std::abs(alpha * value(plain[level].rgba[offset + c]) - expected);
                        plain_differs = plain_differs || plain_error > tolerance;
                    }
                }
        }
        CHECK(plain_differs); // Unweighted color does not satisfy the check above.
    }
}

TEST_CASE("A texel whose sources all have zero alpha takes their unweighted color") {
    // A 4x2 image whose left half holds transparent red and blue, as an image spreads color around the edges of what
    // it covers, and whose right half holds transparent black beside one opaque white texel.
    const auto source = texture_of({4, 2, {255, 0, 0, 0, 0, 0, 255, 0, 0, 0, 0, 0, 255, 255, 255, 255,
                                           255, 0, 0, 0, 0, 0, 255, 0, 0, 0, 0, 0, 0,   0,   0,   0}});
    for (const auto &options :
         {TextureMipOptions{.alpha_coverage_cutoff = .5F}, TextureMipOptions{.alpha_weighted_color = true}}) {
        CAPTURE(options.alpha_weighted_color);
        const auto level = texture_mips(source, options).at(1);
        REQUIRE(level.width == 2);
        // Red and blue average in linear light to half of each, encoded as sRGB 188.
        CHECK(level.rgba[0] == 188);
        CHECK(level.rgba[1] == 0);
        CHECK(level.rgba[2] == 188);
        CHECK(level.rgba[3] == 0);
        // Where any source has alpha, only those sources' color counts: the transparent black adds none.
        CHECK(level.rgba[4] == 255);
        CHECK(level.rgba[5] == 255);
        CHECK(level.rgba[6] == 255);
    }
}

TEST_CASE("Invalid mip inputs are rejected") {
    const auto source = cutout();
    for (const auto cutoff : {0.F, -1.F, 1.01F, std::numeric_limits<float>::quiet_NaN()}) {
        CAPTURE(cutoff);
        CHECK_THROWS_WITH_AS(texture_mips(source, {.alpha_coverage_cutoff = cutoff}),
                             "Alpha coverage cutoff must be finite and in (0, 1]", std::invalid_argument);
    }
    CHECK_THROWS_WITH_AS(texture_mips(Texture{}), invalid_texture, std::invalid_argument); // No image.
    CHECK_THROWS_WITH_AS(texture_mips(texture_of({})), invalid_texture, std::invalid_argument);
    // 2^31 x 2^31 texels of 4 bytes wrap size_t to 0, the empty byte count, so only the overflow check rejects them.
    constexpr auto wrapping_edge = std::uint32_t{1} << 31;
    CHECK_THROWS_WITH_AS(texture_mips(texture_of({wrapping_edge, wrapping_edge, {}})), invalid_texture,
                         std::invalid_argument);
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

TEST_CASE("Blended base-color maps get alpha-weighted mips, shared whatever the alpha factor") {
    const auto sources = textures();
    auto blended = materials()[0];
    blended.alpha_mode = AlphaMode::blend;
    auto fainter = blended;
    fainter.alpha = .25F;
    const auto authored = materials();
    const std::array all{authored[0], authored[3], blended, fainter};
    const auto plan = material_texture_plan(all, sources);
    REQUIRE(plan.bindings.size() == 5);
    const auto &mask = plan.images.at(plan.bindings[1][0]).mips, &plain = plan.images.at(plan.bindings[2][0]).mips;
    const auto &weighted = plan.images.at(plan.bindings[3][0]).mips;
    CHECK(weighted.alpha_weighted_color);
    CHECK_FALSE(weighted.alpha_coverage_cutoff); // Blending needs no coverage correction.
    CHECK_FALSE(mask.alpha_weighted_color);
    CHECK(mask.alpha_coverage_cutoff);
    CHECK(plain == TextureMipOptions{});
    CHECK(plan.bindings[3][0] == plan.bindings[4][0]); // Weighting does not depend on the alpha factor.
    CHECK(plan.bindings[3][0] != plan.bindings[1][0]);
    CHECK(plan.bindings[3][0] != plan.bindings[2][0]);
    CHECK(plan.images.size() == 4); // The fallback, then the masked, plain and weighted cutouts.
    // Without mips, the blended map shares the plain image.
    auto no_mips = sources[0];
    no_mips.sampler.mipmapped = false;
    const auto unmipmapped = material_texture_plan(std::array{authored[3], blended}, std::array{no_mips});
    CHECK(unmipmapped.bindings.at(1)[0] == unmipmapped.bindings.at(2)[0]);
    // Mesh preparation filters the weighted image as the plan asks.
    Asset upload_source;
    upload_source.nodes.resize(1);
    upload_source.materials.assign(all.begin(), all.end());
    upload_source.textures.assign(sources.begin(), sources.end());
    const MeshPreparation prepared(Mesh::compile(upload_source));
    CHECK(prepared.plan().bindings == plan.bindings);
    const auto expected = texture_mips(sources[0], {.alpha_weighted_color = true});
    const auto &actual = prepared.images().at(plan.bindings[3][0]);
    REQUIRE(actual.size() == expected.size());
    for (std::size_t level = 0; level < expected.size(); ++level) {
        CAPTURE(level);
        CHECK(actual[level].rgba == expected[level].rgba);
    }
    CHECK(actual.at(1).rgba != texture_mips(sources[0]).at(1).rgba); // The cutout's transparent magenta is gone.
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
    CHECK(unfiltered.images()[1][0].rgba == no_mips.image->rgba);
    CHECK_THROWS_WITH_AS(MeshPreparation(nullptr), "Cannot prepare a null render asset", std::invalid_argument);
}
