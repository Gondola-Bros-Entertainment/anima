#include <anima/assets/material_textures.hpp>
#include <anima/assets/mesh_preparation.hpp>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class F> void invalid(F action) {
    try {
        action();
    } catch (const std::invalid_argument &) {
        return;
    }
    throw std::runtime_error("Invalid mip input accepted");
}
std::size_t covered(const anima::MipLevel &level, float cutoff) {
    std::size_t count = 0;
    for (std::size_t i = 3; i < level.rgba.size(); i += 4)
        count += float(level.rgba[i]) / 255 >= cutoff;
    return count;
}
anima::Texture cutout() {
    anima::Texture texture{8, 8, std::vector<std::uint8_t>(8 * 8 * 4), {}};
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
} // namespace
int main() {
    using namespace anima;
    try {
        auto source = cutout();
        const auto original = source.rgba;
        const auto plain = texture_mips(source);
        const auto masked = texture_mips(source, {.alpha_coverage_cutoff = .75F});
        require(source.rgba == original && masked.front().rgba == original, "Source/base level changed");
        require(covered(masked.front(), .75F) == 22 && covered(plain[1], .75F) == 2 && covered(masked[1], .75F) == 5,
                "Cutout area was not restored to nearest attainable coverage");
        for (std::size_t i = 0; i < masked[1].rgba.size(); i += 4)
            if (masked[1].rgba[i + 3])
                require(masked[1].rgba[i] == 255 && masked[1].rgba[i + 1] == 255 && masked[1].rgba[i + 2] == 255,
                        "Transparent RGB contaminated a visible leaf");
        for (std::size_t level = 1; level < masked.size(); ++level) {
            const auto desired = 22.0 / 64 * (masked[level].width * masked[level].height);
            require(std::abs(double(covered(masked[level], .75F)) - desired) <=
                        std::abs(double(covered(plain[level], .75F)) - desired),
                    "Coverage error increased");
        }
        require(texture_mips(source, {.alpha_coverage_cutoff = .75F})[1].rgba == masked[1].rgba,
                "Mip generation is not deterministic");
        // One visible texel out of four: the 1x1 mip must be below the cutoff,
        // but retain as much alpha as possible for interpolation with its parent.
        Texture sparse{2, 2, {255, 255, 255, 127, 255, 255, 255, 127, 255, 255, 255, 127, 255, 255, 255, 255}, {}};
        const auto sparse_tail = texture_mips(sparse, {.alpha_coverage_cutoff = .5F}).back();
        require(sparse_tail.rgba[3] == 127, "Coverage reduction unnecessarily erased mip alpha");
        for (const auto alpha : {std::uint8_t{0}, std::uint8_t{255}}) {
            auto solid = source;
            for (std::size_t i = 3; i < solid.rgba.size(); i += 4)
                solid.rgba[i] = alpha;
            for (const auto &mip : texture_mips(solid, {.alpha_coverage_cutoff = 1.F}))
                require(covered(mip, 1.F) == (alpha ? mip.width * mip.height : 0U), "Uniform alpha changed coverage");
        }
        Texture odd{3, 1, {255, 0, 255, 0, 255, 0, 255, 0, 0, 255, 0, 255}, {}};
        const auto tail = texture_mips(odd, {.alpha_coverage_cutoff = .5F}).back();
        require(tail.width == 1 && tail.height == 1 && tail.rgba[0] == 0 && tail.rgba[1] == 255 && tail.rgba[2] == 0,
                "Odd edge or alpha-weighted colour filtering failed");
        Texture colour{2, 1, {0, 0, 0, 255, 255, 255, 255, 255}, {}};
        require(texture_mips(colour).back().rgba[0] == 188, "Default sRGB filtering changed");
        colour.encoding = TextureEncoding::linear;
        require(texture_mips(colour).back().rgba[0] == 128, "Linear data filtering changed");
        for (auto cutoff : {0.F, -1.F, 1.01F, std::numeric_limits<float>::quiet_NaN()})
            invalid([&] { (void)texture_mips(source, {.alpha_coverage_cutoff = cutoff}); });
        invalid([&] { (void)texture_mips(Texture{}); });
        auto oversized = source;
        oversized.width = oversized.height = std::numeric_limits<std::uint32_t>::max();
        invalid([&] { (void)texture_mips(oversized); });

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
        const std::array materials{mask, same, different, opaque};
        const std::array textures{source, colour}; // The unused data image should not upload.
        const auto plan = material_texture_plan(materials, textures);
        require(plan.images.size() == 4 && plan.bindings.size() == 5, "Unexpected image variants");
        require(plan.bindings[1][0] == plan.bindings[2][0], "Equivalent material alpha factors failed to share mips");
        require(plan.bindings[1][0] != plan.bindings[3][0] && plan.bindings[1][0] != plan.bindings[4][0],
                "Conflicting cutoffs or ordinary RGB use shared corrected pixels");
        require(plan.bindings[4][0] == plan.bindings[4][3], "Ordinary colour/emission uses duplicated one image");
        for (const auto &image : plan.images)
            require(image.source != 1, "Unused texture was scheduled");
        for (const auto cutoff : {0.F, 2.F}) {
            auto never_correct = mask;
            never_correct.alpha_cutoff = cutoff;
            const auto special = material_texture_plan(std::array{never_correct}, textures);
            require(!special.images[special.bindings[1][0]].mips.alpha_coverage_cutoff, "Degenerate cutoff was scaled");
        }
        auto no_mips = source;
        no_mips.sampler.mipmapped = false;
        const auto unfiltered = material_texture_plan(materials, std::array{no_mips});
        require(unfiltered.images.size() == 2, "Unmipmapped image duplicated per cutoff");
        Asset upload_source;
        upload_source.nodes.resize(1);
        upload_source.materials.assign(materials.begin(), materials.end());
        upload_source.textures.assign(textures.begin(), textures.end());
        const auto compiled = Mesh::compile(upload_source);
        const MeshPreparation prepared(compiled);
        require(prepared.asset() == compiled && prepared.plan().bindings == plan.bindings &&
                    prepared.images().size() == plan.images.size(),
                "Prepared upload lost asset identity or material bindings");
        require(prepared.images().front().front().rgba == std::vector<std::uint8_t>({255, 255, 255, 255}),
                "Prepared fallback changed");
        for (std::size_t i = 1; i < plan.images.size(); ++i) {
            const auto expected = texture_mips(textures.at(plan.images[i].source), plan.images[i].mips);
            const auto &actual = prepared.images().at(i);
            require(actual.size() == expected.size(), "Prepared mip chain is incomplete");
            for (std::size_t level = 0; level < expected.size(); ++level)
                require(actual[level].width == expected[level].width &&
                            actual[level].height == expected[level].height &&
                            actual[level].rgba == expected[level].rgba,
                        "Background preparation changed filtered texture bytes");
        }
        upload_source.textures.front() = no_mips;
        const MeshPreparation unfiltered_prepared(Mesh::compile(upload_source));
        require(unfiltered_prepared.images().size() == 2 && unfiltered_prepared.images()[1].size() == 1 &&
                    unfiltered_prepared.images()[1][0].rgba == no_mips.rgba,
                "Preparation ignored unmipmapped sampler");
        invalid([&] { (void)MeshPreparation(nullptr); });
        std::cout
            << "PASS: mask coverage, transparent colour, odd mips, pure inputs, cutoff isolation and image reuse\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
