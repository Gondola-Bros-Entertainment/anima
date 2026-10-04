// Levels of detail: how Mesh::load, Mesh::compile and Mesh::compile_static simplify draws, which draws they leave
// whole, the rules their options follow, and what snapshots count of them. GPU checks cover how the renderer chooses
// levels.
#include <algorithm>
#include <anima/assets/scene_budget.hpp>
#include <anima/mesh_placements.hpp>
#include <anima/scene.hpp>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <numbers>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace anima;

namespace {
// Rings @p first to @p last of a unit sphere of @p rings by @p segments quads, as a list of triangle corners with
// outward normals. Positions repeat exactly where quads meet, at the poles and along the UV seam at segment 0, where
// the texture coordinates split, so the whole sphere is closed.
SourcePrimitive sphere_band(int rings, int segments, int first, int last, int material = 0) {
    SourcePrimitive primitive;
    primitive.material = material;
    const auto corner = [&](int ring, int segment) {
        const float theta = std::numbers::pi_v<float> * float(ring) / float(rings),
                    phi = 2 * std::numbers::pi_v<float> * float(segment % segments) / float(segments);
        SourceVertex vertex;
        vertex.position = ring == 0 ? Vec3{0, 1, 0}
                          : ring == rings
                              ? Vec3{0, -1, 0}
                              : Vec3{std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)};
        vertex.normal = vertex.position;
        vertex.uv = {float(segment) / float(segments), float(ring) / float(rings)};
        return vertex;
    };
    for (int ring = first; ring < last; ++ring)
        for (int segment = 0; segment < segments; ++segment) {
            const auto a = corner(ring, segment), b = corner(ring + 1, segment), c = corner(ring + 1, segment + 1),
                       d = corner(ring, segment + 1);
            for (const auto &v : {a, b, c, a, c, d})
                primitive.vertices.push_back(v);
        }
    return primitive;
}
SourcePrimitive sphere(int rings, int segments, int material = 0) {
    return sphere_band(rings, segments, 0, rings, material);
}
Asset sphere_asset(AlphaMode mode = AlphaMode::opaque) {
    Asset asset;
    asset.nodes.resize(1);
    Material material;
    material.alpha_mode = mode;
    asset.materials.push_back(material);
    asset.primitives.push_back(sphere(24, 48));
    return asset;
}
// sphere_asset() flat shaded: each triangle has its own normal, so no vertex welds with a neighbor's, as in a model
// with hard edges, and only positions are shared.
Asset faceted_sphere_asset() {
    auto asset = sphere_asset();
    auto &vertices = asset.primitives[0].vertices;
    for (std::size_t t = 0; t < vertices.size(); t += 3) {
        const auto normal = normalized(
            cross(vertices[t + 1].position - vertices[t].position, vertices[t + 2].position - vertices[t].position));
        for (std::size_t k = 0; k < 3; ++k)
            vertices[t + k].normal = normal;
    }
    return asset;
}
// A GLB file whose one node draws @p primitive's triangle corners, unindexed, with their positions and normals and no
// material, so the importer gives it glTF's default material.
std::string glb(const SourcePrimitive &primitive) {
    const auto count = primitive.vertices.size();
    std::string binary, minimum, maximum;
    const auto word = [](std::string &out, std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            out.push_back(static_cast<char>((value >> shift) & 0xff));
    };
    for (const auto member : {&SourceVertex::position, &SourceVertex::normal})
        for (const auto &vertex : primitive.vertices)
            for (const float value : {(vertex.*member).x, (vertex.*member).y, (vertex.*member).z})
                word(binary, std::bit_cast<std::uint32_t>(value));
    // glTF requires a position accessor's bounds, which the shortest form of each float states exactly.
    for (const auto axis : {&Vec3::x, &Vec3::y, &Vec3::z}) {
        const auto [low, high] = std::minmax_element(
            primitive.vertices.begin(), primitive.vertices.end(),
            [&](const SourceVertex &a, const SourceVertex &b) { return a.position.*axis < b.position.*axis; });
        for (const auto &[text, vertex] : {std::pair{&minimum, low}, std::pair{&maximum, high}}) {
            std::array<char, 32> digits{};
            const auto end = std::to_chars(digits.data(), digits.data() + digits.size(), vertex->position.*axis).ptr;
            *text += (text->empty() ? "" : ",") + std::string(digits.data(), end);
        }
    }
    const auto bytes = std::to_string(count * 3 * sizeof(float));
    std::string json = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
                       R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1}}]}],"accessors":[)"
                       R"({"bufferView":0,"componentType":5126,"count":)" +
                       std::to_string(count) + R"(,"type":"VEC3","min":[)" + minimum + R"(],"max":[)" + maximum +
                       R"(]},{"bufferView":1,"componentType":5126,"count":)" + std::to_string(count) +
                       R"(,"type":"VEC3"}],"bufferViews":[{"buffer":0,"byteLength":)" + bytes +
                       R"(},{"buffer":0,"byteOffset":)" + bytes + R"(,"byteLength":)" + bytes +
                       R"(}],"buffers":[{"byteLength":)" + std::to_string(binary.size()) + "}]}";
    json.append((4 - json.size() % 4) % 4, ' ');
    constexpr std::uint32_t magic = 0x46546c67, version = 2, json_chunk = 0x4e4f534a, binary_chunk = 0x004e4942;
    constexpr std::uint32_t header_bytes = 12, chunk_header_bytes = 8;
    std::string file;
    word(file, magic);
    word(file, version);
    word(file, static_cast<std::uint32_t>(header_bytes + 2 * chunk_header_bytes + json.size() + binary.size()));
    word(file, static_cast<std::uint32_t>(json.size()));
    word(file, json_chunk);
    file += json;
    word(file, static_cast<std::uint32_t>(binary.size()));
    word(file, binary_chunk);
    return file + binary;
}
// A file of @p bytes that the destructor removes.
struct TemporaryFile {
    std::filesystem::path path;
    explicit TemporaryFile(const std::string &bytes) {
        std::random_device random;
        path = std::filesystem::temp_directory_path() / ("anima-mesh-lods-" + std::to_string(random()) + ".glb");
        std::ofstream file(path, std::ios::binary);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        REQUIRE(file.good());
    }
    ~TemporaryFile() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
    TemporaryFile(const TemporaryFile &) = delete;
    TemporaryFile &operator=(const TemporaryFile &) = delete;
};
// Triangles that one draw, or one of its levels, draws: @p count indices of @p mesh from @p first.
struct Triangles {
    const Mesh *mesh{};
    std::uint32_t first{}, count{};
};
// The draw and then each level of draw @p index of @p mesh.
std::vector<Triangles> draw_and_levels(const Mesh &mesh, std::size_t index) {
    const auto &draw = mesh.primitives()[index];
    std::vector<Triangles> result{{&mesh, draw.first_index, draw.index_count}};
    for (const auto &level : draw.levels)
        result.push_back({&mesh, level.first_index, level.index_count});
    return result;
}
// A triangle's indices rotated to start at the smallest, which keeps its winding, so equal triangles compare equal.
std::array<std::uint32_t, 3> canonical(const std::uint32_t *corners) {
    const auto first = std::size_t(std::min_element(corners, corners + 3) - corners);
    return {corners[first], corners[(first + 1) % 3], corners[(first + 2) % 3]};
}
// Whether the triangles of @p level that @p previous also draws, unchanged, keep the order they have in @p previous,
// and how many there are.
std::pair<bool, std::size_t> keeps_order(const Mesh &mesh, const Triangles &previous, const Triangles &level) {
    std::map<std::array<std::uint32_t, 3>, std::uint32_t> position;
    for (std::uint32_t t = 0; t < previous.count; t += 3)
        position.emplace(canonical(&mesh.indices()[previous.first + t]), t);
    bool ordered = true;
    std::size_t unchanged = 0;
    std::uint32_t last = 0;
    for (std::uint32_t t = 0; t < level.count; t += 3)
        if (const auto found = position.find(canonical(&mesh.indices()[level.first + t])); found != position.end()) {
            ordered = ordered && (!unchanged || found->second > last);
            last = found->second;
            ++unchanged;
        }
    return {ordered, unchanged};
}
// Whether @p parts close up together: each edge, by the positions of its ends, is matched by one that runs the other
// way, so no crack opens anywhere between them.
bool closed(std::initializer_list<Triangles> parts) {
    using Point = std::array<float, 3>;
    std::map<std::pair<Point, Point>, int> edges;
    for (const auto &part : parts) {
        const auto at = [&](std::uint32_t i) {
            const auto &p = part.mesh->vertices()[part.mesh->indices()[i]].position;
            return Point{p.x, p.y, p.z};
        };
        for (auto t = part.first; t < part.first + part.count; t += 3)
            for (std::uint32_t e = 0; e < 3; ++e)
                if (const auto a = at(t + e), b = at(t + (e + 1) % 3); a != b) {
                    ++edges[{a, b}];
                    --edges[{b, a}];
                }
    }
    return std::all_of(edges.begin(), edges.end(), [](const auto &edge) { return edge.second == 0; });
}
// The distance from @p p to the nearest point of triangle @p a, @p b, @p c (Ericson, Real-Time Collision Detection,
// 5.1.5).
float distance_to_triangle(Vec3 p, Vec3 a, Vec3 b, Vec3 c) {
    const auto ab = b - a, ac = c - a, ap = p - a;
    const float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0)
        return length(p - a);
    const auto bp = p - b;
    const float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3)
        return length(p - b);
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0)
        return length(p - (a + ab * (d1 / (d1 - d3))));
    const auto cp = p - c;
    const float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6)
        return length(p - c);
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0)
        return length(p - (a + ac * (d2 / (d2 - d6))));
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0)
        return length(p - (b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)))));
    const float denominator = 1 / (va + vb + vc);
    return length(p - (a + ab * (vb * denominator) + ac * (vc * denominator)));
}
// The farthest that any vertex of draw @p index of @p mesh lies from the triangles of @p level.
float deviation(const Mesh &mesh, std::size_t index, const PrimitiveLevel &level) {
    const auto &draw = mesh.primitives()[index];
    const auto at = [&](std::uint32_t i) { return mesh.vertices()[mesh.indices()[i]].position; };
    float farthest = 0;
    for (auto i = draw.first_index; i < draw.first_index + draw.index_count; ++i) {
        float nearest = INFINITY;
        for (auto t = level.first_index; t < level.first_index + level.index_count; t += 3)
            nearest = std::min(nearest, distance_to_triangle(at(i), at(t), at(t + 1), at(t + 2)));
        farthest = std::max(farthest, nearest);
    }
    return farthest;
}
} // namespace

