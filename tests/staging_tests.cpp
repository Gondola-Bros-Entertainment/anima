#include "consumer/presentation.hpp"
#include "staging_fixture.hpp"
#include <anima/assets/actor_presentation.hpp>
#include <anima/assets/asset.hpp>
#include <anima/assets/attachments.hpp>
#include <anima/assets/fitted.hpp>
#include <anima/assets/motion_runtime.hpp>
#include <anima/assets/preview.hpp>
#include <anima/core/math_error.hpp>
#include <anima/prefab.hpp>
#include <anima/scene_set.hpp>
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace anima;
using namespace staging_fixture;
constexpr auto cancelled = "Staging was cancelled";
// Limits that the scene document readers document.
constexpr std::size_t maximum_document_bytes = 16 * 1024 * 1024;

std::string substitute(std::string text, std::string_view from, std::string_view to) {
    const auto at = text.find(from);
    REQUIRE(at != std::string::npos);
    text.replace(at, from.size(), to);
    return text;
}
// A scene document and a scene set document of linked, meshed objects, with the meshes and codecs they need.
struct Documents {
    Meshes meshes = compiled_meshes();
    CodecCalls calls;
    ComponentCodecs codecs = counting_codecs(calls);
    MeshName name = [this](const std::shared_ptr<const Mesh> &mesh) { return meshes.name(mesh); };
    MeshResolver resolve = [this](std::string_view key) { return meshes.find(key); };
    // `count` roots, alternating between the meshes "one" and "two", each with a linked child.
    std::string scene;
    // Member "alpha" as `scene`, and member "beta" with one root of mesh "one" and its child.
    std::string set;
    explicit Documents(std::size_t count) {
        Scene source;
        populate(source, meshes, count);
        scene = serialize_scene(source, name, codecs);
        SceneSet sources;
        populate(sources.create("alpha").get(), meshes, count);
        populate(sources.create("beta").get(), meshes, 1);
        set = sources.serialize(name, codecs);
        calls.encoded = 0;
    }
    Documents(const Documents &) = delete;
    Documents &operator=(const Documents &) = delete;
};
// A GLB file that the destructor removes.
struct TemporaryFile {
    std::filesystem::path path;
    explicit TemporaryFile(const std::vector<std::byte> &bytes) {
        std::random_device random;
        path = std::filesystem::temp_directory_path() / ("anima-staging-" + std::to_string(random()) + ".glb");
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        REQUIRE(file.good());
    }
    ~TemporaryFile() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
    TemporaryFile(const TemporaryFile &) = delete;
    TemporaryFile &operator=(const TemporaryFile &) = delete;
};
// Runs @p stage on another thread and returns its future, whose get() rethrows what the thread threw.
template <class Stage> auto elsewhere(Stage stage) { return std::async(std::launch::async, std::move(stage)); }

TEST_CASE("Stop sources and tokens share one permanent stop state") {
    CHECK_FALSE(StopToken{}.stop_requested());
    StopSource source;
    const auto token = source.get_token(), copy = token;
    CHECK_FALSE(copy.stop_requested());
    CHECK(source.request_stop());
    CHECK_FALSE(source.request_stop());
    CHECK(source.stop_requested());
    CHECK(token.stop_requested());
    CHECK(copy.stop_requested());
    const auto shared = source;
    CHECK(shared.stop_requested());
    StopSource moved;
    const auto observer = moved.get_token();
    const auto owner = std::move(moved);
    // A moved-from source has no state.
    CHECK_FALSE(moved.request_stop());
    CHECK_FALSE(moved.stop_requested());
    CHECK_FALSE(moved.get_token().stop_requested());
    CHECK_FALSE(observer.stop_requested());
    CHECK(owner.stop_requested() == observer.stop_requested());
}

