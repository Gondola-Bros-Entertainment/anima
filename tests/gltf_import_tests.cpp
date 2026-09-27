#include <anima/assets/asset.hpp>
#include <anima/mesh.hpp>
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Files that glTF 2.0 allows, each built in memory: ones the importer must accept, and ones beyond its documented
// limits.
namespace {
using namespace anima;

constexpr std::uint32_t glb_magic = 0x46546c67;
constexpr std::uint32_t glb_version = 2;
constexpr std::uint32_t json_chunk = 0x4e4f534a;
constexpr std::uint32_t bin_chunk = 0x004e4942;
constexpr std::size_t glb_header_bytes = 12;
constexpr std::size_t chunk_header_bytes = 8;
constexpr std::size_t chunk_alignment = 4;
// A 1x1 RGBA PNG holding the texel (255, 128, 64, 255).
constexpr std::array<std::uint8_t, 70> fallback_png{
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
    0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xdf, 0xe0, 0xf0, 0x1f, 0x00, 0x07, 0x00, 0x02, 0xbf,
    0x2b, 0xd7, 0xc7, 0xe2, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
constexpr std::array<std::uint8_t, 4> fallback_texel{255, 128, 64, 255};
// Import limits that load_asset documents.
constexpr std::size_t import_texture_limit = 4096, import_image_limit = 4096, import_key_limit = 8'000'000;
// The largest square image within the pixel limit, and how many of them fill the 1 GiB decoded limit.
constexpr std::uint32_t largest_image_edge = 4096;
constexpr std::size_t largest_images_in_byte_limit = 16;
// Keys in the longest clip that motion_clips() builds.
constexpr std::size_t keys_per_long_clip = 10'000;

void append(std::vector<std::byte> &bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xff));
}
void append(std::vector<std::byte> &bytes, float value) { append(bytes, std::bit_cast<std::uint32_t>(value)); }
void append_big_endian(std::vector<std::byte> &bytes, std::uint32_t value) {
    for (unsigned shift = 32; shift > 0; shift -= 8)
        bytes.push_back(static_cast<std::byte>((value >> (shift - 8)) & 0xff));
}

// Binary glTF container: a header, the JSON chunk padded with spaces and the BIN chunk padded with zeros.
std::vector<std::byte> glb(std::string json, std::vector<std::byte> bin) {
    while (json.size() % chunk_alignment)
        json += ' ';
    while (bin.size() % chunk_alignment)
        bin.push_back(std::byte{0});
    std::vector<std::byte> result;
    append(result, glb_magic);
    append(result, glb_version);
    append(result, static_cast<std::uint32_t>(glb_header_bytes + 2 * chunk_header_bytes + json.size() + bin.size()));
    append(result, static_cast<std::uint32_t>(json.size()));
    append(result, json_chunk);
    for (const char c : json)
        result.push_back(static_cast<std::byte>(c));
    append(result, static_cast<std::uint32_t>(bin.size()));
    append(result, bin_chunk);
    result.insert(result.end(), bin.begin(), bin.end());
    return result;
}

// One triangle: three VEC3 positions, then one VEC4 per vertex (a tangent or a placeholder).
constexpr std::size_t triangle_vertices = 3;
constexpr std::size_t position_bytes = triangle_vertices * 3 * sizeof(float);
constexpr std::size_t vec4_bytes = triangle_vertices * 4 * sizeof(float);
std::vector<std::byte> triangle(std::array<float, 4> vec4) {
    std::vector<std::byte> bin;
    for (const auto &p : {std::array{0.F, 0.F, 0.F}, std::array{1.F, 0.F, 0.F}, std::array{0.F, 1.F, 0.F}})
        for (const float f : p)
            append(bin, f);
    for (std::size_t vertex = 0; vertex < triangle_vertices; ++vertex)
        for (const float f : vec4)
            append(bin, f);
    return bin;
}
// Buffer views 0 and 1 and accessors 0 (POSITION) and 1 (a VEC4 per vertex) over triangle(); @p more_views
// appends further buffer views and @p more_accessors further accessors.
std::string triangle_json(std::size_t buffer_bytes, const std::string &more_views = {},
                          const std::string &more_accessors = {}) {
    return R"("buffers":[{"byteLength":)" + std::to_string(buffer_bytes) + R"(}],
      "bufferViews":[{"buffer":0,"byteLength":)" +
           std::to_string(position_bytes) + R"(},
                     {"buffer":0,"byteOffset":)" +
           std::to_string(position_bytes) + R"(,"byteLength":)" + std::to_string(vec4_bytes) + "}" + more_views + R"(],
      "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},
                   {"bufferView":1,"componentType":5126,"count":3,"type":"VEC4"})" +
           more_accessors + "]";
}
std::string view(std::size_t offset, std::size_t bytes) {
    return R"(,{"buffer":0,"byteOffset":)" + std::to_string(offset) + R"(,"byteLength":)" + std::to_string(bytes) + "}";
}

