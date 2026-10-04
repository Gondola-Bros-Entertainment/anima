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
    // earlier record of @p path that something else keeps alive, or null. Calls for one path that find a load in
    // progress wait for it and rethrow its failure, except a StagingCancelled while @p stop, the waiting call's own
    // token, reports no stop: another caller cancelled that load, so the waiting call looks again and loads the file
    // itself when no other call has started. A failed load caches nothing, so the next call loads again. A load holds
    // up no call for another path.
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
            try {
                return loading.get();
            } catch (const StagingCancelled &) {
                if (stop.stop_requested())
                    throw;
            }
            lock.lock();
        }
        std::shared_ptr<const Mesh> resident;
        for (auto mesh = entry.meshes.rbegin(); mesh != entry.meshes.rend() && !resident; ++mesh)
            resident = mesh->lock();
        std::promise<std::shared_ptr<const T>> promise;
        entry.loading = promise.get_future().share();
        lock.unlock();
        try {
            std::shared_ptr<const T> value = std::forward<Load>(load)(std::move(resident));
            lock.lock();
            entry.loading = {};
            entry.value = value;
            std::erase_if(entry.meshes, [](const auto &mesh) { return mesh.expired(); });
            if (std::ranges::none_of(entry.meshes, [&](const auto &mesh) { return mesh.lock() == value->render; }))
                entry.meshes.push_back(value->render);
            lock.unlock();
            promise.set_value(value);
            return value;
        } catch (...) {
            if (!lock.owns_lock())
                lock.lock();
            entry.loading = {};
            lock.unlock();
            promise.set_exception(std::current_exception());
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
    struct Entry {
        std::weak_ptr<const T> value;
        // Meshes of the path's records that may be alive, oldest first. A loader that keeps the resident Mesh adds
        // none, so there are several only while a Mesh it did not keep, from an earlier version of the file, lives.
        std::vector<std::weak_ptr<const Mesh>> meshes;
        // The load in progress, if any.
        std::shared_future<std::shared_ptr<const T>> loading;
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
