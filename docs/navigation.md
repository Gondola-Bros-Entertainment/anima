# In-house navigation

`anima::core` supplies [navigation](../include/anima/navigation.hpp): immutable
weighted directed graphs, grid construction, A* route queries and a route follower.
`anima::assets` adds [scene agents](../include/anima/navigation_scene.hpp) and an
explicit prefab codec. There is no Recast/Detour, physics, networking, SDL or GPU
dependency. The application supplies traversability, edge costs, route endpoints
and movement/collision policy.

```cpp
#include <anima/navigation.hpp>
namespace n = anima::navigation;
n::Grid grid{3, 3, {}, 1, {1,1,1, 1,0,1, 1,1,1}, false};
const auto graph = n::make_grid(grid);
const auto path = n::find_path(graph, 0, 8);
if (path.status == n::PathStatus::found) {
    n::Follower follower(n::waypoints(graph, path));
    const auto velocity = follower.steer({0,0,0}, 2, 1.0 / 60);
    // The application applies permitted movement and reports its actual next position.
}
```

## Graph and search contract

A graph owns nodes with finite positions and a walkable flag, and directed edges
with finite nonnegative costs. Node IDs index the immutable node array. Parallel
edges, zero-cost edges, self-links and coincident nodes are supported. Reverse
travel requires a reverse edge. Graphs have 1–1,000,000 nodes and at most 8,000,000
edges; positions are bounded to ±1,000,000 and edge cost to 1e12. These bounds are
validation/memory controls, not a guarantee of large-world geometric precision.
Applications can share immutable graphs and issue independent queries concurrently;
query working memory and queues are local to the call.

A* derives a conservative Euclidean heuristic from the minimum edge cost per
world distance. This supports arbitrary cost units and cheap links without an
incorrect heuristic selecting a more expensive route. Zero-cost links may reduce
the heuristic to zero (Dijkstra). Blocked endpoints return `unreachable`; invalid
indices/settings throw. Ties use node IDs; floating-point solver/replay equivalence
across platforms is not promised.

Queries distinguish `found`, `unreachable`, and `budget_exhausted`. Expansion and
edge-visit limits bound work even for a high-degree node. Defaults are 100,000
expansions and 1,000,000 edge visits; maximum configurable limits match graph
bounds. Results report both counts. Exhaustion returns no partial route; the
caller decides whether/when to retry. A walkable start equal to the goal returns
a one-node, zero-cost route even with a zero budget. `waypoints` converts a
successful route against the same immutable graph; do not use its IDs with a
different graph. Route cost includes edges only, not a starting-node charge.

## Grids

`Grid` defines an XZ plane at `origin.y`. Row-major ID is `z * width + x`;
node position is `origin + (x,0,z) * cell_size`. Costs must have exactly one entry
per cell. Zero blocks a cell, and a positive cost charges distance times the
**entered** cell's value, so differently weighted cells can yield asymmetric
travel costs. Cell size is [0.001,10000], cost [0,1,000,000].

Cardinal links are always considered. Optional diagonals use sqrt(2) distance and
require both adjacent cardinal cells to be walkable: routes never cut blocked
corners. Node IDs are retained for blocked cells. Updating a grid means creating a
new validated graph and replanning explicitly; existing queries/route snapshots
are not silently changed. Navigation meshes and collider-to-grid baking are not
provided by this API. Clearance remains application input or a physics query.

## Following routes and scene agents

A `Follower` retains an immutable route snapshot and an independent cursor.
Copies share point storage but never cursor state. `steer` consumes only waypoints
within the observed arrival distance and returns velocity toward the next point,
capped so this time step cannot intentionally overshoot that point. It does not
consume a waypoint merely because it suggested movement; a blocked actor retains
its route. It advances at most to the next unreached corner, leaving any remaining
frame travel unused. Route replacement validates before changing accepted state.

Speed is [0,10000], arrival distance [0,10000], and step [0.000001,0.1] seconds.
A zero speed can still acknowledge an already reached waypoint. Empty/completed
routes output zero. Callers must choose sensible tolerances for their world scale
and handle displacement/teleports, dynamic obstacles, replanning, turning,
acceleration and collision response.

```cpp
#include <anima/navigation_scene.hpp>
auto object = scene.create("route follower");
object.add_component<n::Agent>(n::waypoints(graph, path), 2.F, .05F);
n::update_agents(scene, 1.0 / 60);
auto agent = object.get_component<n::Agent>();
const auto wanted = agent->desired_velocity();
// Apply wanted through the application's chosen motor/authority rules.
anima::ComponentCodecs codecs;
n::add_component_codec(codecs);
```

`update_agents` reads world positions, including parent transforms. It updates
route cursors and movement suggestions transactionally across the collected
agents; invalid parameters/positions leave prior intent and cursors untouched.
It accepts either a `Scene` or a `SceneSet`, covering the whole selection before
publication. Drivers require idle scenes: callbacks, construction and set mutation
reject before consuming routes. See [runtime ordering](runtime-lifecycle.md).
It neither moves objects nor calls Scene/physics updates or user callbacks, so
applications choose the scheduling order. Disabled components produce zero and
retain their route cursor at the next agent update. Removing components or
tearing down a scene releases its route ownership. The codec
`anima.navigation-agent.v1` persists route points/cursor, speed and arrival distance;
normal component persistence stores enablement. Restored instances have independent
cursors, and malformed state participates in prefab rollback. Desired velocity is
recomputed; graph assets, game goals and network authority are not serialized here.

## Evidence and limits

`navigation_queries` compares A* with an independent Bellman-Ford oracle over
300 reproducible directed weighted graphs, including blocked nodes, zero-cost
links and cycles, and covers grid costs, corner clearance, budgets and follower
arrival/obstruction. `navigation_scene` checks world coordinates, intent-only
behavior, enablement, snapshot validation, independent prefab state and rollback.
The copied external core/asset consumers exercise complete plan/follow and
agent/prefab flows using public targets.

```sh
cmake --preset headless
cmake --build --preset headless
ctest --preset headless -R 'navigation_|consumer_(core|assets)' --output-on-failure
```

Graphs and queries are explicit snapshots. The API does not bake navigation
meshes, update obstacles, avoid other agents or schedule background searches.
