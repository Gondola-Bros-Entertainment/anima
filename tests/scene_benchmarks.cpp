// Times hierarchy construction, bulk destruction and per-frame scene work at a chosen object count.
//
//   anima_scene_benchmarks [--objects N] [--repetitions R]
//
// Each scenario builds its own scene, times only the operation it names and checks the result, so
// a smaller count doubles as a smoke test. It prints the median of R runs. Build it in Release to
// measure; the default object count is the scene document object limit. The load scenarios load as
// many of the objects as one document holds within its byte limit.
#include <anima/prefab.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace anima;
using Clock = std::chrono::steady_clock;

constexpr std::size_t default_object_count = 65'536;             // The scene and prefab document object limit.
constexpr std::size_t maximum_document_bytes = 16 * 1024 * 1024; // The scene document byte limit.
constexpr std::size_t maximum_benchmark_objects = 1U << 24;
constexpr std::size_t default_repetitions = 5;
constexpr std::size_t maximum_benchmark_repetitions = 1000;
constexpr std::size_t hooked_stride = 64; // One object in this many has an update hook.
constexpr double frame_seconds = 1.0 / 60;

void require(bool accepted, const char *failure) {
    if (!accepted)
        throw std::runtime_error(failure);
}

// A one-triangle mesh, so rendering cost stays small next to the scene bookkeeping being timed.
std::shared_ptr<const Mesh> triangle_mesh() {
    Asset source;
    source.nodes.resize(1);
    SourcePrimitive primitive;
    for (const auto corner : {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}}) {
        SourceVertex vertex;
        vertex.position = corner;
        vertex.normal = {0, 0, 1};
        primitive.vertices.push_back(vertex);
    }
    source.primitives.push_back(primitive);
    return Mesh::compile(source);
}

struct Tag {
    int value{};
};
struct Spinner {
    std::size_t *frames;
    void on_update(double) { ++*frames; }
};
struct Failing {};

// Seconds taken by @p operation.
template <class Operation> double seconds(Operation &&operation) {
    const auto start = Clock::now();
    operation();
    return std::chrono::duration<double>(Clock::now() - start).count();
}

// A wide parent with @p count - 1 children, or a chain of @p count objects when @p deep.
void hierarchy(Scene &scene, std::size_t count, bool deep, const std::shared_ptr<const Mesh> &mesh = {}) {
    auto parent = scene.create("root", mesh);
    for (std::size_t i = 1; i < count; ++i) {
        auto child = scene.create({}, mesh);
        child.set_parent(parent, ReparentMode::keep_local);
        if (deep)
            parent = child;
    }
}

// A compact scene document and the number of objects it holds.
struct Document {
    std::string text;
    std::size_t objects = 0;
};
// A compact scene document of @p count objects, or of as many as fit the byte limit. Even compact,
// an object with every field takes about 280 bytes, so the limit holds about 60,000 of them, and
// the indented document that serialize_scene writes holds fewer.
Document document(std::size_t count, bool deep) {
    const auto envelope = [](std::size_t objects) {
        return R"({"version":4,"kind":"anima.scene","next_key":")" + std::to_string(objects + 1) + R"(","objects":[)";
    };
    // The envelope with the longest next_key that this call can write.
    const auto envelope_bytes = envelope(count).size() + std::string_view("]}").size();
    std::string objects;
    std::size_t written = 0;
    for (; written < count; ++written) {
        std::string object = written ? "," : "";
        object += R"({"key":")" + std::to_string(written + 1) + R"(","name":"","parent":)";
        object += written == 0 ? "null" : std::to_string(deep ? written - 1 : 0);
        object += R"(,"local":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"mesh":null,"pose":null,"visible":true,)"
                  R"("active":true,"material_factors":[],"custom_materials":[],"primitive_visible":[],)"
                  R"("casts_shadows":true,"placements":null,"visibility_range":null,"components":[]})";
        if (envelope_bytes + objects.size() + object.size() > maximum_document_bytes)
            break;
        objects += object;
    }
    return {envelope(written) + objects + "]}", written};
}

double attach(std::size_t count, bool deep) {
    Scene scene;
    std::vector<GameObject> objects;
    for (std::size_t i = 0; i < count; ++i)
        objects.push_back(scene.create());
    const auto elapsed = seconds([&] {
        for (std::size_t i = 1; i < count; ++i)
            objects[i].set_parent(objects[deep ? i - 1 : 0], ReparentMode::keep_local);
    });
    require(deep ? objects.back().parent()->id() == objects[count - 2].id()
                 : objects.front().children().size() == count - 1,
            "Attaching built the wrong hierarchy");
    return elapsed;
}

