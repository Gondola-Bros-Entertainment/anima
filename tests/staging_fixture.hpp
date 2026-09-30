#pragma once
// Inputs of known size for the staging suites: GLB files of textured triangles, a component whose codec counts
// its calls, and scene documents that name their meshes.
#include <anima/scene_set.hpp>

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace staging_fixture {
using namespace anima;

// A 1x1 RGBA PNG holding the texel (255, 128, 64, 255).
inline constexpr std::array<std::uint8_t, 70> png{
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
    0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xdf, 0xe0, 0xf0, 0x1f, 0x00, 0x07, 0x00, 0x02, 0xbf,
    0x2b, 0xd7, 0xc7, 0xe2, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

inline void append(std::vector<std::byte> &bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xff));
}

// A GLB whose default scene has one node, rotated by the quaternion @p rotation (JSON numbers), with one mesh of
// @p primitives triangles, and @p images embedded PNG images, each decoded separately and used by its own
// texture and material; primitive i uses material i modulo @p images. Without images the primitives have no
// material.
inline std::vector<std::byte> glb(std::size_t primitives, std::size_t images, std::string_view rotation = "0,0,0,1") {
    std::vector<std::byte> bin;
    for (const float value : {0.F, 0.F, 0.F, 1.F, 0.F, 0.F, 0.F, 1.F, 0.F})
        append(bin, std::bit_cast<std::uint32_t>(value));
    for (const float value : {0.F, 0.F, 1.F, 0.F, 0.F, 1.F})
        append(bin, std::bit_cast<std::uint32_t>(value));
    constexpr std::size_t positions = 36, coordinates = 24;
    for (const auto byte : png)
        bin.push_back(std::byte{byte});
    while (bin.size() % 4)
        bin.push_back(std::byte{0});
    std::string json = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0,)"
                       R"("rotation":[)" +
                       std::string(rotation) + R"(]}],"meshes":[{"primitives":[)";
    for (std::size_t i = 0; i < primitives; ++i) {
        json += i ? "," : "";
        json += R"({"attributes":{"POSITION":0,"TEXCOORD_0":1})";
        if (images)
            json += R"(,"material":)" + std::to_string(i % images);
        json += '}';
    }
    json += "]}]";
    if (images) {
        std::string materials, textures, sources;
        for (std::size_t i = 0; i < images; ++i) {
            const auto index = std::to_string(i);
            const std::string separator = i ? "," : "";
            materials += separator + R"({"pbrMetallicRoughness":{"baseColorTexture":{"index":)" + index + "}}}";
            textures += separator + R"({"source":)" + index + '}';
            sources += separator + R"({"bufferView":2,"mimeType":"image/png"})";
        }
        json += R"(,"materials":[)" + materials + R"(],"textures":[)" + textures + R"(],"images":[)" + sources + ']';
    }
    json += R"(,"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],)"
            R"("max":[1,1,0]},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"}],"bufferViews":[)"
            R"({"buffer":0,"byteOffset":0,"byteLength":)" +
            std::to_string(positions) + R"(},{"buffer":0,"byteOffset":)" + std::to_string(positions) +
            R"(,"byteLength":)" + std::to_string(coordinates) + R"(},{"buffer":0,"byteOffset":)" +
            std::to_string(positions + coordinates) + R"(,"byteLength":)" + std::to_string(png.size()) +
            R"(}],"buffers":[{"byteLength":)" + std::to_string(bin.size()) + "}]}";
    while (json.size() % 4)
        json += ' ';
    constexpr std::uint32_t magic = 0x46546c67, version = 2, json_chunk = 0x4e4f534a, bin_chunk = 0x004e4942;
    constexpr std::uint32_t header_bytes = 12, chunk_header_bytes = 8;
    std::vector<std::byte> file;
    append(file, magic);
    append(file, version);
    append(file, static_cast<std::uint32_t>(header_bytes + 2 * chunk_header_bytes + json.size() + bin.size()));
    append(file, static_cast<std::uint32_t>(json.size()));
    append(file, json_chunk);
    for (const char c : json)
        file.push_back(static_cast<std::byte>(c));
    append(file, static_cast<std::uint32_t>(bin.size()));
    append(file, bin_chunk);
    file.insert(file.end(), bin.begin(), bin.end());
    return file;
}

// Calls of each Counted codec callback, from any thread.
struct CodecCalls {
    std::atomic<int> encoded{}, decoded{}, linked{};
};
// An application component with an opaque payload and one object link; a payload of "fail" is rejected.
struct Counted {
    std::string state;
    GameObject link;
};
inline constexpr std::string_view counted_key = "test.counted.v1";
inline constexpr std::string_view rejected_payload = "Rejected counted payload";
// A registry whose Counted codec counts every encode, decode and link callback in @p calls.
inline ComponentCodecs counting_codecs(CodecCalls &calls) {
    ComponentCodecs codecs;
    codecs.add<Counted>(
        std::string(counted_key),
        [&calls](const Counted &value, const ObjectReferences &references) {
            ++calls.encoded;
            return references.key(value.link).string() + ':' + value.state;
        },
        [&calls](GameObject object, std::string_view payload, const ObjectReferences &references) {
            ++calls.decoded;
            const auto separator = payload.find(':');
            const auto state = payload.substr(separator + 1);
            if (state == "fail")
                throw std::invalid_argument(std::string(rejected_payload));
            object.add_component<Counted>(std::string(state),
                                          references.resolve(ObjectKey::parse(payload.substr(0, separator))));
        },
        [&calls](Counted &value, ObjectLinks &links) {
            ++calls.linked;
            links.add(value.link);
        });
    return codecs;
}

// Meshes by the keys that documents store, fixed before any thread reads them.
struct Meshes {
    std::map<std::string, std::shared_ptr<const Mesh>, std::less<>> by_key;
    // The key of @p mesh, for MeshName.
    [[nodiscard]] std::string name(const std::shared_ptr<const Mesh> &mesh) const {
        for (const auto &[key, value] : by_key)
            if (value == mesh)
                return key;
        throw std::invalid_argument("Unnamed test mesh");
    }
    // The mesh stored under @p key, or null.
    [[nodiscard]] std::shared_ptr<const Mesh> find(std::string_view key) const {
        const auto found = by_key.find(key);
        return found == by_key.end() ? nullptr : found->second;
    }
};
// Two meshes compiled from GLB files of one and two primitives.
inline Meshes compiled_meshes() {
    Meshes result;
    result.by_key.emplace("one", Mesh::compile(*load_asset(glb(1, 1))));
    result.by_key.emplace("two", Mesh::compile(*load_asset(glb(2, 1))));
    return result;
}
// Fills @p scene with @p count roots, alternating between the meshes "one" and "two", each with a child that
// has no mesh and a Counted component linking to its parent.
inline void populate(Scene &scene, const Meshes &meshes, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        auto root = scene.create("root-" + std::to_string(i), meshes.find(i % 2 ? "two" : "one"));
        auto child = scene.create("child");
        child.set_parent(root);
        child.add_component<Counted>(Counted{"payload-" + std::to_string(i), root});
    }
}
} // namespace staging_fixture