TEST_CASE("Compiling with levels simplifies each draw into coarser levels") {
    const auto mesh = Mesh::compile(sphere_asset(), {.lods = {.levels = 4}});
    REQUIRE(mesh->primitives().size() == 1);
    const auto &draw = mesh->primitives()[0];
    REQUIRE(!draw.levels.empty());
    CHECK(draw.levels.size() <= 4);
    CHECK(closed({{mesh.get(), draw.first_index, draw.index_count}}));
    std::uint32_t previous_count = draw.index_count;
    float previous_error = 0;
    // The draw's own vertices, which every level must stay within.
    std::uint32_t lowest = UINT32_MAX, highest = 0;
    for (std::uint32_t i = draw.first_index; i < draw.first_index + draw.index_count; ++i) {
        lowest = std::min(lowest, mesh->indices()[i]);
        highest = std::max(highest, mesh->indices()[i]);
    }
    for (const auto &level : draw.levels) {
        CHECK(level.index_count % 3 == 0);
        CHECK(level.index_count > 0);
        CHECK(float(level.index_count) <= float(previous_count) * .85F);
        CHECK(level.error >= previous_error);
        // No step moves the surface by more than the draw's extent, which a unit sphere's diameter sets.
        CHECK(level.error <= 2 + 1e-4F);
        CHECK(level.first_index >= draw.first_index + draw.index_count);
        CHECK(std::size_t(level.first_index) + level.index_count <= mesh->indices().size());
        for (std::uint32_t i = level.first_index; i < level.first_index + level.index_count; ++i) {
            CHECK(mesh->indices()[i] >= lowest);
            CHECK(mesh->indices()[i] <= highest);
        }
        // The UV seam and the poles collapse on both sides together, so the sphere stays closed.
        CHECK(closed({{mesh.get(), level.first_index, level.index_count}}));
        previous_count = level.index_count;
        previous_error = level.error;
    }
    // The first level halves the sphere's indices with an error of a few hundredths of its radius.
    CHECK(draw.levels[0].index_count <= draw.index_count / 2 + 3);
    CHECK(draw.levels[0].error < .05F);
}

