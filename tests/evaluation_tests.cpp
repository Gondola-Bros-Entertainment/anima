#include "near.hpp"
#include <anima/assets/evaluation.hpp>
#include <anima/assets/interaction.hpp>
#include <anima/assets/motion_runtime.hpp>
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

using namespace anima;
namespace {
constexpr float tolerance = 1e-5F; // Absolute error allowed in evaluated matrix elements and lengths.

// A default-initialized contact result holds defined values: reading an uninitialized member would not be a
// constant expression, so this would not compile.
static_assert([] {
    MotionEvaluation::Contact contact;
    return contact.error == 0 && !contact.reachable && contact.weight == 1;
}());
// Largest absolute difference between corresponding elements.
float difference(const Mat4 &a, const Mat4 &b) {
    float d = 0;
    for (unsigned i = 0; i < 16; ++i)
        d = std::max(d, std::abs(a[i] - b[i]));
    return d;
}
Mat4 translated(Vec3 v) {
    Transform t;
    t.translation = v;
    return matrix(t);
}
Vec3 at(const Mat4 &m) { return point(m, {}); }
// Nodes directly below the root, placed along an arm from the shoulder to the finger, plus another limb and an
// attachment below the wrist. joints() chains the arm; the skin uses every node but the attachment.
Asset fixture() {
    Asset a;
    for (const auto *name : {"root", "chest", "shoulder", "elbow", "wrist", "finger", "other", "attachment"}) {
        AssetNode n;
        n.name = name;
        n.parent = a.nodes.empty() ? -1 : 0;
        a.nodes.push_back(n);
    }
    a.nodes[3].rest.translation = {1, 0, 0};
    a.nodes[4].rest.translation = {2, 0, 0};
    a.nodes[5].rest.translation = {2.1F, 0, 0};
    a.nodes[6].rest.translation = {0, -1, 0};
    a.nodes[7].parent = 4;
    a.nodes[7].rest.translation = {.2F, 0, 0};
    a.skins.push_back({{0, 1, 2, 3, 4, 5, 6}, std::vector<Mat4>(7, identity())});
    return a;
}
std::vector<EvaluationJoint> joints() {
    return {{"root", 0, -1}, {"chest", 1, 0},  {"shoulder", 2, 1}, {"elbow", 3, 2},
            {"wrist", 4, 3}, {"finger", 5, 4}, {"other", 6, 0}};
}
// The rest pose with a rotated, unequally scaled chest. Unequal scale plus differently oriented child induces
// real affine shear.
Pose sheared_pose(const Asset &asset) {
    auto source = sample_pose(asset);
    Transform chest;
    chest.rotation = {0, 0, std::sin(.35F), std::cos(.35F)};
    chest.scale = {1.5F, .7F, 1};
    source.world[1] = matrix(chest);
    return source;
}
Mat4 sheared() {
    Transform t;
    t.rotation = {0, 0, std::sin(.6F), std::cos(.6F)};
    t.scale = {2, .7F, 1.4F};
    auto shear = identity();
    shear[4] = .35F;
    shear[9] = -.2F;
    return matrix(t) * shear;
}
} // namespace

TEST_CASE("Affine blending returns its exact endpoints and interpolates rotation and stretch") {
    const auto a = sheared();
    CHECK(difference(blend_affine(a, a, .37F), a) < tolerance);
    CHECK(blend_affine(a, identity(), 0) == a);
    CHECK(blend_affine(identity(), a, 1) == a);
    // Halfway to a half turn about Z is a quarter turn, which keeps unit length.
    auto rotation = identity();
    rotation[0] = -1;
    rotation[5] = -1;
    const auto half = blend_affine(identity(), rotation, .5F);
    CHECK(length(at(half * translated({1, 0, 0}))) == Near{1, tolerance});
    CHECK(std::abs(half[1]) == Near{1, tolerance});
    // A collapsed transform has no rotation to extract, so it blends element by element.
    auto singular = identity();
    singular[0] = 0;
    const auto shrunk = blend_affine(identity(), singular, .5F);
    CHECK(shrunk[0] == .5F);
    CHECK(shrunk[5] == 1);
    CHECK(shrunk[10] == 1);
}

