#pragma once
#include <anima/audio_scene.hpp>
#include <anima/scene_set.hpp>
#include <stdexcept>

namespace scene_set_test {
using namespace anima;
inline void check(bool value, const char *reason) {
    if (!value)
        throw std::runtime_error(reason);
}
template <class F> void rejects(F operation) {
    try {
        operation();
    } catch (const std::exception &) {
        return;
    }
    throw std::runtime_error("Invalid scene set operation accepted");
}
struct Counts {
    int enabled{}, disabled{}, destroyed{}, invalid{}, updates{}, rejected{};
};
struct Probe {
    GameObject owner, other;
    Counts *counts;
    SceneSet *set;
    Probe(GameObject object, Counts &values, SceneSet &collection, GameObject peer = {})
        : owner(object), other(peer), counts(&values), set(&collection) {}
    ~Probe() { ++counts->destroyed; }
    void on_enable() noexcept { ++counts->enabled; }
    void on_disable() noexcept {
        ++counts->disabled;
        if (!owner.valid() && !other.valid())
            ++counts->invalid;
        try {
            set->clear();
        } catch (const std::logic_error &) {
            ++counts->rejected;
        }
    }
    void on_update(double) {
        ++counts->updates;
        rejects([&] { set->clear(); });
        rejects([&] { (void)set->create("during-update"); });
        rejects([&] { set->set_active(set->active()); });
    }
};
struct Construct {
    Construct(GameObject object, SceneSet &set) {
        rejects([&] { set.unload(set.active()); });
        rejects([&] { set.active()->synchronize_lifecycle(); });
        object.set_name("constructed");
    }
};
struct PhaseCounts {
    int frame{}, late{}, added{}, fixed{}, cleanup_rejected{};
};
struct Added {
    PhaseCounts *counts;
    void on_update(double) { ++counts->added; }
};
struct PhaseProbe {
    SceneSet *set;
    SceneRef peer;
    GameObject destination, dormant;
    PhaseCounts *counts;
    bool add;
    void on_update(double) {
        ++counts->frame;
        rejects([&] { set->update(0); });
        rejects([&] { peer->update(0); });
        rejects([&] { set->unload(peer); });
        if (add) {
            destination.add_component<Added>(counts);
            dormant.set_active(true);
            add = false;
        }
    }
    void on_late_update(double) {
        check(counts->frame == 2, "Late phase ran before every scene's frame phase");
        ++counts->late;
    }
    void on_fixed_update(double) { ++counts->fixed; }
};
struct PinnedCleanup {
    GameObject owner;
    SceneSet *set;
    PhaseCounts *counts;
    PinnedCleanup(GameObject object, SceneSet &scenes, PhaseCounts &values)
        : owner(object), set(&scenes), counts(&values) {}
    void on_update(double) { owner.remove_component<PinnedCleanup>(); }
    ~PinnedCleanup() {
        try {
            set->clear();
        } catch (const std::logic_error &) {
            ++counts->cleanup_rejected;
        }
    }
};
struct SetLink {
    GameObject target;
};
struct PersistenceCounts {
    int live{}, enabled{}, destroyed{};
    bool check_invalidation{}, all_invalid = true;
    std::vector<GameObject> decoded;
};
struct SetOwned {
    PersistenceCounts *counts;
    bool accepted = true;
    explicit SetOwned(PersistenceCounts &values) : counts(&values) { ++counts->live; }
    ~SetOwned() {
        --counts->live;
        ++counts->destroyed;
        if (counts->check_invalidation)
            for (const auto &object : counts->decoded)
                counts->all_invalid = counts->all_invalid && !object.valid();
    }
    void on_enable() noexcept { ++counts->enabled; }
};
inline ComponentCodecs persistence_codecs(PersistenceCounts &counts, SceneSet *destination = nullptr) {
    ComponentCodecs codecs;
    codecs.add<SetLink>(
        "test.set-link.v1",
        [](const SetLink &link, const ObjectReferences &references) { return references.key(link.target).string(); },
        [](GameObject object, std::string_view state, const ObjectReferences &references) {
            object.add_component<SetLink>(references.resolve(ObjectKey::parse(state)));
        });
    codecs.add<SetOwned>(
        "test.set-owned.v1",
        [](const SetOwned &value, const ObjectReferences &) { return value.accepted ? "ok" : "fail"; },
        [&counts, destination](GameObject object, std::string_view state, const ObjectReferences &) {
            if (destination) {
                rejects([&] { destination->clear(); });
                rejects([&] { destination->update(0); });
                rejects([&] { (void)destination->serialize({}); });
            }
            object.add_component<SetOwned>(counts);
            counts.decoded.push_back(object);
            if (state != "ok")
                throw std::invalid_argument("Rejected scene set fixture payload");
        });
    return codecs;
}
inline std::shared_ptr<const Mesh> persistence_mesh() {
    Asset asset;
    asset.nodes.resize(1);
    SourcePrimitive primitive;
    for (const auto position : {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}}) {
        SourceVertex vertex;
        vertex.position = position;
        vertex.normal = {0, 0, 1};
        primitive.vertices.push_back(vertex);
    }
    asset.primitives.push_back(std::move(primitive));
    return Mesh::compile(asset);
}
inline void persistence() {
    PersistenceCounts authored_counts, restored_counts;
    Counts retired_counts;
    SceneSet authored, destination;
    const auto mesh = persistence_mesh();
    auto alpha = authored.create("alpha"), beta = authored.create("beta"), empty = authored.create("empty");
    auto a = alpha->create("first", mesh), b = beta->create("second", mesh);
    auto self = alpha->create("self"), null = alpha->create("null");
    self.set_parent(a, ReparentMode::keep_local);
    self.set_local_position({2, 0, 0});
    a.add_component<SetLink>(b);
    b.add_component<SetLink>(a);
    self.add_component<SetLink>(self).set_enabled(false);
    null.add_component<SetLink>(GameObject{});
    a.add_component<SetOwned>(authored_counts);
    auto bad_payload = b.add_component<SetOwned>(authored_counts);
    b.set_active(false);
    authored.set_active(beta);
    auto deleted = alpha->create(), empty_deleted = empty->create();
    const auto deleted_key = deleted.key(), empty_deleted_key = empty_deleted.key();
    deleted.destroy();
    empty_deleted.destroy();
    check(a.key() == b.key(), "Persistence fixture must exercise duplicate scene-local keys");
    auto codecs = persistence_codecs(authored_counts);
    int named = 0, resolved = 0;
    const MeshName name = [&](const auto &resource) {
        check(resource == mesh, "Set capture changed shared mesh identity");
        ++named;
        return "shared-mesh";
    };
    const MeshResolver resolve = [&](std::string_view key) {
        check(key == "shared-mesh", "Set restore changed mesh key");
        ++resolved;
        return mesh;
    };
    const auto document = authored.serialize(
        [&](const auto &resource) {
            rejects([&] { authored.clear(); });
            rejects([&] { alpha->update(0); });
            return name(resource);
        },
        codecs);
    check(named == 1, "Set capture named one shared resource more than once");
    rejects([&] { (void)serialize_scene(alpha.get(), name, codecs); });
    bad_payload->accepted = false;
    const auto failed_document = authored.serialize(name, codecs);
    bad_payload->accepted = true;

    auto old_alpha = destination.create("alpha"), old_beta = destination.create("beta");
    auto old_a = old_alpha->create(), old_b = old_beta->create();
    old_a.add_component<Probe>(retired_counts, destination, old_b);
    old_b.add_component<Probe>(retired_counts, destination, old_a);
    destination.set_active(old_beta);
    destination.synchronize_lifecycle();
    const auto old_address = destination.address(old_a);
    const auto old_view = old_alpha.render_scene();
    auto destination_codecs = persistence_codecs(restored_counts, &destination);
    rejects([&] {
        destination.restore(document, [](auto) -> std::shared_ptr<const Mesh> { return {}; }, destination_codecs);
    });
    rejects([&] { destination.restore(document, resolve); });
    restored_counts.check_invalidation = true;
    rejects([&] { destination.restore(failed_document, resolve, destination_codecs); });
    check(old_alpha && old_beta && old_a.valid() && old_b.valid() && destination.active().key() == "beta" &&
              destination.find(old_address).id() == old_a.id() && old_view->size() == 1 &&
              retired_counts.disabled == 0 && restored_counts.live == 0 && restored_counts.destroyed == 2 &&
              restored_counts.enabled == 0 && restored_counts.all_invalid,
          "Failed set restore changed published scenes or failed to retire staged resources together");
    restored_counts.check_invalidation = false;
    resolved = 0;
    destination.restore(document, resolve, destination_codecs);
    check(resolved == 1 && !old_alpha && !old_beta && !old_a.valid() && !old_b.valid() && old_view->size() == 0 &&
              retired_counts.disabled == 2 && retired_counts.destroyed == 2 && retired_counts.invalid == 2 &&
              retired_counts.rejected == 2,
          "Set restore did not atomically retire old scene identities or share resource resolution");
    const auto selection = destination.scenes();
    check(selection.size() == 3 && selection[0].key() == "alpha" && selection[1].key() == "beta" &&
              selection[2].key() == "empty" && destination.active().key() == "beta",
          "Set persistence lost scene enumeration, empty scenes or active selection");
    auto restored_a = destination.find(SceneAddress{"alpha", a.key()});
    auto restored_b = destination.find(SceneAddress{"beta", b.key()});
    auto restored_self = destination.find(SceneAddress{"alpha", self.key()});
    auto restored_null = destination.find(SceneAddress{"alpha", null.key()});
    check(restored_a.get_component<SetLink>()->target.id() == restored_b.id() &&
              restored_b.get_component<SetLink>()->target.id() == restored_a.id() &&
              restored_self.get_component<SetLink>()->target.id() == restored_self.id() &&
              !restored_null.get_component<SetLink>()->target.valid() &&
              restored_self.parent()->id() == restored_a.id() && restored_self.local_position().x == 2 &&
              !restored_self.get_component<SetLink>().enabled() && !restored_b.active_self() &&
              restored_a.renderer().mesh() == mesh && restored_b.renderer().mesh() == mesh,
          "Set persistence lost linked identities, hierarchy, activation or shared resources");
    check(destination.find(old_address).id() == restored_a.id() && restored_a.id() != old_a.id() &&
              restored_counts.live == 2 && restored_counts.enabled == 0 &&
              destination.serialize(name, destination_codecs) == document,
          "Set restore rebound old handles, ran lifecycle hooks or changed authored state");
    destination.synchronize_lifecycle();
    check(restored_counts.enabled == 1, "Restored set did not defer enabled lifecycle to synchronization");
    restored_b.set_active(true);
    destination.synchronize_lifecycle();
    check(restored_counts.enabled == 2, "Restored inactive component failed to enable later");
    check(selection[0]->create().key().value > deleted_key.value &&
              selection[2]->create().key().value > empty_deleted_key.value,
          "Set snapshot recycled deleted keys or lost empty-scene allocator history");

    Scene outside;
    a.get_component<SetLink>()->target = outside.create();
    rejects([&] { (void)authored.serialize(name, codecs); });
    a.get_component<SetLink>()->target = b;
    auto collision = empty->create("different shared key", persistence_mesh());
    rejects([&] { (void)authored.serialize([](const auto &) { return "same-key"; }, codecs); });
    collision.destroy();
    SceneSet vacant;
    const auto vacant_document = vacant.serialize({});
    restored_counts.check_invalidation = true;
    destination.restore(vacant_document, {});
    check(destination.size() == 0 && !destination.active() && !selection[0] && !restored_a.valid() &&
              restored_counts.live == 0 && restored_counts.all_invalid && destination.serialize({}) == vacant_document,
          "Empty set restore did not clear selection, retire resources or round-trip null activity");
}
inline void phases() {
    PhaseCounts counts;
    SceneSet set;
    auto a = set.create("a"), b = set.create("b");
    auto first = a->create(), second = b->create(), dormant = b->create();
    dormant.add_component<Added>(&counts);
    dormant.set_active(false);
    first.add_component<PhaseProbe>(&set, b, second, dormant, &counts, true);
    second.add_component<PhaseProbe>(&set, a, first, dormant, &counts, false);
    first.add_component<PinnedCleanup>(set, counts);
    set.update(0);
    check(counts.frame == 2 && counts.late == 2 && counts.added == 0 && counts.cleanup_rejected == 1,
          "Cross-scene snapshot or pinned retirement boundary failed");
    counts.frame = counts.late = 0;
    set.update(0);
    check(counts.added == 2 && counts.late == 2, "Deferred cross-scene additions did not participate");
    set.fixed_update(0);
    check(counts.fixed == 2, "Set fixed phases did not tick each component once");
    struct Throwing {
        void on_update(double) { throw std::runtime_error("callback"); }
    };
    auto throwing = first.add_component<Throwing>();
    counts.frame = counts.late = 0;
    rejects([&] { set.update(0); });
    first.remove_component<Throwing>();
    counts.frame = counts.late = 0;
    set.update(0);
    check(!throwing && counts.late == 2, "Exception left scene scheduling locked");
    set.clear();
    set.update(0);
    set.fixed_update(0);
    set.synchronize_lifecycle();
}
inline void run() {
    phases();
    persistence();
    Counts counts, cleared;
    SceneSet set;
    check(set.size() == 0 && !set.active() && set.scenes().empty(), "Scene set not initially empty");
    rejects([&] { (void)set.create(""); });
    rejects([&] { (void)set.create(std::string("bad\0key", 7)); });
    rejects([&] { (void)set.create(std::string(4097, 'x')); });
    auto level = set.create("level"), overlay = set.create("overlay");
    check(set.active().key() == "level" && set.size() == 2, "Additive loading changed selection");
    auto a = level->create(), b = overlay->create();
    const auto address = set.address(a);
    check(a.key() == b.key() && set.address(b).scene == "overlay" && set.find(address).id() == a.id(),
          "Scene namespaces collided");
    a.add_component<Construct>(set);
    a.add_component<Probe>(counts, set);
    b.add_component<Probe>(counts, set);
    set.update(0);
    check(counts.updates == 2 && counts.enabled == 2, "Caller could not drive additive scenes");
    set.set_active(overlay);
    check(a.active_in_hierarchy() && b.active_in_hierarchy(), "Selection changed object activation");
    rejects([&] { (void)set.create("level"); });
    SceneSet other;
    auto foreign = other.create("level");
    rejects([&] { set.unload(foreign); });
    rejects([&] { set.set_active(foreign); });
    rejects([&] { (void)set.address(foreign->create()); });
    rejects([&] { (void)set.address({}); });
    check(!set.find(SceneAddress{"missing", a.key()}).valid() && !set.find(SceneAddress{"level", {}}).valid(),
          "Missing address unexpectedly bound");
    auto render_view = level.render_scene();
    auto old_handles = set.scenes();
    set.unload(level);
    check(!level && !a.valid() && b.valid() && overlay && !old_handles[0] && old_handles[1] &&
              render_view->size() == 0 && render_view->instances().empty() && !render_view->bounds().valid &&
              !set.find(address).valid() && counts.destroyed == 1 && counts.disabled == 1 && counts.invalid == 1 &&
              counts.rejected == 1,
          "Unload retained owned state or invalidated another scene");
    rejects([&] { (void)level.get(); });
    rejects([&] { (void)level.render_scene(); });
    rejects([&] { set.unload(level); });
    rejects([&] { (void)set.address(a); });
    auto reloaded = set.create("level");
    auto again = reloaded->create();
    check(set.find(address).id() == again.id() && !a.valid() && !level,
          "Explicit address lookup or stale runtime identity contract failed");
    set.unload(overlay);
    check(set.active().key() == "level", "Unloading selected scene did not choose first remaining scene");

    // Failed loading is unpublished; successful replacement keeps its namespace/slot.
    Scene authored;
    auto original = authored.create("new object");
    const auto document = serialize_scene(authored, {});
    rejects([&] { (void)set.replace(reloaded, "{}", {}); });
    rejects([&] { (void)set.load("broken", "{}", {}); });
    check(set.size() == 1 && again.valid() && set.active().key() == "level", "Invalid document changed live scenes");
    auto replacement = set.replace(reloaded, document, {});
    check(!reloaded && !again.valid() && replacement && replacement.key() == "level" &&
              set.active()->find(original.key()).name() == "new object",
          "Replacement commit lost selection/identity");
    auto extra = set.load("extra", document, {});
    check(set.scenes()[0].key() == "level" && set.scenes()[1].key() == "extra", "Scene enumeration order changed");

    // A decoder failure releases staged owned components; the old scene remains valid.
    auto clip = AudioClip::pcm(std::vector<float>(16, .25F), 1, 8000);
    Audio audio(8000, 1);
    ComponentCodecs codecs;
    add_audio_component_codecs(codecs, audio, [&](auto) { return "tone"; }, [&](auto) { return clip; });
    Scene sound_scene;
    struct Failure {
        bool valid{true};
    };
    codecs.add<Failure>(
        "test.failure.v1",
        [](const Failure &value, const ObjectReferences &) { return value.valid ? "{}" : "malformed"; },
        [&](GameObject object, std::string_view state, const ObjectReferences &) {
            rejects([&] { set.unload(extra); });
            object.add_component<Failure>();
            if (state != "{}")
                throw std::invalid_argument("Fixture state must be an empty object");
        });
    auto source_object = sound_scene.create();
    source_object.add_component<AudioSource>(audio, clip);
    auto failure = source_object.add_component<Failure>();
    const auto audio_document = serialize_scene(sound_scene, {}, codecs);
    failure->valid = false;
    const auto failed_document = serialize_scene(sound_scene, {}, codecs);
    source_object.destroy();
    rejects([&] { (void)set.replace(replacement, failed_document, {}, codecs); });
    check(replacement && extra && set.active().key() == "level", "Decode failure changed published membership");
    auto playing = set.load("audio", audio_document, {}, codecs);
    check(playing->components<Failure>().size() == 1 && playing->components<Failure>()[0]->valid,
          "Valid fixture state did not restore its component");
    auto retained_audio_view = playing.render_scene();
    auto source = playing->components<AudioSource>()[0];
    source->play();
    synchronize_audio(playing.get(), audio);
    check(source->playing(), "Loaded audio source failed to play");
    set.unload(playing);
    check(!source && retained_audio_view->size() == 0, "Retained scene view delayed audio teardown");
    auto voice = audio.sound(clip); // capacity was released by both failed load and unload
    voice.play();

    // Clear invalidates all scenes before the first cleanup hook, even with retained views.
    auto left = replacement->create(), right = extra->create();
    left.add_component<Probe>(cleared, set, right);
    right.add_component<Probe>(cleared, set, left);
    replacement->synchronize_lifecycle();
    extra->synchronize_lifecycle();
    auto retained = extra.render_scene();
    set.clear();
    check(!left.valid() && !right.valid() && !replacement && !extra && !set.active() && set.size() == 0 &&
              cleared.destroyed == 2 && cleared.invalid == 2 && cleared.rejected == 2 && retained->size() == 0,
          "Clear did not invalidate the entire set before resource cleanup");
    set.clear();
    check(set.create("after-clear").valid(), "Clear did not leave a reusable set");
    Counts destroyed;
    SceneRef survivor;
    std::shared_ptr<const Scene> view;
    GameObject object;
    {
        SceneSet temporary;
        survivor = temporary.create("temporary");
        view = survivor.render_scene();
        object = survivor->create();
        object.add_component<Probe>(destroyed, temporary);
        survivor->synchronize_lifecycle();
    }
    check(!survivor && !object.valid() && view->size() == 0 && destroyed.destroyed == 1 && destroyed.invalid == 1,
          "Scene set destruction leaked through retained handles/views");
}
} // namespace scene_set_test
