# Action input

`anima::core` provides platform-independent action maps and per-context state.
`anima::assets` adds configuration serialization and Scene/prefab components.
Optional `anima::input_sdl` converts SDL events without a graphics or SDL runtime
dependency. Games supply action names, bindings, device assignment and responses;
the engine never moves characters, invokes game commands or decides authority.

```cpp
#include <anima/input.hpp>
namespace i = anima::input;
i::Context controls({
    {"move", i::ActionType::vector2, {
        {{i::ControlKind::key, 7}, i::Channel::x, 1},  // SDL physical D
        {{i::ControlKind::key, 4}, i::Channel::x, -1}, // SDL physical A
        {{i::ControlKind::key, 26}, i::Channel::y, 1},
        {{i::ControlKind::key, 22}, i::Channel::y, -1}}},
    {"interact", i::ActionType::button, {{{i::ControlKind::key, 44}}}},
    {"save", i::ActionType::button, {
        {{i::ControlKind::key, 22}, i::Channel::x, 1, 0,
         {{i::ControlKind::key, 224}}}}} // SDL physical Left Ctrl+S
});
controls.begin_frame();
controls.process({i::EventType::control, {i::ControlKind::key, 44, 0}, 1});
const auto interact = controls.state("interact");
// The consuming game decides what to do with interact.pressed.
```

## Values, transitions and limits

A map contains up to 128 uniquely named actions, each with up to 32 bindings.
Names are 1–128 printable ASCII characters without spaces. Button, scalar-axis
and two-axis actions share the same input events. A binding selects a key, mouse
button, gamepad button or gamepad axis, plus its device, X/Y channel, signed scale
and axial deadzone. It can also require up to four digital modifier controls.
Y bindings require a two-axis action. An empty binding list is valid and leaves
that action inactive.

Control codes are bounded to 0–511 for keys, 1–32 for mouse buttons, 0–63 for
pad buttons and 0–15 for pad axes. Core code has no SDL types; an adapter chooses
its code convention. The SDL adapter uses physical scancodes and SDL button/axis
indices. Text and keyboard layout interpretation are separate from physical actions.

Digital events have value 0 or 1, axes [-1,1]. Values must be finite. Each binding
remaps the signed magnitude outside its deadzone to [0,1], then applies its scale
[-16,16]. Deadzone is [0,1). Axes sum contributions, clamp each channel to [-1,1],
and normalize a two-axis value if its length exceeds one. Buttons take the maximum
positive binding contribution and expose binary 0/1 at their threshold; negate a
binding scale to trigger on the negative side of an analog axis. The activity
threshold is [0.001,1], default 0.5. Axis activity uses vector magnitude, while
its reported value remains continuous below the activity threshold.

`begin_frame` clears only `pressed`, `released` and `canceled`. Feed every event
in order before reading action state. Edges latch across that interval, so a
press and release in one event pump reports both even when the final value is
zero. Duplicate held values do not generate new presses. Action reads return
copies and unknown names throw. Copies of a Context have independent definitions,
physical values and edge latches; contexts are used on one caller thread.

The context retains only nonzero physical controls observed by its primary
bindings or modifiers, with a maximum of 1024 active physical controls. Invalid
events and capacity exhaustion reject before changing accepted state. It starts no threads and reads
no OS devices or clocks. Applications choose frame/fixed-step consumption; do not
replay the same pressed edge in every catch-up simulation tick unintentionally.

## Devices, focus and rebinding

Bindings may select a concrete uint32 device ID or `any_device`. Events require
concrete IDs; zero can represent an unknown or virtual device. Device IDs are
separate for keyboard, mouse and gamepad classes. A wildcard binding takes the
greatest absolute value of its matching controls whose modifiers are satisfied;
ties use the lowest device ID.
Separate keyboards retain independent pressed state, so releasing one does not
release a key still held on another. A disconnect clears only that device class
and ID, including both gamepad buttons and axes, then reevaluates remaining input.

A chord contributes only while every modifier is held. Modifiers may be keys,
mouse buttons or gamepad buttons; axes cannot be modifiers. Repeating a control's
kind/code within one binding, including its primary control, is rejected. Pressing
the modifier before or after the primary control has the same result. Releasing
a required modifier releases the action when no other eligible source keeps it
active; pressing it again can trigger another press while the primary remains held.
Chords do not consume events or suppress other actions bound to their controls.

All controls from the same device class in a chord must come from one device.
For example, one gamepad's shoulder button cannot enable another gamepad's axis.
An explicit device ID on either the primary or a modifier fixes that class's
device; conflicting explicit IDs are invalid. Wildcards select a device that
satisfies every control of that class. Different classes select independently,
so a keyboard modifier can qualify a mouse button even when their IDs differ.
For analog primaries, the greatest-magnitude choice is made among eligible
devices: an unqualified stronger axis does not mask a qualified weaker one.

