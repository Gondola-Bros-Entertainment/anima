#pragma once
#include <anima/scene.hpp>
#include <concepts>
#include <functional>
#include <utility>

/// @file
/// Component handles and explicit component persistence. Part of the `anima::assets` target;
/// scene.hpp includes this header.

namespace anima {
namespace detail {
enum class ComponentPhase { frame, fixed, late };
struct ComponentValue {
    virtual ~ComponentValue() = default;
    virtual void *address() noexcept = 0;
    virtual void update(ComponentPhase phase, double seconds) = 0;
    virtual void activate(bool active) noexcept = 0;
};
template <class T> struct ComponentBox final : ComponentValue {
    T value;
    template <class... Args> static T construct(GameObject object, Args &&...args) {
        // Use list initialization for aggregates: older supported Apple Clang
        // versions do not implement C++20 parenthesized aggregate construction.
        // Test the same syntax we use, including optional owner injection.
        if constexpr (std::is_aggregate_v<T>) {
            if constexpr (requires { T{object, std::forward<Args>(args)...}; })
                return T{std::move(object), std::forward<Args>(args)...};
            else
                return T{std::forward<Args>(args)...};
        } else if constexpr (std::constructible_from<T, GameObject, Args...>)
            return T(std::move(object), std::forward<Args>(args)...);
        else
            return T(std::forward<Args>(args)...);
    }
    template <class... Args>
    ComponentBox(GameObject object, Args &&...args)
        : value(construct(std::move(object), std::forward<Args>(args)...)) {}
    void *address() noexcept override { return &value; }
    void activate(bool active) noexcept override {
        if (active) {
            if constexpr (requires {
                              { value.on_enable() } -> std::same_as<void>;
                          }) {
                static_assert(noexcept(value.on_enable()), "on_enable must be noexcept");
                value.on_enable();
            }
        } else {
            if constexpr (requires {
                              { value.on_disable() } -> std::same_as<void>;
                          }) {
                static_assert(noexcept(value.on_disable()), "on_disable must be noexcept");
                value.on_disable();
            }
        }
    }
    void update(ComponentPhase phase, double seconds) override {
        if (phase == ComponentPhase::frame) {
            if constexpr (requires {
                              { value.on_update(seconds) } -> std::same_as<void>;
                          })
                value.on_update(seconds);
        } else if (phase == ComponentPhase::fixed) {
            if constexpr (requires {
                              { value.on_fixed_update(seconds) } -> std::same_as<void>;
                          })
                value.on_fixed_update(seconds);
        } else {
            if constexpr (requires {
                              { value.on_late_update(seconds) } -> std::same_as<void>;
                          })
                value.on_late_update(seconds);
        }
    }
};
struct ComponentRecord {
    GameObject object;
    bool attached{}, enabled = true;
    bool lifecycle_active{}, enabling{};
    std::unique_ptr<ComponentValue> value;
    std::shared_ptr<ComponentRecord> retired_next;
    explicit ComponentRecord(GameObject owner) : object(std::move(owner)) {}
    void disable() noexcept {
        if (!lifecycle_active)
            return;
        lifecycle_active = false;
        // Self-removal in on_enable finishes that callback before on_disable.
        if (!enabling)
            value->activate(false);
    }
    void enable() noexcept {
        lifecycle_active = true;
        enabling = true;
        value->activate(true);
        enabling = false;
        if (!lifecycle_active)
            value->activate(false);
    }
};
} // namespace detail

/// Checked, non-owning identity of one component attachment.
///
/// Removing the component, or destroying its object or scene, invalidates the handle for good,
/// even if a new `T` is attached later. Every access except valid() and `operator bool` then throws
/// `std::out_of_range`.
template <class T> class ComponentRef {
  public:
    ComponentRef() = default;
    /// Whether the attachment still exists.
    [[nodiscard]] bool valid() const noexcept {
        const auto record = record_.lock();
        return record && record->attached && record->object.valid();
    }
    explicit operator bool() const noexcept { return valid(); }
    /// Object the component is attached to.
    [[nodiscard]] GameObject object() const { return lock()->object; }
    /// Authored enabled flag; for a MeshRenderer, its visibility.
    [[nodiscard]] bool enabled() const { return lock()->enabled; }
    /// Whether the component participates: enabled, on an object active in the hierarchy.
    [[nodiscard]] bool active() const {
        const auto record = lock();
        return record->enabled && record->object.active_in_hierarchy();
    }
    /// Sets the authored enabled flag. Disabling stops the component's hooks at once, and
    /// `on_disable()` or `on_enable()` follows at the next lifecycle reconciliation when active()
    /// changed. For a MeshRenderer this sets its visibility; disabling an ObjectTransform throws
    /// `std::logic_error`.
    void set_enabled(bool enabled) {
        auto record = lock();
        if constexpr (std::same_as<T, ObjectTransform>) {
            if (!enabled)
                throw std::logic_error("The GameObject transform cannot be disabled");
        } else if constexpr (std::same_as<T, MeshRenderer>) {
            static_cast<T *>(record->value->address())->set_visible(enabled);
        } else {
            record->enabled = enabled;
        }
    }
    /// Borrowed reference to the value; do not keep it across scene or component changes.
    T &get() const { return *static_cast<T *>(lock()->value->address()); }
    /// Same as get().
    T &operator*() const { return get(); }
    /// Keeps the component value alive until the end of the full expression.
    class Access {
      public:
        explicit Access(std::shared_ptr<detail::ComponentRecord> record) : record_(std::move(record)) {}
        T *operator->() const { return static_cast<T *>(record_->value->address()); }

      private:
        std::shared_ptr<detail::ComponentRecord> record_;
    };
    /// Member access that pins the value through the full expression, so a member function may
    /// remove its own component or destroy its object.
    Access operator->() const { return Access(lock()); }

  private:
    friend class GameObject;
    explicit ComponentRef(const std::shared_ptr<detail::ComponentRecord> &record) : record_(record) {}
    std::shared_ptr<detail::ComponentRecord> lock() const {
        auto record = record_.lock();
        if (!record || !record->attached || !record->object.valid())
            throw std::out_of_range("Expired component handle");
        return record;
    }
    std::weak_ptr<detail::ComponentRecord> record_;
};

template <class T, class... Args> ComponentRef<T> GameObject::add_component(Args &&...args) {
    static_assert(std::same_as<T, std::remove_cvref_t<T>>, "Component type must be an unqualified value type");
    if constexpr (std::same_as<T, ObjectTransform>) {
        throw std::logic_error("Every GameObject already has a transform");
    } else if constexpr (std::same_as<T, MeshRenderer>) {
        (void)add_mesh(std::forward<Args>(args)...);
        return get_component<T>();
    } else {
        auto &owner = scene();
        const std::type_index type = typeid(T);
        auto record = std::make_shared<detail::ComponentRecord>(*this);
        // Reserve the type before invoking user construction to reject recursive adds.
        if (!owner.slot(id_).components.emplace(type, record).second)
            throw std::logic_error("GameObject already has this component type");
        struct Construction {
            std::size_t &depth;
            explicit Construction(std::size_t &value) : depth(value) { ++depth; }
            ~Construction() { --depth; }
        } construction(owner.constructing_);
        try {
            auto value = std::make_unique<detail::ComponentBox<T>>(*this, std::forward<Args>(args)...);
            if (!valid() || scene().component(id_, type) != record)
                throw std::logic_error("Component owner or attachment was removed during construction");
            record->value = std::move(value);
            record->attached = true;
        } catch (...) {
            if (valid() && scene().component(id_, type) == record)
                scene().detach_component(id_, type);
            throw;
        }
        return ComponentRef<T>(record);
    }
}
template <class T> ComponentRef<T> GameObject::get_component() const {
    static_assert(std::same_as<T, std::remove_cvref_t<T>>, "Component type must be unqualified");
    return ComponentRef<T>(scene().component(id_, typeid(T)));
}
template <class T> bool GameObject::has_component() const { return get_component<T>().valid(); }
template <class T> bool GameObject::remove_component() {
    static_assert(std::same_as<T, std::remove_cvref_t<T>>, "Component type must be unqualified");
    if constexpr (std::same_as<T, ObjectTransform>) {
        throw std::logic_error("The GameObject transform cannot be removed");
    } else if constexpr (std::same_as<T, MeshRenderer>) {
        const bool present = has_renderer();
        remove_mesh();
        return present;
    } else {
        return scene().detach_component(id_, typeid(T));
    }
}
template <class T> std::vector<ComponentRef<T>> Scene::components() {
    std::vector<ComponentRef<T>> result;
    for (std::size_t i = 0; i < slots_.size(); ++i)
        if (slots_[i].alive) {
            auto found = object({owner_, slots_[i].generation, i}).get_component<T>();
            if (found)
                result.push_back(found);
        }
    return result;
}

/// Persisted state of one component.
struct ComponentData {
    /// Key of the codec that wrote the payload; see ComponentCodecs::add.
    std::string type;
    /// Opaque UTF-8 payload defined by the codec.
    std::string state;
    /// The component's authored enabled flag.
    bool enabled = true;
};
/// Immutable mapping between ObjectKey values and live objects for one persistence operation.
///
/// Codecs translate every object link through it rather than storing Scene::Id values, names,
/// pointers or raw keys, so that links remap when loaded. It holds weak, checked handles and never
/// refreshes. Scene, prefab and scene-set operations build the correctly scoped mapping; build one
/// directly only for explicit ComponentCodecs::capture and ComponentCodecs::restore calls, using an
/// empty mapping for components without links.
class ObjectReferences {
  public:
    /// One document key and the object it maps to.
    struct Entry {
        ObjectKey key;
        GameObject object;
    };
    /// Empty mapping, which rejects every link except null.
    ObjectReferences() = default;
    /// Maps each of @p objects to its own GameObject::key. Throws `std::out_of_range` for an invalid
    /// object and `std::invalid_argument` for a repeated object or key.
    explicit ObjectReferences(std::span<const GameObject> objects);
    /// Maps each entry's key to its object. Throws `std::invalid_argument` for a null key, an invalid
    /// object, or a repeated key or object.
    explicit ObjectReferences(std::span<const Entry> entries);
    /// Key of @p object, or null for a default handle. Throws `std::invalid_argument` for a stale
    /// object or one outside the mapping; clear links to destroyed objects before capturing.
    [[nodiscard]] ObjectKey key(GameObject object) const;
    /// Object mapped to @p key, or a default handle for null. Throws `std::invalid_argument` when
    /// the key is not mapped or its object no longer exists.
    [[nodiscard]] GameObject resolve(ObjectKey key) const;

