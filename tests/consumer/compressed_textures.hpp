#pragma once
// Block-compressed textures through the public API: BC7 images from KTX2 files, drawn as a mesh's base color and
// through a custom material, against their uncompressed sources, on the device's BC7 path and on the CPU decoding
// that devices without BC7 get, with the device and CPU memory of a representative texture in each format. The
// fixtures, their sources and the commands that made them are in tests/textures.
#include "custom_materials.hpp"
#include "gltf_fixture.hpp"
#include "gpu_checks.hpp"
#include "rejection.hpp"
#include "texture_memory.hpp"
#include <algorithm>
#include <anima/assets/ktx2.hpp>
#include <anima/assets/mesh_preparation.hpp>
#include <anima/desktop/vulkan_renderer.hpp>
#include <anima/scene.hpp>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace compressed_texture_test {
using rejection::rejects;
using texture_memory_test::Harness;
using texture_memory_test::require;

inline std::vector<std::uint8_t> read_file(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    require(bool(file), "Cannot open " + path.string());
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(bool(file), "Cannot read " + path.string());
    return bytes;
}
// A quad facing +Z at z = -3, 2 units square, whose unlit material shows the PNG @p png as its base color, repeated
// @p repeats times across it: through nearest magnification close up, so that each pixel shows one texel, and
// mipmapped minification when repeated. The importer decodes the PNG, so the asset holds the uncompressed source.
inline std::shared_ptr<const anima::Asset> source_quad(const std::vector<std::uint8_t> &png, float repeats) {
    constexpr int nearest = 9728, linear_mipmap_linear = 9987, repeat = 10497, clamp_to_edge = 33071;
    const auto wrap = repeats > 1 ? repeat : clamp_to_edge;
    std::vector<float> vertices;
    for (const auto &[x, y] :
         std::array<std::array<float, 2>, 6>{{{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}}})
        vertices.insert(vertices.end(), {x, y, -3, 0, 0, 1, (x + 1) / 2 * repeats, (1 - y) / 2 * repeats});
    gltf_fixture::Builder builder;
    const auto first = builder.interleaved(vertices, 8, {{"VEC3", 0}, {"VEC3", 3}, {"VEC2", 6}});
    const auto image = builder.image(png, "image/png");
    const auto glb = builder.glb(
        R"("scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],"textures":[{"source":)" + std::to_string(image) +
        R"(,"sampler":0}],"samplers":[{"magFilter":)" + std::to_string(nearest) + R"(,"minFilter":)" +
        std::to_string(linear_mipmap_linear) + R"(,"wrapS":)" + std::to_string(wrap) + R"(,"wrapT":)" +
        std::to_string(wrap) +
        R"(}],"materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}},)"
        R"("extensions":{"KHR_materials_unlit":{}}}],"meshes":[{"primitives":[{"attributes":{"POSITION":)" +
        std::to_string(first) + R"(,"NORMAL":)" + std::to_string(first + 1) + R"(,"TEXCOORD_0":)" +
        std::to_string(first + 2) +
        R"(},"material":0}]}],"extensionsUsed":["KHR_materials_unlit"],"extensionsRequired":["KHR_materials_unlit"])");
    return anima::load_asset(std::span<const std::byte>(glb));
}
// @p source with its texture's image replaced by @p image, as an application substitutes a BC7 file for an
// imported texture.
inline std::shared_ptr<const anima::Asset> substituted(const anima::Asset &source,
                                                       std::shared_ptr<const anima::Image> image) {
    auto copy = std::make_shared<anima::Asset>(source);
    copy->textures.at(0).image = std::move(image);
    return copy;
}
inline double to_display(double byte, bool srgb) {
    if (srgb)
        return byte; // The display encodes sRGB again, so an sRGB texel shows as it is stored.
    const auto value = byte / 255;
    return 255 * (value <= .0031308 ? 12.92 * value : 1.055 * std::pow(value, 1 / 2.4) - .055);
}
// The largest difference between the displayed values of @p image's base level, decoded, and @p source's texels:
// the encoding error that a frame can show where each pixel shows one texel. RGB only; the quads are opaque.
inline double encoding_error(const anima::Image &image, const anima::Image &source, bool srgb) {
    const auto decoded = anima::decode_image(image).front();
    require(decoded.rgba.size() == source.rgba.size(), "A BC7 fixture does not match its source's size");
    double largest = 0;
    for (std::size_t i = 0; i < decoded.rgba.size(); ++i)
        if (i % 4 != 3)
            largest = std::max(largest, std::abs(to_display(decoded.rgba[i], srgb) - to_display(source.rgba[i], srgb)));
    return largest;
}
// The largest channel difference between two captures of one size.
inline int largest_difference(const gpu_check::Image &a, const gpu_check::Image &b) {
    require(gpu_check::same_size(a, b), "Compared captures differ in size");
    int largest = 0;
    for (std::size_t i = 0; i < a.rgb.size(); ++i)
        largest = std::max(largest, std::abs(int(a.rgb[i]) - int(b.rgb[i])));
    return largest;
}
// Requires captures @p a and @p b to differ by at most @p tolerance levels in every channel.
inline void require_within(const gpu_check::Captures &images, const std::string &a, const std::string &b, int tolerance,
                           const std::string &what) {
    const auto largest = largest_difference(images[a], images[b]);
    std::cout << "COMPRESSED TEXTURES " << what << ": " << a << " and " << b << " differ by at most " << largest
              << " levels (tolerance " << tolerance << ")\n";
    images.require(largest <= tolerance,
                   what + ": " + a + " and " + b + " differ by " + std::to_string(largest) + " levels, more than " +
                       std::to_string(tolerance),
                   {a, b});
}
// A 2048-texel square image in either format, for measuring memory: RGBA8 texels, or BC7 blocks of mode 6 over the
// full mip chain.
constexpr std::uint32_t measured_edge = 2048;
inline std::shared_ptr<const anima::Image> measured(bool bc7) {
    if (!bc7)
        return std::make_shared<const anima::Image>(
            anima::Image{measured_edge, measured_edge,
                         std::vector<std::uint8_t>(std::size_t{measured_edge} * measured_edge * 4, 128)});
    anima::Image image{measured_edge, measured_edge, {}, anima::ImageFormat::bc7, 12, {}};
    for (std::uint32_t level = 0; level < image.levels; ++level) {
        const auto edge = std::max(measured_edge >> level, 1U);
        for (std::size_t block = 0; block < std::size_t{(edge + 3) / 4} * ((edge + 3) / 4); ++block) {
            image.blocks.push_back(0x40); // Mode 6, whose zero endpoints and indices decode to transparent black.
            image.blocks.insert(image.blocks.end(), 15, 0);
        }
    }
    return std::make_shared<const anima::Image>(std::move(image));
}