TEST_CASE("Faceted draws simplify across their hard edges and keep their texture seams") {
    const auto mesh = Mesh::compile(faceted_sphere_asset(), {.lods = {.levels = 4}});
    const auto &draw = mesh->primitives()[0];
    REQUIRE(draw.levels.size() >= 3);
    CHECK(draw.levels[0].index_count <= draw.index_count * 6 / 10);
    float previous_error = 0;
    for (const auto &level : draw.levels) {
        CHECK(level.error > previous_error);
        previous_error = level.error;
        CHECK(closed({{mesh.get(), level.first_index, level.index_count}}));
        // The texture wraps from 1 back to 0 along segment 0. No triangle may join the two sides of that seam,
        // which would stretch the whole texture across it.
        for (auto t = level.first_index; t < level.first_index + level.index_count; t += 3) {
            float lowest = 1, highest = 0;
            for (std::uint32_t k = 0; k < 3; ++k) {
                const auto u = mesh->vertices()[mesh->indices()[t + k]].uv[0];
                lowest = std::min(lowest, u);
                highest = std::max(highest, u);
            }
            CHECK(highest - lowest < .5F);
        }
    }
}

TEST_CASE("Faceted draws keep the splits in attributes that simplification does not weigh") {
    // Each half of a faceted sphere, split along two meridians away from its texture seam, takes its own value of one
    // attribute. Simplification writes only indices, so collapsing across the split would give the moved corners the
    // other half's value: the alpha would move, the normal map would mirror, and the posed skin would tear.
    enum class Split { alpha, handedness, joints };
    for (const auto split : {Split::alpha, Split::handedness, Split::joints}) {
        CAPTURE(int(split));
        auto asset = faceted_sphere_asset();
        auto &primitive = asset.primitives[0];
        if (split == Split::joints) {
            asset.nodes.resize(3);
            asset.skins.push_back({{1, 2}, {identity(), identity()}});
            primitive.skin = 0;
        }
        for (std::size_t v = 0; v < primitive.vertices.size(); ++v) {
            // Six corners per quad and 48 quads per ring; segments 12 to 35 form one half.
            const bool half = (v / 6 % 48 + 36) % 48 < 24;
            auto &vertex = primitive.vertices[v];
            vertex.weights = {split == Split::joints ? 1.F : 0.F, 0, 0, 0};
            if (split == Split::alpha)
                vertex.alpha = half ? 1.F : .5F;
            else if (split == Split::handedness)
                vertex.tangent = {1, 0, 0, half ? 1.F : -1.F};
            else
                vertex.joints = {half ? 1U : 0U, 0, 0, 0};
        }
        const auto value = [&](const SourceVertex &vertex) {
            return split == Split::alpha        ? vertex.alpha
                   : split == Split::handedness ? vertex.tangent[3]
                                                : float(vertex.joints[0]);
        };
        const auto mesh = Mesh::compile(asset, {.lods = {.levels = 4}});
        const auto &draw = mesh->primitives()[0];
        REQUIRE(draw.levels.size() >= 3);
        // The protected splits are two meridians, so the hard edges elsewhere still simplify.
        CHECK(draw.levels[0].index_count <= draw.index_count * 6 / 10);
        for (const auto &level : draw.levels) {
            std::size_t joined = 0;
            for (auto t = level.first_index; t < level.first_index + level.index_count; t += 3) {
                const auto at = [&](std::uint32_t k) { return value(mesh->vertices()[mesh->indices()[t + k]]); };
                joined += at(0) != at(1) || at(0) != at(2);
            }
            CHECK(joined == 0);
        }
    }
}