// A clip on node "root" with one key per sampler: time 0 and translation (0, 2, 0). @p channels is the JSON
// channel list.
std::vector<std::byte> single_key_motion(const std::string &channels) {
    std::vector<std::byte> bin;
    append(bin, 0.F);
    for (const float f : {0.F, 2.F, 0.F})
        append(bin, f);
    const std::string json = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
      "nodes":[{"name":"root"}],"buffers":[{"byteLength":16}],
      "bufferViews":[{"buffer":0,"byteLength":4},{"buffer":0,"byteOffset":4,"byteLength":12}],
      "accessors":[{"bufferView":0,"componentType":5126,"count":1,"type":"SCALAR","min":[0],"max":[0]},
                   {"bufferView":1,"componentType":5126,"count":1,"type":"VEC3"}],
      "animations":[{"name":"pose","samplers":[{"input":0,"output":1}],"channels":)" +
                             channels + "}]}";
    return glb(json, std::move(bin));
}

// A triangle plus two images: the PNG fallback and four placeholder bytes standing in for the extension's own
// source, which the importer must not decode. @p texture is the JSON texture object.
std::vector<std::byte> textured(const std::string &extension, const std::string &texture) {
    constexpr std::size_t extension_source_bytes = 4;
    auto bin = triangle({});
    const auto png_offset = bin.size();
    for (const auto b : fallback_png)
        bin.push_back(static_cast<std::byte>(b));
    const auto extension_offset = bin.size();
    bin.resize(bin.size() + extension_source_bytes);
    const std::string mime = extension == "EXT_texture_webp" ? "image/webp" : "image/ktx2";
    const std::string json = R"({"asset":{"version":"2.0"},"extensionsUsed":[")" + extension + R"("],
      "scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],
      "meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],)" +
                             triangle_json(bin.size(), view(png_offset, fallback_png.size()) +
                                                           view(extension_offset, extension_source_bytes)) +
                             R"(,"images":[{"bufferView":2,"mimeType":"image/png"},{"bufferView":3,"mimeType":")" +
                             mime + R"("}],"textures":[)" + texture + "]}";
    return glb(json, std::move(bin));
}

// An 8-bit RGBA PNG of @p width x @p height texels without image data: the signature, an IHDR chunk and an IEND
// chunk, with zero CRCs, which stb_image skips. Its header reads as a valid image, but decoding it fails before
// allocating any pixels.
std::vector<std::byte> png_header(std::uint32_t width, std::uint32_t height) {
    constexpr std::uint32_t ihdr_bytes = 13;
    constexpr std::uint8_t bit_depth = 8, rgba_color_type = 6;
    std::vector<std::byte> bytes;
    for (const auto b : std::to_array<std::uint8_t>({0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a}))
        bytes.push_back(std::byte{b});
    append_big_endian(bytes, ihdr_bytes);
    for (const char c : std::string_view{"IHDR"})
        bytes.push_back(static_cast<std::byte>(c));
    append_big_endian(bytes, width);
    append_big_endian(bytes, height);
    // Bit depth, color type, then default compression, filtering and interlacing.
    for (const auto b : std::to_array<std::uint8_t>({bit_depth, rgba_color_type, 0, 0, 0}))
        bytes.push_back(std::byte{b});
    append_big_endian(bytes, 0); // CRC
    append_big_endian(bytes, 0); // IEND length
    for (const char c : std::string_view{"IEND"})
        bytes.push_back(static_cast<std::byte>(c));
    append_big_endian(bytes, 0); // CRC
    return bytes;
}

