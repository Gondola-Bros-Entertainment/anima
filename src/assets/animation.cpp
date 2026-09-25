#include <anima/assets/asset.hpp>
#include <functional>
#include <limits>
#include <stdexcept>

namespace anima {
namespace {
void resolve_world(const Asset &asset, Pose &pose);
}
Pose sample_pose(const Asset &asset, const Animation *animation, double time) {
    if (!std::isfinite(time) || time < 0)
        throw std::invalid_argument("Pose time must be finite and nonnegative");
    Pose pose;
    pose.local.reserve(asset.nodes.size());
    pose.world.resize(asset.nodes.size());
    for (const auto &node : asset.nodes)
        pose.local.push_back(node.rest);
    if (animation)
        for (const auto &channel : animation->channels) {
            if (channel.node >= asset.nodes.size() || channel.times.empty() ||
                channel.times.size() != channel.values.size())
                throw std::runtime_error("Invalid animation channel");
            if (asset.nodes[channel.node].has_matrix)
                throw std::runtime_error("Animation cannot target a matrix node");
            auto upper = std::upper_bound(channel.times.begin(), channel.times.end(), time);
            std::size_t a =
                upper == channel.times.begin() ? 0 : static_cast<std::size_t>(upper - channel.times.begin() - 1);
            const auto b = std::min(a + 1, channel.times.size() - 1);
            const auto fraction =
                b == a || time <= channel.times[a]
                    ? 0.F
                    : static_cast<float>((time - channel.times[a]) / (channel.times[b] - channel.times[a]));
            auto value = channel.values[a];
            if (channel.interpolation == Interpolation::linear && b != a) {
                if (channel.path == ChannelPath::rotation)
                    value = slerp(value, channel.values[b], fraction);
                else
                    for (unsigned i = 0; i < 4; ++i)
                        value[i] += (channel.values[b][i] - value[i]) * fraction;
            }
            auto &local = pose.local[channel.node];
            if (channel.path == ChannelPath::rotation)
                local.rotation = unit_quaternion(value);
            else if (channel.path == ChannelPath::translation)
                local.translation = {value[0], value[1], value[2]};
            else
                local.scale = {value[0], value[1], value[2]};
        }
    resolve_world(asset, pose);
    return pose;
}
Pose pose_from_local(const Asset &asset, std::span<const Transform> local) {
    if (local.size() != asset.nodes.size())
        throw std::invalid_argument("Local pose must cover every asset node");
    Pose result;
    result.local.assign(local.begin(), local.end());
    result.world.resize(local.size());
    resolve_world(asset, result);
    return result;
}
Pose blend_pose(const Asset &asset, const Pose &from, const Pose &to, float weight) {
    if (!std::isfinite(weight) || weight < 0 || weight > 1 || from.local.size() != asset.nodes.size() ||
        to.local.size() != asset.nodes.size())
        throw std::invalid_argument("Pose blend requires matching local poses and a weight in [0,1]");
    Pose pose;
    pose.local.resize(asset.nodes.size());
    pose.world.resize(asset.nodes.size());
    for (std::size_t i = 0; i < asset.nodes.size(); ++i) {
        const auto &a = from.local[i];
        const auto &b = to.local[i];
        auto &result = pose.local[i];
        if (asset.nodes[i].has_matrix)
            result = asset.nodes[i].rest;
        else {
            result.translation = a.translation * (1 - weight) + b.translation * weight;
            result.scale = a.scale * (1 - weight) + b.scale * weight;
            result.rotation = slerp(a.rotation, b.rotation, weight);
        }
    }
    resolve_world(asset, pose);
    return pose;
}
namespace {
void resolve_world(const Asset &asset, Pose &pose) {
    std::vector<unsigned char> visited(asset.nodes.size());
    const auto visit = [&](const auto &self, std::size_t i) -> void {
        if (visited[i] == 2)
            return;
        if (visited[i] == 1)
            throw std::runtime_error("Cycle in asset node hierarchy");
        visited[i] = 1;
        const auto &node = asset.nodes[i];
        auto local = node.has_matrix ? node.rest_matrix : matrix(pose.local[i]);
        if (node.parent >= 0) {
            const auto parent = static_cast<std::size_t>(node.parent);
            if (parent >= asset.nodes.size())
                throw std::runtime_error("Invalid node parent");
            self(self, parent);
            local = pose.world[parent] * local;
        }
        for (auto value : local)
            if (!std::isfinite(value))
                throw std::runtime_error("Non-finite posed transform");
        pose.world[i] = local;
        visited[i] = 2;
    };
    for (std::size_t i = 0; i < asset.nodes.size(); ++i)
        visit(visit, i);
}
} // namespace
const Animation &find_animation(const Asset &asset, std::string_view name) {
    const Animation *result = nullptr;
    for (const auto &clip : asset.animations)
        if (clip.name == name) {
            if (result)
                throw std::runtime_error("Ambiguous animation name: " + std::string(name));
            result = &clip;
        }
    if (!result)
        throw std::runtime_error("Missing animation: " + std::string(name));
    return *result;
}
std::size_t unique_node(const Asset &asset, const std::string &name) {
    auto result = asset.nodes.size();
    for (std::size_t i = 0; i < asset.nodes.size(); ++i)
        if (asset.nodes[i].name == name) {
            if (result != asset.nodes.size())
                throw std::runtime_error("Ambiguous node name: " + name);
            result = i;
        }
    if (result == asset.nodes.size())
        throw std::runtime_error("Missing node: " + name);
    return result;
}
} // namespace anima
