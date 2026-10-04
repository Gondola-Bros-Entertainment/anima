// Stages GLB imports, mesh compilation, texture preparation, prefabs and scene documents on several threads at once
// while the owning thread updates its scenes and commits the results. In the ThreadSanitizer build
// (ANIMA_ENABLE_THREAD_SANITIZER) a data race between those calls fails the test.
#include "staging_fixture.hpp"
#include <anima/assets/asset.hpp>
#include <anima/assets/mesh_preparation.hpp>
#include <anima/prefab.hpp>
#include <anima/scene_set.hpp>
#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using namespace anima;
using namespace staging_fixture;

// Counts the owning thread's updates.
struct Ticker {
    int *ticks;
    void on_update(double) { ++*ticks; }
};
// Joins the workers even when the owning thread's part of the test throws.
struct Workers {
    std::vector<std::thread> threads;
    Workers() = default;
    Workers(const Workers &) = delete;
    Workers &operator=(const Workers &) = delete;
    ~Workers() { join(); }
    void join() {
        for (auto &thread : threads)
            if (thread.joinable())
                thread.join();
    }
};
// What one worker round staged, for the owning thread to commit.
struct Staged {
    StagedScene scene;
    StagedSceneSet set;
    Prefab prefab;
};

TEST_CASE("Loads stage on several threads while the owning thread updates and commits") {
    constexpr unsigned workers = 4, rounds = 6;
    // Four primitives alternating between two textured materials: the parse, two images and four primitives.
    const auto bytes = glb(4, 2);
    constexpr std::uint64_t glb_steps = 1 + 2 + 4;
    const Meshes meshes = compiled_meshes();
    const auto shared = meshes.find("two");
    std::atomic<int> resolved{0};
    const MeshResolver resolve = [&](std::string_view key) {
        ++resolved;
        return meshes.find(key);
    };
    const MeshName name = [&](const std::shared_ptr<const Mesh> &mesh) { return meshes.name(mesh); };
    CodecCalls calls;
    const auto codecs = counting_codecs(calls);
    Scene source;
    populate(source, meshes, 8);
    const auto scene_document = serialize_scene(source, name, codecs);
    constexpr std::uint64_t scene_steps = 1 + 16 + 2;
    SceneSet sources;
    populate(sources.create("alpha").get(), meshes, 4);
    populate(sources.create("beta").get(), meshes, 2);
    const auto set_document = sources.serialize(name, codecs);
    constexpr std::uint64_t set_steps = 1 + 12 + 2;
    const auto prefab_document = Prefab::capture(source.roots().front(), codecs).serialize(name);

    SceneSet scenes;
    auto level = scenes.load("level", scene_document, resolve, codecs);
    int ticks = 0;
    auto ticking = scenes.create("ticking");
    ticking->create("ticker").add_component<Ticker>(&ticks);
    SceneSet archive;
    archive.restore(set_document, resolve, codecs);

    std::mutex mutex;
    std::vector<Staged> results;
    std::vector<std::string> failures;
    unsigned finished = 0;
    StagingProgress progress;
    Workers threads;
    threads.threads.reserve(workers);
    for (unsigned worker = 0; worker < workers; ++worker)
        threads.threads.emplace_back([&] {
            try {
                for (unsigned round = 0; round < rounds; ++round) {
                    const auto asset = load_asset(bytes, {{}, &progress});
                    const auto mesh = Mesh::compile(*asset);
                    MeshCompileOptions options;
                    options.max_vertices = 3;
                    const auto pieces = Mesh::compile_static(*asset, options);
                    // The owning thread instantiates the shared mesh meanwhile.
                    const MeshPreparation prepared_shared(shared), prepared_own(mesh);
                    auto prefab = Prefab::deserialize(prefab_document, resolve, codecs);
                    auto staged = stage_scene(scene_document, resolve, {}, {{}, &progress});
                    auto staged_set = stage_scene_set(set_document, resolve, {}, {{}, &progress});
                    const std::lock_guard lock(mutex);
                    if (pieces.size() != 4 || prepared_own.images().empty() || prepared_shared.mesh() != shared)
                        failures.emplace_back("A worker staged unexpected content");
                    results.push_back({std::move(staged), std::move(staged_set), std::move(prefab)});
                }
            } catch (const std::exception &error) {
                const std::lock_guard lock(mutex);
                failures.emplace_back(error.what());
            }
            const std::lock_guard lock(mutex);
            ++finished;
        });

    // Objects created in the level since it was last replaced.
    std::size_t created = 0;
    unsigned committed = 0, updates = 0;
    bool done = false;
    while (!done) {
        std::vector<Staged> batch;
        {
            const std::lock_guard lock(mutex);
            // A worker counts itself finished after its last result, under the same lock.
            done = finished == workers;
            batch.swap(results);
        }
        scenes.update(1. / 60);
        ++updates;
        (void)level->create("shared", shared);
        ++created;
        for (const auto &staged : batch) {
            level = scenes.replace(level, staged.scene, codecs);
            created = 0;
            (void)staged.prefab.instantiate(level.get());
            archive.restore(staged.set, codecs);
            (void)archive.replace(archive.find("beta"), staged.set, codecs);
            (void)serialize_scene(level.get(), name, codecs);
            ++committed;
        }
        std::this_thread::yield();
    }
    threads.join();

    std::string reported;
    for (const auto &failure : failures)
        reported += failure + '\n';
    CHECK_MESSAGE(failures.empty(), reported);
    CHECK(committed == workers * rounds);
    CHECK(ticks == static_cast<int>(updates));
    CHECK(progress.total() == workers * rounds * (glb_steps + scene_steps + set_steps));
    CHECK(progress.completed() == progress.total());
    // The last staged level holds the document's sixteen objects, one prefab instance of two objects, and those
    // created since.
    CHECK(level->size() == 16 + 2 + created);
    CHECK(archive.size() == 2);
    CHECK(resolved > 0);
}
} // namespace