Selectors are application-owned configuration, not persistent hardware identity.
SDL instance IDs can change across sessions. Authored maps should normally use
wildcards; select the current device after enumeration/pairing when needed. The
engine does not assign devices to players or maintain user accounts/control schemes.

Focus loss, disabling or explicit `cancel()` clear physical values and pending
presses, release active actions and latch `canceled` for those actions. Input is
ignored while disabled or unfocused. Resuming requires fresh input; held state is
not resurrected automatically. Set the initial focus from the owning window before
dispatching input. Focus events still update focus while a context is disabled.

`rebind(name, bindings)` validates a replacement before publication. Success
cancels the whole context, requiring fresh input under the new configuration;
failure preserves its old bindings and state. This avoids an old held control
remaining stuck or a newly selected held control immediately triggering an action.
Binding capture UI, conflict decisions and persistence file IO belong to the caller.

## Scene and configuration

```cpp
#include <anima/input_scene.hpp>
auto object = scene.create("input context");
object.add_component<i::ActionInput>(map);
anima::ComponentCodecs codecs;
i::add_component_codec(codecs);

i::begin_frame(scene);
i::dispatch(scene, event);
const auto state = object.get_component<i::ActionInput>()->context().state("interact");
```

The driver reconciles component enablement at `begin_frame` and each `dispatch`.
Disabling cancels the context; a later enable does not revive ignored controls.
Dispatch stages every context before publishing changes, so a later capacity or
validation failure does not partially deliver the event to earlier components.
Both functions also accept a `SceneSet`; dispatch stages all its scenes together,
and `begin_frame` clears their edges once before the shared event pump. Drivers
require idle scenes and reject component callbacks, construction and set mutation.
See [runtime ordering](runtime-lifecycle.md).
No Scene callbacks, frame phases or game callbacks are invoked. Direct access to
`context()` is available; callers using it directly own enablement/scheduling.

`serialize_map` / `deserialize_map` use the current version-2 configuration format
with strict field/type/count validation and a 1 MiB bound on both input and output.
Each binding includes a `modifiers` array, including when it is empty. The
`anima.action-input.v2` component codec stores that configuration; Scene persistence
stores component enablement. It stores no live held values, focus, edge latches or
OS device handles. Instances own independent maps and state, and malformed prefab
components participate in rollback. Use these functions for binding profiles or an
explicit [asset importer](asset-reimport.md), with file IO chosen by the application.
Version-1 configurations and component identifiers are rejected; the engine does
not retain compatibility readers. Scene and prefab envelope versions are unchanged.

## SDL integration

Enable `ANIMA_BUILD_INPUT_SDL=ON` and point `ANIMA_INPUT_SDL_INCLUDE_DIR` at the SDK
include directory containing `SDL3/SDL_events.h`. SDL 3.2+ headers suffice; the
adapter links only `anima::core`. The application supplies and links its own SDL
runtime and must use compatible headers. No SDL package, Vulkan, assets, UI or
physics dependency is configured by a core-only build. An input-only consumer
can convert injected SDL events with no window, device or SDL library at all.

```cpp
#include <anima/input_sdl.hpp>
#include <SDL3/SDL.h>
controls.begin_frame();
SDL_Event event;
while (SDL_PollEvent(&event)) {
    if (auto converted = i::from_sdl(event, SDL_GetWindowID(window)))
        controls.process(*converted);
    // Other application/renderer/UI event handling follows its own policy.
}
```

The target window ID must be nonzero. Key, mouse-button and focus events are
filtered to that window. Keyboard repeats and unknown scancodes are ignored;
scancodes use the physical-key field documented by [SDL](https://wiki.libsdl.org/SDL3/SDL_KeyboardEvent).
The adapter normalizes the asymmetric signed [axis endpoints](https://wiki.libsdl.org/SDL3/SDL_GamepadAxisEvent)
to exactly -1 and +1. Gamepad removal/remapping and keyboard/mouse removal clear
corresponding device state. Gamepad events have no window ID; Context focus gates
their delivery. The application still opens/closes gamepads through SDL and feeds
events from its single existing pump. Device addition alone does not open a device.

Text/IME, touch-emulated mouse input, pointer motion/wheel, raw joystick events,
haptics and sensor inputs are not converted. UI capture should explicitly
cancel/disable gameplay contexts while continuing focus/disconnect delivery;
silently dropping selected key-up events can leave application-managed input stuck.
There is no event consumption/priority stack or automatic UI focus arbitration.

## Qualification

`input_actions` covers taps, repeats, opposition/diagonal values, deadzones, chords,
device isolation/removal, focus, cancellation, rebinding and capacity. `input_scene`
covers configuration, independent prefab state, enablement and transactional
failure. `input_sdl` exercises conversion using injected events without any OS
initialization. Independent copied core, assets and input-only SDL consumers use
public targets. Use the [local qualification workflow](local-qualification.md);
actual device discovery, physical gamepad behavior and input latency need separate
hardware validation.