// A triangle with one embedded image per entry of @p encoded, then one glTF image per entry of @p image_sources,
// reading the encoded image that the entry names, and @p textures textures: texture i samples image i modulo
// the image count.
std::vector<std::byte> textured_triangle(const std::vector<std::vector<std::byte>> &encoded,
                                         const std::vector<std::size_t> &image_sources, std::size_t textures) {
    constexpr std::size_t first_image_view = 2; // After triangle_json's two views.
    auto bin = triangle({});
    std::string views;
    for (const auto &image : encoded) {
        views += view(bin.size(), image.size());
        bin.insert(bin.end(), image.begin(), image.end());
    }
    std::string images;
    for (const auto source : image_sources)
        images += std::string(images.empty() ? "" : ",") + R"({"bufferView":)" +
                  std::to_string(first_image_view + source) + R"(,"mimeType":"image/png"})";
    std::string texture_list;
    for (std::size_t i = 0; i < textures; ++i)
        texture_list += std::string(texture_list.empty() ? "" : ",") + R"({"source":)" +
                        std::to_string(i % image_sources.size()) + "}";
    const std::string json = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],
      "meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],)" +
                             triangle_json(bin.size(), views) + R"(,"images":[)" + images + R"(],"textures":[)" +
                             texture_list + "]}";
    return glb(json, std::move(bin));
}
// fallback_png as bytes.
std::vector<std::byte> png_bytes() {
    std::vector<std::byte> bytes;
    for (const auto b : fallback_png)
        bytes.push_back(static_cast<std::byte>(b));
    return bytes;
}

// A motion file with node 0, node 1 authored as a matrix, and one clip per entry of @p clip_keys, whose single
// channel animates node 0's translation with that many keys, at most keys_per_long_clip. Clips with the same key
// count share one pair of accessors, and every accessor reads the same key data. With @p first_targets_matrix,
// the first clip targets node 1 instead, which the importer rejects when it reads that clip, before copying any
// keys. With @p ignored_channel, the second clip also has a channel without a target node that reads the longest
// accessors.
std::vector<std::byte> motion_clips(const std::vector<std::size_t> &clip_keys, bool first_targets_matrix,
                                    bool ignored_channel = false) {
    constexpr std::size_t time_bytes = sizeof(float), value_bytes = 3 * sizeof(float);
    std::vector<std::byte> bin;
    for (std::size_t key = 0; key < keys_per_long_clip; ++key)
        append(bin, static_cast<float>(key));
    bin.resize(bin.size() + keys_per_long_clip * value_bytes); // Zero translations.
    // One pair of time and value accessors per distinct key count, over the same two buffer views.
    std::vector<std::size_t> counts;
    std::string accessors;
    for (const auto keys : clip_keys) {
        if (std::ranges::find(counts, keys) != counts.end())
            continue;
        counts.push_back(keys);
        accessors += std::string(accessors.empty() ? "" : ",") +
                     R"({"bufferView":0,"componentType":5126,"type":"SCALAR","min":[0],"count":)" +
                     std::to_string(keys) + R"(,"max":[)" + std::to_string(keys - 1) + R"(]},
                     {"bufferView":1,"componentType":5126,"type":"VEC3","count":)" +
                     std::to_string(keys) + "}";
    }
    std::string animations;
    for (std::size_t i = 0; i < clip_keys.size(); ++i) {
        const auto pair = 2 * static_cast<std::size_t>(std::ranges::find(counts, clip_keys[i]) - counts.begin());
        std::string samplers = R"({"input":)" + std::to_string(pair) + R"(,"output":)" + std::to_string(pair + 1) + "}";
        const auto node = i == 0 && first_targets_matrix ? 1 : 0;
        std::string channels =
            R"({"sampler":0,"target":{"node":)" + std::to_string(node) + R"(,"path":"translation"}})";
        if (i == 1 && ignored_channel) {
            const auto longest =
                2 * static_cast<std::size_t>(std::ranges::find(counts, keys_per_long_clip) - counts.begin());
            samplers += R"(,{"input":)" + std::to_string(longest) + R"(,"output":)" + std::to_string(longest + 1) + "}";
            channels += R"(,{"sampler":1,"target":{"path":"translation"}})";
        }
        animations += std::string(animations.empty() ? "" : ",") + R"({"samplers":[)" + samplers + R"(],"channels":[)" +
                      channels + "]}";
    }
    const std::string json = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0,1]}],
      "nodes":[{"name":"root"},{"name":"fixed","matrix":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]}],
      "buffers":[{"byteLength":)" +
                             std::to_string(bin.size()) + R"(}],"bufferViews":[{"buffer":0,"byteLength":)" +
                             std::to_string(keys_per_long_clip * time_bytes) + R"(},{"buffer":0,"byteOffset":)" +
                             std::to_string(keys_per_long_clip * time_bytes) + R"(,"byteLength":)" +
                             std::to_string(keys_per_long_clip * value_bytes) + R"(}],"accessors":[)" + accessors +
                             R"(],"animations":[)" + animations + "]}";
    return glb(json, std::move(bin));
}