TEST_CASE("A blended draw's levels keep their triangles in source order") {
    // The sphere's triangles in a scrambled order, which ordering for the vertex cache changes. A blended draw
    // composites its triangles in order, so each of its levels keeps the triangles it shares unchanged with the one
    // before it in that one's order; an opaque draw's levels are reordered.
    for (const auto mode : {AlphaMode::blend, AlphaMode::opaque}) {
        CAPTURE(int(mode));
        auto asset = sphere_asset(mode);
        auto &vertices = asset.primitives[0].vertices;
        const auto triangles = vertices.size() / 3;
        // 1009 is prime and does not divide the 2,304 triangles, so the stride visits each once.
        std::vector<SourceVertex> scrambled;
        scrambled.reserve(vertices.size());
        for (std::size_t k = 0; k < triangles; ++k)
            for (std::size_t c = 0; c < 3; ++c)
                scrambled.push_back(vertices[k * 1009 % triangles * 3 + c]);
        vertices = std::move(scrambled);
        const auto mesh = Mesh::compile(asset, {.lods = {.levels = 4}});
        const auto steps = draw_and_levels(*mesh, 0);
        REQUIRE(steps.size() >= 3);
        bool every_level_ordered = true;
        for (std::size_t k = 1; k < steps.size(); ++k) {
            const auto [ordered, unchanged] = keeps_order(*mesh, steps[k - 1], steps[k]);
            CHECK(unchanged >= 10);
            every_level_ordered = every_level_ordered && ordered;
        }
        CHECK(every_level_ordered == (mode == AlphaMode::blend));
    }
}