TEST_CASE("Reflected and projective transforms and invalid weights are rejected") {
    auto reflected = identity();
    reflected[0] = -1;
    CHECK_THROWS_WITH_AS(blend_affine(identity(), reflected, .5F), "Evaluation needs transforms without reflection",
                         std::invalid_argument);
    auto projective = identity();
    projective[3] = .2F;
    CHECK_THROWS_WITH_AS(blend_affine(identity(), projective, .5F), "Evaluation needs affine transforms",
                         std::invalid_argument);
    const auto a = sheared();
    CHECK_THROWS_WITH_AS(blend_affine(a, a, std::numeric_limits<float>::quiet_NaN()),
                         "Layer/contact weight must be in [0,1]", std::invalid_argument);
}

TEST_CASE("A joint scaled to zero stays evaluable when the pose has local transforms") {
    Asset asset;
    for (const auto *name : {"root", "arm", "hand", "tip"}) {
        AssetNode node;
        node.name = name;
        node.parent = static_cast<int>(asset.nodes.size()) - 1;
        node.rest.translation = {0, asset.nodes.empty() ? 0.F : 1.F, 0};
        asset.nodes.push_back(node);
    }
    const EvaluationRig rig(asset, {{"root", 0, -1}, {"arm", 1, 0}, {"hand", 2, 1}});
    auto local = sample_pose(asset).local;
    local[1].scale = {0, 0, 0};
    const auto pose = pose_from_local(asset, local);
    const auto encoded = rig.encode(pose);
    const auto world = rig.world(encoded);
    for (std::size_t joint = 0; joint < rig.size(); ++joint) {
        CAPTURE(joint);
        CHECK(difference(world[joint], pose.world[rig.asset_node(joint)]) < tolerance);
    }
    // The tip is unmapped and hangs below the collapsed hand.
    CHECK(difference(rig.render_pose(pose, encoded).world[3], pose.world[3]) < tolerance);
    // A world-only pose has no local transforms to recover the hand below the collapsed arm.
    Pose world_only;
    world_only.world = pose.world;
    CHECK_THROWS_WITH_AS(rig.encode(world_only),
                         "A joint below a collapsed joint needs the source pose's local transforms",
                         std::invalid_argument);
}

TEST_CASE("An unknown evaluation joint is reported as std::out_of_range") {
    const auto asset = fixture();
    const EvaluationRig rig(asset, joints());
    CHECK_THROWS_WITH_AS(rig.joint("missing"), "Unknown evaluation joint: missing", std::out_of_range);
}

TEST_CASE("A rig round-trips a sheared pose and propagates a local offset to its descendants") {
    const auto asset = fixture();
    const EvaluationRig rig(asset, joints());
    const auto source = sheared_pose(asset);
    const auto encoded = rig.encode(source);
    const auto world = rig.world(encoded);
    for (std::size_t i = 0; i < rig.size(); ++i) {
        CAPTURE(i);
        CHECK(difference(world[i], source.world[i]) < tolerance);
    }
    const auto roundtrip = rig.render_pose(source, encoded);
    CHECK(roundtrip.local.empty()); // A render palette has no TRS locals.
    for (std::size_t i = 0; i < source.world.size(); ++i) {
        CAPTURE(i);
        CHECK(difference(roundtrip.world[i], source.world[i]) < tolerance);
    }
    CHECK_THROWS_WITH_AS(blend_pose(asset, source, roundtrip, .5F),
                         "Pose blend requires matching local poses and a weight in [0,1]", std::invalid_argument);
    auto offset = encoded;
    offset.local[1] = translated({0, .15F, 0}) * offset.local[1];
    const auto moved = rig.world(offset);
    CHECK(at(moved[4]).y - at(world[4]).y == Near{.15F, tolerance}); // The wrist follows the chest.
    CHECK(difference(moved[6], world[6]) < tolerance);               // The other limb does not.
    const auto rendered = rig.render_pose(source, offset);
    // The unmapped attachment follows the wrist.
    CHECK(at(rendered.world[7]).y - at(source.world[7]).y == Near{.15F, tolerance});
}

