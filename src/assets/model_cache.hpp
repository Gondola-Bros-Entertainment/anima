#pragma once
#include <algorithm>
#include <anima/assets/asset.hpp>
#include <anima/assets/staging.hpp>
#include <anima/mesh.hpp>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace anima::detail {
// One loaded record per model file, shared while it is alive. T holds the Mesh compiled from the file in its `render`
// member. The cache keeps only weak references, keyed by canonical path, and the loads in progress. Every member may
// run on any thread.
template <class T> class ModelCache {
  public:
    // The record of @p path: the one alive, else the one another call is loading, else @p load(resident), which runs
    // on this thread without the lock and returns a record that is not null. resident is the newest Mesh of an
    // earlier record of @p path that something else keeps alive, or null. A call for one path that finds a load in
    // progress waits for it and returns its record. A failure stays with the call that loaded, since an exception
    // object that several threads catch is freed by whichever releases it last, through a reference count in the C++
    // runtime that ThreadSanitizer cannot see. So when that load was cancelled and @p stop, the waiting call's own
    // token, reports a stop, the waiting call throws a StagingCancelled of its own; otherwise it looks again and loads
    // the file itself when no other call has started, so that it fails, if it does, with its own exception. A failed
    // load caches nothing, so the next call loads again. A load holds up no call for another path.
    template <class Load>
    std::shared_ptr<const T> load(const std::filesystem::path &path, const StopToken &stop, Load &&load) {
        std::unique_lock lock(mutex_);
        // Entries are never erased, so the reference outlives the unlocked load.
        auto &entry = entries_[path];
        for (;;) {
            if (auto value = entry.value.lock())
                return value;
            if (!entry.loading.valid())
                break;
            const auto loading = entry.loading;
            lock.unlock();
            const Outcome &outcome = loading.get();
            if (outcome.value)
                return outcome.value;
            if (outcome.cancelled && stop.stop_requested())
                throw StagingCancelled{};
            lock.lock();
        }
        std::shared_ptr<const Mesh> resident;
        for (auto mesh = entry.meshes.rbegin(); mesh != entry.meshes.rend() && !resident; ++mesh)
            resident = mesh->lock();
        std::promise<Outcome> promise;
        entry.loading = promise.get_future().share();
        lock.unlock();
        // Ends the load in progress as failed, telling the calls that wait for it whether it was cancelled.
        const auto fail = [&](bool cancelled) {
            if (!lock.owns_lock())
                lock.lock();
            entry.loading = {};
            lock.unlock();
            promise.set_value({nullptr, cancelled});
        };
        try {
            std::shared_ptr<const T> value = std::forward<Load>(load)(std::move(resident));
            lock.lock();
            entry.loading = {};
            entry.value = value;
            std::erase_if(entry.meshes, [](const auto &mesh) { return mesh.expired(); });
            if (std::ranges::none_of(entry.meshes, [&](const auto &mesh) { return mesh.lock() == value->render; }))
                entry.meshes.push_back(value->render);
            lock.unlock();
            promise.set_value({value, false});
            return value;
        } catch (const StagingCancelled &) {
            fail(true);
            throw;
        } catch (...) {
            fail(false);
            throw;
        }
    }
    // Every Mesh of a record of this cache that something else keeps alive, once each, in no particular order.
    [[nodiscard]] std::vector<std::shared_ptr<const Mesh>> resident_meshes() const {
        const std::scoped_lock lock(mutex_);
        std::vector<std::shared_ptr<const Mesh>> result;
        for (const auto &[path, entry] : entries_) {
            (void)path;
            for (const auto &mesh : entry.meshes)
                if (auto render = mesh.lock())
                    result.push_back(std::move(render));
        }
        return result;
    }

  private:
    // How a load ended: its record, or null and whether it was cancelled. It carries no exception, so that none
    // reaches more than one thread.
    struct Outcome {
        std::shared_ptr<const T> value;
        bool cancelled = false;
    };
    struct Entry {
        std::weak_ptr<const T> value;
        // Meshes of the path's records that may be alive, oldest first. A loader that keeps the resident Mesh adds
        // none, so there are several only while a Mesh it did not keep, from an earlier version of the file, lives.
        std::vector<std::weak_ptr<const Mesh>> meshes;
        // The load in progress, if any.
        std::shared_future<Outcome> loading;
    };
    mutable std::mutex mutex_;
    std::map<std::filesystem::path, Entry> entries_;
};
// The Mesh for a model imported again as @p source: @p resident when it is not null, accepts @p source as an animation
// source (the same node names, parents and rest pose) and has as many textures, else @p source compiled with
// @p texel_retention. Throws as Mesh::accepts_animation_source and Mesh::compile do.
[[nodiscard]] inline std::shared_ptr<const Mesh>
resident_or_compile(const Asset &source, std::shared_ptr<const Mesh> resident, TexelRetention texel_retention) {
    if (resident && resident->description()->textures.size() == source.textures.size() &&
        resident->accepts_animation_source(source))
        return resident;
    return Mesh::compile(source, {.texel_retention = texel_retention});
}
} // namespace anima::detail