TEST_CASE("A document staged on another thread commits as the synchronous load does, any number of times") {
    Documents documents(3);
    const auto expected = serialize_scene(*load_scene(documents.scene, documents.resolve, documents.codecs),
                                          documents.name, documents.codecs);
    const auto staged = elsewhere([&] { return stage_scene(documents.scene, documents.resolve); }).get();
    const auto first = load_scene(staged, documents.codecs), second = load_scene(staged, documents.codecs);
    CHECK(first != second);
    CHECK(serialize_scene(*first, documents.name, documents.codecs) == expected);
    CHECK(serialize_scene(*second, documents.name, documents.codecs) == expected);
    const auto copy = staged;
    CHECK(serialize_scene(*load_scene(copy, documents.codecs), documents.name, documents.codecs) == expected);

    SceneSet scenes;
    auto level = scenes.load("level", staged, documents.codecs);
    CHECK(serialize_scene(level.get(), documents.name, documents.codecs) == expected);
    const auto replaced = scenes.replace(level, staged, documents.codecs);
    CHECK_FALSE(level.valid());
    CHECK(serialize_scene(replaced.get(), documents.name, documents.codecs) == expected);

    SceneSet synchronous;
    synchronous.restore(documents.set, documents.resolve, documents.codecs);
    const auto expected_set = synchronous.serialize(documents.name, documents.codecs);
    CHECK(expected_set == documents.set);
    const auto staged_set = elsewhere([&] { return stage_scene_set(documents.set, documents.resolve); }).get();
    SceneSet restored;
    restored.restore(staged_set, documents.codecs);
    CHECK(restored.serialize(documents.name, documents.codecs) == expected_set);
    const auto beta = restored.find("beta");
    const auto alpha_object = restored.find("alpha")->roots().front();
    const auto new_beta = restored.replace(beta, staged_set, documents.codecs);
    CHECK_FALSE(beta.valid());
    CHECK(new_beta.valid());
    CHECK(alpha_object.valid());
    CHECK(restored.serialize(documents.name, documents.codecs) == expected_set);
    restored.restore(staged_set, documents.codecs);
    CHECK(restored.serialize(documents.name, documents.codecs) == expected_set);
}

TEST_CASE("Staging runs no codec callback, and a commit decodes each component once") {
    Documents documents(4);
    Scene source;
    populate(source, documents.meshes, 1);
    const auto prefab = Prefab::capture(source.roots().front(), documents.codecs).serialize(documents.name);
    documents.calls.encoded = 0;
    const auto staged = stage_scene(documents.scene, documents.resolve);
    const auto staged_set = stage_scene_set(documents.set, documents.resolve);
    const auto deserialized = Prefab::deserialize(prefab, documents.resolve, documents.codecs);
    CHECK(documents.calls.encoded == 0);
    CHECK(documents.calls.decoded == 0);
    CHECK(documents.calls.linked == 0);
    (void)load_scene(staged, documents.codecs);
    CHECK(documents.calls.decoded == 4);
    SceneSet scenes;
    scenes.restore(staged_set, documents.codecs);
    CHECK(documents.calls.decoded == 4 + 4 + 1);
    (void)deserialized.instantiate(scenes.find("beta").get());
    CHECK(documents.calls.decoded == 4 + 4 + 1 + 1);
    CHECK(documents.calls.encoded == 0);
    CHECK(documents.calls.linked == 0);
}