// A triangle with UVs whose material samples texture 0 as base color and as normal map, and three textures of
// one PNG image, the second with nearest magnification.
std::vector<std::byte> shared_image() {
    auto bin = triangle({});
    const auto png_offset = bin.size();
    for (const auto b : fallback_png)
        bin.push_back(static_cast<std::byte>(b));
    const std::string json = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],
      "materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}},"normalTexture":{"index":0}}],
      "meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":2},"material":0}]}],)" +
                             triangle_json(bin.size(), view(png_offset, fallback_png.size()),
                                           R"(,{"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"})") +
                             R"(,"samplers":[{"magFilter":9728}],"images":[{"bufferView":2,"mimeType":"image/png"}],
      "textures":[{"source":0},{"source":0,"sampler":0},{"source":0}]})";
    return glb(json, std::move(bin));
}
} // namespace

TEST_CASE("Supplied tangents are ignored when normals are generated") {
    // glTF 2.0: when normals are not specified, flat normals are computed and the provided tangents MUST be
    // ignored. A tangent w of 0 is the importer's absent-tangent value.
    const auto bytes = glb(R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],
      "meshes":[{"primitives":[{"attributes":{"POSITION":0,"TANGENT":1}}]}],)" +
                               triangle_json(position_bytes + vec4_bytes) + "}",
                           triangle({1, 0, 0, 1}));
    const auto asset = load_asset(bytes);
    REQUIRE(asset->primitives.size() == 1u);
    for (const auto &vertex : asset->primitives[0].vertices) {
        CHECK(vertex.tangent == std::array<float, 4>{});
        CHECK(vertex.normal.z == doctest::Approx(1));
    }
}

TEST_CASE("A metallic factor outside [0, 1] is rejected by material validation") {
    // load_asset checks every material with validate_material, which owns the factor ranges.
    const auto bytes = glb(R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],
      "materials":[{"pbrMetallicRoughness":{"metallicFactor":1.5}}],
      "meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":0}]}],)" +
                               triangle_json(position_bytes + vec4_bytes) + "}",
                           triangle({}));
    CHECK_THROWS_WITH_AS(load_asset(bytes), "Invalid material factors", std::invalid_argument);
}

TEST_CASE("A clip whose samplers hold one key imports as a pose of zero duration") {
    const auto asset =
        load_motion_asset(single_key_motion(R"([{"sampler":0,"target":{"node":0,"path":"translation"}}])"));
    REQUIRE(asset->animations.size() == 1u);
    CHECK(asset->animations[0].duration == 0);
    const auto pose = sample_pose(*asset, &asset->animations[0], 0);
    CHECK(pose.local[0].translation.y == doctest::Approx(2));
}

TEST_CASE("A channel without a target node is ignored") {
    // glTF 2.0: "When node isn't defined, channel SHOULD be ignored."
    const auto asset = load_motion_asset(single_key_motion(
        R"([{"sampler":0,"target":{"path":"translation"}},{"sampler":0,"target":{"node":0,"path":"translation"}}])"));
    REQUIRE(asset->animations.size() == 1u);
    CHECK(asset->animations[0].channels.size() == 1u);
}

TEST_CASE("An optional compressed texture source falls back to the PNG source") {
    for (const std::string extension : {"KHR_texture_basisu", "EXT_texture_webp"}) {
        CAPTURE(extension);
        const auto asset =
            load_asset(textured(extension, R"({"source":0,"extensions":{")" + extension + R"(":{"source":1}}})"));
        REQUIRE(asset->textures.size() == 1u);
        REQUIRE(asset->textures[0].image);
        const auto &image = *asset->textures[0].image;
        CHECK(image.width == 1u);
        CHECK(image.height == 1u);
        CHECK(std::equal(fallback_texel.begin(), fallback_texel.end(), image.rgba.begin()));
        CHECK_THROWS_WITH_AS(load_asset(textured(extension, R"({"extensions":{")" + extension + R"(":{"source":1}}})")),
                             "Texture needs a PNG or JPEG source", std::runtime_error);
    }
}

