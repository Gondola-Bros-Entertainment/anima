#include <anima/assets/evaluation.hpp>
#include <anima/assets/interaction.hpp>
#include <anima/assets/motion_runtime.hpp>
#include <iostream>
#include <limits>
#include <numbers>

namespace {
using namespace anima;
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
void close(float a, float b, float tolerance = 1e-5F) { check(std::abs(a - b) < tolerance, "Numeric mismatch"); }
void same(const Mat4 &a, const Mat4 &b, float tolerance = 1e-5F) {
    for (unsigned i = 0; i < 16; ++i)
        close(a[i], b[i], tolerance);
}
template <class F> void rejects(F f) {
    try {
        f();
    } catch (const std::exception &) {
        return;
    }
    throw std::runtime_error("Expected invalid evaluation input to be rejected");
}
// A default-initialized contact result holds defined values: reading an uninitialized member would not be a
// constant expression, so this would not compile.
static_assert([] {
    MotionEvaluation::Contact contact;
    return contact.error == 0 && !contact.reachable && contact.weight == 1;
}());
Mat4 translated(Vec3 v) {
    Transform t;
    t.translation = v;
    return matrix(t);
}
Vec3 at(const Mat4 &m) { return point(m, {}); }
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
void affine_tests() {
    Transform t;
    t.rotation = {0, 0, std::sin(.6F), std::cos(.6F)};
    t.scale = {2, .7F, 1.4F};
    auto shear = identity();
    shear[4] = .35F;
    shear[9] = -.2F;
    const auto a = matrix(t) * shear;
    same(blend_affine(a, a, .37F), a);
    check(blend_affine(a, identity(), 0) == a, "Zero blend changed exact input");
    check(blend_affine(identity(), a, 1) == a, "Full blend changed exact input");
    auto rotation = identity();
    rotation[0] = -1;
    rotation[5] = -1;
    const auto half = blend_affine(identity(), rotation, .5F);
    close(length(at(half * translated({1, 0, 0}))), 1);
    close(std::abs(half[1]), 1);
    auto singular = identity();
    singular[0] = 0;
    auto reflected = identity();
    reflected[0] = -1;
    auto projective = identity();
    projective[3] = .2F;
    const auto shrunk = blend_affine(identity(), singular, .5F);
    check(shrunk[0] == .5F && shrunk[5] == 1 && shrunk[10] == 1,
          "A collapsed transform did not blend element by element");
    rejects([&] { (void)blend_affine(identity(), reflected, .5F); });
    rejects([&] { (void)blend_affine(identity(), projective, .5F); });
    rejects([&] { (void)blend_affine(a, a, std::numeric_limits<float>::quiet_NaN()); });
}
// A joint scaled to zero, as a clip hides a part, stays evaluable when the pose has local transforms.
void collapse_tests() {
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
    for (std::size_t joint = 0; joint < rig.size(); ++joint)
        same(world[joint], pose.world[rig.asset_node(joint)]);
    // The tip is unmapped and hangs below the collapsed hand.
    same(rig.render_pose(pose, encoded).world[3], pose.world[3]);
    // A world-only pose has no local transforms to recover the hand below the collapsed arm.
    Pose world_only;
    world_only.world = pose.world;
    bool needs_locals = false;
    try {
        (void)rig.encode(world_only);
    } catch (const std::invalid_argument &error) {
        needs_locals = std::string_view(error.what()) ==
                       "A joint below a collapsed joint needs the source pose's local transforms";
    }
    check(needs_locals, "A world-only pose below a collapsed joint was not rejected with its reason");
}
void rig_tests() {
    const auto asset = fixture();
    const EvaluationRig rig(asset, joints());
    bool unknown_joint = false;
    try {
        (void)rig.joint("missing");
    } catch (const std::out_of_range &error) {
        unknown_joint = std::string_view(error.what()) == "Unknown evaluation joint: missing";
    }
    check(unknown_joint, "An unknown evaluation joint was not reported as std::out_of_range");
    auto source = sample_pose(asset);
    // Unequal scale plus differently oriented child induces real affine shear.
    Transform chest;
    chest.rotation = {0, 0, std::sin(.35F), std::cos(.35F)};
    chest.scale = {1.5F, .7F, 1};
    source.world[1] = matrix(chest);
    const auto encoded = rig.encode(source);
    const auto world = rig.world(encoded);
    for (std::size_t i = 0; i < rig.size(); ++i)
        same(world[i], source.world[i]);
    const auto roundtrip = rig.render_pose(source, encoded);
    check(roundtrip.local.empty(), "Render palette pretends to contain TRS locals");
    for (std::size_t i = 0; i < source.world.size(); ++i)
        same(roundtrip.world[i], source.world[i]);
    rejects([&] { (void)blend_pose(asset, source, roundtrip, .5F); });
    auto offset = encoded;
    offset.local[1] = translated({0, .15F, 0}) * offset.local[1];
    const auto moved = rig.world(offset);
    close(at(moved[4]).y - at(world[4]).y, .15F);
    same(moved[6], world[6]);
    const auto rendered = rig.render_pose(source, offset);
    close(at(rendered.world[7]).y - at(source.world[7]).y, .15F);
    // An additive wrist action uses a declared reference and only its region.
    auto action = encoded;
    action.local[4] = action.local[4] * translated({0, .2F, 0});
    const auto mask = rig.subtree_mask(4, .5F);
    const auto result = rig.layer(offset, action, mask, LayerMode::additive, &encoded);
    const auto expected = offset.local[4] * translated({0, .1F, 0});
    same(result.local[4], expected);
    same(result.local[1], offset.local[1]);
    const auto override = rig.layer(offset, encoded, rig.subtree_mask(2));
    same(override.local[1], offset.local[1]);
    same(override.local[4], encoded.local[4]);
    rejects([&] { (void)rig.layer(encoded, action, mask, LayerMode::additive); });
    auto bad = joints();
    bad[0].parent = 5;
    rejects([&] { EvaluationRig invalid(asset, bad); });
    bad = joints();
    bad.pop_back();
    rejects([&] { EvaluationRig invalid(asset, bad); });
    bad = joints();
    bad[1].asset_node = 0;
    rejects([&] { EvaluationRig invalid(asset, bad); });
    rejects([&] { (void)rig.subtree_mask(100); });
}
void contact_tests() {
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
    check(solved.reachable && solved.error < 2e-5F, "Reachable contact missed target");
    const auto w = rig.world(solved.pose);
    close(length(at(w[3]) - at(w[2])), 1);
    close(length(at(w[4]) - at(w[3])), 1);
    close(length(at(w[5]) - at(w[4])), .1F);
    check(at(w[3]).z > 0, "Elbow ignored pole");
    for (unsigned i = 0; i < 12; ++i)
        close(w[4][i], identity()[i]);
    same(w[6], source.world[6]);
    c.target = {10, 0, 0};
    const auto far = solve_contact(rig, initial, c);
    check(!far.reachable, "Unreachable target was reported reachable");
    close(length(at(rig.world(far.pose)[4])), 2);
    c.target = {1, 1, 0};
    c.maximum_angle = 1.0F;
    check(!solve_contact(rig, initial, c).reachable, "Joint limit was ignored");
    c.maximum_angle = std::numbers::pi_v<float>;
    c.weight = 0;
    const auto none = rig.world(solve_contact(rig, initial, c).pose);
    for (std::size_t i = 0; i < rig.size(); ++i)
        same(none[i], source.world[i]);
    c.weight = .5F;
    const auto partial = solve_contact(rig, initial, c);
    check(partial.error > 0 && partial.error < length(c.target - at(source.world[4])),
          "Weighted solve did not move toward contact");
    const auto partial_world = rig.world(partial.pose);
    close(length(at(partial_world[3]) - at(partial_world[2])), 1);
    close(length(at(partial_world[4]) - at(partial_world[3])), 1);
    c.weight = 1;
    c.target = {0, 0, 0};
    c.pole = {0, 0, 0};
    const auto folded = solve_contact(rig, initial, c);
    check(folded.error < 1e-4F, "Folded/degenerate-direction fallback failed");
    c.middle = 6;
    rejects([&] { (void)solve_contact(rig, initial, c); });
}
void interaction_tests() {
    const PhaseTrack leader({{0, 0}, {.25, .3}, {.6, .55}, {1, .8}});
    const PhaseTrack follower({{0, 0}, {.25, .15}, {.6, .7}, {1, 1.1}});
    close(static_cast<float>(leader.time(.25, false)), .3F);
    close(static_cast<float>(follower.time(.25, false)), .15F);
    close(static_cast<float>(leader.time(3.25, true)), .3F);
    close(static_cast<float>(follower.time(2, false)), 1.1F);
    rejects([] { PhaseTrack bad({{0, 0}, {.8, .3}, {.4, .8}, {1, 1}}); });
    rejects([&] { (void)leader.time(-1, true); });
    // Deliberately different node counts and clip timings: no shared skeleton
    // is required between an interaction's participants.
    Pose mount, rider;
    mount.world.resize(3, identity());
    rider.world.resize(7, identity());
    rider.world[5] = translated({0, 1, 0});
    const PoseFrame seat{2, translated({0, .12F, 0})}, pelvis{5, identity()}, rein{1, translated({.1F, 0, .2F})};
    for (unsigned i = 0; i < 40; ++i) {
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
        close(length(at(placed) - at(target)), 0);
        close(length(Vec3{rider_world[0], rider_world[1], rider_world[2]}), 1);
        const auto contact = interaction_contact(rider_world, mount_world, mount, rein);
        same(rider_world * contact, mount_world * pose_frame(mount, rein));
    }
}
} // namespace
int main() {
    try {
        affine_tests();
        rig_tests();
        collapse_tests();
        contact_tests();
        interaction_tests();
        std::cout << "PASS affine evaluation, layers, skin mapping, bounded contacts and interaction frames\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