TEST_CASE("An additive layer applies its declared reference within its mask") {
    const auto asset = fixture();
    const EvaluationRig rig(asset, joints());
    const auto encoded = rig.encode(sheared_pose(asset));
    auto offset = encoded;
    offset.local[1] = translated({0, .15F, 0}) * offset.local[1];
    // An additive wrist action uses a declared reference and only its region.
    auto action = encoded;
    action.local[4] = action.local[4] * translated({0, .2F, 0});
    const auto mask = rig.subtree_mask(4, .5F);
    const auto result = rig.layer(offset, action, mask, LayerMode::additive, &encoded);
    CHECK(difference(result.local[4], offset.local[4] * translated({0, .1F, 0})) < tolerance);
    CHECK(difference(result.local[1], offset.local[1]) < tolerance);
    const auto override = rig.layer(offset, encoded, rig.subtree_mask(2));
    CHECK(difference(override.local[1], offset.local[1]) < tolerance);
    CHECK(difference(override.local[4], encoded.local[4]) < tolerance);
    CHECK_THROWS_WITH_AS(rig.layer(encoded, action, mask, LayerMode::additive),
                         "Layer reference does not match its mode", std::invalid_argument);
}

TEST_CASE("Invalid rigs and mask roots are rejected") {
    const auto asset = fixture();
    auto bad = joints();
    bad[0].parent = 5;
    CHECK_THROWS_WITH_AS(EvaluationRig(asset, bad), "Cyclic evaluation/asset hierarchy", std::invalid_argument);
    bad = joints();
    bad.pop_back();
    CHECK_THROWS_WITH_AS(EvaluationRig(asset, bad), "Unmapped skin joint", std::invalid_argument);
    bad = joints();
    bad[1].asset_node = 0;
    CHECK_THROWS_WITH_AS(EvaluationRig(asset, bad), "Duplicate/missing evaluation node", std::invalid_argument);
    const EvaluationRig rig(asset, joints());
    CHECK_THROWS_WITH_AS(rig.subtree_mask(100), "Invalid mask root", std::invalid_argument);
}

TEST_CASE("A two-bone contact reaches within its limits, follows its pole and blends by weight") {
    const auto asset = fixture();
    const EvaluationRig rig(asset, joints());
    const auto source = sample_pose(asset);
    const auto initial = rig.encode(source);
    TwoBoneContact c;
    c.start = 2;
    c.middle = 3;
    c.end = 4;
    c.target = {1, 1, 0};
    c.pole = {0, 0, 1};
    c.end_rotation = Quat{0, 0, 0, 1};
    const auto solved = solve_contact(rig, initial, c);
    CHECK(solved.reachable);
    CHECK(solved.error < 2e-5F);
    const auto w = rig.world(solved.pose);
    // The limbs keep their lengths, the elbow bends toward the pole, and the wrist takes the end rotation.
    CHECK(length(at(w[3]) - at(w[2])) == Near{1, tolerance});
    CHECK(length(at(w[4]) - at(w[3])) == Near{1, tolerance});
    CHECK(length(at(w[5]) - at(w[4])) == Near{.1F, tolerance});
    CHECK(at(w[3]).z > 0);
    for (unsigned i = 0; i < 12; ++i) {
        CAPTURE(i);
        CHECK(w[4][i] == Near{identity()[i], tolerance});
    }
    CHECK(difference(w[6], source.world[6]) < tolerance);
    c.target = {10, 0, 0};
    const auto far = solve_contact(rig, initial, c);
    CHECK_FALSE(far.reachable);
    CHECK(length(at(rig.world(far.pose)[4])) == Near{2, tolerance}); // Fully extended toward it.
    c.target = {1, 1, 0};
    c.maximum_angle = 1.0F;
    CHECK_FALSE(solve_contact(rig, initial, c).reachable); // Beyond the joint limit.
    c.maximum_angle = std::numbers::pi_v<float>;
    c.weight = 0;
    const auto none = rig.world(solve_contact(rig, initial, c).pose);
    for (std::size_t i = 0; i < rig.size(); ++i) {
        CAPTURE(i);
        CHECK(difference(none[i], source.world[i]) < tolerance);
    }
    c.weight = .5F;
    const auto partial = solve_contact(rig, initial, c);
    // A weighted solve moves toward the contact.
    CHECK(partial.error > 0);
    CHECK(partial.error < length(c.target - at(source.world[4])));
    const auto partial_world = rig.world(partial.pose);
    CHECK(length(at(partial_world[3]) - at(partial_world[2])) == Near{1, tolerance});
    CHECK(length(at(partial_world[4]) - at(partial_world[3])) == Near{1, tolerance});
    // A target at the start with no pole falls back to a defined bend direction.
    c.weight = 1;
    c.target = {0, 0, 0};
    c.pole = {0, 0, 0};
    CHECK(solve_contact(rig, initial, c).error < 1e-4F);
    c.middle = 6;
    CHECK_THROWS_WITH_AS(solve_contact(rig, initial, c), "Contact needs an ordered, distinct two-bone chain",
                         std::invalid_argument);
}