double load(const Document &document) {
    std::shared_ptr<Scene> loaded;
    const auto elapsed = seconds([&] { loaded = load_scene(document.text, {}); });
    require(loaded->size() == document.objects && loaded->roots().size() == 1, "Loading built the wrong hierarchy");
    return elapsed;
}

double destroy_children(std::size_t count) {
    Scene scene;
    hierarchy(scene, count, false);
    auto children = scene.roots().front().children();
    const auto elapsed = seconds([&] {
        for (auto &child : children)
            child.destroy();
    });
    require(scene.size() == 1, "Destroying children left objects behind");
    return elapsed;
}

double destroy_renderers(std::size_t count, const std::shared_ptr<const Mesh> &mesh) {
    Scene scene;
    std::vector<GameObject> objects;
    for (std::size_t i = 0; i < count; ++i)
        objects.push_back(scene.create({}, mesh));
    const auto elapsed = seconds([&] {
        for (auto &object : objects)
            object.destroy();
    });
    require(scene.size() == 0 && scene.instances().empty(), "Destroying renderers left objects behind");
    return elapsed;
}

double destroy_subtree(std::size_t count, bool deep, const std::shared_ptr<const Mesh> &mesh) {
    Scene scene;
    hierarchy(scene, count, deep, mesh);
    auto root = scene.roots().front();
    const auto elapsed = seconds([&] { root.destroy(); });
    require(scene.size() == 0 && scene.instances().empty(), "Destroying a subtree left objects behind");
    return elapsed;
}

// Instantiates a wide prefab whose last component fails to decode, so every staged object is
// destroyed again.
double prefab_rollback(std::size_t count, const std::shared_ptr<const Mesh> &mesh) {
    ComponentCodecs codecs;
    codecs.add<Failing>(
        "benchmark.failing.v1", [](const Failing &, const ObjectReferences &) { return std::string{}; },
        [](GameObject object, std::string_view state, const ObjectReferences &) {
            object.add_component<Failing>();
            if (state == "reject")
                throw std::invalid_argument("Benchmark component rejects its state");
        });
    std::vector<Prefab::Node> nodes(count);
    for (std::size_t i = 0; i < count; ++i) {
        nodes[i].renderer.mesh = mesh;
        if (i)
            nodes[i].parent = 0;
    }
    nodes.back().components.push_back({"benchmark.failing.v1", "reject", true});
    const Prefab prefab(std::move(nodes), codecs);
    Scene scene;
    bool rejected = false;
    const auto elapsed = seconds([&] {
        try {
            (void)prefab.instantiate(scene);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
    });
    require(rejected && scene.size() == 0 && scene.instances().empty(), "Prefab rollback left objects behind");
    return elapsed;
}

// Repeatedly destroys the objects in the lowest and highest slots and creates two replacements,
// which reuse those slots, the second after every slot between them.
double reuse_slots(std::size_t count) {
    Scene scene;
    std::vector<GameObject> objects;
    for (std::size_t i = 0; i < count; ++i)
        objects.push_back(scene.create());
    const auto elapsed = seconds([&] {
        for (std::size_t i = 0; i < count; i += 2) {
            objects.front().destroy();
            objects.back().destroy();
            objects.front() = scene.create();
            objects.back() = scene.create();
        }
    });
    require(scene.size() == count && objects.front().id().slot == 0 && objects.back().id().slot == count - 1,
            "Slot reuse changed the scene or skipped a free slot");
    return elapsed;
}

// A scene of static props: every object renders and carries a hookless component, and one in
// hooked_stride also has an update hook.
struct Props {
    Scene scene;
    std::vector<GameObject> objects;
    std::size_t frames{};
    Props(std::size_t count, const std::shared_ptr<const Mesh> &mesh) {
        for (std::size_t i = 0; i < count; ++i) {
            auto object = scene.create({}, mesh);
            (void)object.add_component<Tag>();
            if (i % hooked_stride == 0)
                (void)object.add_component<Spinner>(&frames);
            objects.push_back(object);
        }
        scene.synchronize_lifecycle();
    }
    [[nodiscard]] std::size_t hooked() const { return (objects.size() + hooked_stride - 1) / hooked_stride; }
};

double update_frame(Props &props) {
    const auto before = props.frames;
    const auto elapsed = seconds([&] { props.scene.update(frame_seconds); });
    require(props.frames - before == props.hooked(), "An update skipped or repeated a hook");
    return elapsed;
}

double query_frame(Props &props) {
    std::size_t found = 0;
    const auto elapsed = seconds([&] { found = props.scene.components<Spinner>().size(); });
    require(found == props.hooked(), "The component query missed a component");
    return elapsed;
}

double move_frame(Props &props, float offset) {
    const auto elapsed = seconds([&] {
        for (auto &object : props.objects)
            object.set_position({offset, 0, 0});
    });
    require(props.objects.back().position().x == offset, "Moving objects lost a position");
    return elapsed;
}

double move_hierarchy(Scene &scene, float offset) {
    auto root = scene.roots().front();
    const auto elapsed = seconds([&] { root.set_position({offset, 0, 0}); });
    require(root.position().x == offset, "Moving a hierarchy lost its position");
    return elapsed;
}

std::size_t count_option(std::string_view text, std::size_t minimum, std::size_t maximum) {
    std::size_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value < minimum || value > maximum)
        throw std::invalid_argument("Expected an integer between " + std::to_string(minimum) + " and " +
                                    std::to_string(maximum) + ": " + std::string(text));
    return value;
}

