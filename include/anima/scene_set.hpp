#pragma once
#include <anima/prefab.hpp>

namespace anima {
namespace detail {
struct SceneRecord {
    std::string key;
    std::shared_ptr<Scene> scene;
    bool attached{};
};
} // namespace detail

// Explicit namespace plus scene-local key. Lookup is intentional; runtime handles
// never rebind when a scene with the same namespace is loaded again.
struct SceneAddress {
    std::string scene;
    ObjectKey object;
    bool operator==(const SceneAddress &) const = default;
};

// Weak identity of one loaded scene. Unloading/replacing invalidates all copies.
class SceneRef {
  public:
    SceneRef() = default;
    [[nodiscard]] bool valid() const noexcept;
    explicit operator bool() const noexcept { return valid(); }
    [[nodiscard]] std::string key() const;
    // Borrowed until unload/replacement. Never retain across membership mutation.
    [[nodiscard]] Scene &get() const;
    class Access {
      public:
        explicit Access(std::shared_ptr<detail::SceneRecord> record) : record_(std::move(record)) {}
        Scene *operator->() const { return record_->scene.get(); }

      private:
        std::shared_ptr<detail::SceneRecord> record_;
    };
    Access operator->() const { return Access(lock()); }
    // Read-only renderer ownership cannot delay unloading. A retained view becomes
    // permanently empty when this scene unloads; it never selects a replacement.
    [[nodiscard]] std::shared_ptr<const Scene> render_scene() const;

  private:
    friend class SceneSet;
    explicit SceneRef(const std::shared_ptr<detail::SceneRecord> &record) : record_(record) {}
    std::shared_ptr<detail::SceneRecord> lock() const;
    std::weak_ptr<detail::SceneRecord> record_;
};

// Owns multiple independently loaded scenes. Single-threaded; membership changes
// occur between updates and draws. Callers drive set phases and systems explicitly.
class SceneSet {
  public:
    SceneSet() = default;
    ~SceneSet();
    SceneSet(const SceneSet &) = delete;
    SceneSet &operator=(const SceneSet &) = delete;
    [[nodiscard]] SceneRef create(std::string key);
    [[nodiscard]] SceneRef load(std::string key, std::string_view document, const MeshResolver &resolve,
                                const ComponentCodecs &codecs = {});
    // Stage the complete replacement first; failure preserves membership, active
    // selection and old objects. Success keeps the namespace and enumeration slot.
    [[nodiscard]] SceneRef replace(SceneRef target, std::string_view document, const MeshResolver &resolve,
                                   const ComponentCodecs &codecs = {});
    void unload(SceneRef scene);
    void clear();
    [[nodiscard]] std::size_t size() const noexcept { return scenes_.size(); }
    [[nodiscard]] std::vector<SceneRef> scenes() const;
    // One snapshot across the set. All frame hooks precede all late hooks;
    // additions/activations during callbacks wait until the next call.
    void update(double seconds);
    void fixed_update(double seconds);
    void synchronize_lifecycle();
    template <class T> [[nodiscard]] std::vector<ComponentRef<T>> components() const {
        std::vector<ComponentRef<T>> result;
        for (const auto &record : scenes_) {
            auto values = record->scene->components<T>();
            result.insert(result.end(), values.begin(), values.end());
        }
        return result;
    }
    // Explicit snapshot of renderer selection. Replacement never retargets it.
    [[nodiscard]] std::vector<std::shared_ptr<const Scene>> render_scenes() const;
    [[nodiscard]] SceneRef find(std::string_view key) const noexcept;
    [[nodiscard]] SceneRef active() const noexcept;
    // Selection is a caller default, not activation, simulation or rendering policy.
    void set_active(SceneRef scene);
    [[nodiscard]] SceneAddress address(GameObject object) const;
    [[nodiscard]] GameObject find(const SceneAddress &address) const noexcept;

  private:
    friend struct detail::SceneDriver;
    class Mutation;
    std::size_t index(SceneRef scene) const;
    void check_key(std::string_view key) const;
    SceneRef append(std::string key, std::shared_ptr<Scene> scene);
    void retire_all() noexcept;
    void run_components(double seconds, bool fixed, bool lifecycle_only = false);
    std::vector<std::shared_ptr<detail::SceneRecord>> scenes_;
    std::weak_ptr<detail::SceneRecord> active_;
    bool mutating_{};
};
} // namespace anima
