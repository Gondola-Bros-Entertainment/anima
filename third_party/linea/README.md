# Linea

Optional vector graphics library, MIT licensed, upstream
https://github.com/Gondola-Bros-Entertainment/linea. It has no releases yet; Anima pins
version 0.1.0 at commit `16648e644c4864be81811ee765455a3397448ae1`.

Download: <https://codeload.github.com/Gondola-Bros-Entertainment/linea/tar.gz/16648e644c4864be81811ee765455a3397448ae1>

Archive SHA-256: `f4655c2d15139f41ff030ff82e60d1b13472bb88527e8bb07b6861074ad0dffe`

The MIT license is retained in [LICENSE](LICENSE). Source is fetched by
`cmake/AnimaVector.cmake` only when `ANIMA_BUILD_VECTOR=ON`; source is not vendored here.
Linea is C++20 and depends only on the standard library. Its tests are off and its
warnings are not errors, so a newer compiler's diagnostics in it do not stop Anima's
build. `anima::vector` exposes its headers, because applications describe their art
as `linea::Drawing` values.