TEST_CASE("A level's error covers every step that produced it") {
    // Each step's error is measured from the level before it, so a level that kept only the largest step's error
    // would claim less than it moves the surface, and the renderer would draw it nearer than its threshold allows.
    // A single step's error is meshoptimizer's own estimate, which the first level shows: this sphere's lies 17%
    // beyond it with fused multiply-adds and 29% without. A deeper level, made by several steps, may understate its
    // distance no more than that; levels that kept only the largest step lay up to 64% beyond their error.
    const auto mesh = Mesh::compile(sphere_asset(), {.lods = {.levels = 6}});
    const auto &levels = mesh->primitives()[0].levels;
    REQUIRE(levels.size() >= 5);
    const auto one_step = deviation(*mesh, 0, levels[0]) / levels[0].error;
    for (std::size_t k = 1; k < levels.size(); ++k)
        CHECK(deviation(*mesh, 0, levels[k]) <= levels[k].error * one_step);
}

TEST_CASE("Draws record the palette matrices that may place their vertices") {
    const auto rigid = Mesh::compile(sphere_asset());
    CHECK(rigid->primitives()[0].palette_count == 1);
    auto asset = sphere_asset();
    asset.nodes.resize(4);
    asset.skins.push_back({{1, 2, 3}, {identity(), identity(), identity()}});
    for (auto &vertex : asset.primitives[0].vertices) {
        vertex.joints = {0, 2, 0, 0};
        vertex.weights = {.5F, .5F, 0, 0};
    }
    asset.primitives[0].skin = 0;
    const auto skinned = Mesh::compile(asset);
    CHECK(skinned->primitives()[0].skinned);
    CHECK(skinned->primitives()[0].palette_offset == 4);
    CHECK(skinned->primitives()[0].palette_count == 3);
}

TEST_CASE("Draws that meet stay closed whichever levels each draws") {
    // A sphere whose northern and southern halves are separate draws, as the material subsets of one mesh are.
    Asset asset;
    asset.nodes.resize(1);
    asset.materials.resize(2);
    asset.primitives.push_back(sphere_band(24, 48, 0, 12, 0));
    asset.primitives.push_back(sphere_band(24, 48, 12, 24, 1));
    const auto mesh = Mesh::compile(asset, {.lods = {.levels = 4}});
    REQUIRE(mesh->primitives().size() == 2);
    REQUIRE(!mesh->primitives()[0].levels.empty());
    REQUIRE(!mesh->primitives()[1].levels.empty());
    for (const auto &north : draw_and_levels(*mesh, 0))
        for (const auto &south : draw_and_levels(*mesh, 1))
            CHECK(closed({north, south}));
}

TEST_CASE("Draws without levels of detail") {
    CHECK(Mesh::compile(sphere_asset(), {.lods = {.levels = 0}})->primitives()[0].levels.empty());
    CHECK(Mesh::compile(sphere_asset())->primitives()[0].levels.empty());
    // Masked draws keep only themselves: their cutout edges follow texture coordinates, which simplification does not
    // weigh.
    CHECK(Mesh::compile(sphere_asset(AlphaMode::mask), {.lods = {.levels = 4}})->primitives()[0].levels.empty());
    CHECK_FALSE(Mesh::compile(sphere_asset(AlphaMode::blend), {.lods = {.levels = 4}})->primitives()[0].levels.empty());
    CHECK(Mesh::compile(sphere_asset(), {.lods = {.levels = MeshLodOptions::max_levels}})
              ->primitives()[0]
              .levels.size() <= MeshLodOptions::max_levels);
    CHECK_THROWS_WITH_AS((void)Mesh::compile(sphere_asset(), {.lods = {.levels = MeshLodOptions::max_levels + 1}}),
                         "Mesh LOD levels must be from 0 to 8", std::invalid_argument);
    // A double pyramid of six triangles cannot lose 15% of its triangles without collapsing, so it keeps at most one.
    Asset tiny;
    tiny.nodes.resize(1);
    tiny.materials.emplace_back();
    tiny.primitives.push_back(sphere(2, 3));
    CHECK(Mesh::compile(tiny, {.lods = {.levels = 4}})->primitives()[0].levels.size() <= 1);
}

