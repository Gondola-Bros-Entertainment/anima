#pragma once
// The order of the main view's opaque and masked draws against VulkanRenderer's contract: the draw calls that cannot
// discard before those that may, a placement cluster's included; each group's objects nearest first, in perspective and
// orthographic views, and at equal distances in selection and instance order; and each object's draws in
// Mesh::primitives() order within a group. Depth testing hides the order except between surfaces at equal depth, where
// the one drawn first shows, so each case adds coincident unlit quads in the order opposite to the one the contract
// gives, or ties them where the contract keeps the order they were added in, and requires the frame of the quad that
// must draw first, drawn alone. A masked card in front of an opaque quad, added first, must still hide the quad with
// its kept half and show it through its cut half.
//
// Coincident quads reach equal depth through different pipelines because their positions are computed exactly: the
// views' camera sits at the origin looking down -Z, every world matrix and placement is the identity, and the corners
// lie at 1 or -1 in X and Y on a plane whose depth is a power of two. Every multiplication that goes into
// resource.vert's gl_Position is then exact and each of its coordinates rounds at most once, so any order or fusing of
// those operations, which Vulkan allows where they lack NoContraction, gives the same bits. Declaring gl_Position
// invariant would promise nothing more here: GLSL extends that promise only to positions reached through the same
// control flow, which a placed copy does not share with an object without placements, and the rasterizer lies beyond
// it. Vulkan does not promise that pipelines that differ, as the two of each tie between groups do in mesh.frag's
// specialization, rasterize equal positions to equal depth (its invariance rules cover identical pipelines), so those
// ties rest on the drivers that run this check doing so.
#include "blending.hpp"
#include <anima/assets/asset.hpp>
#include <anima/mesh_placements.hpp>
#include <anima/scene.hpp>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace draw_order_test {
using blending_test::Color;
using blending_test::require;
constexpr Color yellow{1, 1, 0};

