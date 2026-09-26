#include <anima/assets/asset.hpp>
#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

// Files that glTF 2.0 allows, which the importer must accept, each built in memory.
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

void append(std::vector<std::byte> &bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xff));
}
void append(std::vector<std::byte> &bytes, float value) { append(bytes, std::bit_cast<std::uint32_t>(value)); }

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
// appends further buffer views.
std::string triangle_json(std::size_t buffer_bytes, const std::string &more_views = {}) {
    return R"("buffers":[{"byteLength":)" + std::to_string(buffer_bytes) + R"(}],
      "bufferViews":[{"buffer":0,"byteLength":)" +
           std::to_string(position_bytes) + R"(},
                     {"buffer":0,"byteOffset":)" +
           std::to_string(position_bytes) + R"(,"byteLength":)" + std::to_string(vec4_bytes) + "}" + more_views + R"(],
      "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},
                   {"bufferView":1,"componentType":5126,"count":3,"type":"VEC4"}])";
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
        CHECK(asset->textures[0].width == 1u);
        CHECK(asset->textures[0].height == 1u);
        CHECK(std::equal(fallback_texel.begin(), fallback_texel.end(), asset->textures[0].rgba.begin()));
        CHECK_THROWS_AS(load_asset(textured(extension, R"({"extensions":{")" + extension + R"(":{"source":1}}})")),
                        std::runtime_error);
    }
}