void report(std::string_view name, std::size_t objects, std::size_t repetitions, const std::function<double()> &run) {
    std::vector<double> samples;
    for (std::size_t i = 0; i < repetitions; ++i)
        samples.push_back(run());
    std::sort(samples.begin(), samples.end());
    const auto median = samples[samples.size() / 2];
    std::printf("%-22.*s %10zu %12.3f %12.1f\n", static_cast<int>(name.size()), name.data(), objects, median * 1e3,
                median * 1e9 / static_cast<double>(objects));
    std::fflush(stdout);
}
} // namespace

int main(int argc, char **argv) {
    try {
        std::size_t count = default_object_count, repetitions = default_repetitions;
        for (int i = 1; i < argc; ++i) {
            const std::string_view option = argv[i];
            if (i + 1 < argc && option == "--objects")
                count = count_option(argv[++i], 2, maximum_benchmark_objects);
            else if (i + 1 < argc && option == "--repetitions")
                repetitions = count_option(argv[++i], 1, maximum_benchmark_repetitions);
            else
                throw std::invalid_argument("Unknown option: " + std::string(option));
        }
        const auto mesh = triangle_mesh();
        std::printf("%-22s %10s %12s %12s\n", "scenario", "objects", "median ms", "ns/object");
        report("attach_wide", count, repetitions, [&] { return attach(count, false); });
        report("attach_deep", count, repetitions, [&] { return attach(count, true); });
        const auto wide_document = document(count, false), deep_document = document(count, true);
        report("load_wide", wide_document.objects, repetitions, [&] { return load(wide_document); });
        report("load_deep", deep_document.objects, repetitions, [&] { return load(deep_document); });
        report("destroy_children", count, repetitions, [&] { return destroy_children(count); });
        report("destroy_renderers", count, repetitions, [&] { return destroy_renderers(count, mesh); });
        report("destroy_wide_subtree", count, repetitions, [&] { return destroy_subtree(count, false, mesh); });
        report("destroy_deep_subtree", count, repetitions, [&] { return destroy_subtree(count, true, mesh); });
        report("prefab_rollback", count, repetitions, [&] { return prefab_rollback(count, mesh); });
        report("reuse_slots", count, repetitions, [&] { return reuse_slots(count); });
        Props props(count, mesh);
        report("update_frame", count, repetitions, [&] { return update_frame(props); });
        report("query_frame", count, repetitions, [&] { return query_frame(props); });
        float offset = 0;
        report("move_objects_frame", count, repetitions, [&] { return move_frame(props, offset += 1); });
        Scene wide, deep;
        hierarchy(wide, count, false, mesh);
        hierarchy(deep, count, true, mesh);
        report("move_wide_hierarchy", count, repetitions, [&] { return move_hierarchy(wide, offset += 1); });
        report("move_deep_hierarchy", count, repetitions, [&] { return move_hierarchy(deep, offset += 1); });
    } catch (const std::exception &error) {
        std::fprintf(stderr, "anima_scene_benchmarks: %s\n", error.what());
        return 1;
    }
}