TEST_CASE("A commit applies fully or leaves the set, its members and every handle unchanged") {
    Documents documents(2);
    SceneSet scenes;
    auto expired = scenes.create("expired");
    (void)scenes.unload(expired);
    auto alpha = scenes.load("alpha", documents.scene, documents.resolve, documents.codecs);
    auto beta = scenes.load("beta", documents.scene, documents.resolve, documents.codecs);
    scenes.set_active(beta);
    std::vector<GameObject> objects;
    for (auto member : scenes.scenes())
        for (auto root : member->roots()) {
            objects.push_back(root);
            for (auto child : root.children())
                objects.push_back(child);
        }
    const auto components = scenes.components<Counted>();
    REQUIRE(objects.size() == 8);
    REQUIRE(components.size() == 4);
    const auto before = scenes.serialize(documents.name, documents.codecs);
    const auto unchanged = [&] {
        bool same = scenes.size() == 2 && alpha.valid() && beta.valid() && scenes.active().key() == "beta" &&
                    scenes.scenes().front().key() == "alpha";
        for (const auto &object : objects)
            same = same && object.valid();
        for (const auto &component : components)
            same = same && component.valid();
        return same && scenes.serialize(documents.name, documents.codecs) == before;
    };
    const auto staged = stage_scene(documents.scene, documents.resolve);
    const auto staged_set = stage_scene_set(documents.set, documents.resolve);
    const auto failing = stage_scene(substitute(documents.scene, "payload-1", "fail"), documents.resolve);
    const auto failing_set = stage_scene_set(substitute(documents.set, "payload-1", "fail"), documents.resolve);
    const ComponentCodecs without_types;

    // A decoder rejects a payload after every object exists.
    CHECK_THROWS_WITH_AS((void)scenes.load("gamma", failing, documents.codecs), rejected_payload.data(),
                         std::invalid_argument);
    CHECK(unchanged());
    CHECK_THROWS_WITH_AS((void)scenes.replace(beta, failing, documents.codecs), rejected_payload.data(),
                         std::invalid_argument);
    CHECK(unchanged());
    CHECK_THROWS_WITH_AS((void)scenes.replace(alpha, failing_set, documents.codecs), rejected_payload.data(),
                         std::invalid_argument);
    CHECK(unchanged());
    CHECK_THROWS_WITH_AS(scenes.restore(failing_set, documents.codecs), rejected_payload.data(), std::invalid_argument);
    CHECK(unchanged());
    // An expired target.
    CHECK_THROWS_WITH_AS((void)scenes.replace(expired, staged, documents.codecs), "Expired scene handle",
                         std::out_of_range);
    CHECK_THROWS_WITH_AS((void)scenes.replace(expired, staged_set, documents.codecs), "Expired scene handle",
                         std::out_of_range);
    CHECK(unchanged());
    // A duplicate namespace.
    CHECK_THROWS_WITH_AS((void)scenes.load("alpha", staged, documents.codecs), "Duplicate scene namespace",
                         std::invalid_argument);
    CHECK(unchanged());
    // A component type missing from the codecs, which is checked before any object is created.
    const auto decoded = documents.calls.decoded.load();
    CHECK_THROWS_WITH_AS((void)scenes.load("gamma", staged, without_types), "Unknown serialized component type",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS((void)scenes.replace(beta, staged, without_types), "Unknown serialized component type",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS((void)scenes.replace(beta, staged_set, without_types), "Unknown serialized component type",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(scenes.restore(staged_set, without_types), "Unknown serialized component type",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS((void)load_scene(staged, without_types), "Unknown serialized component type",
                         std::invalid_argument);
    CHECK(documents.calls.decoded == decoded);
    CHECK(unchanged());
    // A set without the target's namespace.
    SceneSet other;
    auto gamma = other.create("gamma");
    CHECK_THROWS_WITH_AS((void)other.replace(gamma, staged_set, documents.codecs),
                         "Replaced scene namespace is missing", std::invalid_argument);
    CHECK(gamma.valid());
    // A successful commit publishes the staged content.
    const auto loaded = scenes.load("gamma", staged, documents.codecs);
    CHECK(scenes.size() == 3);
    CHECK(serialize_scene(loaded.get(), documents.name, documents.codecs) ==
          serialize_scene(alpha.get(), documents.name, documents.codecs));
}

TEST_CASE("Cancelling before, during or after staging") {
    Documents documents(3);
    const auto bytes = glb(3, 2);
    const TemporaryFile file(bytes);
    std::atomic<int> resolved{0};
    const MeshResolver counting = [&](std::string_view key) {
        ++resolved;
        return documents.meshes.find(key);
    };
    static_assert(!std::is_base_of_v<std::invalid_argument, StagingCancelled>);
    static_assert(!std::is_base_of_v<std::runtime_error, StagingCancelled>);
    static_assert(!std::is_base_of_v<std::logic_error, StagingCancelled>);
    static_assert(std::is_base_of_v<std::exception, StagingCancelled>);

    // Before: a stopped token ends the call before its first step, even for content it would reject.
    StopSource before;
    before.request_stop();
    StagingProgress progress;
    const StagingOptions stopped{before.get_token(), &progress};
    CHECK_THROWS_WITH_AS((void)stage_scene(documents.scene, counting, {}, stopped), cancelled, StagingCancelled);
    CHECK_THROWS_WITH_AS((void)stage_scene("not a document", counting, {}, stopped), cancelled, StagingCancelled);
    CHECK_THROWS_WITH_AS((void)stage_scene_set(documents.set, counting, {}, stopped), cancelled, StagingCancelled);
    CHECK_THROWS_WITH_AS((void)load_asset(bytes, stopped), cancelled, StagingCancelled);
    CHECK_THROWS_WITH_AS((void)load_asset(file.path, stopped), cancelled, StagingCancelled);
    CHECK_THROWS_WITH_AS((void)load_motion_asset(bytes, stopped), cancelled, StagingCancelled);
    CHECK_THROWS_WITH_AS((void)load_motion_asset(file.path, stopped), cancelled, StagingCancelled);
    CHECK_THROWS_WITH_AS((void)elsewhere([&] { return stage_scene(documents.scene, counting, {}, stopped); }).get(),
                         cancelled, StagingCancelled);
    CHECK_THROWS_WITH_AS((void)elsewhere([&] { return load_asset(bytes, stopped); }).get(), cancelled,
                         StagingCancelled);
    CHECK(resolved == 0);
    // Each call counted its first step, the read or the parse, and completed none.
    CHECK(progress.completed() == 0);
    CHECK(progress.total() == 9);
    // Handlers for rejected content do not catch it.
    const auto caught = [&] {
        try {
            (void)stage_scene(documents.scene, counting, {}, stopped);
        } catch (const std::invalid_argument &) {
            return "invalid_argument";
        } catch (const std::runtime_error &) {
            return "runtime_error";
        } catch (const StagingCancelled &) {
            return "cancelled";
        }
        return "none";
    };
    CHECK(std::string_view(caught()) == "cancelled");

    // During: the first of two mesh keys stops the call, which ends before resolving the second.
    for (const auto *document : {&documents.scene, &documents.set}) {
        CAPTURE(*document);
        StopSource during;
        resolved = 0;
        const MeshResolver stopping = [&](std::string_view key) {
            ++resolved;
            during.request_stop();
            return documents.meshes.find(key);
        };
        const StagingOptions options{during.get_token(), nullptr};
        if (document == &documents.scene)
            CHECK_THROWS_WITH_AS((void)elsewhere([&] { return stage_scene(*document, stopping, {}, options); }).get(),
                                 cancelled, StagingCancelled);
        else
            CHECK_THROWS_WITH_AS(
                (void)elsewhere([&] { return stage_scene_set(*document, stopping, {}, options); }).get(), cancelled,
                StagingCancelled);
        CHECK(resolved == 1);
    }

    // After: a stop requested once the call returned changes nothing.
    StopSource after;
    const auto staged = stage_scene(documents.scene, documents.resolve, {}, {after.get_token(), nullptr});
    const auto asset = load_asset(bytes, {after.get_token(), nullptr});
    after.request_stop();
    CHECK(load_scene(staged, documents.codecs)->size() == 6);
    CHECK(asset->primitives.size() == 3);
    CHECK_THROWS_WITH_AS((void)stage_scene(documents.scene, documents.resolve, {}, {after.get_token(), nullptr}),
                         cancelled, StagingCancelled);
}

TEST_CASE("Presentation loaders pass their staging options to the imports they run") {
    const presentation_test::Workspace workspace;
    const auto fixture = presentation_test::actor_fixture(workspace.directory, 2);
    const auto profile = fixture.directory / "actor.profile.json";
    StopSource source;
    source.request_stop();
    const StagingOptions stopped{source.get_token(), nullptr};
    CHECK_THROWS_WITH_AS(ActorPresentation(profile, {}, TexelRetention::keep, stopped), cancelled, StagingCancelled);
    CHECK_THROWS_WITH_AS(AssetPreview(fixture.directory / "actor.asset.json", TexelRetention::keep, stopped), cancelled,
                         StagingCancelled);

    // An actor imports its model and then its motion, and counts both imports.
    StagingProgress body, motion;
    (void)load_asset(fixture.directory / "actor.glb", {{}, &body});
    (void)load_motion_asset(fixture.directory / "motion.glb", {{}, &motion});
    StagingProgress progress;
    const ActorPresentation actor(profile, {}, TexelRetention::until_upload, {{}, &progress});
    CHECK(progress.total() == body.total() + motion.total());
    CHECK(progress.completed() == progress.total());
    CHECK(actor.render->texel_retention() == TexelRetention::until_upload);
    CHECK_THROWS_WITH_AS((void)MotionRuntime::load(actor.actor.asset, actor.manifest, stopped), cancelled,
                         StagingCancelled);
    CHECK_THROWS_WITH_AS(MotionRuntime(actor.actor.asset, actor.manifest, fixture.contract, stopped), cancelled,
                         StagingCancelled);

    // A cancelled library load caches nothing, so the next load imports the file.
    const AttachmentLibrary attachments(decode_attachment_catalog(fixture.catalog, fixture.directory));
    const auto sockets = decode_attachment_sockets(fixture.sockets, actor.manifest, *actor.actor.asset);
    CHECK_THROWS_WITH_AS((void)attachments.load("instrument", stopped), cancelled, StagingCancelled);
    CHECK_THROWS_WITH_AS((void)AttachmentSet::prepare(attachments, sockets, {{"tool", "probe"}}, stopped), cancelled,
                         StagingCancelled);
    CHECK(attachments.resident_meshes().empty());
    StagingProgress instrument, prepared;
    (void)load_asset(fixture.directory / "instrument.glb", {{}, &instrument});
    const auto held = AttachmentSet::prepare(attachments, sockets, {{"tool", "probe"}}, {{}, &prepared});
    CHECK(prepared.total() == instrument.total());
    CHECK(prepared.completed() == prepared.total());
    // A load that returns the live model adds no steps, and its stopped token does not cancel it.
    CHECK(attachments.load("instrument", {source.get_token(), &prepared}) == held.roles.at("tool").asset);
    CHECK(prepared.total() == instrument.total());

    const auto fits = R"({"version":2,"items":[{"id":"shell","fits":{"consumer.profile":{"model":"actor.glb",)"
                      R"("skeleton":"consumer.rig","bind_signature":")" +
                      actor.manifest.bind_signature + R"("}}}]})";
    const FittedLibrary fitted(actor.actor.asset, actor.manifest, "consumer.profile", fits);
    CHECK_THROWS_WITH_AS((void)fitted.load("shell", stopped), cancelled, StagingCancelled);
    CHECK(fitted.resident_meshes().empty());
    StagingProgress shell;
    (void)fitted.load("shell", {{}, &shell});
    CHECK(shell.total() == body.total());
    CHECK(shell.completed() == shell.total());
}

