#include "rotation_matrix.hpp"
#include <anima/assets/evaluation.hpp>
#include <numbers>
#include <numeric>
#include <set>

namespace anima {
namespace {
// Vectors no longer than this have no usable direction.
constexpr float minimum_direction_length = 1e-8F;

void require(bool value, const char *message) {
    if (!value)
        throw std::invalid_argument(message);
}
void weight_valid(float weight) {
    require(std::isfinite(weight) && weight >= 0 && weight <= 1, "Layer/contact weight must be in [0,1]");
}
using M3 = std::array<std::array<double, 3>, 3>;
double determinant(const M3 &m) {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}
M3 linear(const Mat4 &m) {
    // Largest summed deviation of the bottom row from (0, 0, 0, 1) that still counts as affine.
    constexpr float affine_tolerance = 1e-5F;
    constexpr double minimum_determinant = 1e-12;
    for (float x : m)
        require(std::isfinite(x), "Nonfinite affine transform");
    require(std::abs(m[3]) + std::abs(m[7]) + std::abs(m[11]) + std::abs(m[15] - 1) < affine_tolerance,
            "Evaluation needs affine transforms");
    M3 result{};
    for (unsigned r = 0; r < 3; ++r)
        for (unsigned c = 0; c < 3; ++c)
            result[r][c] = m[c * 4 + r];
    require(determinant(result) > minimum_determinant, "Evaluation needs nonsingular, positive-determinant transforms");
    return result;
}
M3 inverse_transpose(const M3 &m) {
    // The inverse divides by the determinant, so smaller magnitudes count as singular.
    constexpr double singular_determinant = 1e-20;
    const double d = determinant(m);
    require(std::isfinite(d) && std::abs(d) > singular_determinant, "Polar decomposition is singular");
    M3 result{};
    for (unsigned r = 0; r < 3; ++r)
        for (unsigned c = 0; c < 3; ++c)
            result[r][c] = (m[(r + 1) % 3][(c + 1) % 3] * m[(r + 2) % 3][(c + 2) % 3] -
                            m[(r + 1) % 3][(c + 2) % 3] * m[(r + 2) % 3][(c + 1) % 3]) /
                           d;
    return result;
}
Quat quaternion(const M3 &m) {
    const auto q = detail::rotation_quaternion(m);
    require(q.has_value(), "Invalid polar rotation");
    return *q;
}
struct Polar {
    Quat rotation;
    M3 stretch;
};
Polar polar(const Mat4 &m) {
    constexpr unsigned maximum_iterations = 64;
    // The iteration has converged once a step changes no element by this much.
    constexpr double convergence = 1e-12;
    const auto a = linear(m);
    auto r = a;
    bool converged = false;
    for (unsigned iteration = 0; iteration < maximum_iterations; ++iteration) {
        const auto it = inverse_transpose(r);
        double nr = 0, ni = 0, error = 0;
        for (unsigned i = 0; i < 3; ++i)
            for (unsigned j = 0; j < 3; ++j) {
                nr += r[i][j] * r[i][j];
                ni += it[i][j] * it[i][j];
            }
        const auto gamma = std::pow(ni / nr, .25);
        for (unsigned i = 0; i < 3; ++i)
            for (unsigned j = 0; j < 3; ++j) {
                const double next = .5 * (gamma * r[i][j] + it[i][j] / gamma);
                error = std::max(error, std::abs(next - r[i][j]));
                r[i][j] = next;
            }
        if (error < convergence) {
            converged = true;
            break;
        }
    }
    require(converged, "Polar decomposition did not converge");
    M3 s{};
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 3; ++j)
            for (unsigned k = 0; k < 3; ++k)
                s[i][j] += r[k][i] * a[k][j];
    return {quaternion(r), s};
}
std::vector<std::size_t> order(const std::vector<int> &parents) {
    std::vector<unsigned char> visited(parents.size());
    std::vector<std::size_t> result;
    const auto visit = [&](const auto &self, std::size_t i) -> void {
        require(visited[i] != 1, "Cyclic evaluation/asset hierarchy");
        if (visited[i] == 2)
            return;
        visited[i] = 1;
        require(parents[i] >= -1 && parents[i] < static_cast<int>(parents.size()), "Missing evaluation/asset parent");
        if (parents[i] >= 0)
            self(self, static_cast<std::size_t>(parents[i]));
        visited[i] = 2;
        result.push_back(i);
    };
    for (std::size_t i = 0; i < parents.size(); ++i)
        visit(visit, i);
    return result;
}
// Returns a vector perpendicular to the unit vector @p v, at least 0.6 long: its cross product with the X
// axis, or with the Y axis when @p v lies near the X axis.
Vec3 perpendicular(Vec3 v) {
    constexpr float near_x_axis = .8F;
    return cross(v, std::abs(v.x) < near_x_axis ? Vec3{1, 0, 0} : Vec3{0, 1, 0});
}
Quat rotation_between(Vec3 from, Vec3 to) {
    // Below this cosine the directions are opposite and their half-way axis is undefined.
    constexpr float opposite_cosine = -.999999F;
    require(length(from) > minimum_direction_length && length(to) > minimum_direction_length,
            "Degenerate contact direction");
    from = normalized(from);
    to = normalized(to);
    const float cosine = std::clamp(dot(from, to), -1.F, 1.F);
    if (cosine < opposite_cosine) {
        const auto axis = normalized(perpendicular(from));
        return {axis.x, axis.y, axis.z, 0};
    }
    const auto axis = cross(from, to);
    return unit_quaternion({axis.x, axis.y, axis.z, 1 + cosine});
}
Mat4 rotation_matrix(Quat q) {
    Transform t;
    t.rotation = q;
    return matrix(t);
}
Mat4 about(Vec3 pivot, Quat q) {
    auto result = rotation_matrix(q);
    set_translation(result, pivot - point(result, pivot));
    return result;
}
} // namespace
Mat4 blend_affine(const Mat4 &from, const Mat4 &to, float weight) {
    weight_valid(weight);
    (void)linear(from);
    (void)linear(to);
    if (weight == 0)
        return from;
    if (weight == 1)
        return to;
    const auto a = polar(from), b = polar(to);
    const auto rotation = rotation_matrix(slerp(a.rotation, b.rotation, weight));
    Mat4 result = identity();
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 3; ++j) {
            double value = 0;
            for (unsigned k = 0; k < 3; ++k)
                value += rotation[k * 4 + i] * (a.stretch[k][j] * (1 - weight) + b.stretch[k][j] * weight);
            result[j * 4 + i] = static_cast<float>(value);
        }
    set_translation(result, translation_of(from) * (1 - weight) + translation_of(to) * weight);
    (void)linear(result);
    return result;
}
Quat affine_rotation(const Mat4 &transform) { return polar(transform).rotation; }
EvaluationRig::EvaluationRig(const Asset &asset, std::vector<EvaluationJoint> joints) : joints_(std::move(joints)) {
    require(!joints_.empty(), "Evaluation rig has no joints");
    mapping_.assign(asset.nodes.size(), -1);
    std::set<std::string> names;
    std::vector<int> parents;
    for (std::size_t i = 0; i < joints_.size(); ++i) {
        const auto &j = joints_[i];
        require(!j.name.empty() && names.insert(j.name).second, "Duplicate/empty evaluation joint");
        require(j.asset_node < asset.nodes.size() && mapping_[j.asset_node] < 0, "Duplicate/missing evaluation node");
        mapping_[j.asset_node] = static_cast<int>(i);
        parents.push_back(j.parent);
    }
    order_ = order(parents);
    for (const auto &skin : asset.skins)
        for (auto node : skin.joints)
            require(node < mapping_.size() && mapping_[node] >= 0, "Unmapped skin joint");
    for (const auto &node : asset.nodes)
        asset_parents_.push_back(node.parent);
    asset_order_ = order(asset_parents_);
}
std::size_t EvaluationRig::joint(std::string_view name) const {
    for (std::size_t i = 0; i < joints_.size(); ++i)
        if (joints_[i].name == name)
            return i;
    throw std::invalid_argument("Unknown evaluation joint: " + std::string(name));
}
bool EvaluationRig::descendant(std::size_t child, std::size_t ancestor) const {
    require(child < size() && ancestor < size(), "Invalid evaluation joint index");
    for (int n = static_cast<int>(child); n >= 0; n = joints_[static_cast<std::size_t>(n)].parent)
        if (static_cast<std::size_t>(n) == ancestor)
            return true;
    return false;
}
EvaluationPose EvaluationRig::encode(const Pose &source) const {
    require(source.world.size() == mapping_.size(), "Evaluation source does not match asset");
    std::vector<Mat4> values;
    for (const auto &joint : joints_)
        values.push_back(source.world[joint.asset_node]);
    return from_world(values);
}
EvaluationPose EvaluationRig::from_world(std::span<const Mat4> values) const {
    require(values.size() == size(), "Evaluation world count mismatch");
    EvaluationPose pose;
    pose.local.resize(size());
    for (auto i : order_) {
        (void)linear(values[i]);
        const auto p = joints_[i].parent;
        pose.local[i] = p < 0 ? values[i] : inverse(values[static_cast<std::size_t>(p)]) * values[i];
    }
    return pose;
}
std::vector<Mat4> EvaluationRig::world(const EvaluationPose &pose) const {
    require(pose.local.size() == size(), "Evaluation pose count mismatch");
    std::vector<Mat4> result(size());
    for (auto i : order_) {
        (void)linear(pose.local[i]);
        const auto p = joints_[i].parent;
        result[i] = p < 0 ? pose.local[i] : result[static_cast<std::size_t>(p)] * pose.local[i];
    }
    return result;
}
std::vector<float> EvaluationRig::subtree_mask(std::size_t root, float weight) const {
    weight_valid(weight);
    require(root < size(), "Invalid mask root");
    std::vector<float> result(size());
    for (std::size_t i = 0; i < size(); ++i)
        if (descendant(i, root))
            result[i] = weight;
    return result;
}
EvaluationPose EvaluationRig::layer(const EvaluationPose &base, const EvaluationPose &contribution,
                                    std::span<const float> weights, LayerMode mode,
                                    const EvaluationPose *reference) const {
    require(base.local.size() == size() && contribution.local.size() == size() && weights.size() == size(),
            "Layer pose/mask count mismatch");
    require((mode == LayerMode::additive && reference && reference->local.size() == size()) ||
                (mode == LayerMode::override_pose && !reference),
            "Layer reference does not match its mode");
    auto result = base;
    for (std::size_t i = 0; i < size(); ++i) {
        weight_valid(weights[i]);
        if (weights[i] == 0)
            continue;
        if (mode == LayerMode::override_pose)
            result.local[i] = blend_affine(base.local[i], contribution.local[i], weights[i]);
        else
            result.local[i] =
                base.local[i] *
                blend_affine(identity(), inverse(reference->local[i]) * contribution.local[i], weights[i]);
    }
    return result;
}
Pose EvaluationRig::render_pose(const Pose &source, const EvaluationPose &evaluated) const {
    require(source.world.size() == mapping_.size(), "Render source does not match asset");
    const auto values = world(evaluated);
    Pose result;
    result.world.resize(source.world.size());
    for (auto i : asset_order_) {
        if (mapping_[i] >= 0)
            result.world[i] = values[static_cast<std::size_t>(mapping_[i])];
        else if (asset_parents_[i] >= 0) {
            const auto p = static_cast<std::size_t>(asset_parents_[i]);
            result.world[i] = result.world[p] * inverse(source.world[p]) * source.world[i];
        } else
            result.world[i] = source.world[i];
    }
    return result;
}
ContactResult solve_contact(const EvaluationRig &rig, const EvaluationPose &pose, const TwoBoneContact &c) {
    weight_valid(c.weight);
    require(c.start != c.middle && c.middle != c.end && rig.descendant(c.middle, c.start) &&
                rig.descendant(c.end, c.middle),
            "Contact needs an ordered, distinct two-bone chain");
    // Limbs, and the shortest planned reach, must be longer than this.
    constexpr float minimum_limb_length = 1e-6F;
    // A bend direction shorter than this does not define the bend plane.
    constexpr float minimum_bend_length = 1e-7F;
    // A target this far outside the reach the angle limits allow still counts as reachable.
    constexpr float reach_tolerance = 1e-5F;
    require(std::isfinite(c.minimum_angle) && std::isfinite(c.maximum_angle) && c.minimum_angle >= 0 &&
                c.maximum_angle <= std::numbers::pi_v<float> && c.minimum_angle < c.maximum_angle,
            "Invalid contact angle limits");
    for (auto v : {c.target.x, c.target.y, c.target.z, c.pole.x, c.pole.y, c.pole.z})
        require(std::isfinite(v), "Nonfinite contact target/pole");
    auto w = rig.world(pose);
    const auto start = translation_of(w[c.start]), middle = translation_of(w[c.middle]), end = translation_of(w[c.end]);
    const float a = length(middle - start), b = length(end - middle), requested = length(c.target - start);
    require(a > minimum_limb_length && b > minimum_limb_length, "Degenerate contact limb");
    const auto distance_for = [&](float angle) {
        return std::sqrt(std::max(0.F, a * a + b * b - 2 * a * b * std::cos(angle)));
    };
    const float minimum = std::max(distance_for(c.minimum_angle), minimum_limb_length),
                maximum = distance_for(c.maximum_angle);
    require(maximum >= minimum, "Contact limits have no usable reach");
    const float distance = std::clamp(requested, minimum, maximum);
    const auto direction = normalized(requested > minimum_direction_length ? c.target - start : end - start);
    auto bend = c.pole - start - direction * dot(c.pole - start, direction);
    if (length(bend) < minimum_bend_length)
        bend = middle - start - direction * dot(middle - start, direction);
    if (length(bend) < minimum_bend_length)
        bend = perpendicular(direction);
    bend = normalized(bend);
    const float along = (a * a - b * b + distance * distance) / (2 * distance);
    const auto new_middle = start + direction * along + bend * std::sqrt(std::max(0.F, a * a - along * along));
    const auto new_end = start + direction * distance;
    const auto rotate = [&](std::size_t joint, Vec3 pivot, Quat rotation) {
        const auto transform = about(pivot, rotation);
        for (std::size_t i = 0; i < rig.size(); ++i)
            if (rig.descendant(i, joint))
                w[i] = transform * w[i];
    };
    if (c.weight > 0) {
        rotate(c.start, start, rotation_between(middle - start, new_middle - start));
        rotate(c.middle, translation_of(w[c.middle]),
               rotation_between(translation_of(w[c.end]) - translation_of(w[c.middle]),
                                new_end - translation_of(w[c.middle])));
        if (c.end_rotation) {
            const auto desired = rotation_matrix(unit_quaternion(*c.end_rotation));
            const auto current = rotation_matrix(polar(w[c.end]).rotation);
            const auto delta = desired * inverse(current);
            rotate(c.end, translation_of(w[c.end]), quaternion(linear(delta)));
        }
    }
    auto result = rig.from_world(w);
    if (c.weight < 1)
        result = rig.layer(pose, result, rig.subtree_mask(c.start, c.weight));
    const auto final = rig.world(result);
    return {std::move(result), length(translation_of(final[c.end]) - c.target), a, b,
            requested >= minimum - reach_tolerance && requested <= maximum + reach_tolerance};
}
} // namespace anima
