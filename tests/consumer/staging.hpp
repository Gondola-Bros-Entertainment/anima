#pragma once
// Stages a scene document with meshes on a worker thread while the owning thread keeps updating its scene set,
// cancels one load, commits another through SceneSet::replace, and compares the result with a synchronous load
// of the same document.
#include <anima/scene_set.hpp>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <version>

namespace staging_test {
using namespace anima;
inline void check(bool value, const char *reason) {
    if (!value)
        throw std::runtime_error(reason);
}
#ifdef __cpp_lib_jthread
using Worker = std::jthread;
#else
// Standard libraries that declare std::jthread only as an experimental feature, such as the libc++ of Apple
// Clang 17, get a thread that joins when destroyed, as std::jthread does.
class Worker {
  public:
    template <class Function> explicit Worker(Function function) : thread_(std::move(function)) {}
    Worker(const Worker &) = delete;
    Worker &operator=(const Worker &) = delete;
    ~Worker() {
        if (thread_.joinable())
            thread_.join();
    }

  private:
    std::thread thread_;
};
#endif
// Counts the owning thread's updates.
struct Tick {
    int *updates;
    void on_update(double) { ++*updates; }
};
inline std::shared_ptr<const Mesh> triangle_mesh() {
    Asset asset;
    asset.nodes.resize(1);
    asset.nodes[0].name = "triangle";
    asset.materials.push_back({"surface", {1, 1, 1}, -1});
    SourcePrimitive primitive;
    primitive.material = 0;
    for (const auto position : {Vec3{-.4F, 0, 0}, Vec3{.4F, 0, 0}, Vec3{0, 1, 0}}) {
        SourceVertex vertex;
        vertex.position = position;
        vertex.normal = {0, 0, 1};
        primitive.vertices.push_back(vertex);
    }
    asset.primitives.push_back(primitive);
    return Mesh::compile(asset);
}
// Stages @p document on a worker with @p options while the owning thread updates @p scenes and calls @p during
// after each update, until @p during has returned true and the worker has finished. Returns the staged scene, or
// rethrows what staging threw.
template <class During>
StagedScene stage_while_updating(SceneSet &scenes, const std::string &document, const MeshResolver &resolve,
                                 const StagingOptions &options, During during) {
    std::promise<StagedScene> promise;
    auto result = promise.get_future();
    {
        const Worker worker([&] {
            try {
                promise.set_value(stage_scene(document, resolve, {}, options));
            } catch (...) {
                promise.set_exception(std::current_exception());
            }
        });
        for (bool released = false;;) {
            scenes.update(1. / 60);
            released = during() || released;
            if (released && result.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                break;
            std::this_thread::yield();
        }
    }
    return result.get();
}
inline void run() {
    const auto mesh = triangle_mesh();
    const MeshName name = [&](const std::shared_ptr<const Mesh> &named) {
        check(named == mesh, "Staging consumer named an unknown mesh");
        return std::string("triangle");
    };
    const MeshResolver resolve = [&](std::string_view key) {
        return key == "triangle" ? mesh : std::shared_ptr<const Mesh>{};
    };
    Scene source;
    for (int i = 0; i < 16; ++i) {
        auto root = source.create("root", mesh);
        root.transform().set_position({float(i), 0, 0});
        source.create("child").set_parent(root);
    }
    const auto document = serialize_scene(source, name);

    SceneSet scenes;
    int updates = 0;
    scenes.create("clock")->create("tick").add_component<Tick>(&updates);
    auto level = scenes.create("level");
    const auto placeholder = level->create("placeholder");

    // The first load waits in its resolver until the owning thread, after some updates, requests a stop.
    StopSource cancel;
    const MeshResolver waiting = [&](std::string_view key) {
        while (!cancel.stop_requested())
            std::this_thread::yield();
        return resolve(key);
    };
    constexpr int updates_before_cancel = 5;
    bool cancelled = false;
    try {
        (void)stage_while_updating(scenes, document, waiting, {cancel.get_token(), nullptr}, [&] {
            if (updates >= updates_before_cancel)
                cancel.request_stop();
            return cancel.stop_requested();
        });
    } catch (const StagingCancelled &) {
        cancelled = true;
    }
    check(cancelled, "Cancelled staging did not throw StagingCancelled");
    check(updates >= updates_before_cancel, "The owning thread did not update while staging ran");
    check(placeholder.valid() && level->size() == 1, "Cancelled staging changed the scene set");

    // The second load runs to completion while the owning thread reads its progress.
    StagingProgress progress;
    std::uint64_t completed = 0;
    bool increasing = true;
    const auto staged = stage_while_updating(scenes, document, resolve, {StopToken{}, &progress}, [&] {
        const auto now = progress.completed();
        increasing = increasing && now >= completed && now <= progress.total();
        completed = now;
        return true;
    });
    check(increasing, "Staging progress decreased");
    // The parse, 32 objects and one mesh key.
    check(progress.completed() == 34 && progress.total() == 34, "Staging progress did not reach its totals");
    level = scenes.replace(level, staged);
    check(!placeholder.valid() && level->size() == 32, "Committing the staged scene did not replace the member");
    const auto synchronous = load_scene(document, resolve);
    check(serialize_scene(level.get(), name) == serialize_scene(*synchronous, name) &&
              serialize_scene(level.get(), name) == document,
          "A staged scene differs from a synchronous load of the same document");
    const auto before = updates;
    scenes.update(1. / 60);
    check(updates == before + 1, "The set stopped updating after the commit");
}
} // namespace staging_test
