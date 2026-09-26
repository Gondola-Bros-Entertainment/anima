#include "consumer/prefab_composition.hpp"
#include <doctest/doctest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace anima;
namespace {
constexpr std::size_t maximum_parts = 1024;
constexpr auto mount = R"({"part":"root","object":"41"})";
constexpr auto part_count = "Invalid prefab composition part count";
constexpr auto invalid_key = "Invalid prefab composition part or resource key";
constexpr auto unsupported_version = "Unsupported prefab composition document version";
constexpr auto missing_version = "Missing JSON field: version";
constexpr auto duplicate_field = "Duplicate JSON document field";
constexpr auto one_root = "Prefab composition requires one first root part";
constexpr auto earlier_part = "Prefab composition mount must name an earlier part";
constexpr auto null_mount = "Null prefab composition mount object key";
constexpr auto invalid_object_key = "Invalid object key";
constexpr auto matrix_size = "Prefab composition matrix requires 16 scalars";
constexpr auto not_affine = "Instance transform must be affine";
constexpr auto nonfinite_transform = "Non-finite instance transform";

std::string substitute(std::string value, std::string_view from, std::string_view to) {
    const auto at = value.find(from);
    REQUIRE(at != std::string::npos);
    value.replace(at, from.size(), to);
    return value;
}
std::string part(std::string_view key, std::string_view parent = "null") {
    return "{\"key\":\"" + std::string(key) + "\",\"prefab\":\"shared\",\"parent\":" + std::string(parent) +
           ",\"placement\":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]}";
}
std::string envelope(const std::string &parts) {
    return "{\"version\":1,\"kind\":\"anima.prefab-composition\",\"parts\":[" + parts + "]}";
}
// A root part and a child mounted on the root's object 41.
std::string valid_document() { return envelope(part("root") + "," + part("child", mount)); }
PrefabComposition decode(const std::string &document) { return PrefabComposition::deserialize(document); }
// A root and as many parts mounted on its object 1 as a composition allows.
std::vector<PrefabComposition::Part> most_parts() {
    std::vector<PrefabComposition::Part> parts;
    parts.reserve(maximum_parts + 1);
    parts.push_back({"root", "shared", std::nullopt, identity()});
    for (std::size_t i = 1; i < maximum_parts; ++i)
        parts.push_back({"part-" + std::to_string(i), "shared", PrefabComposition::Mount{"root", {1}}, identity()});
    return parts;
}
} // namespace

TEST_CASE("Composed prefabs link their parts in the destination, keep bounds finite and roll back failures") {
    prefab_composition_test::run();
}

TEST_CASE("A canonical composition document decodes its parts and a maximum-width mount key") {
    const auto valid = valid_document();
    const auto decoded = decode(valid);
    REQUIRE(decoded.parts().size() == 2);
    REQUIRE(decoded.parts()[1].parent);
    CHECK(decoded.parts()[1].parent->object.value == 41);
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto widest =
        decode(substitute(valid, "\"object\":\"41\"", "\"object\":\"" + std::to_string(maximum) + "\""));
    REQUIRE(widest.parts().size() == 2);
    REQUIRE(widest.parts()[1].parent);
    CHECK(widest.parts()[1].parent->object.value == maximum);
}

TEST_CASE("Malformed composition documents are rejected with their reason") {
    const auto valid = valid_document();
    CHECK_THROWS_WITH_AS(decode("{}"), missing_version, std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(envelope("")), part_count, std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"version\":1", "\"version\":2")), unsupported_version,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"version\":1", "\"version\":1.0")), unsupported_version,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"version\":1,", "")), missing_version, std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "anima.prefab-composition", "anima.prefab")),
                         "Invalid prefab composition document kind", std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "{", "{\"unexpected\":null,")), "Unknown JSON field: unexpected",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "{", "{\"version\":1,")), duplicate_field, std::invalid_argument);
    // A key that decodes to one already present is a duplicate.
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "{", "{\"ver\\u0073ion\":1,")), duplicate_field,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"key\":\"root\"", "\"key\":\"\"")), invalid_key,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"key\":\"child\"", "\"key\":\"root\"")),
                         "Duplicate prefab composition part key", std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"key\":\"root\",", "")), "Missing JSON field: key",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"key\":\"root\"", "\"key\":\"root\",\"k\\u0065y\":\"other\"")),
                         duplicate_field, std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"key\":\"root\"", "\"key\":\"" + std::string(4097, 'x') + "\"")),
                         invalid_key, std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"prefab\":\"shared\"", "\"prefab\":\"\"")), invalid_key,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"prefab\":\"shared\"", "\"prefab\":null")),
                         "[json.exception.type_error.302] type must be string, but is null", std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"prefab\":\"shared\",", "")), "Missing JSON field: prefab",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"parent\":null", std::string("\"parent\":") + mount)), one_root,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(envelope(part("root") + "," + part("child"))), one_root, std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"part\":\"root\"", "\"part\":\"child\"")), earlier_part,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"part\":\"root\"", "\"part\":\"later\"")), earlier_part,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"part\":\"root\",", "")), "Missing JSON field: part",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"object\":\"41\"", "\"object\":\"0\"")), null_mount,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"object\":\"41\"", "\"object\":\"041\"")), invalid_object_key,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"object\":\"41\"", "\"object\":41")),
                         "[json.exception.type_error.302] type must be string, but is number", std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"object\":\"41\"", "\"object\":\"18446744073709551616\"")),
                         invalid_object_key, std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"object\":\"41\"", "\"object\":\"41\",\"extra\":null")),
                         "Unknown JSON field: extra", std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"parent\":null,", "")), "Missing JSON field: parent",
                         std::invalid_argument);
    constexpr auto placement = "\"placement\":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]";
    CHECK_THROWS_WITH_AS(decode(substitute(valid, placement, "\"placement\":null")), matrix_size,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, placement, "\"placement\":[]")), matrix_size, std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, placement, "\"placement\":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,2]")),
                         not_affine, std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, placement, "\"placement\":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1e1000]")),
                         "[json.exception.out_of_range.406] number overflow parsing '1e1000'", std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(std::string(16 * 1024 * 1024, ' ') + valid), "JSON document exceeds byte limit",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(
        decode(substitute(valid, "\"parent\":null", "\"parent\":" + std::string(32, '[') + "0" + std::string(32, ']'))),
        "JSON document exceeds nesting limit", std::invalid_argument);
}