TEST_CASE("A load that waits for an import that another caller cancels imports the file itself") {
    // Enough primitives that the first import is still running when the second load starts waiting for it.
    constexpr std::size_t primitives = 20'000;
    const TemporaryFile file(glb(primitives, 0));
    AttachmentCatalog catalog;
    catalog.directory = file.path.parent_path();
    AttachmentVisual visual;
    visual.id = "large";
    visual.model = file.path.filename();
    catalog.visuals.emplace(visual.id, visual);
    const auto running = [](const auto &future) {
        return future.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
    };
    // Each round needs the first import to be stopped before it ends. Nothing in the public API can hold
    // it, so a preempted test thread lets it finish first: an optimized build under load misses about
    // three rounds in four. A hundred rounds make missing every one negligible, and each round that
    // catches the import checks the cancelled path exactly.
    bool cancelled_first = false;
    for (int round = 0; round < 100 && !cancelled_first; ++round) {
        CAPTURE(round);
        // A new library has nothing loaded, so the first load imports the file.
        const AttachmentLibrary library(catalog);
        StopSource stop;
        StagingProgress first_progress, second_progress;
        auto first = elsewhere([&] { return library.load("large", {stop.get_token(), &first_progress}); });
        // Once the first import is under way, the second load finds it in progress and waits for it.
        while (first_progress.completed() < 2 && running(first))
            std::this_thread::yield();
        auto second = elsewhere([&] { return library.load("large", {{}, &second_progress}); });
        // The first import stops halfway through its primitives, which leaves the second load time to start waiting.
        while (first_progress.completed() < 2 + primitives / 2 && running(first))
            std::this_thread::yield();
        stop.request_stop();
        // The second load's own token never stops, so it imports the file once the shared import is cancelled.
        const auto loaded = second.get();
        CHECK(loaded->source->primitives.size() == primitives);
        std::shared_ptr<const AttachmentAsset> shared;
        try {
            shared = first.get();
        } catch (const StagingCancelled &) {
            cancelled_first = true;
        }
        if (cancelled_first) {
            // The second load ran its own import: its read, its parse and every primitive.
            CHECK(second_progress.total() == 2 + primitives);
            CHECK(second_progress.completed() == second_progress.total());
            CHECK(library.resident_meshes() == std::vector{loaded->render});
        } else {
            CHECK(shared == loaded);
        }
    }
    CHECK(cancelled_first);
}