inline int run(int argc, char **argv) {
    require(argc == 4, "Usage: consumer --compressed-textures OUTPUT TEXTURES");
    const std::filesystem::path output = argv[2], textures = argv[3];
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    // The opaque fixture: where alpha is zero an encoder may give texels any color, which an opaque quad would show.
    const auto png = read_file(textures / "opaque.png");
    // Close up each texel covers about 6 pixels; far, 16 repeats put about 2.7 texels in each pixel.
    constexpr float far_repeats = 16;
    const auto close = source_quad(png, 1), far = source_quad(png, far_repeats);
    const auto &source = *close->textures.at(0).image;
    const auto srgb = anima::load_ktx2(textures / "opaque-bc7.ktx2", anima::TextureEncoding::srgb);
    const auto linear = anima::load_ktx2(textures / "opaque-linear-bc7.ktx2", anima::TextureEncoding::linear);
    rejects<std::runtime_error>(
        [&] { (void)anima::load_ktx2(textures / "opaque-bc7.ktx2", anima::TextureEncoding::linear); },
        "KTX2 format VK_FORMAT_BC7_SRGB_BLOCK does not match a linear texture");
    rejects<std::runtime_error>([&] { (void)anima::load_ktx2(textures / "opaque.png", anima::TextureEncoding::srgb); },
                                "Not a KTX 2.0 file");
    // Pixels show single texels close up, where the renders may differ by the encoding's own error after the display
    // transform, plus a level for rounding in each.
    const auto srgb_error = encoding_error(*srgb, source, true), linear_error = encoding_error(*linear, source, false);
    const int srgb_tolerance = int(std::ceil(srgb_error)) + 2, linear_tolerance = int(std::ceil(linear_error)) + 2;
    std::cout << "COMPRESSED TEXTURES largest displayed BC7 encoding error of the base level: sRGB " << srgb_error
              << ", linear " << linear_error << " levels\n";
    const auto close_rgba = anima::Mesh::compile(*close), far_rgba = anima::Mesh::compile(*far);
    const auto close_bc7 = anima::Mesh::compile(*substituted(*close, srgb)),
               far_bc7 = anima::Mesh::compile(*substituted(*far, srgb));
    // A preparation of a mesh that holds its BC7 texels only until upload, from a copy that nothing else keeps: the
    // first renderer's upload lets the mesh's texels go, and the preparation still uploads to the second.
    const auto prepared_bc7 = anima::MeshPreparation(anima::Mesh::compile(
        *substituted(*close, std::make_shared<const anima::Image>(*srgb)), anima::TexelRetention::until_upload));
    // The linear pair samples the texels as data through the consumer's effect shader, on the close quad.
    const auto effect = [&](std::shared_ptr<const anima::Image> image) {
        auto definition = custom_material_test::effect_definition("pattern", anima::CustomBlend::opaque, {{1, 1, 1}});
        auto &texture = definition.textures.at(0);
        texture.image = std::move(image);
        texture.encoding = anima::TextureEncoding::linear;
        texture.sampler = close->textures.at(0).sampler;
        auto scene = texture_memory_test::scene_of(close_rgba);
        scene->set_custom_material(scene->instances().front(), 0,
                                   std::make_shared<const anima::CustomMaterial>(std::move(definition)));
        return scene;
    };
    auto linear_source = std::make_shared<anima::Image>(source);
    const auto effect_rgba = effect(linear_source), effect_bc7 = effect(linear);
    const auto memory = [&](Harness &harness, bool bc7) {
        const auto before = harness.renderer().resource_stats().resident_texture_bytes;
        auto asset = *close;
        asset.textures.at(0).image = measured(bc7);
        const auto mesh = anima::Mesh::compile(asset);
        harness.renderer().prepare_meshes(std::span(&mesh, 1));
        return harness.renderer().resource_stats().resident_texture_bytes - before;
    };
    const auto draw = [](Harness &harness, const std::string &name, std::shared_ptr<const anima::Scene> scene) {
        harness.select({std::move(scene)});
        harness.render(name);
    };
    const auto prepare = [&](Harness &harness, const std::string &name) {
        harness.renderer().prepare_mesh(prepared_bc7);
        draw(harness, name, texture_memory_test::scene_of(prepared_bc7.asset()));
    };

    gpu_check::Captures captures(output);
    std::uint64_t rgba_device{}, bc7_device{}, decoded_device{};
    bool sampled{};
    {
        Harness harness(output);
        sampled = harness.renderer().samples_bc7();
        draw(harness, "close_rgba8", texture_memory_test::scene_of(close_rgba));
        draw(harness, "close_bc7", texture_memory_test::scene_of(close_bc7));
        draw(harness, "far_bc7", texture_memory_test::scene_of(far_bc7));
        draw(harness, "effect_rgba8", effect_rgba);
        draw(harness, "effect_bc7", effect_bc7);
        prepare(harness, "prepared_bc7");
        rgba_device = memory(harness, false);
        bc7_device = memory(harness, true);
        for (const auto *name : {"close_rgba8", "close_bc7", "far_bc7", "effect_rgba8", "effect_bc7", "prepared_bc7"})
            captures.add(name, harness.images[name]);
        harness.finish();
    }
    {
        // The CPU decoding that a device without BC7 gets, on this device.
        Harness harness(output, true);
        require(!harness.renderer().samples_bc7(), "RendererOptions::decode_bc7 must decode BC7 images");
        draw(harness, "close_decoded", texture_memory_test::scene_of(close_bc7));
        draw(harness, "far_decoded", texture_memory_test::scene_of(far_bc7));
        draw(harness, "effect_decoded", effect_bc7);
        prepare(harness, "prepared_decoded");
        decoded_device = memory(harness, true);
        for (const auto *name : {"close_decoded", "far_decoded", "effect_decoded", "prepared_decoded"})
            captures.add(name, harness.images[name]);
        harness.finish();
    }
    captures.require_foreground("close_bc7", "The BC7 quad is not visible");
    captures.require_foreground("far_bc7", "The repeated BC7 quad is not visible");
    captures.require_foreground("effect_bc7", "The custom material's BC7 texture is not visible");
    require_within(captures, "close_rgba8", "close_bc7", srgb_tolerance, "sRGB BC7 base color against its source");
    require_within(captures, "effect_rgba8", "effect_bc7", linear_tolerance,
                   "linear BC7 custom material texture against its source");
    // Both paths sample the same stored levels, so they match but for filtering precision.
    constexpr int path_tolerance = 1;
    require_within(captures, "close_bc7", "close_decoded", path_tolerance, "BC7 sampled against BC7 decoded, close");
    require_within(captures, "far_bc7", "far_decoded", path_tolerance, "BC7 sampled against BC7 decoded, minified");
    require_within(captures, "effect_bc7", "effect_decoded", path_tolerance,
                   "BC7 sampled against BC7 decoded, custom material");
    captures.require_same("close_bc7", "prepared_bc7", "A prepared BC7 upload drew differently");
    captures.require_same("close_decoded", "prepared_decoded",
                          "A preparation drew differently after an earlier upload let its mesh's texels go");
    const auto rgba_bytes = measured(false)->rgba.size(), bc7_bytes = measured(true)->blocks.size();
    std::cout << "COMPRESSED TEXTURES " << measured_edge << "x" << measured_edge << " texture with mips, device bytes: "
              << "RGBA8 " << rgba_device << ", BC7 " << bc7_device << (sampled ? " sampled" : " decoded")
              << ", BC7 decoded " << decoded_device << "; CPU texel bytes: RGBA8 " << rgba_bytes
              << " (base level; mips are built at upload), BC7 " << bc7_bytes << " (every level)\n";
    // RGBA8 takes 4 bytes a texel and BC7 1, so BC7 takes a quarter of the device memory where it is sampled.
    require(!sampled || bc7_device * 4 <= rgba_device + rgba_device / 64,
            "Sampled BC7 images must take about a quarter of the device memory of RGBA8 ones");
    require(decoded_device == rgba_device, "Decoded BC7 images must take the device memory of RGBA8 ones");
    std::cout << "PASS compressed textures: BC7 images from KTX2 drew within their encoding error of their sources, "
              << (sampled ? "sampled as BC7" : "decoded on the CPU, since this device does not sample BC7")
              << ", and matched their CPU decoding\n";
    return 0;
}
} // namespace compressed_texture_test
