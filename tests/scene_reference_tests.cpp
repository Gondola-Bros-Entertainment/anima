#include "consumer/references.hpp"
#include <iostream>
namespace {
using namespace anima;
using references_test::check;
using references_test::rejects;
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
void destination_bindings() {
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
    check(direct.position().x == 9 && direct_child.position().x == 12 &&
              direct.get_component<BoundLink>()->binding == 2 &&
              direct_child.get_component<BoundLink>()->binding == 2 &&
              direct.get_component<BoundLink>()->target.id() == direct_child.id() &&
              direct_child.get_component<BoundLink>()->target.id() == direct.id(),
          "Destination codecs changed placement or escaped the instance reference graph");
    auto parent = target.create();
    parent.set_position({10, 0, 0});
    parent.set_active(false);
    auto nested = prefab.instantiate(parent, placed, destination);
    auto nested_child = nested.children().front();
    check(nested.position().x == 19 && nested_child.position().x == 22 && !nested.active_in_hierarchy() &&
              nested.get_component<BoundLink>()->binding == 2 &&
              nested.get_component<BoundLink>()->target.id() == nested_child.id() &&
              nested_child.get_component<BoundLink>()->target.id() == nested.id(),
          "Parent destination overload lost placement, inherited activation or reference remapping");
    auto retained = prefab.instantiate(target);
    auto retained_nested = prefab.instantiate(parent, placed);
    check(retained.get_component<BoundLink>()->binding == 1 &&
              retained_nested.get_component<BoundLink>()->binding == 1 && retained_nested.position().x == 19 &&
              source_live == 6 && destination_live == 4 && prefab.serialize({}) == original_document,
          "Destination instantiation mutated the prefab or replaced its retained codecs");

    // A missing type on a later node rejects before decoding or allocating keys.
    auto incomplete = destination_codecs(2, destination_live, destination_decoded, false);
    auto sentinel = target.create();
    const auto before = target.size(), decoded_before = destination_decoded.size();
    rejects([&] { (void)prefab.instantiate(target, identity(), incomplete); });
    check(target.size() == before && destination_decoded.size() == decoded_before && destination_live == 4,
          "Incomplete destination codecs partially decoded the graph");
    auto next = target.create();
    check(next.key().value == sentinel.key().value + 1, "Missing codec validation consumed object identities");
    next.destroy();
    auto failing = destination_codecs(2, destination_live, destination_decoded, true, true);
    rejects([&] { (void)prefab.instantiate(parent, identity(), failing); });
    check(target.size() == before && destination_live == 4 && destination_decoded.size() == decoded_before + 2 &&
              !destination_decoded[decoded_before].valid() && !destination_decoded[decoded_before + 1].valid() &&
              sentinel.valid(),
          "Failed destination decoder leaked objects/resources or invalidated existing state");
}
} // namespace
int main() {
    try {
        references_test::run();
        destination_bindings();
        std::cout << "PASS stable scene keys, references, prefab destination bindings, remapping and rollback\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
