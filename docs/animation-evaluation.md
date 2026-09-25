# Independent motion and actor evaluation

`anima::assets` supplies anatomy-independent presentation primitives. The game
owns rig recipes, class/skill selection, item catalogs and gameplay authority.

## Resources and binding

`load_asset` loads renderable GLB resources. `load_motion_asset` loads independent
node/animation resources and rejects render geometry, skins, materials and
textures. `MotionRuntime` verifies node identity, ancestry and bind compatibility
before transferring motion onto a model. `pose_from_local` rebuilds the complete
model hierarchy, including unkeyed descendants.

## Local-pose transitions

`blend_pose(asset, from, to, weight)` is a pure operation on two local poses for
one asset. The weight must be finite and between zero and one; both local arrays
must match the asset's node count. The caller owns asset/pose identity. Source
poses are unchanged, and source world arrays are ignored because they are derived.

Translation and scale use linear interpolation. Joint rotation uses normalized,
shortest-arc quaternion interpolation. Matrix-authored nodes retain their fixed
rest matrices. The result rebuilds the hierarchy with the same parent/cycle and
finite-matrix checks used by `sample_pose`. Consumers publish the resulting pose
once; they do not blend skinned vertex arrays or world-space joint matrices.

The asset manifest optionally accepts positive finite `reference_speed` on a
clip, in asset units per second (the current manifest contract uses metres).
Absence means the clip has no authored travel speed. A consumer
can advance an in-place travel clip by `elapsed * actual_speed / reference_speed`.
This metadata does not choose gameplay movement speed, stamina, gait or attack
rules. The engine does not select a transition duration or a game class.

Tests cover endpoints, intermediate poses, quaternion sign equivalence, matrix
nodes, invalid weights/counts, absent travel metadata and invalid speeds.

## Pose composition and contacts

`EvaluationRig` maps explicit semantic joints to the model's skin. Its evaluation
hierarchy may differ from the storage hierarchy: existing flattened exports can
retain their bind and geometry. Affine locals preserve scale/shear; rotation
blends use shortest-arc quaternions and symmetric stretch. Singular transforms
and reflections are rejected.

`layer` applies a checked mask as an override or relative to an additive reference.
Additive order is `base * blend(identity, inverse(reference) * layer, weight)`.
`render_pose` maps the result back to world transforms and updates unmapped
children. Its world-only result is deliberately unsuitable for the TRS-only
`blend_pose` operation.

`solve_contact` rotates a declared two-bone chain toward a model-space target and
pole, with angle limits, optional end orientation and a weight. It preserves limb
lengths and reports reachability/error. The consumer declares solve order and
resolves competing contacts; this is not automatic anatomy inference.

## Actions and coordinated actors

`ActionTimeline` samples named timed/held phases from caller-supplied elapsed and
release times. Cues use an explicit cursor; seeking does not create catch-up
bursts. Timelines are presentation clocks and never apply gameplay damage.

`PhaseTrack` maps shared interaction progress onto each actor's authored timing.
Socket frames, directed placement dependencies and model-space contact transforms
support independently animated participants. Attachment alignment transfers
position/rotation without resizing the rider when a parent joint is scaled.
The game owns collision, navigation, interruptions, inventory and network policy.

## Presentation APIs

All the following are compiled implementations exposed by `anima::assets`. The
public API uses engine types and UTF-8 JSON document strings; JSON parser headers
and the parser's dependency remain private. Documents are bounded to 4 MiB and
64 nesting levels, and duplicate/unknown fields are rejected.

| Public header | Responsibility |
| --- | --- |
| `motion_runtime.hpp` | Load a v3 independent motion contract; bind declared joints/masks/chains; compose disjoint carry layers; apply layers, local offsets and contacts |
| `action_runtime.hpp` | Bind a v1 action document to motion; evaluate phase layers, animated prop tracks and contact weights; validate caller timing |
| `attachments.hpp` | Parse v2 attachment catalogs and checked socket frames; share asset resources; prepare named role sets; validate carry/contact ownership and required action roles/tracks |
| `fitted.hpp` | Load v1 fitted catalogs, validate binds, share geometry residency and copy the final owner pose; slots remain caller data |
| `actor_presentation.hpp` | Load a v1 actor profile with a manifest, semantic capabilities and named sockets; resolve v1 action sets |
| `interaction_runtime.hpp` | Bind a v1 coordinated role graph; evaluate placements/contacts in parent-before-child order from caller worlds and release time |

`anima/core/capabilities.hpp` is available from `anima::core` alone. It resolves
variants whose required capabilities are a subset of those available, choosing
the most specific match and rejecting ties or no match. Capability, action, role,
socket and joint names are caller data. No class, inventory, damage or networking
rules enter these APIs.

Typical assembly uses `ActorPresentation(profile_path)`, constructs an
`AttachmentLibrary` from `decode_attachment_catalog(document, directory)`, and
prepares an `AttachmentSet` for the caller's role-to-item map. Validate ownership
before adding the prepared set to a `Scene`; validate a requested action
before installing it. `prepare` leaves the live scene untouched on failure.
Primary sockets are exclusive by default; the caller may relax that policy, but
pose masks and contact chains still require disjoint ownership. Use the action
sample's semantic tracks with `sample_attachment_pose`; animated marker/grip
transforms feed attachment placement and declared support contacts.

Actor profile socket `frame` values are model-space bind frames. A null frame
uses the declared node's bind position with model-forward orientation. Attachment
socket documents instead provide checked `rest_joints` and node-local `local`
frames. Catalog items use `category` for grouping, and `empty_handling` names the
profile used without attachments.

`ActionRequest::duration` adapts timed actions to a caller duration. Held actions
supply elapsed and optional release time directly; they cannot use that adapter.
Cue cursors remain caller-owned, so late observers can seek without replaying
old events. Runtime handles retain immutable resources through shared ownership;
scene mutation and sampling into consumer-owned state remain caller-coordinated.

`ActionWeight` also accepts directly constructed curves. Sampling validates 2–32
strictly ordered keys covering 0–1 with finite normalized weights. Finite sample
phases clamp to the curve endpoints; invalid curves and nonfinite phases reject.

The executable [independent consumer](../tests/consumer/presentation.hpp) is a
complete reference for these document formats and calls. It generates its own
seven- and thirteen-joint actors, separate motion files and animated props. It
imports no game assets, implementation headers or JSON library. It tests two
named attachments, independent capability selection, held/released actions,
animated grips/markers, contact solving and docking between different anatomies.
It also rejects incompatible binds, null resources, missing roles/tracks,
ambiguous variants, overlapping carries, cyclic role graphs, incomplete placement
inputs and duplicate JSON fields.

## Validation and capability boundary

The asset, evaluation, action and interaction tests cover independent resource
loading, affine masks/additive composition, contacts, held/released cues and
placement dependencies. The external asset and core consumers are part of the
headless suite and build with tools/tests disabled as library clients. No Blender,
application checkout, network, display, GPU or downloaded dependency is needed.
Run the engine presets described in the README. Gameplay and GPU qualification
belong to the consuming application's additional tests.

Applications own asset compilation, resource splitting and export validation.
The C++ runtime is consumed from source through CMake.
