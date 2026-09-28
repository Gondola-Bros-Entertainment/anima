#include "consumer/scene_set.hpp"
#include <doctest/doctest.h>

#include <cstddef>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

constexpr auto follow_key = "test.follow.v1";
constexpr auto lacks_key = "Replacement lacks a linked object key";
constexpr auto unmapped_target = "Object reference target is missing or expired";
constexpr auto stale_link = "Object reference is stale or outside the captured graph";
// An application component with one object link, which may cross members.
struct Follow {
    explicit Follow(GameObject linked) : target(linked) {}
    GameObject target;
};
// A Follow codec, reporting the link to scene sets only when @p report is true. Each call of the link
// callback runs @p during first.
ComponentCodecs follow_codecs(bool report, std::function<void()> during = {}) {
    const auto write = [](const Follow &follow, const ObjectReferences &map) {
        return map.key(follow.target).string();
    };
    const auto read = [](GameObject owner, std::string_view state, const ObjectReferences &map) {
        owner.add_component<Follow>(map.resolve(ObjectKey::parse(state)));
    };
    ComponentCodecs result;
    if (report)
        result.add<Follow>(follow_key, write, read, [during = std::move(during)](Follow &follow, ObjectLinks &found) {
            if (during)
                during();
            found.add(follow.target);
        });
    else
        result.add<Follow>(follow_key, write, read);
    return result;
}
// Records, from its destructor, the target of a Follow in another member when this component's scene
// is cleaned up.
struct CleanupWitness {
    ComponentRef<Follow> watched;
    Scene::Id *seen;
    ~CleanupWitness() {
        if (watched)
            *seen = watched->target.id();
    }
};
// Members alpha and beta whose objects a and b link to each other through Follow, and a document of
// them with beta active.
struct Linked {
    SceneSet scenes;
    SceneRef alpha = scenes.create("alpha"), beta = scenes.create("beta");
    GameObject a = alpha->create("a"), b = beta->create("b");
    ObjectKey b_key = b.key();
    ComponentCodecs codecs = follow_codecs(true);
    std::string saved;
    Linked() {
        a.add_component<Follow>(b);
        b.add_component<Follow>(a);
        scenes.set_active(beta);
        saved = scenes.serialize({}, codecs);
        scenes.set_active(alpha);
    }
    // Whether the set, its selection, both members and both links are as the constructor left them.
    bool unchanged() {
        if (scenes.size() != 2 || !alpha.valid() || !beta.valid() || scenes.active().key() != "alpha")
            return false;
        scenes.set_active(beta);
        const bool same = a.valid() && b.valid() && a.get_component<Follow>()->target.id() == b.id() &&
                          b.get_component<Follow>()->target.id() == a.id() && scenes.serialize({}, codecs) == saved;
        scenes.set_active(alpha);
        return same;
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
        // A removed field does not hide another version.
        {substitute(substitute(valid, "\"version\":1", "\"version\":3"), "{", "{\"removed\":0,"), unsupported_version},
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

TEST_CASE("A scene object may omit its settings, which load as their defaults and are written back") {
    const std::string document =
        R"({"version":3,"kind":"anima.scene","next_key":"2","objects":[{"key":"1",)"
        R"("name":"root","parent":null,"local":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"mesh":null}]})";
    const auto loaded = load_scene(document, {});
    const auto roots = loaded->roots();
    REQUIRE(roots.size() == 1);
    CHECK(roots.front().active_self());
    CHECK_FALSE(roots.front().has_renderer());
    const auto written = serialize_scene(*loaded, {});
    for (const auto field : {R"("pose": null)", R"("visible": true)", R"("active": true)", R"("material_factors": [])",
                             R"("primitive_visible": [])", R"("components": [])"}) {
        CAPTURE(field);
        CHECK(written.find(field) != std::string::npos);
    }
    // An explicit null is not an omission, so a setting that cannot be null still rejects it.
    CHECK_THROWS_WITH_AS((void)load_scene(substitute(document, R"("mesh":null)", R"("mesh":null,"active":null)"), {}),
                         "[json.exception.type_error.302] type must be boolean, but is null", std::invalid_argument);
    // A removed field does not hide another version.
    CHECK_THROWS_WITH_AS((void)load_scene(substitute(substitute(document, R"("version":3)", R"("version":2)"),
                                                     R"("mesh":null)", R"("mesh":null,"removed":0)"),
                                          {}),
                         "Unsupported scene document version", std::invalid_argument);
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

TEST_CASE_FIXTURE(Linked, "Replacing a member from a set document keeps the links to and from it") {
    Scene::Id seen{};
    b.add_component<CleanupWitness>(a.get_component<Follow>(), &seen);
    const auto replaced = scenes.replace(beta, saved, {}, codecs);
    const auto restored = replaced->find(b_key);
    CHECK_FALSE(beta.valid());
    CHECK_FALSE(b.valid());
    REQUIRE(restored.valid());
    // The member keeps its namespace, position and selection; the document's active member is ignored.
    CHECK(scenes.scenes()[1].key() == "beta");
    CHECK(scenes.active().key() == "alpha");
    // The survivor's link rebinds by key, and the replacement's link resolves by address.
    CHECK(a.get_component<Follow>()->target.id() == restored.id());
    CHECK(restored.get_component<Follow>()->target.id() == a.id());
    // The old scene's cleanup already saw the rebound link.
    CHECK(seen == restored.id());
    scenes.set_active(replaced);
    CHECK(scenes.serialize({}, codecs) == saved);
}

TEST_CASE_FIXTURE(Linked, "Replacing a member from a scene document rebinds the links into it by key") {
    Scene authored;
    (void)authored.create("first");
    auto second = authored.create("second");
    REQUIRE(second.key() != b_key);
    auto same_key = authored.find(b_key);
    REQUIRE(same_key.valid());
    same_key.set_name("rebound");
    const auto replaced = scenes.replace(beta, serialize_scene(authored, {}), {}, codecs);
    CHECK_FALSE(b.valid());
    CHECK(a.get_component<Follow>()->target.name() == "rebound");
    CHECK(a.get_component<Follow>()->target.id() == replaced->find(b_key).id());
}

TEST_CASE_FIXTURE(Linked, "A replacement that lacks a linked key or target fails without changing the set") {
    Scene vacant;
    CHECK_THROWS_WITH_AS((void)scenes.replace(beta, serialize_scene(vacant, {}), {}, codecs), lacks_key,
                         std::invalid_argument);
    CHECK(unchanged());
    SceneSet other;
    (void)other.create("alpha");
    (void)other.create("beta");
    CHECK_THROWS_WITH_AS((void)scenes.replace(beta, other.serialize({}), {}, codecs), lacks_key, std::invalid_argument);
    CHECK(unchanged());
    // The replacement links to a, which no longer exists in the member that stays.
    a.destroy();
    CHECK_THROWS_WITH_AS((void)scenes.replace(beta, saved, {}, codecs), unmapped_target, std::invalid_argument);
    CHECK(beta.valid());
    CHECK(b.valid());
    CHECK(b.get_component<Follow>()->target.id() == a.id());
}

TEST_CASE_FIXTURE(Linked, "Unloading a member clears the links into it and reports them") {
    auto second = alpha->create("second");
    second.add_component<Follow>(b);
    auto bystander = alpha->create("bystander");
    bystander.add_component<Follow>(second);
    Scene::Id seen{1, 1, 1};
    b.add_component<CleanupWitness>(a.get_component<Follow>(), &seen);
    const auto cleared = scenes.unload(beta, codecs);
    CHECK_FALSE(beta.valid());
    CHECK_FALSE(b.valid());
    REQUIRE(cleared.size() == 2);
    for (const auto &link : cleared) {
        CHECK((link.owner.id() == a.id() || link.owner.id() == second.id()));
        CHECK(link.component == follow_key);
        CHECK(link.target == SceneAddress{"beta", b_key});
    }
    CHECK(cleared[0].owner.id() != cleared[1].owner.id());
    CHECK(a.get_component<Follow>()->target.id() == Scene::Id{});
    CHECK(second.get_component<Follow>()->target.id() == Scene::Id{});
    CHECK(bystander.get_component<Follow>()->target.id() == second.id());
    CHECK(seen == Scene::Id{});
    // A cleared link persists as null.
    SceneSet restored;
    restored.restore(scenes.serialize({}, codecs), {}, codecs);
    CHECK_FALSE(restored.find(SceneAddress{"alpha", a.key()}).get_component<Follow>()->target.valid());
}

TEST_CASE_FIXTURE(Linked, "Links that no codec reports expire with their scene") {
    const auto silent = follow_codecs(false);
    const auto replaced = scenes.replace(beta, saved, {}, silent);
    CHECK_FALSE(a.get_component<Follow>()->target.valid());
    // The replacement's own link still resolves by address.
    CHECK(replaced->find(b_key).get_component<Follow>()->target.id() == a.id());
    CHECK_THROWS_WITH_AS(scenes.serialize({}, silent), stale_link, std::invalid_argument);
    a.get_component<Follow>()->target = replaced->find(b_key);
    CHECK(scenes.unload(replaced).empty());
    CHECK_FALSE(a.get_component<Follow>()->target.valid());
    CHECK_THROWS_WITH_AS(scenes.serialize({}, silent), stale_link, std::invalid_argument);
}

TEST_CASE_FIXTURE(Linked, "A failing link callback leaves the set unchanged, and callbacks cannot change membership") {
    int calls = 0;
    const auto nested = follow_codecs(true, [&] {
        ++calls;
        CHECK_THROWS_WITH_AS(scenes.clear(), "Scene membership changes cannot be nested", std::logic_error);
    });
    const auto failing = follow_codecs(true, [] { throw std::runtime_error("Link callback failed"); });
    CHECK_THROWS_WITH_AS((void)scenes.replace(beta, saved, {}, failing), "Link callback failed", std::runtime_error);
    CHECK(unchanged());
    CHECK_THROWS_WITH_AS(scenes.unload(beta, failing), "Link callback failed", std::runtime_error);
    CHECK(unchanged());
    // Only the survivor's component is visited; the retired member's own links are not.
    const auto replaced = scenes.replace(beta, saved, {}, nested);
    CHECK(calls == 1);
    CHECK(scenes.unload(replaced, nested).size() == 1);
    CHECK(calls == 2);
}

TEST_CASE("A set document replacement validates the whole document but loads only its member") {
    const auto first_mesh = scene_set_test::persistence_mesh(), second_mesh = scene_set_test::persistence_mesh();
    SceneSet scenes;
    auto alpha = scenes.create("alpha"), beta = scenes.create("beta");
    const auto kept = alpha->create("kept", first_mesh);
    (void)beta->create("replaced", second_mesh);
    const auto saved =
        scenes.serialize([&](const auto &mesh) { return mesh == first_mesh ? "first-mesh" : "second-mesh"; });
    std::vector<std::string> resolved;
    const MeshResolver resolve = [&](std::string_view name) -> std::shared_ptr<const Mesh> {
        resolved.emplace_back(name);
        return name == "second-mesh" ? second_mesh : nullptr;
    };
    // Every mesh key of the document resolves, including those of members that are not loaded.
    CHECK_THROWS_WITH_AS((void)scenes.replace(beta, saved, resolve), "Scene mesh key could not be resolved",
                         std::invalid_argument);
    CHECK(resolved == std::vector<std::string>{"first-mesh"});
    CHECK(beta.valid());
    const MeshResolver both = [&](std::string_view name) {
        resolved.emplace_back(name);
        return name == "first-mesh" ? first_mesh : second_mesh;
    };
    resolved.clear();
    auto renamed = saved;
    for (auto at = renamed.find("\"beta\""); at != std::string::npos; at = renamed.find("\"beta\"", at))
        renamed.replace(at, 6, "\"gamma\"");
    CHECK_THROWS_WITH_AS((void)scenes.replace(beta, renamed, both), "Replaced scene namespace is missing",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS((void)scenes.replace(beta, substitute(saved, "\"version\": 1", "\"version\": 2"), both),
                         unsupported_version, std::invalid_argument);
    CHECK(beta.valid());
    resolved.clear();
    const auto replaced = scenes.replace(beta, saved, both);
    CHECK(resolved == std::vector<std::string>{"first-mesh", "second-mesh"});
    CHECK(kept.valid());
    CHECK(alpha->size() == 1);
    CHECK(replaced->size() == 1);
    CHECK(replaced->find(ObjectKey{1}).renderer().mesh() == second_mesh);
}