TEST_CASE("A phase track maps phase to clip time") {
    const PhaseTrack leader({{0, 0}, {.25, .3}, {.6, .55}, {1, .8}});
    const PhaseTrack follower({{0, 0}, {.25, .15}, {.6, .7}, {1, 1.1}});
    CHECK(leader.time(.25, false) == Near{.3, tolerance});
    CHECK(follower.time(.25, false) == Near{.15, tolerance});
    CHECK(leader.time(3.25, true) == Near{.3, tolerance});
    CHECK(follower.time(2, false) == Near{1.1, tolerance});
    CHECK_THROWS_WITH_AS(PhaseTrack({{0, 0}, {.8, .3}, {.4, .8}, {1, 1}}),
                         "Interaction marker phases and times must increase strictly", std::invalid_argument);
    CHECK_THROWS_WITH_AS(leader.time(-1, true), "Invalid interaction phase", std::invalid_argument);
}

TEST_CASE("Interaction alignment puts the follower's anchor on the leader's moving attachment") {
    // Deliberately different node counts and clip timings: no shared skeleton
    // is required between an interaction's participants.
    Pose mount, rider;
    mount.world.resize(3, identity());
    rider.world.resize(7, identity());
    rider.world[5] = translated({0, 1, 0});
    const PoseFrame seat{2, translated({0, .12F, 0})}, pelvis{5, identity()}, rein{1, translated({.1F, 0, .2F})};
    for (unsigned i = 0; i < 40; ++i) {
        CAPTURE(i);
        Transform moving;
        moving.translation = {static_cast<float>(i) * .1F, .2F, 2};
        moving.rotation = {0, std::sin(i * .04F), 0, std::cos(i * .04F)};
        Transform back;
        back.translation = {0, 1.2F + std::sin(i * .15F) * .1F, 0};
        back.scale = {1.3F, .8F, 1.1F};
        mount.world[2] = matrix(back);
        mount.world[1] = translated({0, 1.5F, .5F});
        const auto mount_world = matrix(moving);
        const auto rider_world = align_interaction(mount_world, mount, seat, rider, pelvis);
        const auto placed = rider_world * pose_frame(rider, pelvis);
        const auto target = mount_world * pose_frame(mount, seat);
        CHECK(length(at(placed) - at(target)) == Near{0, tolerance});
        CHECK(length(Vec3{rider_world[0], rider_world[1], rider_world[2]}) == Near{1, tolerance}); // Unscaled.
        const auto contact = interaction_contact(rider_world, mount_world, mount, rein);
        CHECK(difference(rider_world * contact, mount_world * pose_frame(mount, rein)) < tolerance);
    }
}
