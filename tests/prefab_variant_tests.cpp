#include "consumer/prefab_variant.hpp"
#include <iostream>
#include <limits>

namespace {
using namespace anima;
using prefab_variant_test::check;
using prefab_variant_test::rejects;
std::string substitute(std::string value, std::string_view from, std::string_view to) {
    const auto at = value.find(from);
    check(at != std::string::npos, "Prefab variant malformed fixture field missing");
    value.replace(at, from.size(), to);
    return value;
}
std::string change() {
    return "{\"key\":\"41\",\"name\":\"override\",\"local\":null,\"active\":null,\"renderer\":null,"
           "\"set_components\":[],\"remove_components\":[]}";
}
std::string envelope(const std::string &overrides) {
    return "{\"version\":1,\"kind\":\"anima.prefab-variant\",\"base\":\"base\",\"overrides\":[" + overrides + "]}";
}
void malformed_documents() {
    const auto valid = envelope(change());
    const auto decoded = PrefabVariant::deserialize(valid, {});
    check(decoded.base_key() == "base" && decoded.overrides().size() == 1 &&
              decoded.overrides()[0].key == ObjectKey{41} && decoded.overrides()[0].name == "override",
          "Variant reader rejected the canonical typed override envelope");
    const auto invalid = [&](const std::string &document) {
        rejects([&] { (void)PrefabVariant::deserialize(document, {}); });
    };
    for (const auto &document : {
             std::string("{}"),
             substitute(valid, "\"version\":1", "\"version\":2"),
             substitute(valid, "\"version\":1", "\"version\":1.0"),
             substitute(valid, "\"version\":1,", ""),
             substitute(valid, "anima.prefab-variant", "anima.prefab"),
             substitute(valid, "{", "{\"unexpected\":null,"),
             substitute(valid, "{", "{\"version\":1,"),
             substitute(valid, "{", "{\"ver\\u0073ion\":1,"),
             substitute(valid, "\"base\":\"base\"", "\"base\":\"\""),
             substitute(valid, "\"base\":\"base\"", "\"base\":null"),
             substitute(valid, "\"base\":\"base\",", ""),
             substitute(valid, "\"base\":\"base\"", "\"base\":\"" + std::string(4097, 'x') + "\""),
             envelope(change() + "," + change()),
             substitute(valid, "\"key\":\"41\"", "\"key\":\"0\""),
             substitute(valid, "\"key\":\"41\"", "\"key\":\"041\""),
             substitute(valid, "\"key\":\"41\"", "\"key\":\"-1\""),
             substitute(valid, "\"key\":\"41\"", "\"key\":41"),
             substitute(valid, "\"key\":\"41\"", "\"key\":\"18446744073709551616\""),
             substitute(valid, "\"key\":\"41\",", ""),
             substitute(valid, "\"key\":\"41\"", "\"key\":\"41\",\"k\\u0065y\":\"42\""),
             substitute(valid, "\"name\":\"override\"", "\"name\":null"),
             substitute(valid, "\"name\":\"override\"", "\"name\":42"),
             substitute(valid, "\"name\":\"override\",", ""),
             substitute(valid, "\"local\":null", "\"local\":[]"),
             substitute(valid, "\"local\":null", "\"local\":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1e1000]"),
             substitute(valid, "\"active\":null", "\"active\":1"),
             substitute(valid, "\"renderer\":null", "\"renderer\":{}"),
             substitute(valid, "\"renderer\":null,", ""),
             substitute(valid, "\"renderer\":null", "\"renderer\":null,\"parent\":null"),
             substitute(valid, "\"set_components\":[]", "\"set_components\":null"),
             substitute(valid, "\"set_components\":[]", "\"set_components\":[{}]"),
             substitute(valid, "\"remove_components\":[]", "\"remove_components\":[\"\"]"),
             substitute(valid, "\"remove_components\":[]", "\"remove_components\":[\"type\",\"type\"]"),
             substitute(valid, "\"remove_components\":[]", "\"remove_components\":[true]"),
             substitute(valid, ",\"remove_components\":[]", ""),
         })
        invalid(document);
    const std::string component = "{\"type\":\"test.marker.v1\",\"state\":\"ok\",\"enabled\":true}";
    const auto with_component = substitute(valid, "\"set_components\":[]", "\"set_components\":[" + component + "]");
    (void)PrefabVariant::deserialize(with_component, {}); // Codecs are required only when resolving the base.
    invalid(substitute(with_component, "\"state\":\"ok\",", ""));
    invalid(substitute(with_component, "\"state\":\"ok\"", "\"state\":42"));
    invalid(substitute(with_component, "\"enabled\":true", "\"enabled\":1"));
    invalid(substitute(with_component, "\"enabled\":true", "\"enabled\":true,\"extra\":null"));
    invalid(substitute(valid, "\"set_components\":[]", "\"set_components\":[" + component + "," + component + "]"));
    invalid(substitute(with_component, "\"remove_components\":[]", "\"remove_components\":[\"test.marker.v1\"]"));
    const std::string empty_renderer =
        "{\"mesh\":null,\"pose\":null,\"visible\":true,\"material_factors\":[],\"primitive_visible\":[]}";
    const auto with_renderer = substitute(valid, "\"renderer\":null", "\"renderer\":" + empty_renderer);
    (void)PrefabVariant::deserialize(with_renderer, {});
    invalid(substitute(with_renderer, "\"visible\":true", "\"visible\":false"));
    invalid(substitute(with_renderer, "\"pose\":null", "\"pose\":[]"));
    invalid(substitute(with_renderer, "\"pose\":null,", ""));
    invalid(substitute(with_renderer, "\"primitive_visible\":[]", "\"primitive_visible\":[false]"));
    invalid(substitute(with_renderer, "\"material_factors\":[]", "\"material_factors\":[[1,1,1]]"));
    invalid(substitute(with_renderer, "\"mesh\":null", "\"mesh\":null,\"unknown\":0"));
    const auto maximum = std::to_string(std::numeric_limits<std::uint64_t>::max());
    check(PrefabVariant::deserialize(substitute(valid, "\"key\":\"41\"", "\"key\":\"" + maximum + "\""), {})
                  .overrides()[0]
                  .key.value == std::numeric_limits<std::uint64_t>::max(),
          "Variant document truncated a maximum-width authored key");
    invalid(
        substitute(valid, "\"name\":\"override\"", "\"name\":" + std::string(32, '[') + "0" + std::string(32, ']')));
    invalid(std::string(16 * 1024 * 1024, ' ') + valid);
    std::string too_many;
    too_many.reserve(3 * 65537);
    for (std::size_t i = 0; i < 65537; ++i) {
        if (i)
            too_many += ',';
        too_many += "{}";
    }
    invalid(envelope(too_many));
}
void programmatic_validation() {
    PrefabVariant::Override value;
    value.key = {41};
    value.name = "valid";
    rejects([&] { (void)PrefabVariant("", {value}); });
    rejects([&] { (void)PrefabVariant(std::string(4097, 'b'), {value}); });
    rejects([&] { (void)PrefabVariant("base", {value, value}); });
    auto bad = value;
    bad.key = {};
    rejects([&] { (void)PrefabVariant("base", {bad}); });
    bad = value;
    bad.name.reset();
    rejects([&] { (void)PrefabVariant("base", {bad}); });
    bad = value;
    bad.local = identity();
    (*bad.local)[12] = std::numeric_limits<float>::quiet_NaN();
    rejects([&] { (void)PrefabVariant("base", {bad}); });
    bad = value;
    bad.set_components = {{"type", "a", true}, {"type", "b", false}};
    rejects([&] { (void)PrefabVariant("base", {bad}); });
    bad.set_components.resize(1);
    bad.remove_components = {"type"};
    rejects([&] { (void)PrefabVariant("base", {bad}); });
    bad = value;
    bad.set_components = {{"", "state", true}};
    rejects([&] { (void)PrefabVariant("base", {bad}); });
    bad.set_components[0].type.assign(4097, 't');
    rejects([&] { (void)PrefabVariant("base", {bad}); });
    bad = value;
    bad.set_components.resize(1025);
    for (std::size_t i = 0; i < bad.set_components.size(); ++i)
        bad.set_components[i].type = "type-" + std::to_string(i);
    rejects([&] { (void)PrefabVariant("base", {bad}); });
    bad = value;
    bad.remove_components.resize(1025);
    for (std::size_t i = 0; i < bad.remove_components.size(); ++i)
        bad.remove_components[i] = "type-" + std::to_string(i);
    rejects([&] { (void)PrefabVariant("base", {bad}); });
    bad = value;
    bad.renderer.emplace();
    bad.renderer->visible = false;
    rejects([&] { (void)PrefabVariant("base", {bad}); });
    bad.renderer->mesh = prefab_variant_test::mesh();
    bad.renderer->pose.emplace();
    rejects([&] { (void)PrefabVariant("base", {bad}); });
    bad.renderer->pose = bad.renderer->mesh->rest_pose();
    bad.renderer->pose->world[0][12] = std::numeric_limits<float>::quiet_NaN();
    rejects([&] { (void)PrefabVariant("base", {bad}); });
    bad.renderer->pose->world[0] = identity();
    bad.renderer->pose->world[0][15] = 2;
    rejects([&] { (void)PrefabVariant("base", {bad}); });
    bad.renderer->pose.reset();
    bad.renderer->material_factors = {{1, 1, 1}, {1, 1, 1}};
    rejects([&] { (void)PrefabVariant("base", {bad}); });
    bad = value;
    bad.set_components = {{"type", std::string(16 * 1024 * 1024, 's'), true}};
    rejects([&] { (void)PrefabVariant("base", {bad}).serialize({}); });
}
} // namespace
int main() {
    try {
        prefab_variant_test::run();
        malformed_documents();
        programmatic_validation();
        std::cout
            << "PASS typed prefab variants, inheritance, resources, references, destination bindings and rollback\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
