#include "../detail/navigation.hpp"
#include <algorithm>
#include <anima/navigation.hpp>
#include <cmath>
#include <limits>
#include <numbers>
#include <queue>
#include <stdexcept>

namespace anima::navigation {
namespace {
constexpr float maximum_coordinate = 1e6F;
constexpr double maximum_edge_cost = 1e12;
constexpr float minimum_cell_size = .001F, maximum_cell_size = 10'000;
constexpr float maximum_cell_cost = 1e6F;
void point(Vec3 p) {
    for (float v : {p.x, p.y, p.z})
        if (!std::isfinite(v) || std::abs(v) > maximum_coordinate)
            throw std::invalid_argument("Navigation position outside finite supported range");
}
double distance(Vec3 a, Vec3 b) {
    const double x = double(a.x) - b.x, y = double(a.y) - b.y, z = double(a.z) - b.z;
    return std::sqrt(x * x + y * y + z * z);
}
void index(const Graph &graph, NodeId node) {
    if (node >= graph.nodes().size())
        throw std::out_of_range("Navigation node outside graph");
}
} // namespace
Graph::Graph(std::vector<Node> nodes, std::vector<Edge> edges) : nodes_(std::move(nodes)) {
    if (nodes_.empty() || nodes_.size() > detail::maximum_navigation_nodes ||
        edges.size() > detail::maximum_navigation_edges)
        throw std::invalid_argument("Navigation graph size outside supported range");
    for (const auto &node : nodes_)
        point(node.position);
    offsets_.resize(nodes_.size() + 1);
    double scale = std::numeric_limits<double>::infinity();
    for (const auto &edge : edges) {
        index(*this, edge.from);
        index(*this, edge.to);
        if (!std::isfinite(edge.cost) || edge.cost < 0 || edge.cost > maximum_edge_cost)
            throw std::invalid_argument("Navigation edge cost outside [0, 1e12]");
        ++offsets_[edge.from + 1];
        const auto length = distance(nodes_[edge.from].position, nodes_[edge.to].position);
        if (length > 0)
            scale = std::min(scale, edge.cost / length);
    }
    for (std::size_t i = 1; i < offsets_.size(); ++i)
        offsets_[i] += offsets_[i - 1];
    edges_.resize(edges.size());
    auto cursor = offsets_;
    for (const auto &edge : edges)
        edges_[cursor[edge.from]++] = edge;
    // Round down to keep the lower-bound estimate conservative at float/double boundaries.
    heuristic_scale_ = std::isfinite(scale) ? std::nextafter(scale, 0.0) : 0;
}
std::span<const Edge> Graph::outgoing(NodeId node) const {
    index(*this, node);
    return std::span<const Edge>(edges_).subspan(offsets_[node], offsets_[node + 1] - offsets_[node]);
}
Graph make_grid(const Grid &g) {
    point(g.origin);
    const auto count = std::uint64_t(g.width) * g.height;
    if (!g.width || !g.height || count > detail::maximum_navigation_nodes || g.costs.size() != count ||
        !std::isfinite(g.cell_size) || g.cell_size < minimum_cell_size || g.cell_size > maximum_cell_size)
        throw std::invalid_argument("Invalid navigation grid dimensions/cost count");
    for (float cost : g.costs)
        if (!std::isfinite(cost) || cost < 0 || cost > maximum_cell_cost)
            throw std::invalid_argument("Invalid navigation cell cost");
    std::vector<Node> nodes;
    nodes.reserve(static_cast<std::size_t>(count));
    std::vector<Edge> edges;
    for (std::uint32_t z = 0; z < g.height; ++z) {
        for (std::uint32_t x = 0; x < g.width; ++x) {
            const NodeId from = z * g.width + x;
            const Vec3 position{g.origin.x + static_cast<float>(x) * g.cell_size, g.origin.y,
                                g.origin.z + static_cast<float>(z) * g.cell_size};
            point(position);
            nodes.push_back({position, g.costs[from] > 0});
            if (g.costs[from] == 0)
                continue;
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx) {
                    if ((!dx && !dz) || (!g.diagonal && dx && dz))
                        continue;
                    const auto nx = std::int64_t(x) + dx, nz = std::int64_t(z) + dz;
                    if (nx < 0 || nz < 0 || nx >= g.width || nz >= g.height)
                        continue;
                    const auto to = static_cast<NodeId>(nz * g.width + nx);
                    if (g.costs[to] == 0)
                        continue;
                    if (dx && dz &&
                        (g.costs[z * g.width + static_cast<std::uint32_t>(nx)] == 0 ||
                         g.costs[static_cast<std::uint32_t>(nz) * g.width + x] == 0))
                        continue;
                    edges.push_back(
                        {from, to, double(g.cell_size) * (dx && dz ? std::numbers::sqrt2 : 1.0) * g.costs[to]});
                }
        }
    }
    return Graph(std::move(nodes), std::move(edges));
}
Path find_path(const Graph &graph, NodeId start, NodeId goal, SearchSettings settings) {
    index(graph, start);
    index(graph, goal);
    if (settings.max_expansions > detail::maximum_navigation_nodes ||
        settings.max_edge_visits > detail::maximum_navigation_edges)
        throw std::invalid_argument("Navigation expansion budget exceeds graph limit");
    Path result;
    const auto nodes = graph.nodes();
    if (!nodes[start].walkable || !nodes[goal].walkable)
        return result;
    struct Candidate {
        double estimate{}, cost{};
        NodeId node{};
    };
    const auto later = [](const Candidate &a, const Candidate &b) {
        return a.estimate != b.estimate ? a.estimate > b.estimate : a.node > b.node;
    };
    std::priority_queue<Candidate, std::vector<Candidate>, decltype(later)> open(later);
    std::vector<double> costs(nodes.size(), std::numeric_limits<double>::infinity());
    std::vector<NodeId> parent(nodes.size(), UINT32_MAX);
    const auto estimate = [&](NodeId id) {
        return graph.heuristic_scale() * distance(nodes[id].position, nodes[goal].position);
    };
    costs[start] = 0;
    open.push({estimate(start), 0, start});
    while (!open.empty()) {
        const auto candidate = open.top();
        open.pop();
        if (candidate.cost != costs[candidate.node])
            continue;
        if (candidate.node == goal) {
            result.status = PathStatus::found;
            result.cost = candidate.cost;
            for (auto id = goal; id != UINT32_MAX; id = parent[id])
                result.nodes.push_back(id);
            std::reverse(result.nodes.begin(), result.nodes.end());
            return result;
        }
        if (result.expansions == settings.max_expansions) {
            result.status = PathStatus::budget_exhausted;
            return result;
        }
        ++result.expansions;
        for (const auto &edge : graph.outgoing(candidate.node)) {
            if (result.edge_visits == settings.max_edge_visits) {
                result.status = PathStatus::budget_exhausted;
                return result;
            }
            ++result.edge_visits;
            if (!nodes[edge.to].walkable)
                continue;
            const auto cost = candidate.cost + edge.cost;
            if (cost >= costs[edge.to])
                continue;
            costs[edge.to] = cost;
            parent[edge.to] = candidate.node;
            open.push({cost + estimate(edge.to), cost, edge.to});
        }
    }
    return result;
}
std::vector<Vec3> waypoints(const Graph &graph, const Path &path) {
    if (path.status != PathStatus::found || path.nodes.empty())
        throw std::invalid_argument("Navigation path is not complete");
    std::vector<Vec3> result;
    result.reserve(path.nodes.size());
    for (auto id : path.nodes) {
        index(graph, id);
        if (!graph.nodes()[id].walkable)
            throw std::invalid_argument("Navigation route includes blocked node");
        result.push_back(graph.nodes()[id].position);
    }
    return result;
}
Follower::Follower(std::vector<Vec3> route, std::size_t next) { set_route(std::move(route), next); }
void Follower::set_route(std::vector<Vec3> route, std::size_t next) {
    if (route.size() > detail::maximum_navigation_nodes || next > route.size())
        throw std::invalid_argument("Invalid navigation route/cursor");
    for (auto p : route)
        point(p);
    route_ = std::make_shared<const std::vector<Vec3>>(std::move(route));
    next_ = next;
}
Vec3 Follower::steer(Vec3 position, float speed, double seconds, float arrival_distance) {
    point(position);
    detail::navigation_settings(speed, arrival_distance);
    detail::navigation_step(seconds);
    while (!finished() && distance(position, (*route_)[next_]) <= arrival_distance)
        ++next_;
    if (finished())
        return {};
    const double length = distance(position, (*route_)[next_]);
    const float factor = static_cast<float>(std::min(double(speed), length / seconds) / length);
    return ((*route_)[next_] - position) * factor;
}
} // namespace anima::navigation
