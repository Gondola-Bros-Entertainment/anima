# Runtime ownership and system ordering

Compose Anima's runtime at one application boundary. Keep service owners, one
configured persistence registry, the scene set and the clock together there.
Game components use checked object/component handles; they do not wire scene
drivers or carry codecs through their update methods.

## Ownership

| Owner | Owns | Lifetime rule |
| --- | --- | --- |
| Application/session | Optional physics worlds, mixer, UI host, scenes, clock and persistence configuration | Services outlive scenes and operations using their codecs |
| `SceneSet` | Loaded scenes and namespaces | Unload/replacement invalidates old handles; they never select replacements implicitly |
| `Scene` | Objects, hierarchy and component attachments | Teardown invalidates handles before cleanup; active calls may pin removed values until they return |
| Component | Its body, voice, document or other resource handle | Destruction releases resources, including failed loading/prefab cleanup |
| `Prefab` | Authored node data and a copy of configured codecs | Immutable resources may be shared; instances own independent mutable state |

Declare C++ members in dependency order: service owners, persistence configuration,
then scenes. Reverse destruction releases scene-owned resources before services.
Keep the session at a stable address when callbacks borrow its members. The UI
document host must also shut down before its RmlUi context/globals; `UiContext`
manages that ordering for desktop applications.

Scene drivers accept either one `Scene` or a complete `SceneSet`. The set's
`active()` scene is only a caller default; it does not filter simulation, input,
audio, navigation or UI. Use a single-scene overload for intentionally narrower
selection. Shared physics worlds and mixers need one driver call covering all
their scene bindings.

## Component phases

`Scene::update` and `SceneSet::update` reconcile lifecycle, call all `on_update`
hooks, then all `on_late_update` hooks. The set uses one participant snapshot across
every scene, so one scene's late hooks cannot precede another's frame hooks.
`fixed_update` reconciles lifecycle and calls only `on_fixed_update`; it neither
advances a clock nor steps physics. `synchronize_lifecycle` only reconciles
`on_enable`/`on_disable`.

Each call snapshots its participants. Additions and activations from callbacks
wait for a later call; removed or deactivated components skip remaining hooks.
Order between unrelated components within a phase is unspecified. Update exceptions
propagate after unlocking; earlier callback effects remain. Activation notifications
are `noexcept`, and constructors/destructors remain responsible for resource lifetime.
See [scene lifecycle](scene-objects.md).

LateUpdate is the final component phase inside `update`, not a separately callable
public phase. Drivers run between complete scene calls. There is no dependency
graph, hidden system tick or automatic draining of newly activated components.

## One application frame

This order supports components that consume navigation intent during fixed ticks
and camera/presentation components that follow the resulting poses:

```cpp
// Services, scenes and clock are persistent application-owned values.
// The fixed interval must satisfy the enabled systems' time-step limits.
const auto batch = clock.advance(elapsed);
anima::input::begin_frame(scenes);
for (const auto &event : events)
    anima::input::dispatch(scenes, event);

const auto fixed_seconds = std::chrono::duration<double>(clock.step()).count();
for (std::uint32_t tick = 0; tick < batch.steps; ++tick) {
    anima::navigation::update_agents(scenes, fixed_seconds);
    scenes.fixed_update(fixed_seconds);
    anima::physics::step(scenes, physics3, fixed_seconds);
    anima::physics2d::step(scenes, physics2, fixed_seconds);
}
scenes.update(std::chrono::duration<double>(elapsed).count());
anima::synchronize_audio(scenes, audio);
// With UI enabled, sync_ui_panels(scenes) before layout/presentation.
// Then draw the selected scenes and render/pump audio through the chosen output.
```

Link only the modules used and omit unused drivers. Input/navigation scene drivers
require only `anima::assets`; UI and either physics backend stay optional. The
[independent runtime consumer](../tests/consumer/runtime.hpp) exercises this order
with both physics backends and audio. Standalone input/navigation consumers also
exercise additive scenes without those backends.

Input edges last until the next `begin_frame`, including frames with zero or
multiple fixed ticks. Applications consuming edge-triggered commands in fixed
updates must queue them until a tick consumes them, or define another explicit
consumption policy. Reading `pressed` in every catch-up tick repeats the command;
clearing edges each tick can lose taps on frames with no tick. Continuous action
values can be sampled each tick. UI routing/capture remains application policy;
visibility synchronization alone does not choose gameplay focus.

Navigation publishes desired velocity without moving objects. A fixed component
or application motor applies movement/collision policy. Physics publishes dynamic
poses after fixed callbacks, once per shared world per tick. Audio synchronization
consumes final transforms and enablement without advancing sample time; mixing/output
advances audio separately. Clock catch-up limits, pause, time scale and render
interpolation remain explicit application policy.

## Publication and callback boundaries

Input, navigation, physics and audio validate/stage the complete selection before
publishing. A rejected later scene does not partially deliver an input event,
consume an earlier route, move an earlier body, or pause an earlier source in that
driver call. Input atomicity is per event, not the whole event pump. There is no
transaction across drivers: failed 2D physics does not rewind a completed 3D step.

Drivers reject component callbacks, incomplete component construction, nested scene
scheduling and set load/replace/clear callbacks when the driven selection is busy.
They do not invoke component lifecycle or update hooks themselves. Direct context
and world methods remain available to callers managing their own scheduling.

UI visibility is different: native show/hide operations can dispatch UI events.
`sync_ui_panels` validates every selected document first and holds scheduling and
membership guards while applying visibility. It snapshots panel membership, pins
each panel through its own event dispatch, and skips panels removed or documents
closed by earlier events. New panels wait until the next call. Handlers can mutate
objects, so their effects cannot be rolled back; nested scene phases, drivers and
set membership changes reject. Use the document host's `check_events()` to report
captured handler exceptions. The single-scene overload guards that scene; the set
overload guards all selected scenes, including an empty set's membership.

## Why codecs are explicit

`ComponentCodecs` is persistence configuration, not component registration or a
runtime system registry. Native components attach and update without codecs.
Saving/loading needs explicit adapters because the application knows stable
type/resource keys and destination service bindings.

`serialize_scene`, `load_scene` and `SceneSet::load/replace` borrow a registry for
that operation; scenes retain none. `Prefab::capture`, its constructor and
`deserialize` retain a registry copy for later `instantiate` calls. Physics codecs
borrow checked world bindings; UI codecs borrow a checked document host; audio
codecs retain their mixer context and bus. Copying a registry copies those bindings,
not new service instances.

Selecting another scene alone does not rebind a prefab's bodies, voices or
documents. Supply the destination registry explicitly when creating an instance:

```cpp
auto instance = prefab.instantiate(destination_scene, anima::identity(), destination_codecs);
```

The registry is borrowed for that call; it replaces neither the prefab's retained
registry nor its authored data. The parent-object overload accepts the same
placement and registry arguments. Both validate every serialized component type
before creating objects, remap object links within the new instance, and destroy
staged objects/resources if decoding fails. A destination override can instantiate
after the original physics world or UI host expires, provided its own bindings
are live. Components retain their normal resource lifetime requirements.

Keep persistence calls at the session boundary to avoid forwarding codecs through
gameplay code. There is no global registry or implicit destination lookup, keeping
headless applications, independent sessions and optional modules isolated.

Object-reference contexts map authored keys to one staged graph and remap prefab
links per instance. They are operation-scoped, not service locators. Cross-document
references, scene-set persistence and prefab variants/nesting remain separate
incomplete contracts.