TEST_CASE("Progress never decreases and ends at the input's images, primitives, objects and mesh keys") {
    // A GLB of three primitives and two images: the parse, two images and three primitives.
    const auto bytes = glb(3, 2);
    StagingProgress progress;
    const auto asset = load_asset(bytes, {{}, &progress});
    REQUIRE(asset->primitives.size() == 3);
    CHECK(progress.total() == 1 + 2 + 3);
    CHECK(progress.completed() == progress.total());
    // A file adds its read, and calls that share the object add to its counters.
    const TemporaryFile file(bytes);
    (void)load_asset(file.path, {{}, &progress});
    CHECK(progress.total() == 6 + 1 + 6);
    CHECK(progress.completed() == progress.total());
    // Without images, the parse and one step per primitive.
    StagingProgress plain;
    (void)load_asset(glb(4, 0), {{}, &plain});
    CHECK(plain.total() == 1 + 4);
    CHECK(plain.completed() == plain.total());

    // Three roots and their children, which name two distinct meshes: the parse, six objects and two keys.
    Documents documents(3);
    StagingProgress scene;
    (void)stage_scene(documents.scene, documents.resolve, {}, {{}, &scene});
    CHECK(scene.total() == 1 + 6 + 2);
    CHECK(scene.completed() == scene.total());
    // Members of six and two objects share their two mesh keys.
    StagingProgress set;
    (void)stage_scene_set(documents.set, documents.resolve, {}, {{}, &set});
    CHECK(set.total() == 1 + 8 + 2);
    CHECK(set.completed() == set.total());
}

