#pragma once
#include <anima/prefab.hpp>
#include <functional>
#include <stdexcept>

namespace lifecycle_test {
using namespace anima;
inline void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class F> void rejects(F operation) {
    try {
        operation();
    } catch (const std::exception &) {
        return;
    }
    throw std::runtime_error("Lifecycle operation should reject");
}
struct Counts {
    int enables{}, disables{}, frames{}, fixed{}, late{}, destroyed{}, invalid_disables{};
};
struct Probe {
    GameObject owner;
    Counts *counts;
    Probe(GameObject object, Counts &value) : owner(object), counts(&value) {}
    ~Probe() { ++counts->destroyed; }
    void on_enable() noexcept { ++counts->enables; }
    void on_disable() noexcept {
        ++counts->disables;
        if (!owner.valid())
            ++counts->invalid_disables;
    }
    void on_update(double) { ++counts->frames; }
    void on_fixed_update(double) { ++counts->fixed; }
    void on_late_update(double) { ++counts->late; }
};
struct Callback {
    std::function<void()> enable, disable, frame;
    void on_enable() noexcept {
        if (enable)
            enable();
    }
    void on_disable() noexcept {
        if (disable)
            disable();
    }
    void on_update(double) {
        if (frame)
            frame();
    }
};
inline void run() {
    Counts counts;
    Scene scene;
    auto parent = scene.create("group"), child = scene.create("child");
    child.set_parent(parent);
    auto probe = child.add_component<Probe>(counts);
    check(counts.enables == 0 && probe.enabled() && probe.active(), "Construction fired lifecycle hooks");
    scene.update(.1);
    scene.fixed_update(.1);
    scene.synchronize_lifecycle();
    check(counts.enables == 1 && counts.frames == 1 && counts.late == 1 && counts.fixed == 1,
          "Initial lifecycle/update ordering failed");
    parent.set_active(false);
    check(child.active_self() && !child.active_in_hierarchy() && probe.enabled() && !probe.active(),
          "Inherited activation changed local flags");
    check(scene.components<Probe>().size() == 1, "Inactive components disappeared from queries");
    scene.update(.1);
    check(counts.disables == 1 && counts.frames == 1, "Inactive object ticked or missed disable");
    child.set_active(false);
    parent.set_active(true);
    scene.update(.1);
    check(!child.active_in_hierarchy() && counts.enables == 1, "Parent overwrote child's activation");
    child.set_active(true);
    probe.set_enabled(false);
    scene.update(.1);
    check(counts.enables == 1 && child.active_in_hierarchy(), "Disabled component was enabled by object");
    probe.set_enabled(true);
    scene.synchronize_lifecycle();
    check(counts.enables == 2, "Explicit lifecycle synchronization missed enable");
    parent.set_active(false);
    scene.synchronize_lifecycle();
    child.clear_parent();
    scene.fixed_update(.1);
    check(probe.active() && counts.enables == 3 && counts.fixed == 2, "Reparent did not update activation");
    child.set_parent(parent);
    scene.synchronize_lifecycle();
    check(counts.disables == 3, "Reparent under inactive object missed disable");
    rejects([&] { parent.set_parent(child); });
    check(!child.active_in_hierarchy() && !parent.parent(), "Failed reparent changed activation");
    parent.set_active(true);
    parent.set_active(false);
    scene.synchronize_lifecycle();
    check(counts.enables == 3, "Coalesced activation triggered spurious enable");
    parent.set_active(true);
    scene.synchronize_lifecycle();
    auto old = probe;
    child.remove_component<Probe>();
    check(!old && counts.disables == 4 && counts.destroyed == 1, "Removal missed disable/retirement");
    probe = child.add_component<Probe>(counts);
    scene.update(0);
    parent.destroy();
    check(!probe && counts.disables == 5 && counts.invalid_disables == 1, "Subtree removal lifecycle invalidation");
    auto reused = scene.create();
    check(reused.active_self() && reused.active_in_hierarchy(), "Reused slot retained inactive state");

    // Mutations are safe and bounded by one snapshot, with no recursive scheduler.
    Counts spawned;
    auto first = scene.create();
    ComponentRef<Probe> added;
    int nested_rejections = 0;
    first.add_component<Callback>(Callback{[&] {
                                               try {
                                                   scene.synchronize_lifecycle();
                                               } catch (const std::logic_error &) {
                                                   ++nested_rejections;
                                               }
                                               added = scene.create().add_component<Probe>(spawned);
                                           },
                                           {},
                                           {}});
    scene.update(0);
    check(nested_rejections == 1 && added && spawned.enables == 0 && spawned.frames == 0,
          "Enable callback additions entered current snapshot");
    scene.update(0);
    check(spawned.enables == 1 && spawned.frames == 1, "Deferred addition did not activate");
    added.object().destroy();
    auto self = scene.create();
    bool enabling = false, disabled_after_enable = false;
    self.add_component<Callback>(Callback{[&] {
                                              enabling = true;
                                              self.destroy();
                                              enabling = false;
                                          },
                                          [&] { disabled_after_enable = !enabling && !self.valid(); },
                                          {}});
    scene.synchronize_lifecycle();
    check(disabled_after_enable, "Self-removal did not serialize enable/disable callbacks");
    Counts stopped;
    auto stopper = scene.create();
    stopper.add_component<Probe>(stopped);
    stopper.add_component<Callback>(Callback{{}, {}, [&] { stopper.set_active(false); }});
    scene.update(0);
    check(stopped.late == 0, "Deactivated object still ran late phase");
    scene.synchronize_lifecycle();
    check(stopped.enables == 1 && stopped.disables == 1, "Update mutation missed next-boundary disable");
    auto throwing = scene.create();
    throwing.add_component<Callback>(Callback{{}, {}, [] { throw std::runtime_error("tick"); }});
    rejects([&] { scene.update(0); });
    throwing.destroy();
    scene.update(0); // exception released scheduler
    stopper.destroy();
    first.destroy();
    auto removing = scene.create();
    removing.add_component<Callback>(Callback{{},
                                              [&] {
                                                  try {
                                                      scene.update(0);
                                                  } catch (const std::logic_error &) {
                                                      ++nested_rejections;
                                                  }
                                              },
                                              {}});
    scene.synchronize_lifecycle();
    removing.remove_component<Callback>();
    check(nested_rejections == 2, "Removal callback recursively entered scheduler");
    auto deep = scene.create();
    auto tip = deep;
    for (int i = 0; i < 2048; ++i) {
        auto next = scene.create();
        next.set_parent(tip, ReparentMode::keep_local);
        tip = next;
    }
    deep.set_active(false);
    check(tip.active_self() && !tip.active_in_hierarchy(), "Deep hierarchy activation did not propagate");
    deep.set_active(true);
    check(tip.active_in_hierarchy(), "Deep hierarchy did not reactivate");
    deep.destroy();
    check(!tip.valid(), "Deep hierarchy destruction failed");

    Counts teardown, never;
    ComponentRef<Probe> expired;
    {
        Scene temporary;
        expired = temporary.create().add_component<Probe>(teardown);
        auto inactive = temporary.create();
        inactive.set_active(false);
        inactive.add_component<Probe>(never);
        temporary.synchronize_lifecycle();
    }
    check(!expired && teardown.disables == 1 && teardown.invalid_disables == 1 && teardown.destroyed == 1 &&
              never.enables == 0 && never.disables == 0 && never.destroyed == 1,
          "Scene teardown did not match observed activation lifetime");

    // Persistence carries authored flags, never notification state.
    Counts loaded_counts;
    ComponentCodecs codecs;
    codecs.add<Probe>(
        "probe.v1", [](const Probe &, const ObjectReferences &) { return "{}"; },
        [&](GameObject object, std::string_view, const ObjectReferences &) {
            object.add_component<Probe>(loaded_counts);
        });
    Scene authored;
    auto root = authored.create("root"), leaf = authored.create("leaf");
    root.set_active(false);
    leaf.set_parent(root);
    leaf.add_component<Probe>(loaded_counts).set_enabled(false);
    const auto document = serialize_scene(authored, {}, codecs);
    auto loaded = load_scene(document, {}, codecs);
    auto restored = loaded->roots()[0];
    check(!restored.active_self() && restored.children()[0].active_self() &&
              !restored.children()[0].get_component<Probe>().enabled() && loaded_counts.enables == 0,
          "Scene persistence lost local activation or fired hooks");
    auto prefab = Prefab::deserialize(Prefab::capture(root, codecs).serialize({}), {}, codecs);
    auto copy = prefab.instantiate(authored);
    copy.set_active(true);
    auto copy_probe = copy.children()[0].get_component<Probe>();
    check(!copy_probe.active(), "Object enable overwrote restored component flag");
    copy_probe.set_enabled(true);
    authored.synchronize_lifecycle();
    check(loaded_counts.enables == 1 && !leaf.active_in_hierarchy(), "Prefab instances share activation state");
    auto malformed = document;
    auto flag = malformed.find("\"active\": false");
    malformed.replace(flag, 15, "\"active\": 0");
    rejects([&] { (void)load_scene(malformed, {}, codecs); });
    auto nodes = std::vector<Prefab::Node>(prefab.nodes().begin(), prefab.nodes().end());
    nodes[0].active = true;
    nodes[1].components[0].enabled = true;
    ComponentCodecs broken;
    broken.add<Probe>(
        "probe.v1", [](const Probe &, const ObjectReferences &) { return "{}"; },
        [&](GameObject object, std::string_view, const ObjectReferences &) {
            object.add_component<Probe>(loaded_counts);
            throw std::runtime_error("decode");
        });
    const auto before = authored.size();
    rejects([&] { (void)Prefab(nodes, broken).instantiate(authored); });
    check(authored.size() == before && loaded_counts.enables == 1, "Failed loading published lifecycle hooks");
    // Callback backing counters outlive all scenes using them.
    copy.destroy();
    root.destroy();
    loaded.reset();
}
} // namespace lifecycle_test