TEST_CASE("Static compilation generates levels in every resulting mesh, which stay closed together") {
    MeshCompileOptions options;
    options.max_vertices = 3000;
    options.mesh.lods.levels = 3;
    const auto pieces = Mesh::compile_static(sphere_asset(), options);
    REQUIRE(pieces.size() == 3);
    for (const auto &piece : pieces)
        for (const auto &draw : piece->primitives())
            CHECK_FALSE(draw.levels.empty());
    for (const auto &first : draw_and_levels(*pieces[0], 0))
        for (const auto &second : draw_and_levels(*pieces[1], 0))
            for (const auto &third : draw_and_levels(*pieces[2], 0))
                CHECK(closed({first, second, third}));
    options.mesh.lods.levels = MeshLodOptions::max_levels + 1;
    CHECK_THROWS_WITH_AS((void)Mesh::compile_static(sphere_asset(), options), "Mesh LOD levels must be from 0 to 8",
                         std::invalid_argument);
}

TEST_CASE("Static compilation without limits compiles with the options' retention and levels") {
    MeshCompileOptions options;
    options.mesh.texel_retention = TexelRetention::until_upload;
    options.mesh.lods.levels = 2;
    const auto pieces = Mesh::compile_static(sphere_asset(), options);
    REQUIRE(pieces.size() == 1);
    const auto expected =
        Mesh::compile(sphere_asset(), {.texel_retention = TexelRetention::until_upload, .lods = {.levels = 2}});
    CHECK(pieces[0]->texel_retention() == TexelRetention::until_upload);
    REQUIRE(pieces[0]->draws().size() == 1);
    CHECK_FALSE(pieces[0]->draws()[0].levels.empty());
    CHECK(pieces[0]->draws()[0].levels.size() == expected->draws()[0].levels.size());
    CHECK(std::ranges::equal(pieces[0]->indices(), expected->indices()));
}

TEST_CASE("Loading a GLB file compiles it with the options given, as compiling its import does") {
    const TemporaryFile file(glb(sphere(24, 48)));
    REQUIRE(Mesh::load(file.path)->primitives().size() == 1);
    CHECK(Mesh::load(file.path)->primitives()[0].levels.empty());
    const auto mesh = Mesh::load(file.path, {.texel_retention = TexelRetention::until_upload, .lods = {.levels = 2}});
    const auto &levels = mesh->primitives()[0].levels;
    CHECK_FALSE(levels.empty());
    CHECK(levels.size() <= 2);
    CHECK(mesh->texel_retention() == TexelRetention::until_upload);
    const auto compiled = Mesh::compile(*load_asset(file.path), {.lods = {.levels = 2}});
    CHECK(std::ranges::equal(mesh->indices(), compiled->indices()));
    CHECK_THROWS_WITH_AS((void)Mesh::load(file.path, {.lods = {.levels = MeshLodOptions::max_levels + 1}}),
                         "Mesh LOD levels must be from 0 to 8", std::invalid_argument);
}

TEST_CASE("Snapshots budget each draw's own triangles, not its levels") {
    const auto mesh = Mesh::compile(sphere_asset(), {.lods = {.levels = 4}});
    const auto corners = mesh->primitives()[0].index_count;
    REQUIRE(mesh->indices().size() > corners);
    Scene scene;
    (void)scene.create({}, mesh);
    const auto bytes = validate_scene_geometry(corners);
    CHECK(scene.snapshot({bytes}).vertices.size() == corners);
    const auto budget = "MeshSnapshot geometry needs " + std::to_string(bytes) + " bytes; budget is " +
                        std::to_string(bytes - 1) + " bytes";
    CHECK_THROWS_WITH_AS((void)scene.snapshot({bytes - 1}), budget.c_str(), SceneCapacityError);
}

TEST_CASE("Placement clusters record their largest scale") {
    const auto mesh = Mesh::compile(sphere_asset());
    auto doubled = identity();
    doubled[5] = 2;
    auto halved = identity();
    halved[0] = halved[5] = halved[10] = .5F;
    const auto placements = MeshPlacements::create(mesh, std::vector<Mat4>{identity(), doubled});
    REQUIRE(placements->clusters().size() == 1);
    CHECK(placements->clusters()[0].scale == doctest::Approx(2));
    CHECK(MeshPlacements::create(mesh, std::vector<Mat4>{halved})->clusters()[0].scale == doctest::Approx(.5));
}