TEST_CASE("Another thread reads progress while staging runs") {
    Documents documents(64);
    std::promise<void> entered, released;
    const auto release = released.get_future().share();
    std::atomic<bool> first{true};
    const MeshResolver blocking = [&](std::string_view key) {
        if (first.exchange(false)) {
            entered.set_value();
            release.wait();
        }
        return documents.meshes.find(key);
    };
    StagingProgress progress;
    auto staging = elsewhere([&] { return stage_scene(documents.scene, blocking, {}, {{}, &progress}); });
    entered.get_future().wait();
    // The staging thread waits in its first resolver call, made for the first object: the parse is complete, and
    // the total counts all 128 objects and both mesh keys.
    CHECK(progress.completed() == 1);
    CHECK(progress.total() == 1 + 128 + 2);
    released.set_value();
    std::uint64_t completed = 0, total = 0;
    bool ordered = true, increasing = true;
    while (staging.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        const auto now_completed = progress.completed();
        const auto now_total = progress.total();
        ordered = ordered && now_completed <= now_total;
        increasing = increasing && now_completed >= completed && now_total >= total;
        completed = now_completed;
        total = now_total;
    }
    (void)staging.get();
    CHECK(ordered);
    CHECK(increasing);
    CHECK(progress.completed() == 1 + 128 + 2);
    CHECK(progress.total() == 1 + 128 + 2);
}

