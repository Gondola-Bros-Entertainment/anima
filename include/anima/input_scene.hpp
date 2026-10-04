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
    /// The owned context, which lives as long as the component. begin_frame() and dispatch() set its enablement from
    /// the component's activity.
    [[nodiscard]] Context &context() { return context_; }
    /// The owned context, which lives as long as the component.
    [[nodiscard]] const Context &context() const { return context_; }

  private:
    Context context_;
};
/// Encodes @p map as a version 4 JSON configuration document.
///
/// The document is an object with exactly `version` (4) and `actions`. Each action has exactly
/// `name`, `type`, `threshold` and `bindings`; each binding has exactly `kind`, `code`,
/// `identity`, `channel`, `scale`, `deadzone` and `modifiers` (an array, possibly empty); each
/// modifier has exactly `kind`, `code` and `identity`. Enumerators are stored by name, the
/// enumerator's own spelling: `type` as `"button"`, `"axis"` or `"vector2"` (ActionType), `kind`
/// as `"key"`, `"mouse_button"`, `"gamepad_button"`, `"gamepad_axis"`, `"mouse_motion"` or
/// `"mouse_wheel"` (ControlKind), and `channel` as `"x"` or `"y"` (Channel). `identity` stores a
/// Control::identity as 32 lowercase hexadecimal digits, first byte first (the text
/// `SDL_GUIDToString` writes for an SDL GUID), or null for none. A device ID names a device only
/// while it is connected, so documents store none: every control and modifier loads with
/// any_device, and a binding that must keep to one device selects it by identity.
/// Only configuration is stored, never recorded controls, focus, enablement or latches.
/// Throws `std::invalid_argument` when validate(const Map &) rejects @p map, a control or modifier
/// selects a device ID, or the document would exceed 1 MiB.
std::string serialize_map(const Map &map);
/// Decodes a version 4 configuration document of at most 1 MiB; see serialize_map().
///
/// Throws `std::invalid_argument` for malformed JSON, another version, unknown, missing or
/// duplicate fields, wrong types, out-of-range values, a `kind`, `type` or `channel` that is not
/// one of the names serialize_map() writes ("Invalid input control kind", "Invalid input action
/// type" or "Invalid input binding channel"), an identity that is not 32 lowercase hexadecimal
/// digits or is all zeros, or a map that validate(const Map &) rejects.
Map deserialize_map(std::string_view data);
/// Registers the `anima.action-input.v4` component codec, whose payload is the serialize_map()
/// document of the component's actions, so capturing a component whose bindings select a device ID
/// throws `std::invalid_argument`. The scene or prefab stores component enablement; each restored
/// component gets a new enabled, focused context with no recorded input, whose bindings match
/// devices by identity as their events arrive. Throws `std::invalid_argument` when @p codecs
/// already has a codec for ActionInput or that key.
void add_component_codec(ComponentCodecs &codecs);
/// Starts an input frame for every ActionInput of @p scene, including inactive ones: clears each
/// context's latches (see Context::begin_frame), then applies the component's activity through
/// Context::set_enabled. Call it once per frame before dispatching that frame's events. Throws
/// `std::logic_error` while the scene is updating, under construction or destroyed.
void begin_frame(Scene &scene);
/// Starts an input frame for every scene of @p scenes as begin_frame(Scene &) does. Also throws
/// `std::logic_error` while the set is busy, as SceneSet describes.
void begin_frame(SceneSet &scenes);
/// Delivers @p event through Context::process to every ActionInput of @p scene, after applying
/// each component's activity through Context::set_enabled.
///
/// Every context first checks the event and allocates what applying it needs, and the event is
/// applied to the contexts only once all have, so a rejected event, invalid, over a context's
/// capacity or overflowing a delta binding's sum, changes no context, nor does a failed
/// allocation. No context is copied, so each costs what Context::process does. Atomicity is per
/// event, not per frame. Throws `std::logic_error` while the scene is updating, under
/// construction or destroyed.
void dispatch(Scene &scene, const Event &event);
/// Delivers @p event across every scene of @p scenes as dispatch(Scene &, const Event &) does,
/// staging all of them before any changes. Also throws `std::logic_error` while the set is
/// busy, as SceneSet describes.
void dispatch(SceneSet &scenes, const Event &event);
} // namespace anima::input
