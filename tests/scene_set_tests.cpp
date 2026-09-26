#include "consumer/scene_set.hpp"
#include <doctest/doctest.h>

#include <cstddef>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
using namespace anima;
// Limits that SceneSet::restore documents.
constexpr std::size_t maximum_document_bytes = 16 * 1024 * 1024, maximum_namespace_bytes = 4096;
constexpr std::size_t maximum_scenes = 1024, maximum_objects = 65536;
constexpr auto busy_scene = "Scene drivers require an idle live scene";
constexpr auto callback_membership = "Scene membership cannot change during scene callbacks";
constexpr auto unsupported_version = "Unsupported scene set document version";
constexpr auto missing_version = "Missing JSON field: version";
constexpr auto duplicate_field = "Duplicate JSON document field";
constexpr auto needs_active = "Scene set requires an active namespace";
constexpr auto invalid_namespace = "Invalid scene namespace";
constexpr auto invalid_key = "Invalid object key";
constexpr auto uncovered = "Scene set reference table must cover every object";
constexpr auto duplicate_reference = "Null or duplicate scene set reference key";
constexpr auto missing_target = "Missing or duplicate scene set reference target";
constexpr auto extra_field = "Unknown JSON field: extra";
constexpr auto number_key = "[json.exception.type_error.302] type must be string, but is number";

std::string substitute(std::string text, std::string_view from, std::string_view to) {
    const auto at = text.find(from);
    REQUIRE(at != std::string::npos);
    text.replace(at, from.size(), to);
    return text;
}
std::string node(std::string_view key = "1") {
    return "{\"key\":\"" + std::string(key) +
           "\",\"name\":\"\",\"parent\":null,\"local\":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"
           "\"mesh\":null,\"pose\":null,\"visible\":true,\"active\":true,\"material_factors\":[],"
           "\"primitive_visible\":[],\"components\":[]}";
}
std::string scene(std::string_view key, const std::string &objects, std::string_view next = "2") {
    return "{\"key\":\"" + std::string(key) + "\",\"next_key\":\"" + std::string(next) + "\",\"objects\":[" + objects +
           "]}";
}
std::string envelope(const std::string &scenes, const std::string &references, std::string_view active = "\"beta\"") {
    return "{\"kind\":\"anima.scene-set\",\"version\":1,\"active\":" + std::string(active) + " ,\"scenes\":[" + scenes +
           "],\"references\":[" + references + "]}";
}
// Calls persistence from on_update; the update that runs the hook decides each error.
struct CallbackBoundary {
    SceneSet *scenes;
    ComponentCodecs *codecs;
    const std::string *empty;
    int *called;
    const char *serialize_error, *restore_error;
    void on_update(double) {
        ++*called;
        CHECK_THROWS_WITH_AS(scenes->serialize({}, *codecs), serialize_error, std::logic_error);
        CHECK_THROWS_WITH_AS(scenes->restore(*empty, {}), restore_error, std::logic_error);
    }
};
struct ConstructorBoundary {
    ConstructorBoundary(SceneSet &scenes, const std::string &empty) {
        CHECK_THROWS_WITH_AS(scenes.serialize({}), busy_scene, std::logic_error);
        CHECK_THROWS_WITH_AS(scenes.restore(empty, {}), callback_membership, std::logic_error);
    }
};
} // namespace

TEST_CASE("The shared consumer scenario for scene ownership, persistence, references, replacement and unload passes") {
    // It reports a failed check by throwing std::runtime_error, which fails this test case.
    scene_set_test::run();
}