TEST_CASE("Each staged rejection crosses threads with the synchronous type and message") {
    Documents documents(1);
    // The Counted component's type, the last of its sorted fields, which a second component of that type can follow.
    const auto counted_type = "\"type\": \"" + std::string(counted_key) + '"';
    const std::pair<std::string, const char *> scenes[]{
        {"[]", "Invalid scene document"},
        {substitute(documents.scene, "\"version\": 3", "\"version\": 2"), "Unsupported scene document version"},
        {substitute(documents.scene, "\"anima.scene\"", "\"anima.prefab\""), "Invalid scene document kind"},
        {substitute(documents.scene, "{", "{\"version\": 3,"), "Duplicate JSON document field"},
        {substitute(documents.scene, "\"next_key\": \"", "\"next_key\": \"0"), "Invalid object key"},
        {substitute(documents.scene, "\"name\": \"root-0\"", "\"name\": 0"),
         "[json.exception.type_error.302] type must be string, but is number"},
        {substitute(documents.scene, "\"one\"", "\"missing\""), "Scene mesh key could not be resolved"},
        {substitute(documents.scene, counted_type,
                    counted_type + "}, {\"enabled\": true, \"state\": \"\", " + counted_type),
         "Duplicate serialized component"},
        {std::string(maximum_document_bytes, ' ') + documents.scene, "JSON document exceeds byte limit"},
    };
    for (const auto &[text, expected] : scenes) {
        const auto &document = text;
        const auto *message = expected;
        CAPTURE(message);
        CHECK_THROWS_WITH_AS((void)load_scene(document, documents.resolve, documents.codecs), message,
                             std::invalid_argument);
        CHECK_THROWS_WITH_AS((void)elsewhere([&] { return stage_scene(document, documents.resolve); }).get(), message,
                             std::invalid_argument);
    }
    const std::pair<std::string, const char *> sets[]{
        {substitute(documents.set, "\"anima.scene-set\"", "\"anima.scene\""), "Invalid scene set document kind"},
        {substitute(documents.set, "\"active\": \"alpha\"", "\"active\": null"),
         "Scene set requires an active namespace"},
        {substitute(documents.set, "\"key\": \"beta\"", "\"key\": \"alpha\""), "Duplicate scene namespace"},
        {substitute(documents.set, "\"one\"", "\"missing\""), "Scene mesh key could not be resolved"},
    };
    for (const auto &[text, expected] : sets) {
        const auto &document = text;
        const auto *message = expected;
        CAPTURE(message);
        SceneSet destination;
        CHECK_THROWS_WITH_AS(destination.restore(document, documents.resolve, documents.codecs), message,
                             std::invalid_argument);
        CHECK_THROWS_WITH_AS((void)elsewhere([&] { return stage_scene_set(document, documents.resolve); }).get(),
                             message, std::invalid_argument);
    }
    // An exception of the resolver keeps its type.
    const MeshResolver failing = [](std::string_view) -> std::shared_ptr<const Mesh> {
        throw std::runtime_error("Resolver failed");
    };
    CHECK_THROWS_WITH_AS((void)load_scene(documents.scene, failing), "Resolver failed", std::runtime_error);
    CHECK_THROWS_WITH_AS((void)elsewhere([&] { return stage_scene(documents.scene, failing); }).get(),
                         "Resolver failed", std::runtime_error);
    // GLB rejections: malformed content, and a rotation that cannot be normalized.
    const std::vector<std::byte> empty;
    CHECK_THROWS_WITH_AS((void)load_asset(empty), "GLB must be between 1 byte and 64 MiB", std::runtime_error);
    CHECK_THROWS_WITH_AS((void)elsewhere([&] { return load_asset(empty); }).get(),
                         "GLB must be between 1 byte and 64 MiB", std::runtime_error);
    const auto unnormalized = glb(1, 0, "0,0,0,0");
    const auto zero = math_error_message(MathErrorCode::zero_quaternion);
    CHECK_THROWS_WITH_AS((void)load_asset(unnormalized), zero, MathError);
    CHECK_THROWS_WITH_AS((void)elsewhere([&] { return load_asset(unnormalized); }).get(), zero, MathError);
}

