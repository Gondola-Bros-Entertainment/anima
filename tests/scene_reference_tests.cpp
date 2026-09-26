#include "consumer/references.hpp"
#include <doctest/doctest.h>

#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using namespace anima;
struct BoundLink {
    GameObject target;
    int binding;
    int *live;
    BoundLink(GameObject link, int session, int &instances) : target(link), binding(session), live(&instances) {
        ++*live;
    }
    ~BoundLink() { --*live; }
};
struct Marker {};
ComponentCodecs destination_codecs(int binding, int &live, std::vector<GameObject> &decoded, bool marker = true,
                                   bool fail = false) {
    ComponentCodecs codecs;
    codecs.add<BoundLink>(
        "test.bound-link.v1",
        [](const BoundLink &link, const ObjectReferences &references) { return references.key(link.target).string(); },
        [binding, &live, &decoded](GameObject object, std::string_view data, const ObjectReferences &references) {
            object.add_component<BoundLink>(references.resolve(ObjectKey::parse(data)), binding, live);
            decoded.push_back(object);
        });
    if (marker)
        codecs.add<Marker>(
            "test.marker.v1", [](const Marker &, const ObjectReferences &) { return "{}"; },
            [fail](GameObject object, std::string_view, const ObjectReferences &) {
                object.add_component<Marker>();
                if (fail)
                    throw std::runtime_error("Destination decoder rejected the instance");
            });
    return codecs;
}
} // namespace

TEST_CASE("The shared consumer scenario for stable scene keys and object references passes") {
    // It reports a failed check by throwing std::runtime_error, which fails this test case.
    references_test::run();
}

TEST_CASE("Destination codecs bind each prefab instance, and a failed binding rolls back") {
    int source_live = 0, destination_live = 0;
    std::vector<GameObject> source_decoded, destination_decoded;
    auto original = destination_codecs(1, source_live, source_decoded);
    auto destination = destination_codecs(2, destination_live, destination_decoded);
    Scene authored, target;
    auto root = authored.create(), child = authored.create();
    root.set_position({2, 0, 0});
    child.set_parent(root, ReparentMode::keep_local);
    child.set_local_position({3, 0, 0});
    root.add_component<BoundLink>(child, 1, source_live);
    child.add_component<BoundLink>(root, 1, source_live);
    child.add_component<Marker>();
    const auto prefab = Prefab::capture(root, original);
    const auto original_document = prefab.serialize({});
    auto placed = identity();
    placed[12] = 7;
    auto direct = prefab.instantiate(target, placed, destination);
    auto direct_child = direct.children().front();
    // Destination codecs keep the placement and remap links inside the instance.
    CHECK(direct.position().x == 9);
    CHECK(direct_child.position().x == 12);
    CHECK(direct.get_component<BoundLink>()->binding == 2);
    CHECK(direct_child.get_component<BoundLink>()->binding == 2);
    CHECK(direct.get_component<BoundLink>()->target.id() == direct_child.id());
    CHECK(direct_child.get_component<BoundLink>()->target.id() == direct.id());
    auto parent = target.create();
    parent.set_position({10, 0, 0});
    parent.set_active(false);
    auto nested = prefab.instantiate(parent, placed, destination);
    auto nested_child = nested.children().front();
    // The parent overload also inherits the parent's activation.
    CHECK(nested.position().x == 19);
    CHECK(nested_child.position().x == 22);
    CHECK_FALSE(nested.active_in_hierarchy());
    CHECK(nested.get_component<BoundLink>()->binding == 2);
    CHECK(nested.get_component<BoundLink>()->target.id() == nested_child.id());
    CHECK(nested_child.get_component<BoundLink>()->target.id() == nested.id());
    // Instances without destination codecs keep the prefab's own, and neither mutates the prefab.
    auto retained = prefab.instantiate(target);
    auto retained_nested = prefab.instantiate(parent, placed);
    CHECK(retained.get_component<BoundLink>()->binding == 1);
    CHECK(retained_nested.get_component<BoundLink>()->binding == 1);
    CHECK(retained_nested.position().x == 19);
    CHECK(source_live == 6);
    CHECK(destination_live == 4);
    CHECK(prefab.serialize({}) == original_document);

    // A missing type on a later node rejects before decoding or allocating keys.
    auto incomplete = destination_codecs(2, destination_live, destination_decoded, false);
    auto sentinel = target.create();
    const auto before = target.size(), decoded_before = destination_decoded.size();
    CHECK_THROWS_WITH_AS(prefab.instantiate(target, identity(), incomplete), "Unknown serialized component type",
                         std::invalid_argument);
    CHECK(target.size() == before);
    CHECK(destination_decoded.size() == decoded_before);
    CHECK(destination_live == 4);
    auto next = target.create();
    CHECK(next.key().value == sentinel.key().value + 1);
    next.destroy();
    auto failing = destination_codecs(2, destination_live, destination_decoded, true, true);
    CHECK_THROWS_WITH_AS(prefab.instantiate(parent, identity(), failing), "Destination decoder rejected the instance",
                         std::runtime_error);
    // Both nodes decoded before the failure, and rollback destroyed them without touching existing objects.
    CHECK(target.size() == before);
    CHECK(destination_live == 4);
    REQUIRE(destination_decoded.size() == decoded_before + 2);
    CHECK_FALSE(destination_decoded[decoded_before].valid());
    CHECK_FALSE(destination_decoded[decoded_before + 1].valid());
    CHECK(sentinel.valid());
}