// The plane of the quads, 4 in front of the views' camera, which looks down -Z from the origin; a power of two, so that
// the projection's depth row times it is exact.
constexpr float plane = -4;
// Vertex alpha across a quad.
enum class Coverage : std::uint8_t {
    // 1 everywhere.
    whole,
    // Rising from 0 at the left edge to 1 at the right, so that a masked material's default cutoff, 0.5, cuts away the
    // left half.
    right_half
};
// What a mesh holds besides its quads.
enum class Reach : std::uint8_t {
    // Nothing.
    quad,
    // A triangle 1 from the camera, far outside both views, which draws no pixel but brings the nearest point of the
    // object's world bounds that near the camera.
    toward_camera
};
// A mesh of one coincident quad of each of @p surfaces, in this order in Mesh::primitives(), from -1 to 1 in X and Y at
// depth @p z, wound counterclockwise toward the camera.
inline std::shared_ptr<const anima::Mesh> layered(std::initializer_list<anima::Material> surfaces, float z = plane,
                                                  Coverage coverage = Coverage::whole, Reach reach = Reach::quad) {
    anima::Asset asset;
    asset.nodes.resize(1);
    for (const auto &surface : surfaces) {
        anima::SourcePrimitive primitive;
        primitive.material = int(asset.materials.size());
        asset.materials.push_back(surface);
        const auto add = [&](anima::Vec3 position, float alpha) {
            anima::SourceVertex vertex;
            vertex.position = position;
            vertex.normal = {0, 0, 1};
            vertex.alpha = alpha;
            primitive.vertices.push_back(vertex);
        };
        using Corners = std::initializer_list<std::array<float, 2>>;
        for (const auto &[x, y] : Corners{{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}})
            add({x, y, z}, coverage == Coverage::whole ? 1 : (x + 1) / 2);
        if (reach == Reach::toward_camera) {
            constexpr float aside = 9, depth = -1;
            for (const auto &[x, y] : Corners{{aside, 0}, {aside + 1, 0}, {aside, 1}})
                add({x, y, depth}, 1);
        }
        asset.primitives.push_back(std::move(primitive));
    }
    return anima::Mesh::compile(asset);
}
// A mesh of one quad of @p surface, as layered() builds it.
inline std::shared_ptr<const anima::Mesh> quad(const anima::Material &surface, float z = plane,
                                               Coverage coverage = Coverage::whole, Reach reach = Reach::quad) {
    return layered({surface}, z, coverage, reach);
}
// A masked material of linear @p color whose alpha, 1, keeps every pixel of a quad of Coverage::whole.
inline anima::Material masked(const Color &color) { return blending_test::material(color, 1, anima::AlphaMode::mask); }
// A scene of @p meshes, added in this order.
inline std::shared_ptr<const anima::Scene> scene_of(std::initializer_list<std::shared_ptr<const anima::Mesh>> meshes) {
    auto scene = std::make_shared<anima::Scene>();
    for (const auto &mesh : meshes)
        (void)scene->add(mesh);
    return scene;
}
// A scene of @p mesh placed once, in place, in a margin of its visibility range, then @p others. The copy's range is
// measured to the center of its quads, 4 from the camera: inside the margin of a range that ends at 6 after a 4 wide
// margin, where half of its pixels draw.
inline std::shared_ptr<const anima::Scene>
dissolving(const std::shared_ptr<const anima::Mesh> &mesh,
           std::initializer_list<std::shared_ptr<const anima::Mesh>> others) {
    constexpr anima::VisibilityRange fading{0, 6, 0, 4};
    const std::array<anima::Mat4, 1> in_place{anima::identity()};
    auto scene = std::make_shared<anima::Scene>();
    const auto id = scene->add(mesh);
    scene->set_placements(id, anima::MeshPlacements::create(mesh, in_place));
    scene->set_visibility_range(id, fading);
    for (const auto &other : others)
        (void)scene->add(other);
    return scene;
}
// Draws @p scenes through @p view as @p name, requiring @p calls main-view draw calls, @p discarding of them through
// the pipeline that may discard.
inline void render(blending_test::Harness &harness, const std::string &name,
                   std::vector<std::shared_ptr<const anima::Scene>> scenes, const anima::Mat4 &view,
                   std::uint64_t calls, std::uint64_t discarding) {
    harness.render(name, std::move(scenes), view, {});
    const auto &drawn = harness.stats;
    harness.images.require(drawn.draw_calls == calls && drawn.discarding_draw_calls == discarding,
                           name + " recorded " + std::to_string(drawn.draw_calls) + " draw calls, " +
                               std::to_string(drawn.discarding_draw_calls) +
                               " through the pipeline that may discard, instead of " + std::to_string(calls) + " and " +
                               std::to_string(discarding),
                           {name});
}
// The share of pixels above which a dissolving copy has drawn.
constexpr double least_drawn = .01;

// A masked quad, and a placed copy that lies in a margin of its visibility range, each coincident with an opaque quad
// and added before it, draw through the pipeline that may discard, so after the opaque quad, which shows.
inline void check_groups(blending_test::Harness &harness) {
    const auto view = blending_test::perspective_view(harness.aspect());
    const auto opaque = quad(blending_test::opaque(blending_test::red));
    render(harness, "opaque", {scene_of({opaque})}, view, 1, 0);
    render(harness, "masked-first", {scene_of({quad(masked(blending_test::green)), opaque})}, view, 2, 1);
    harness.images.require_parity("opaque", "masked-first");
    const auto copied = quad(blending_test::opaque(blending_test::blue));
    render(harness, "empty", {scene_of({})}, view, 0, 0);
    render(harness, "dissolving", {dissolving(copied, {})}, view, 1, 1);
    harness.images.require_changed("empty", "dissolving", least_drawn, "The dissolving copy drew nothing to compare");
    render(harness, "dissolving-first", {dissolving(copied, {opaque})}, view, 2, 1);
    harness.images.require_parity("opaque", "dissolving-first");
    harness.images.discard({"opaque", "masked-first", "empty", "dissolving", "dissolving-first"});
}

// One object's draws in Mesh::primitives() order within a group: a placed copy, in a margin of its visibility range, of
// a mesh whose masked first draw coincides with an opaque second one. The copy's cluster draws both through the
// pipeline that may discard, in its group, the opaque draw included although its own pipeline cannot discard; there the
// masked draw, first in Mesh::primitives(), records first and shows.
inline void check_object_draws(blending_test::Harness &harness) {
    const auto view = blending_test::perspective_view(harness.aspect());
    render(harness, "empty", {scene_of({})}, view, 0, 0);
    render(harness, "masked-layer", {dissolving(quad(masked(blending_test::green)), {})}, view, 1, 1);
    harness.images.require_changed("empty", "masked-layer", least_drawn, "The dissolving copy drew nothing to compare");
    const auto layers = layered({masked(blending_test::green), blending_test::opaque(blending_test::red)});
    render(harness, "masked-layer-first", {dissolving(layers, {})}, view, 2, 2);
    harness.images.require_parity("masked-layer", "masked-layer-first");
    harness.images.discard({"empty", "masked-layer", "masked-layer-first"});
}

