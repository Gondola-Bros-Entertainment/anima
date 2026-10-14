# Linea

Optional vector graphics library, MIT licensed, upstream
https://github.com/Gondola-Bros-Entertainment/linea. It has no releases yet. While the
two are developed together, Anima follows Linea's `main` branch, so each configure
fetches its latest commit. Set `FETCHCONTENT_SOURCE_DIR_LINEA` to a local checkout to
build against that instead.

The MIT license is retained in [LICENSE](LICENSE). Source is fetched by
`cmake/AnimaVector.cmake` only when `ANIMA_BUILD_VECTOR=ON`; source is not vendored here.
Linea is C++20 and depends only on the standard library. Its tests are off and its
warnings are not errors, so a newer compiler's diagnostics in it do not stop Anima's
build. `anima::vector` exposes its headers, because applications describe their art
as `linea::Drawing` values.