TEST_CASE("Malformed or oversized scene set documents are rejected without changing the destination") {
    const std::string references =
        "{\"key\":\"41\",\"scene\":\"alpha\",\"object\":\"1\"},{\"key\":\"99\",\"scene\":\"beta\",\"object\":\"1\"}";
    const auto valid = envelope(scene("alpha", node()) + "," + scene("beta", node()), references);
    SceneSet destination;
    destination.restore(valid, {});
    auto first = destination.find("alpha"), second = destination.find("beta");
    REQUIRE(first.valid());
    REQUIRE(second.valid());
    const auto retained = first->find(ObjectKey{1});
    // The reader accepts sparse operation keys and the same object key in two scenes.
    CHECK(destination.active().key() == "beta");
    CHECK(retained.valid());
    CHECK(second->find(ObjectKey{1}).valid());
    const auto before = destination.serialize({});
    std::string oversized;
    for (std::size_t i = 0; i < maximum_objects; ++i) {
        if (i)
            oversized += ',';
        oversized += "{}";
    }
    std::string too_many;
    for (std::size_t i = 0; i <= maximum_scenes; ++i) {
        if (i)
            too_many += ',';
        too_many += scene("scene-" + std::to_string(i), "", "1");
    }
    const std::pair<std::string, const char *> documents[]{
        {"{}", missing_version},
        {substitute(valid, "\"version\":1", "\"version\":3"), unsupported_version},
        {substitute(valid, "\"version\":1", "\"version\":1.0"), unsupported_version},
        {substitute(valid, "\"version\":1,", ""), missing_version},
        {substitute(valid, "anima.scene-set", "anima.scene"), "Invalid scene set document kind"},
        {substitute(valid, "{", "{\"unexpected\":null,"), "Unknown JSON field: unexpected"},
        {substitute(valid, "{", "{\"version\":1,"), duplicate_field},
        {substitute(valid, "{", "{\"ver\\u0073ion\":1,"), duplicate_field},
        {substitute(valid, "\"active\":\"beta\"", "\"active\":null"), needs_active},
        {substitute(valid, "\"active\":\"beta\"", "\"active\":true"), needs_active},
        {substitute(valid, "\"active\":\"beta\"", "\"active\":\"missing\""), "Active scene namespace is missing"},
        {substitute(valid, "\"key\":\"beta\"", "\"key\":\"alpha\""), "Duplicate scene namespace"},
        {substitute(valid, "\"key\":\"alpha\"", "\"key\":\"\""), invalid_namespace},
        {substitute(valid, "\"key\":\"alpha\"", "\"key\":\"bad\\u0000name\""), invalid_namespace},
        {substitute(valid, "\"key\":\"alpha\"", "\"key\":\"" + std::string(maximum_namespace_bytes + 1, 'x') + "\""),
         invalid_namespace},
        {substitute(valid, "\"next_key\":\"2\"", "\"next_key\":\"1\""), "Invalid scene next object key"},
        {substitute(valid, "\"next_key\":\"2\"", "\"next_key\":\"02\""), invalid_key},
        {substitute(valid, "\"next_key\":\"2\",", ""), "Missing JSON field: next_key"},
        {substitute(valid, "\"next_key\":\"2\"", "\"next_key\":\"2\",\"extra\":0"), extra_field},
        {envelope(scene("alpha", node()), "", "\"alpha\""), uncovered},
        // The count check rejects the extra entry before reading its fields.
        {envelope(scene("alpha", node()) + "," + scene("beta", node()), references + ",{}"), uncovered},
        {substitute(valid, "\"key\":\"99\"", "\"key\":\"41\""), duplicate_reference},
        {substitute(valid, "\"key\":\"41\"", "\"key\":\"0\""), duplicate_reference},
        {substitute(valid, "\"key\":\"41\"", "\"key\":\"041\""), invalid_key},
        {substitute(valid, "\"key\":\"41\"", "\"key\":41"), number_key},
        {substitute(valid, "\"scene\":\"alpha\"", "\"scene\":\"missing\""), "Scene set reference namespace is missing"},
        {substitute(valid, "\"scene\":\"beta\"", "\"scene\":\"alpha\""), missing_target},
        {substitute(valid, "\"object\":\"1\"", "\"object\":\"0\""), missing_target},
        {substitute(valid, "\"object\":\"1\"", "\"object\":\"2\""), missing_target},
        {substitute(valid, "\"object\":\"1\"", "\"object\":\"01\""), invalid_key},
        {substitute(valid, "\"object\":\"1\"", "\"object\":1"), number_key},
        {substitute(valid, "\"scene\":\"alpha\",", ""), "Missing JSON field: scene"},
        {substitute(valid, "\"scene\":\"alpha\"", "\"scene\":\"alpha\",\"extra\":0"), extra_field},
        {envelope("", "", "\"beta\""), "Empty scene set has an active scene"},
        {std::string(maximum_document_bytes, ' ') + valid, "JSON document exceeds byte limit"},
        // The second array is within the per-scene bound, but exceeds the remaining
        // set budget. Reject it before traversing its untrusted object descriptions.
        {envelope(scene("alpha", node()) + "," + scene("beta", oversized), ""), "Scene set exceeds the object limit"},
        {envelope("", oversized + ",{}", "null"), "Invalid scene set reference count"},
        {envelope(too_many, "", "\"scene-0\""), "Invalid scene set scene count"},
    };
    for (std::size_t index = 0; index < std::size(documents); ++index) {
        CAPTURE(index);
        CHECK_THROWS_WITH_AS(destination.restore(documents[index].first, {}), documents[index].second,
                             std::invalid_argument);
        CHECK(first.valid());
        CHECK(second.valid());
        CHECK(retained.valid());
        CHECK(destination.serialize({}) == before);
    }
}

