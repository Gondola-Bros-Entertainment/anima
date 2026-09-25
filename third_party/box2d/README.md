# Box2D 3.1.1

Optional private 2D physics backend, MIT licensed. Source is fetched only when
`ANIMA_BUILD_PHYSICS2D=ON`; source is not vendored here. Core/Jolt/UI consumers do
not configure Box2D. It requires a C17 compiler in addition to Anima's C++20.

Upstream: https://github.com/erincatto/box2d/releases/tag/v3.1.1

Commit: `8c661469c9507d3ad6fbd2fea3f1aa71669c2fe3`

Archive SHA-256: `fb6ef914b50f4312d7d921a600eabc12318bb3c55a0b8c0b90608fa4488ef2e4`

The pinned archive/hash and private target are in `cmake/AnimaPhysics2d.cmake`.
No samples, test framework, threading dependency or SIMD extension requirement is
added. Runtime calls use the calling thread and Box2D's default single-worker mode.
