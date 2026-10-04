#include <anima/assets/fitted.hpp>
#include <anima/assets/preview.hpp>
#include <anima/camera.hpp>
#include <anima/components.hpp>
#include <anima/prefab.hpp>
#include <anima/scene.hpp>
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace anima;
namespace {
constexpr auto camera_codec = "anima.camera.v1";
constexpr auto float_range = "JSON number outside the float range";
constexpr auto invalid_utf8 = "\xff"; // A lone continuation byte is never valid UTF-8.
// Identifiers the JSON library puts in its messages, which the rethrown exceptions keep.
constexpr auto wrong_type = "[json.exception.type_error.302]";
constexpr auto valid_manifest =
    R"({"version":4,"units":"meters","asset_id":"body","model":"body.glb","motion_contract":null,"skeleton":{"id":"rig","joint_count":1,"bind_signature":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"},"clips":[{"name":"idle","loop":false,"reference_speed":null,"events":[]}]})";
struct Tag {};

// Writes @p json to a manifest file that is removed when the object is destroyed.
struct ManifestFile {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("anima-document-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    explicit ManifestFile(std::string_view json) { std::ofstream(path, std::ios::binary) << json; }
    ~ManifestFile() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
};
std::string changed(std::string text, std::string_view from, std::string_view to) {
    const auto at = text.find(from);
    REQUIRE(at != std::string::npos);
    return text.replace(at, from.size(), to);
}
} // namespace

TEST_CASE("Malformed document text is rejected as std::invalid_argument") {
    REQUIRE_THROWS_WITH_AS(
        Prefab::deserialize("{", {}, {}),
        "[json.exception.parse_error.101] parse error at line 1, column 2: syntax error while parsing object key - "
        "unexpected end of input; expected string literal",
        std::invalid_argument);
}

TEST_CASE("A mistyped document field is rejected as std::invalid_argument") {
    Scene scene;
    const ComponentCodecs codecs;
    auto document = Prefab::capture(scene.create("probe"), codecs).serialize({});
    const std::string name_field = R"("name": "probe")";
    const auto at = document.find(name_field);
    REQUIRE(at != std::string::npos);
    document.replace(at, name_field.size(), R"("name": 5)");
    REQUIRE_THROWS_WITH_AS(Prefab::deserialize(document, {}, codecs),
                           "[json.exception.type_error.302] type must be string, but is number", std::invalid_argument);
}

TEST_CASE("Component state that is not UTF-8 is rejected on output as std::invalid_argument") {
    ComponentCodecs codecs;
    codecs.add<Tag>(
        "test.tag.v1", [](const Tag &, const ObjectReferences &) { return std::string(invalid_utf8); },
        [](GameObject object, std::string_view, const ObjectReferences &) { object.add_component<Tag>(); });
    Scene scene;
    auto root = scene.create("probe");
    root.add_component<Tag>();
    const auto prefab = Prefab::capture(root, codecs);
    REQUIRE_THROWS_WITH_AS(prefab.serialize({}), "[json.exception.type_error.316] invalid UTF-8 byte at index 0: 0xFF",
                           std::invalid_argument);
}

TEST_CASE("A number outside the float range is rejected, not narrowed") {
    ComponentCodecs codecs;
    add_camera_component_codecs(codecs);
    Scene scene;
    const ComponentData data{camera_codec, R"({"projection":"perspective","vertical_fov_degrees":60,)"
                                           R"("orthographic_height":10,"near_plane":1e300,"far_plane":1000})"};
    REQUIRE_THROWS_WITH_AS(codecs.restore(scene.create("camera"), std::span(&data, 1), ObjectReferences{}), float_range,
                           std::invalid_argument);
}

TEST_CASE("Manifest fields that are missing or of the wrong JSON type are rejected as std::invalid_argument") {
    REQUIRE_NOTHROW((void)read_manifest(ManifestFile(valid_manifest).path));
    const ManifestFile mistyped(changed(valid_manifest, R"("loop":false)", R"("loop":"no")"));
    REQUIRE_THROWS_WITH_AS((void)read_manifest(mistyped.path),
                           "[json.exception.type_error.302] type must be boolean, but is string",
                           std::invalid_argument);
    const ManifestFile missing(changed(valid_manifest, R"("units":"meters",)", ""));
    REQUIRE_THROWS_WITH_AS((void)read_manifest(missing.path), "Missing JSON field: units", std::invalid_argument);
}

TEST_CASE("A fitted model path must be a relative .glb path without '..'") {
    const auto body = std::make_shared<const Asset>();
    const auto catalog = R"({"version":2,"items":[{"id":"cover","fits":{"profile":)"
                         R"({"model":"../cover.glb","skeleton":"s","bind_signature":"b"}}}]})";
    REQUIRE_THROWS_WITH_AS(FittedLibrary(body, Manifest{}, "profile", catalog),
                           "Fitted model must be a relative .glb path without '..'", std::invalid_argument);
}

TEST_CASE("A fitted library rejects an unknown texel retention") {
    const auto body = std::make_shared<const Asset>();
    REQUIRE_THROWS_WITH_AS(
        FittedLibrary(body, Manifest{}, "profile", R"({"version":2,"items":[]})", static_cast<TexelRetention>(2)),
        "Unknown texel retention", std::invalid_argument);
}

TEST_CASE("A fitted catalog field of the wrong JSON type is rejected as std::invalid_argument") {
    const auto body = std::make_shared<const Asset>();
    const auto catalog = R"({"version":2,"items":[{"id":5,"fits":{}}]})";
    REQUIRE_THROWS_WITH_AS(FittedLibrary(body, Manifest{}, "profile", catalog), doctest::Contains(wrong_type),
                           std::invalid_argument);
}

TEST_CASE("A fitted catalog is version 2, whose items have no slot") {
    const auto body = std::make_shared<const Asset>();
    Manifest manifest;
    manifest.skeleton_id = "s";
    manifest.bind_signature = "b";
    const std::string fits = R"("fits":{"profile":{"model":"cover.glb","skeleton":"s","bind_signature":"b"}})";
    const FittedLibrary library(body, manifest, "profile", R"({"version":2,"items":[{"id":"cover",)" + fits + "}]}");
    REQUIRE(library.contains("cover"));
    CHECK(library.definition("cover").model == "cover.glb");
    // Another version, even an equal float, is reported before the fields, so a field it lacks or
    // adds does not hide it.
    CHECK_THROWS_WITH_AS(
        FittedLibrary(body, manifest, "profile", R"({"version":1,"removed":0,"items":[{"id":"cover",)" + fits + "}]}"),
        "Unsupported fitted catalog version", std::invalid_argument);
    CHECK_THROWS_WITH_AS(
        FittedLibrary(body, manifest, "profile", R"({"version":2.0,"items":[{"id":"cover",)" + fits + "}]}"),
        "Unsupported fitted catalog version", std::invalid_argument);
    CHECK_THROWS_WITH_AS(
        FittedLibrary(body, manifest, "profile", R"({"version":2,"items":[{"id":"cover","slot":"top",)" + fits + "}]}"),
        "Unknown JSON field: slot", std::invalid_argument);
}
