# Jolt Physics

Optional 3D backend, upstream https://github.com/jrouwe/JoltPhysics,
release 5.6.0, commit `e77f175595e64cb44218cc9d9d56fc365ad0e36a`.
Source archive SHA-256: `1f32328fb763135de10a244568d6ccb2ed9b1e6593fafe6dc6db5b2719d330bd`.
The MIT license is retained in LICENSE. Source is fetched by
`cmake/AnimaPhysics.cmake` only when explicitly enabled; compute/graphics,
samples, installation, profiling and newer x86 instruction sets are disabled.
CMake settings are scoped to avoid changing the consuming project's options.