TEST_CASE("Textures made from one image share its decoded pixels, and meshes share them too") {
    const auto asset = load_asset(shared_image());
    // Three textures, then a copy of texture 0 for its second encoding: the normal map claims texture 0 as data
    // first, so the base color reads the copy.
    REQUIRE(asset->textures.size() == 4u);
    const auto &image = asset->textures[0].image;
    REQUIRE(image);
    CHECK(image->width == 1u);
    CHECK(std::ranges::equal(image->rgba, fallback_texel));
    for (const auto &texture : asset->textures)
        CHECK(texture.image == image);
    // Each texture keeps its own sampler and encoding.
    CHECK(asset->textures[0].sampler.mag == Filter::linear);
    CHECK(asset->textures[1].sampler.mag == Filter::nearest);
    REQUIRE(asset->materials.size() == 1u);
    CHECK(asset->materials[0].normal_texture == 0);
    CHECK(asset->materials[0].texture == 3);
    CHECK(asset->textures[0].encoding == TextureEncoding::linear);
    CHECK(asset->textures[3].encoding == TextureEncoding::srgb);
    const auto mesh = Mesh::compile(*asset);
    REQUIRE(mesh->materials()->textures.size() == asset->textures.size());
    for (const auto &texture : mesh->materials()->textures)
        CHECK(texture.image == image);
}

TEST_CASE("Import limits the number of textures and images") {
    const std::vector<std::vector<std::byte>> encoded{png_bytes()};
    // At the limit, every texture shares the one image.
    const auto shared = load_asset(textured_triangle(encoded, {0}, import_texture_limit));
    REQUIRE(shared->textures.size() == import_texture_limit);
    CHECK(std::ranges::all_of(shared->textures,
                              [&](const Texture &texture) { return texture.image == shared->textures[0].image; }));
    CHECK_THROWS_WITH_AS(load_asset(textured_triangle(encoded, {0}, import_texture_limit + 1)),
                         "GLB exceeds import texture limit", std::runtime_error);
    // Images count whether or not a texture uses them.
    const std::vector<std::size_t> images(import_image_limit, 0);
    CHECK(load_asset(textured_triangle(encoded, images, 1))->textures.size() == 1u);
    auto too_many = images;
    too_many.push_back(0);
    CHECK_THROWS_WITH_AS(load_asset(textured_triangle(encoded, too_many, 1)), "GLB exceeds import image limit",
                         std::runtime_error);
}

TEST_CASE("Import limits the decoded bytes of its images before decoding any, counting a shared image once") {
    // The largest images are header-only PNGs, so images within the byte limit reach the decoder, which rejects
    // them.
    const std::vector<std::vector<std::byte>> encoded{png_header(largest_image_edge, largest_image_edge), png_bytes()};
    // Sixteen of them decode to 1 GiB exactly, even though two textures use each.
    const std::vector<std::size_t> at_limit(largest_images_in_byte_limit, 0);
    CHECK_THROWS_WITH_AS(load_asset(textured_triangle(encoded, at_limit, 2 * at_limit.size())),
                         "PNG/JPEG decode failed", std::runtime_error);
    // A further 1x1 image exceeds the limit. It is measured last, so the limit applies before the first image
    // is decoded.
    auto over_limit = at_limit;
    over_limit.push_back(1);
    CHECK_THROWS_WITH_AS(load_asset(textured_triangle(encoded, over_limit, over_limit.size())),
                         "Decoded images exceed import byte limit", std::runtime_error);
}

TEST_CASE("Import limits the animation keys of every channel together, before reading any") {
    // A valid file: each channel copies the keys it reads, including those of accessors that others share.
    const auto valid = load_motion_asset(motion_clips({keys_per_long_clip, keys_per_long_clip, 1}, false));
    REQUIRE(valid->animations.size() == 3u);
    CHECK(valid->animations[0].channels.at(0).times.size() == keys_per_long_clip);
    CHECK(valid->animations[1].channels.at(0).times.size() == keys_per_long_clip);
    CHECK(valid->animations[2].channels.at(0).times.size() == 1u);
    // One key on the matrix node, then clips that share accessors, reaching the limit exactly. The limit admits
    // them, and the matrix node fails the first clip. The ignored channel counts no keys.
    std::vector<std::size_t> at_limit{1};
    at_limit.resize(import_key_limit / keys_per_long_clip, keys_per_long_clip);
    at_limit.push_back(keys_per_long_clip - 1);
    CHECK_THROWS_WITH_AS(load_motion_asset(motion_clips(at_limit, true, true)), "Animation cannot target a matrix node",
                         std::runtime_error);
    // One more key exceeds the limit, which applies before the first clip is read, in both importers.
    auto over_limit = at_limit;
    ++over_limit.back();
    constexpr auto too_many_keys = "Animation keys exceed import limit";
    CHECK_THROWS_WITH_AS(load_motion_asset(motion_clips(over_limit, true)), too_many_keys, std::runtime_error);
    CHECK_THROWS_WITH_AS(load_asset(motion_clips(over_limit, true)), too_many_keys, std::runtime_error);
}