// Coincident quads through one pipeline, opaque and then masked: the one whose world bounds reach toward the camera
// draws first though added last, in perspective and orthographic views.
inline void check_nearest_first(blending_test::Harness &harness) {
    const auto aspect = harness.aspect();
    struct Surfaces {
        std::string kind;
        anima::Material behind, reaching;
        // How many of each quad's draw calls go through the pipeline that may discard.
        std::uint64_t discarding{};
    };
    for (const auto &[kind, behind_surface, reaching_surface, discarding] :
         {Surfaces{"opaque", blending_test::opaque(blending_test::blue), blending_test::opaque(yellow), 0},
          Surfaces{"masked", masked(blending_test::blue), masked(yellow), 1}}) {
        const auto behind = quad(behind_surface);
        const auto reaching = quad(reaching_surface, plane, Coverage::whole, Reach::toward_camera);
        for (const auto &[projection, view] :
             {std::pair{std::string("perspective"), blending_test::perspective_view(aspect)},
              std::pair{std::string("orthographic"), blending_test::orthographic_view(aspect)}}) {
            const auto alone = "reaching-" + kind + "-" + projection, both = "behind-first-" + kind + "-" + projection;
            render(harness, alone, {scene_of({reaching})}, view, 1, discarding);
            render(harness, both, {scene_of({behind, reaching})}, view, 2, 2 * discarding);
            harness.images.require_parity(alone, both);
            harness.images.discard({alone, both});
        }
    }
}

// Coincident opaque quads with equal bounds keep the order in which they were added, within one scene and across the
// selection: the first one shows.
inline void check_ties(blending_test::Harness &harness) {
    const auto view = blending_test::perspective_view(harness.aspect());
    const auto first = quad(blending_test::opaque(blending_test::red)),
               second = quad(blending_test::opaque(blending_test::blue));
    render(harness, "first", {scene_of({first})}, view, 1, 0);
    render(harness, "tied-instances", {scene_of({first, second})}, view, 2, 0);
    harness.images.require_parity("first", "tied-instances");
    render(harness, "tied-scenes", {scene_of({first}), scene_of({second})}, view, 2, 0);
    harness.images.require_parity("first", "tied-scenes");
    harness.images.discard({"first", "tied-instances", "tied-scenes"});
}

// A masked card in front of an opaque quad, added before it: the card's kept half hides the quad, which shows through
// its cut half.
inline void check_masked_card(blending_test::Harness &harness) {
    const auto view = blending_test::perspective_view(harness.aspect());
    constexpr float card_depth = plane + .5F, kept = .5F, cut = -.5F;
    const auto card = quad(masked(blending_test::green), card_depth, Coverage::right_half);
    render(harness, "card-in-front", {scene_of({card, quad(blending_test::opaque(blending_test::red))})}, view, 2, 1);
    harness.expect("card-in-front", view, {kept, 0, card_depth}, blending_test::green,
                   "The kept half of a masked card in front of an opaque quad");
    harness.expect("card-in-front", view, {cut, 0, card_depth}, blending_test::red,
                   "An opaque quad through the cut half of a masked card in front of it");
    harness.images.discard({"card-in-front"});
}

inline int run(int argc, char **argv) {
    require(argc == 3, "Usage: consumer --draw-order OUTPUT");
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    blending_test::Harness harness(argv[2]);
    check_groups(harness);
    check_object_draws(harness);
    check_nearest_first(harness);
    check_ties(harness);
    check_masked_card(harness);
    harness.finish();
    std::cout << "PASS draw order: draw calls that cannot discard before those that may, a dissolving placement "
                 "cluster's included, an object's draws in their order within a group, opaque and masked objects "
                 "nearest first in perspective and orthographic views and tied ones in the order they were added, and "
                 "a masked card in front of an opaque quad\n";
    return 0;
}
} // namespace draw_order_test
