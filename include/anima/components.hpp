#pragma once
#include <anima/scene.hpp>
#include <concepts>
#include <cstddef>
#include <functional>
#include <type_traits>
#include <utility>

/// @file
/// Component handles and explicit component persistence. Part of the `anima::assets` target;
/// scene.hpp includes this header.

namespace anima {
/// The object a component is attached to. GameObject::add_component passes it first to a component
/// type that accepts one, as its first constructor parameter or the first member of an aggregate.
///
/// Only its explicit constructor creates one, and it converts to nothing, so the owner never
/// initializes a GameObject parameter or member, and a GameObject argument never initializes a
/// ComponentOwner. A component constructed directly rather than attached, such as a standalone
/// Animator, takes its object as `ComponentOwner{object}`.
struct ComponentOwner {
    /// Wraps @p owner, which may be any handle, including an invalid one.
    explicit ComponentOwner(GameObject owner) noexcept : object(std::move(owner)) {}
    /// The owning object.
    GameObject object;
};
namespace detail {
enum class ComponentPhase { frame, fixed, late };
// Whether Scene calls any hook of T, as ComponentBox checks each one; only these are scheduled.
template <class T>
concept ScheduledComponent = requires(T &hooked_value, double hooked_seconds) {
    { hooked_value.on_update(hooked_seconds) } -> std::same_as<void>;
} || requires(T &hooked_value, double hooked_seconds) {
    { hooked_value.on_late_update(hooked_seconds) } -> std::same_as<void>;
} || requires(T &hooked_value, double hooked_seconds) {
    { hooked_value.on_fixed_update(hooked_seconds) } -> std::same_as<void>;
} || requires(T &hooked_value) {
    { hooked_value.on_enable() } -> std::same_as<void>;
} || requires(T &hooked_value) {
    { hooked_value.on_disable() } -> std::same_as<void>;
};
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
        // Test the same syntax we use, including the optional owner.
        if constexpr (std::is_aggregate_v<T>) {
            if constexpr (requires { T{ComponentOwner{object}, std::forward<Args>(args)...}; })
                return T{ComponentOwner{std::move(object)}, std::forward<Args>(args)...};
            else
                return T{std::forward<Args>(args)...};
        } else if constexpr (std::constructible_from<T, ComponentOwner, Args...>)
            return T(ComponentOwner{std::move(object)}, std::forward<Args>(args)...);
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
    std::type_index type;
    bool attached{}, enabled = true;
    bool lifecycle_active{}, enabling{};
    // Whether the type has hooks, and the record's positions in its scene's lists while attached.
    bool scheduled{};
    std::size_t type_position{}, schedule_position{};
    std::unique_ptr<ComponentValue> value;
    std::shared_ptr<ComponentRecord> retired_next;
    ComponentRecord(GameObject owner, std::type_index kind, bool hooks)
        : object(std::move(owner)), type(kind), scheduled(hooks) {}
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
/// even if a new `T` is attached later. Every access except valid(), `operator bool` and
/// `operator==` then throws `std::out_of_range`.
template <class T> class ComponentRef {
  public:
    ComponentRef() = default;
    /// Whether the attachment still exists.
    [[nodiscard]] bool valid() const noexcept {
        const auto record = record_.lock();
        return record && record->attached && record->object.valid();
    }
    explicit operator bool() const noexcept { return valid(); }
    /// Whether @p a and @p b refer to the same attachment, or both to none: default handles and those
    /// that GameObject::get_component returned for a missing component are equal. A handle stays equal
    /// to its copies after the attachment ends, and never equals a handle to another attachment, even a
    /// later one of the same type on the same object. There is no `std::hash`: C++20 offers none for the
    /// shared ownership that equality compares.
    friend bool operator==(const ComponentRef &a, const ComponentRef &b) noexcept {
        return !a.record_.owner_before(b.record_) && !b.record_.owner_before(a.record_);
    }
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
    friend class Scene;
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
    static_assert((!std::same_as<std::remove_cvref_t<Args>, ComponentOwner> && ...),
                  "add_component passes the ComponentOwner itself");
    if constexpr (std::same_as<T, ObjectTransform>) {
        throw std::logic_error("Every GameObject already has a transform");
    } else if constexpr (std::same_as<T, MeshRenderer>) {
        (void)add_mesh(std::forward<Args>(args)...);
        return get_component<T>();
    } else {
        auto &owner = scene();
        const std::type_index type = typeid(T);
        auto record = std::make_shared<detail::ComponentRecord>(*this, type, detail::ScheduledComponent<T>);
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
            scene().index_component(record);
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
    static_assert(std::same_as<T, std::remove_cvref_t<T>>, "Component type must be unqualified");
    const auto listed_records = attached_components(typeid(T));
    std::vector<ComponentRef<T>> typed_components;
    typed_components.reserve(listed_records.size());
    for (const auto &listed_record : listed_records)
        typed_components.push_back(ComponentRef<T>(listed_record));
    return typed_components;
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
/// The GameObject links that one component stores, as its codec's link callback reports them.
///
/// SceneSet::replace and SceneSet::unload create one for each component whose codec has a link
/// callback (see ComponentCodecs::add), call the callback, and after validating the whole change
/// assign a new handle to each reported link that names an object of the scene being retired.
/// Applications cannot construct one.
class ObjectLinks {
  public:
    ObjectLinks(const ObjectLinks &) = delete;
    ObjectLinks &operator=(const ObjectLinks &) = delete;
    /// Reports @p link, a GameObject stored in the visited component, which the operation may
    /// overwrite before it returns. Report the component's own members, such as the elements of
    /// a container it owns, not copies. Throws `std::bad_alloc` when memory runs out.
    void add(GameObject &link) { links_.push_back(&link); }

  private:
    friend class ComponentCodecs;
    ObjectLinks() = default;
    std::vector<GameObject *> links_;
};
/// Registry of persistence adapters for component types, with no reflection or global
/// registration.
///
/// Documents store ObjectTransform and MeshRenderer state natively; every other component needs a
/// codec, or capture fails rather than silently dropping it. Encoders and link callbacks must only
/// read, and decoders may attach components only to the object they receive; other components may
/// not be decoded yet, so look linked objects' components up after loading. A copy keeps the
/// callbacks' bindings to services such as a physics world or mixer rather than creating new
/// services. Scene operations borrow a registry for one call; a Prefab keeps its own copy.
class ComponentCodecs {
  public:
    /// Largest codec key, in bytes.
    static constexpr std::size_t max_key_bytes = 4096;

    /// Registers a codec for component type `T` under @p key, a stable name such as
    /// `anima.camera.v1` of 1 to #max_key_bytes bytes that documents store as ComponentData::type.
    ///
    /// @p encode is called as `encode(const T &, const ObjectReferences &)` and returns the payload.
    /// @p decode is called as `decode(GameObject, std::string_view payload, const ObjectReferences &)`
    /// and must attach a `T` to that object; the registry then applies ComponentData::enabled.
    ///
    /// @p links is optional. When given, it is called as `links(T &, ObjectLinks &)` and must report
    /// every GameObject that the component stores through ObjectLinks::add, changing nothing. Given
    /// this registry, SceneSet::replace then rebinds the component's links into the replaced scene
    /// and SceneSet::unload clears its links into the unloaded one. Without it, those links expire
    /// with their scene, as every handle to it does.
    ///
    /// Throws `std::invalid_argument` for an invalid key or when `T` or @p key is already
    /// registered. `T` cannot be ObjectTransform or MeshRenderer.
    template <class T, class Encode, class Decode, class Links = std::nullptr_t>
    void add(std::string key, Encode encode, Decode decode, [[maybe_unused]] Links links = nullptr) {
        static_assert(!std::same_as<T, ObjectTransform> && !std::same_as<T, MeshRenderer>,
                      "Native transform/renderer data has a dedicated scene representation");
        if (key.empty() || key.size() > max_key_bytes)
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
                    },
                    {}};
        if constexpr (!std::is_null_pointer_v<Links>)
            codec.links = [links = std::move(links)](void *component, ObjectLinks &reported) {
                links(*static_cast<T *>(component), reported);
            };
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
    friend class SceneSet;
    struct Codec {
        std::string key;
        std::function<ComponentData(GameObject, const ObjectReferences &)> encode;
        std::function<void(GameObject, const ComponentData &, const ObjectReferences &)> decode;
        // Empty when the codec was registered without a link callback.
        std::function<void(void *, ObjectLinks &)> links;
    };
    struct CollectedLinks {
        const std::string *key{};
        std::vector<GameObject *> links;
    };
    // The links that the link callback of @p type reports for the component value at @p component,
    // with that codec's key; a null key when @p type has no codec with a link callback.
    CollectedLinks collect_links(std::type_index type, void *component) const;
    std::map<std::type_index, Codec> codecs_;
};
} // namespace anima