TEST_CASE("Invalid programmatic compositions are rejected with their reason") {
    const auto valid = prefab_composition_test::parts();
    CHECK_THROWS_WITH_AS(PrefabComposition({}), part_count, std::invalid_argument);
    auto bad = valid;
    bad[0].key.clear();
    CHECK_THROWS_WITH_AS(PrefabComposition(bad), invalid_key, std::invalid_argument);
    bad = valid;
    bad[0].prefab.assign(4097, 'p');
    CHECK_THROWS_WITH_AS(PrefabComposition(bad), invalid_key, std::invalid_argument);
    bad = valid;
    bad[1].parent->part = "leaf";
    CHECK_THROWS_WITH_AS(PrefabComposition(bad), earlier_part, std::invalid_argument);
    bad = valid;
    bad[1].parent->object = {};
    CHECK_THROWS_WITH_AS(PrefabComposition(bad), null_mount, std::invalid_argument);
    bad = valid;
    bad[0].placement[12] = std::numeric_limits<float>::quiet_NaN();
    CHECK_THROWS_WITH_AS(PrefabComposition(bad), nonfinite_transform, std::invalid_argument);
    bad = valid;
    bad[1].placement[15] = 2;
    CHECK_THROWS_WITH_AS(PrefabComposition(bad), not_affine, std::invalid_argument);
}

TEST_CASE("A composition holds at most 1,024 parts") {
    auto parts = most_parts();
    const PrefabComposition maximum(parts);
    CHECK(decode(maximum.serialize()).parts().size() == maximum_parts);
    parts.push_back({"too-many", "shared", PrefabComposition::Mount{"root", {1}}, identity()});
    CHECK_THROWS_WITH_AS(PrefabComposition(parts), part_count, std::invalid_argument);
    auto document = part("root");
    for (std::size_t i = 1; i <= maximum_parts; ++i)
        document += ',' + part("part-" + std::to_string(i), R"({"part":"root","object":"1"})");
    CHECK_THROWS_WITH_AS(decode(envelope(document)), part_count, std::invalid_argument);
}

TEST_CASE("An instantiation beyond the object budget or with a nonfinite placement changes nothing") {
    // One cached 65-node asset repeated 1,024 times exceeds the aggregate object
    // budget. Validate this before allocating any native destination identities.
    std::vector<Prefab::Node> nodes(65);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        nodes[i].key = {i + 1};
        if (i)
            nodes[i].parent = 0;
    }
    const auto base = std::make_shared<const Prefab>(nodes);
    Scene scene;
    const auto existing = scene.create();
    int resolves = 0;
    const auto counted = [&](std::string_view) {
        ++resolves;
        return base;
    };
    const PrefabComposition maximum(most_parts());
    CHECK_THROWS_WITH_AS(maximum.instantiate(scene, counted, {}), "Prefab composition exceeds the object limit",
                         std::invalid_argument);
    const auto next = scene.create();
    CHECK(resolves == 1);
    CHECK(existing.valid());
    CHECK(scene.size() == 2);
    CHECK(next.key().value == existing.key().value + 1); // No destination identity was consumed.
    const PrefabComposition one({{"root", "shared", std::nullopt, identity()}});
    auto invalid_placement = identity();
    invalid_placement[12] = std::numeric_limits<float>::infinity();
    const auto resolve = [&](std::string_view) { return base; };
    CHECK_THROWS_WITH_AS(one.instantiate(scene, resolve, {}, invalid_placement), nonfinite_transform,
                         std::invalid_argument);
    CHECK(scene.size() == 2);
    CHECK(existing.valid());
    CHECK(next.valid());
}
