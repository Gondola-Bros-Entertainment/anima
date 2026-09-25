# Engine and application boundaries

Anima is a standalone C++20 engine. Reusable mechanisms belong here when an
independent consumer can configure them through public data and APIs without
importing a game's content, state or rules.

| Engine mechanism | Application responsibility |
| --- | --- |
| Objects, hierarchy, components, scheduling and prefab construction | Object composition, gameplay components and transition policy |
| Scene/component documents and explicit resource resolvers | Save slots, progression, migrations and network authority |
| Meshes, materials, lighting, visibility, synchronization and rendering | Authored appearance, world layout, quality settings and gameplay effects |
| Terrain queries, collision bodies, contacts, rays and sweeps | Movement, support eligibility, damage and collision response policy |
| Pose blending, timelines, attachments and animation evaluation | Action selection, equipment rules and gameplay outcomes |
| Audio decoding/mixing, input state and navigation queries | Sound selection, action responses, destinations and authoritative movement |
| UI documents, layout, rendering and events | Screens, game data, commands and navigation |
| Import/export and resource lifetime | Production assets, content recipes and deployment |

## Runtime and persistence

A `Scene` owns `GameObject`s with transforms and native C++ components. A `Prefab`
constructs independent object assemblies through those same APIs. `SceneSet` owns
additive scenes and explicit replacement/unload boundaries. See
[scenes and objects](scene-objects.md) for identity, activation and lifetime rules.
The [runtime lifecycle guide](runtime-lifecycle.md) explains service ownership,
component phases, coordinated drivers and persistence-registry lifetimes.
[Camera/view components](cameras.md) supply projection and explicit selection;
applications own camera movement, transitions and drawable dimensions.
[Scene lighting](lighting.md) resolves explicit directional-light links and
environment settings independently of camera and geometry selection.

JSON is the storage representation, not a second scene implementation. Scene and
prefab documents use version 3 exclusively. Whole scene sets use a version-1
envelope with namespace-qualified reference mappings and atomic replacement.
Version-1 prefab variants store a base resource key and typed overrides, resolving
to ordinary prefabs through explicit resource and codec configuration.
`ComponentCodecs` registers stable
component type keys; explicit resolver callbacks supply shared resources. Failed
decoding rolls back staged objects and owned resources. Custom codecs validate
their payloads and obey the documented mutation boundary.

Tools must use the production scene, component and rendering APIs.
Synthetic test fixtures exercise these contracts without production game assets
or rules; moving game code into a test does not make it an engine mechanism.

## Dependency boundaries

- `anima::core` requires C++20 only and includes timing, geometry queries, input,
  navigation and the in-house audio mixer.
- `anima::assets` adds resources, animation, scenes and prefabs. Importer dependencies
  are private; scene composition does not require SDL or Vulkan.
- Jolt and Box2D are independently optional through `anima::physics` and
  `anima::physics2d`. Backend types remain private. Their scene adapters also use
  `anima::assets`.
- `anima::input_sdl` converts events using SDL headers and owns no device or loop.
- `anima::audio_output` optionally sends the in-house mixer's samples to SDL audio.
  It requires no assets, Vulkan or window and owns only its SDL audio reference.
- `anima::desktop` contains SDL/Vulkan rendering. It neither enables nor links
  audio output; consumers explicitly select the modules they need.
- `anima::ui_documents` supports headless RmlUi/FreeType documents; `anima::ui`
  adds desktop integration. Native interoperability deliberately exposes RmlUi.

The copied [consumer project](../tests/consumer/CMakeLists.txt) verifies public
source consumption and optional dependency isolation with engine apps/tests off.
No target requires a consuming application checkout. See [consuming Anima](consuming-anima.md)
for CMake target configuration.

## Rendering and authoring policy

Standard materials, lights, shadows and backgrounds are engine facilities.
Reusable water, cloud and skybox modules also belong in the engine when optional,
configurable and explicit about resource/pass ownership. Applications supply world
placement, art direction, weather behavior and gameplay policy. The
[rendering architecture](rendering-architecture.md) describes the default pipeline.

Animation APIs accept caller-defined clips, masks, joints, sockets, roles and
timelines. Applications own export recipes, authoring property names and material
conversion. The runtime does not select gameplay actions or impose an application's
asset conventions.
