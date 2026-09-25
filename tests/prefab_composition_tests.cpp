#include "consumer/prefab_composition.hpp"
#include <iostream>
#include <limits>

namespace {
using namespace anima;
using prefab_composition_test::check;
using prefab_composition_test::rejects;
std::string substitute(std::string value, std::string_view from, std::string_view to) {
    const auto at = value.find(from);
    check(at != std::string::npos, "Composition malformed fixture field missing");
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
void malformed_documents() {
    const std::string mount = "{\"part\":\"root\",\"object\":\"41\"}";
    const auto valid = envelope(part("root") + "," + part("child", mount));
    const auto decoded = PrefabComposition::deserialize(valid);
    check(decoded.parts().size() == 2 && decoded.parts()[1].parent->object == ObjectKey{41},
          "Composition rejected a canonical named-part document");
    const auto invalid = [&](const std::string &document) {
        rejects([&] { (void)PrefabComposition::deserialize(document); });
    };
    for (const auto &document : {
             std::string("{}"),
             envelope(""),
             substitute(valid, "\"version\":1", "\"version\":2"),
             substitute(valid, "\"version\":1", "\"version\":1.0"),
             substitute(valid, "\"version\":1,", ""),
             substitute(valid, "anima.prefab-composition", "anima.prefab"),
             substitute(valid, "{", "{\"unexpected\":null,"),
             substitute(valid, "{", "{\"version\":1,"),
             substitute(valid, "{", "{\"ver\\u0073ion\":1,"),
             substitute(valid, "\"key\":\"root\"", "\"key\":\"\""),
             substitute(valid, "\"key\":\"child\"", "\"key\":\"root\""),
             substitute(valid, "\"key\":\"root\",", ""),
             substitute(valid, "\"key\":\"root\"", "\"key\":\"root\",\"k\\u0065y\":\"other\""),
             substitute(valid, "\"key\":\"root\"", "\"key\":\"" + std::string(4097, 'x') + "\""),
             substitute(valid, "\"prefab\":\"shared\"", "\"prefab\":\"\""),
             substitute(valid, "\"prefab\":\"shared\"", "\"prefab\":null"),
             substitute(valid, "\"prefab\":\"shared\",", ""),
             substitute(valid, "\"parent\":null", "\"parent\":" + mount),
             envelope(part("root") + "," + part("child")),
             substitute(valid, "\"part\":\"root\"", "\"part\":\"child\""),
             substitute(valid, "\"part\":\"root\"", "\"part\":\"later\""),
             substitute(valid, "\"part\":\"root\",", ""),
             substitute(valid, "\"object\":\"41\"", "\"object\":\"0\""),
             substitute(valid, "\"object\":\"41\"", "\"object\":\"041\""),
             substitute(valid, "\"object\":\"41\"", "\"object\":41"),
             substitute(valid, "\"object\":\"41\"", "\"object\":\"18446744073709551616\""),
             substitute(valid, "\"object\":\"41\"", "\"object\":\"41\",\"extra\":null"),
             substitute(valid, "\"parent\":null,", ""),
             substitute(valid, "\"placement\":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]", "\"placement\":null"),
             substitute(valid, "\"placement\":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]", "\"placement\":[]"),
             substitute(valid, "\"placement\":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]",
                        "\"placement\":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,2]"),
             substitute(valid, "\"placement\":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]",
                        "\"placement\":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1e1000]"),
         })
        invalid(document);
    const auto maximum = std::to_string(std::numeric_limits<std::uint64_t>::max());
    check(PrefabComposition::deserialize(substitute(valid, "\"object\":\"41\"", "\"object\":\"" + maximum + "\""))
                  .parts()[1]
                  .parent->object.value == std::numeric_limits<std::uint64_t>::max(),
          "Composition truncated a maximum-width authored mount identity");
    invalid(std::string(16 * 1024 * 1024, ' ') + valid);
    invalid(substitute(valid, "\"parent\":null", "\"parent\":" + std::string(32, '[') + "0" + std::string(32, ']')));
}
void validation_and_limits() {
    auto valid = prefab_composition_test::parts();
    rejects([&] { (void)PrefabComposition({}); });
    auto bad = valid;
    bad[0].key.clear();
    rejects([&] { (void)PrefabComposition(bad); });
    bad = valid;
    bad[0].prefab.assign(4097, 'p');
    rejects([&] { (void)PrefabComposition(bad); });
    bad = valid;
    bad[1].parent->part = "leaf";
    rejects([&] { (void)PrefabComposition(bad); });
    bad = valid;
    bad[1].parent->object = {};
    rejects([&] { (void)PrefabComposition(bad); });
    bad = valid;
    bad[0].placement[12] = std::numeric_limits<float>::quiet_NaN();
    rejects([&] { (void)PrefabComposition(bad); });
    bad = valid;
    bad[1].placement[15] = 2;
    rejects([&] { (void)PrefabComposition(bad); });
    std::vector<PrefabComposition::Part> many;
    many.reserve(1025);
    many.push_back({"root", "shared", std::nullopt, identity()});
    for (std::size_t i = 1; i < 1024; ++i)
        many.push_back({"part-" + std::to_string(i), "shared", PrefabComposition::Mount{"root", {1}}, identity()});
    const PrefabComposition maximum(many);
    check(PrefabComposition::deserialize(maximum.serialize()).parts().size() == 1024,
          "Composition rejected the supported part-count boundary");
    many.push_back({"too-many", "shared", PrefabComposition::Mount{"root", {1}}, identity()});
    rejects([&] { (void)PrefabComposition(many); });
    auto oversized = part("root");
    for (std::size_t i = 1; i <= 1024; ++i)
        oversized += ',' + part("part-" + std::to_string(i), "{\"part\":\"root\",\"object\":\"1\"}");
    rejects([&] { (void)PrefabComposition::deserialize(envelope(oversized)); });

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
    rejects([&] {
        (void)maximum.instantiate(scene,
                                  [&](auto) {
                                      ++resolves;
                                      return base;
                                  },
                                  {});
    });
    const auto next = scene.create();
    check(resolves == 1 && existing.valid() && scene.size() == 2 && next.key().value == existing.key().value + 1,
          "Composition exceeded its aggregate native budget or partially created an oversized graph");
    const PrefabComposition one({{"root", "shared", std::nullopt, identity()}});
    auto invalid_placement = identity();
    invalid_placement[12] = std::numeric_limits<float>::infinity();
    rejects([&] { (void)one.instantiate(scene, [&](auto) { return base; }, {}, invalid_placement); });
    check(scene.size() == 2 && existing.valid() && next.valid(),
          "Rejected composition call placement changed the destination");
}
} // namespace
int main() {
    try {
        prefab_composition_test::run();
        malformed_documents();
        validation_and_limits();
        std::cout << "PASS prefab composition mounts, reference scopes, destination bindings, staging and rollback\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
