# Heightfield queries

`anima::core` exposes `anima/core/heightfield.hpp` without graphics, an asset
parser or a game dependency. Applications supply terrain samples; the same API
can run in a client, an authoritative server or an authoring tool.

## Data and geometry contract

`Heightfield` owns a rectangular array of world-Y heights. `HeightfieldView`
borrows those samples without copying them. Columns advance along world +X;
rows advance along +Z. Node `(x,z)` has position
`(origin_x + x*spacing_x, heights[z*columns+x], origin_z + z*spacing_z)`.
Spacing must be finite and positive, with at least two rows and columns.
Keep borrowed samples alive and immutable throughout queries.

Every cell has the diagonal from `(0,0)` to `(1,1)`. Its triangles are
`{00,10,11}` and `{00,01,11}`. Render meshes must use this same diagonal:
bilinear interpolation generally describes a different surface. Heights are
interpolated on the selected triangle's plane. If its slopes are `dx` and `dz`,
the upward normal is `normalize(-dx,1,-dz)`. These are face normals for geometry
queries; an application's smooth shading normals may differ.

`validate_heightfield` checks all samples once at acceptance. Queries check
dimensions and touched samples, throwing `std::invalid_argument` on malformed
data. Finite grid extent is checked against float coordinate precision; this API
does not provide large-world origin rebasing. Use local coordinates appropriate
to the grid resolution.

## Pure queries

| Function | Result and cost |
| --- | --- |
| `sample_heightfield` | Optional height and upward normal at X/Z; constant time. Outside or nonfinite query coordinates return no sample. |
| `intersect_heightfield` | First contact of a finite segment with the solid at or below the terrain. Work scales with crossed grid cells. |
| `move_on_heightfield` | Grounded position, travelled surface distance, segment fraction and steep-slope blocking. Work scales with crossed grid cells. |

Segment traversal clips to the grid, visits crossed cells and splits each cell
at its diagonal. It does not sample at a fixed step that could skip narrow
ridges. Starting inside the terrain solid produces contact at the first in-grid
point. `clearance` raises terrain vertically; it is not a sphere/capsule sweep.

Movement follows the supplied X/Z segment. Both endpoints must lie in the grid;
their Y coordinates are finite but replaced by terrain height. The caller owns
the nonnegative distance budget and `minimum_up` slope policy in `[0,1]`.
For a maximum slope angle theta, use `minimum_up = cos(theta)`. Distance includes
height change: a planar piece costs `sqrt(dx*dx + dy*dy + dz*dz)`. Budget exhaustion
stops partway through a piece; encountering a steeper triangle sets `blocked`.
The returned boundary point may need a caller-owned contact margin before a
later point query chooses its neighbouring triangle.

Queries allocate no memory, mutate no state and contain no game movement speed,
networking, actor ownership or graphics policy. An owning grid allocates only
when the application constructs or changes its sample vector.

## Scope and verification

This is a single-valued heightfield, not general physics. Caves, bridges above
ground, overhangs, rigid bodies, gravity, jumping, navigation, actor volumes and
sliding along steep contours require additional systems. Streaming, terrain LOD,
seam management and world origin rebasing are not implemented by these queries.

The heightfield test covers interpolation/diagonals, non-square spacing, borders,
invalid input, vertical and clipped segments, distance budgets, both directions
across narrow ridges and 200 ray comparisons against an independent brute-force
triangle intersection calculation. The standalone external consumer calls all
four functions using only the public core target.
