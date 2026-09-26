#pragma once
#include <anima/input.hpp>
#include <anima/scene.hpp>
#include <utility>

/// @file
/// Scene integration for anima::input: an ActionInput component, frame and event drivers, and
/// configuration persistence.
///
/// Part of `anima::assets`. Use it from the scenes' thread. The drivers run no component hooks,
/// frame phases or game callbacks and advance no time; the application chooses which scenes
/// receive input. Invalid arguments throw `std::invalid_argument` unless a member states otherwise.

namespace anima::input {
/// Component holding one Context; it carries no player identity or game commands.
///
/// begin_frame() and dispatch() disable the context of an inactive component, which cancels it,
/// and enable it again once the component is active, without reviving input held meanwhile.
/// Activity is ComponentRef::active(): component enablement and inherited object activation.
/// Callers that drive context() directly own that reconciliation.
class ActionInput {
  public:
    /// Throws `std::invalid_argument` when validate(const Map &) rejects @p map.
    explicit ActionInput(Map map = {}) : context_(std::move(map)) {}
    Context &context() { return context_; }
    const Context &context() const { return context_; }

  private:
    Context context_;
};
/// Encodes @p map as a version 2 JSON configuration document.
///
/// The document is an object with exactly `version` (2) and `actions`. Each action has exactly
/// `name`, `type`, `threshold` and `bindings`; each binding has exactly `kind`, `code`, `device`,
/// `channel`, `scale`, `deadzone` and `modifiers` (an array, possibly empty); each modifier has
/// exactly `kind`, `code` and `device`. `type`, `kind` and `channel` store enumerator values, and a
/// wildcard `device` stores any_device (4294967295). Only configuration is stored, never recorded
/// controls, focus, enablement or latches. Throws `std::invalid_argument` when
/// validate(const Map &) rejects @p map or the document would exceed 1 MiB.
std::string serialize_map(const Map &map);
/// Decodes a version 2 configuration document of at most 1 MiB; see serialize_map().
///
/// Throws `std::invalid_argument` for malformed JSON, another version, unknown, missing or
/// duplicate fields, wrong types, out-of-range values or a map that validate(const Map &) rejects.
Map deserialize_map(std::string_view data);
/// Registers the `anima.action-input.v2` component codec, whose payload is the serialize_map()
/// document of the component's actions. The scene or prefab stores component enablement; each
/// restored component gets a new enabled, focused context with no recorded input. Throws
/// `std::invalid_argument` when @p codecs already has a codec for ActionInput or that key.
void add_component_codec(ComponentCodecs &codecs);
/// Starts an input frame for every ActionInput of @p scene, including inactive ones: clears each
/// context's latches (see Context::begin_frame), then applies the component's activity through
/// Context::set_enabled. Call it once per frame before dispatching that frame's events. Throws
/// `std::logic_error` while the scene is updating, under construction or destroyed.
void begin_frame(Scene &scene);
/// Starts an input frame for every scene of @p scenes as begin_frame(Scene &) does. Also throws
/// `std::logic_error` while the set is changing.
void begin_frame(SceneSet &scenes);
/// Delivers @p event through Context::process to every ActionInput of @p scene, after applying
/// each component's activity through Context::set_enabled.
///
/// Every context is updated on a copy first and published only when all succeed, so a rejected
/// event, invalid or over a context's capacity, changes no context. Atomicity is per event, not
/// per frame. Throws `std::logic_error` while the scene is updating, under construction or
/// destroyed.
void dispatch(Scene &scene, const Event &event);
/// Delivers @p event across every scene of @p scenes as dispatch(Scene &, const Event &) does,
/// staging all of them before any changes. Also throws `std::logic_error` while the set is
/// changing.
void dispatch(SceneSet &scenes, const Event &event);
} // namespace anima::input