  private:
    void add(ObjectKey key, GameObject object);
    std::map<ObjectKey, GameObject> objects_;
    std::map<Scene::Id, ObjectKey> keys_;
};
/// Registry of persistence adapters for component types, with no reflection or global
/// registration.
///
/// Documents store ObjectTransform and MeshRenderer state natively; every other component needs a
/// codec, or capture fails rather than silently dropping it. Encoders must only read, and decoders
/// may attach components only to the object they receive; other components may not be decoded yet,
/// so look linked objects' components up after loading. A copy keeps the callbacks' bindings to
/// services such as a physics world or mixer rather than creating new services. Scene operations
/// borrow a registry for one call; a Prefab keeps its own copy.
class ComponentCodecs {
  public:
    /// Registers a codec for component type `T` under @p key, a stable name such as
    /// `anima.camera.v1` of 1 to 4,096 bytes that documents store as ComponentData::type.
    ///
    /// @p encode is called as `encode(const T &, const ObjectReferences &)` and returns the payload.
    /// @p decode is called as `decode(GameObject, std::string_view payload, const ObjectReferences &)`
    /// and must attach a `T` to that object; the registry then applies ComponentData::enabled.
    /// Throws `std::invalid_argument` for an invalid key or when `T` or @p key is already
    /// registered. `T` cannot be ObjectTransform or MeshRenderer.
    template <class T, class Encode, class Decode> void add(std::string key, Encode encode, Decode decode) {
        static_assert(!std::same_as<T, ObjectTransform> && !std::same_as<T, MeshRenderer>,
                      "Native transform/renderer data has a dedicated scene representation");
        if (key.empty() || key.size() > 4096)
            throw std::invalid_argument("Invalid component codec key");
        for (const auto &[type, codec] : codecs_)
            if (type == typeid(T) || codec.key == key)
                throw std::invalid_argument("Duplicate component codec");
        Codec codec{std::move(key),
                    [encode = std::move(encode)](GameObject object, const ObjectReferences &references) {
                        auto component = object.get_component<T>();
                        auto access = component.operator->();
                        const auto &value = std::as_const(*access.operator->());
                        return ComponentData{{}, encode(value, references), component.enabled()};
                    },
                    [decode = std::move(decode)](GameObject object, const ComponentData &data,
                                                 const ObjectReferences &references) {
                        decode(object, std::string_view(data.state), references);
                        auto component = object.get_component<T>();
                        if (!component)
                            throw std::invalid_argument("Component decoder did not attach its declared type");
                        component.set_enabled(data.enabled);
                    }};
        codecs_.emplace(typeid(T), std::move(codec));
    }
    /// Encodes every component of @p object except ObjectTransform and MeshRenderer, sorted by type
    /// key. Throws `std::invalid_argument` when a component has no codec.
    [[nodiscard]] std::vector<ComponentData> capture(GameObject object, const ObjectReferences &references) const;
    /// Checks that @p data names only registered types, each at most once, without decoding. Throws
    /// `std::invalid_argument` otherwise.
    void validate(std::span<const ComponentData> data) const;
    /// Validates @p data, then decodes each entry in order onto @p object. Throws
    /// `std::invalid_argument` when a decoder does not attach its type. Components decoded before a
    /// failure stay attached; scene and prefab operations roll back by destroying their objects.
    void restore(GameObject object, std::span<const ComponentData> data, const ObjectReferences &references) const;

  private:
    struct Codec {
        std::string key;
        std::function<ComponentData(GameObject, const ObjectReferences &)> encode;
        std::function<void(GameObject, const ComponentData &, const ObjectReferences &)> decode;
    };
    std::map<std::type_index, Codec> codecs_;
};
} // namespace anima