TEST_CASE("An exhausted key allocator rejects creation but keeps persisted identities") {
    const auto one =
        envelope(scene("alpha", node(), "0"), "{\"key\":\"7\",\"scene\":\"alpha\",\"object\":\"1\"}", "\"alpha\"");
    SceneSet exhausted;
    exhausted.restore(one, {});
    CHECK_THROWS_WITH_AS(exhausted.find("alpha")->create(), "Scene object keys exhausted", std::overflow_error);
    CHECK(exhausted.find("alpha")->find(ObjectKey{1}).valid());
}

TEST_CASE("A set at the scene limit round-trips, and one more scene fails to serialize") {
    SceneSet many;
    for (std::size_t i = 0; i < maximum_scenes; ++i)
        (void)many.create("scene-" + std::to_string(i));
    const auto at_limit = many.serialize({});
    SceneSet boundary;
    boundary.restore(at_limit, {});
    CHECK(boundary.size() == maximum_scenes);
    CHECK(boundary.active().key() == "scene-0");
    (void)many.create("one-too-many");
    CHECK_THROWS_WITH_AS(many.serialize({}), "Scene set exceeds the scene limit", std::invalid_argument);
}

TEST_CASE("Persistence cannot run from component hooks or constructors, and scheduling continues") {
    int called = 0;
    const auto empty = envelope("", "", "null");
    ComponentCodecs codecs;
    codecs.add<CallbackBoundary>(
        "test.set-callback.v1", [](const CallbackBoundary &, const ObjectReferences &) { return "{}"; },
        [](GameObject, std::string_view, const ObjectReferences &) {});
    SceneSet scenes;
    auto member = scenes.create("member");
    auto object = member->create();
    object.add_component<ConstructorBoundary>(scenes, empty);
    object.remove_component<ConstructorBoundary>();
    // Updating the set holds the whole set; updating one member holds only that scene.
    auto boundary = object.add_component<CallbackBoundary>(&scenes, &codecs, &empty, &called,
                                                           "Scene drivers cannot run during set mutation or scheduling",
                                                           "Scene membership changes cannot be nested");
    scenes.update(0);
    boundary->serialize_error = busy_scene;
    boundary->restore_error = callback_membership;
    member->update(0);
    CHECK(called == 2);
    CHECK(member.valid());
    CHECK(object.valid());
}