TEST_CASE("Retained bytes count the decoded objects, payloads and tables") {
    const Meshes meshes = compiled_meshes();
    const MeshResolver resolve = [&](std::string_view key) { return meshes.find(key); };
    constexpr std::string_view identity = "[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]";
    const auto object = [&](std::string_view key, std::string_view name, std::string_view parent,
                            std::string_view rest) {
        return R"({"key":")" + std::string(key) + R"(","name":")" + std::string(name) + R"(","parent":)" +
               std::string(parent) + R"(,"local":)" + std::string(identity) + std::string(rest) + '}';
    };
    // One object with a pose, a material factor, a visibility flag and a component; one without a mesh; one with
    // two visibility flags.
    const auto objects = object("1", "a", "null",
                                R"(,"mesh":"one","pose":[)" + std::string(identity) +
                                    R"(],"material_factors":[[1,1,1]],"primitive_visible":[true],"components":)"
                                    R"([{"type":"test.counted.v1","state":"0:xyz","enabled":true}])") +
                         ',' + object("2", "bb", "0", R"(,"mesh":null)") + ',' +
                         object("3", "", "null", R"(,"mesh":"two","primitive_visible":[true,false])");
    const auto scene_bytes = 3 * sizeof(Prefab::Node) + (1 + 2 + 0) + sizeof(Mat4) + sizeof(Vec3) + 1 + 1 +
                             sizeof(ComponentData) + counted_key.size() + std::string_view("0:xyz").size();
    const auto document = R"({"version":3,"kind":"anima.scene","next_key":"4","objects":[)" + objects + "]}";
    const auto staged = stage_scene(document, resolve);
    CHECK(staged.retained_bytes() == scene_bytes);
    const auto copy = staged;
    CHECK(copy.retained_bytes() == scene_bytes);
    CHECK(stage_scene(R"({"version":3,"kind":"anima.scene","next_key":"1","objects":[]})", resolve).retained_bytes() ==
          0);

    const auto set = R"({"version":1,"kind":"anima.scene-set","active":"alpha","scenes":[{"key":"alpha",)"
                     R"("next_key":"4","objects":[)" +
                     objects + R"(]},{"key":"b","next_key":"2","objects":[)" +
                     object("1", "n", "null", R"(,"mesh":null)") +
                     R"(]}],"references":[{"key":"1","scene":"alpha","object":"1"},)"
                     R"({"key":"2","scene":"alpha","object":"2"},{"key":"3","scene":"alpha","object":"3"},)"
                     R"({"key":"4","scene":"b","object":"1"}]})";
    const auto staged_set = stage_scene_set(set, resolve);
    const auto row = 2 * sizeof(ObjectKey) + sizeof(std::size_t);
    CHECK(staged_set.retained_bytes() ==
          std::string_view("alpha").size() + scene_bytes + 1 + sizeof(Prefab::Node) + 1 + 4 * row);
    // Both documents are valid and commit.
    CodecCalls calls;
    const auto codecs = counting_codecs(calls);
    CHECK(load_scene(staged, codecs)->size() == 3);
    SceneSet scenes;
    scenes.restore(staged_set, codecs);
    CHECK(scenes.size() == 2);
    CHECK(calls.decoded == 2);
}

// Tries every staged commit on @p scenes, which is busy with the activity that @p message names.
void reject_busy_commits(SceneSet &scenes, const StagedScene &staged, const StagedSceneSet &staged_set,
                         const char *message) {
    CHECK_THROWS_WITH_AS((void)scenes.load("busy", staged), message, std::logic_error);
    CHECK_THROWS_WITH_AS((void)scenes.replace(scenes.scenes().front(), staged), message, std::logic_error);
    CHECK_THROWS_WITH_AS((void)scenes.replace(scenes.scenes().front(), staged_set), message, std::logic_error);
    CHECK_THROWS_WITH_AS(scenes.restore(staged_set), message, std::logic_error);
}
// Commits from its update hook.
struct UpdatingCommitter {
    SceneSet *scenes;
    const StagedScene *staged;
    const StagedSceneSet *staged_set;
    int *updates;
    void on_update(double) {
        ++*updates;
        reject_busy_commits(*scenes, *staged, *staged_set, "Scene set is updating");
    }
};
// A component whose decoder commits while its own commit changes the set's membership.
struct DecodingCommitter {};

TEST_CASE("Staged commits refuse a busy set with the message of its activity") {
    const auto document = R"({"version":3,"kind":"anima.scene","next_key":"2","objects":[{"key":"1","name":"a",)"
                          R"("parent":null,"local":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"mesh":null}]})";
    const auto set_document = R"({"version":1,"kind":"anima.scene-set","active":"level","scenes":[{"key":"level",)"
                              R"("next_key":"1","objects":[]}],"references":[]})";
    const auto staged = stage_scene(document, {});
    const auto staged_set = stage_scene_set(set_document, {});
    SceneSet scenes;
    int updates = 0;
    scenes.create("level")
        ->create("committer")
        .add_component<UpdatingCommitter>(&scenes, &staged, &staged_set, &updates);
    scenes.update(0);
    CHECK(updates == 1);

    int decoded = 0;
    ComponentCodecs codecs;
    codecs.add<DecodingCommitter>(
        "test.decoding-committer.v1", [](const DecodingCommitter &, const ObjectReferences &) { return ""; },
        [&](GameObject object, std::string_view, const ObjectReferences &) {
            ++decoded;
            reject_busy_commits(scenes, staged, staged_set, "Scene set is changing membership");
            object.add_component<DecodingCommitter>();
        });
    Scene source;
    source.create("decoder").add_component<DecodingCommitter>();
    const auto with_decoder = stage_scene(serialize_scene(source, {}, codecs), {});
    (void)scenes.load("decoding", with_decoder, codecs);
    CHECK(decoded == 1);
    CHECK(scenes.size() == 2);
}
} // namespace
