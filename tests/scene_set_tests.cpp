#include "consumer/scene_set.hpp"
#include <iostream>
namespace {
using namespace anima;
using scene_set_test::check;
using scene_set_test::rejects;
std::string substitute(std::string text, std::string_view from, std::string_view to) {
    const auto at = text.find(from);
    check(at != std::string::npos, "Scene set malformed fixture field missing");
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
void malformed_documents() {
    const std::string references =
        "{\"key\":\"41\",\"scene\":\"alpha\",\"object\":\"1\"},{\"key\":\"99\",\"scene\":\"beta\",\"object\":\"1\"}";
    const auto valid = envelope(scene("alpha", node()) + "," + scene("beta", node()), references);
    SceneSet destination;
    destination.restore(valid, {});
    auto first = destination.find("alpha"), second = destination.find("beta");
    const auto retained = first->find(ObjectKey{1});
    check(first && second && destination.active().key() == "beta" && retained.valid() &&
              second->find(ObjectKey{1}).valid(),
          "Set reader rejected sparse operation keys or duplicate scene-local keys");
    const auto before = destination.serialize({});
    const auto rejected = [&](const std::string &document) {
        rejects([&] { destination.restore(document, {}); });
        check(first && second && retained.valid() && destination.serialize({}) == before,
              "Rejected envelope altered destination membership, selection or objects");
    };
    for (const auto &document : {
             std::string("{}"),
             substitute(valid, "\"version\":1", "\"version\":3"),
             substitute(valid, "\"version\":1", "\"version\":1.0"),
             substitute(valid, "\"version\":1,", ""),
             substitute(valid, "anima.scene-set", "anima.scene"),
             substitute(valid, "{", "{\"unexpected\":null,"),
             substitute(valid, "{", "{\"version\":1,"),
             substitute(valid, "{", "{\"ver\\u0073ion\":1,"),
             substitute(valid, "\"active\":\"beta\"", "\"active\":null"),
             substitute(valid, "\"active\":\"beta\"", "\"active\":true"),
             substitute(valid, "\"active\":\"beta\"", "\"active\":\"missing\""),
             substitute(valid, "\"key\":\"beta\"", "\"key\":\"alpha\""),
             substitute(valid, "\"key\":\"alpha\"", "\"key\":\"\""),
             substitute(valid, "\"key\":\"alpha\"", "\"key\":\"bad\\u0000name\""),
             substitute(valid, "\"key\":\"alpha\"", "\"key\":\"" + std::string(4097, 'x') + "\""),
             substitute(valid, "\"next_key\":\"2\"", "\"next_key\":\"1\""),
             substitute(valid, "\"next_key\":\"2\"", "\"next_key\":\"02\""),
             substitute(valid, "\"next_key\":\"2\",", ""),
             substitute(valid, "\"next_key\":\"2\"", "\"next_key\":\"2\",\"extra\":0"),
             envelope(scene("alpha", node()), "", "\"alpha\""),
             envelope(scene("alpha", node()) + "," + scene("beta", node()), references + ",{}"),
             substitute(valid, "\"key\":\"99\"", "\"key\":\"41\""),
             substitute(valid, "\"key\":\"41\"", "\"key\":\"0\""),
             substitute(valid, "\"key\":\"41\"", "\"key\":\"041\""),
             substitute(valid, "\"key\":\"41\"", "\"key\":41"),
             substitute(valid, "\"scene\":\"alpha\"", "\"scene\":\"missing\""),
             substitute(valid, "\"scene\":\"beta\"", "\"scene\":\"alpha\""),
             substitute(valid, "\"object\":\"1\"", "\"object\":\"0\""),
             substitute(valid, "\"object\":\"1\"", "\"object\":\"2\""),
             substitute(valid, "\"object\":\"1\"", "\"object\":\"01\""),
             substitute(valid, "\"object\":\"1\"", "\"object\":1"),
             substitute(valid, "\"scene\":\"alpha\",", ""),
             substitute(valid, "\"scene\":\"alpha\"", "\"scene\":\"alpha\",\"extra\":0"),
             envelope("", "", "\"beta\""),
         })
        rejected(document);

    const auto one =
        envelope(scene("alpha", node(), "0"), "{\"key\":\"7\",\"scene\":\"alpha\",\"object\":\"1\"}", "\"alpha\"");
    SceneSet exhausted;
    exhausted.restore(one, {});
    rejects([&] { (void)exhausted.find("alpha")->create(); });
    check(exhausted.find("alpha")->find(ObjectKey{1}).valid(),
          "Exhausted allocator rejected existing persisted identity");

    rejected(std::string(16 * 1024 * 1024, ' ') + valid);
    std::string oversized;
    for (int i = 0; i < 65536; ++i) {
        if (i)
            oversized += ',';
        oversized += "{}";
    }
    // The second array is within the per-scene bound, but exceeds the remaining
    // set budget. Reject it before traversing its untrusted object descriptions.
    rejected(envelope(scene("alpha", node()) + "," + scene("beta", oversized), ""));
    rejected(envelope("", oversized + ",{}", "null"));

    SceneSet many;
    for (int i = 0; i < 1024; ++i)
        (void)many.create("scene-" + std::to_string(i));
    const auto at_limit = many.serialize({});
    SceneSet boundary;
    boundary.restore(at_limit, {});
    check(boundary.size() == 1024 && boundary.active().key() == "scene-0",
          "Set scene-count boundary rejected a valid document");
    (void)many.create("one-too-many");
    rejects([&] { (void)many.serialize({}); });
    std::string too_many;
    for (int i = 0; i < 1025; ++i) {
        if (i)
            too_many += ',';
        too_many += scene("scene-" + std::to_string(i), "", "1");
    }
    rejected(envelope(too_many, "", "\"scene-0\""));
}
struct CallbackBoundary {
    SceneSet *scenes;
    ComponentCodecs *codecs;
    const std::string *empty;
    int *called;
    void on_update(double) {
        ++*called;
        rejects([&] { (void)scenes->serialize({}, *codecs); });
        rejects([&] { scenes->restore(*empty, {}); });
    }
};
struct ConstructorBoundary {
    ConstructorBoundary(SceneSet &scenes, const std::string &empty) {
        rejects([&] { (void)scenes.serialize({}); });
        rejects([&] { scenes.restore(empty, {}); });
    }
};
void callback_boundaries() {
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
    object.add_component<CallbackBoundary>(&scenes, &codecs, &empty, &called);
    scenes.update(0);
    member->update(0);
    check(called == 2 && member && object.valid(), "Persistence callback guards did not preserve live scheduling");
}
} // namespace
int main() {
    try {
        scene_set_test::run();
        malformed_documents();
        callback_boundaries();
        std::cout << "PASS additive scene ownership, set persistence, reference tables, replacement and unload\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
