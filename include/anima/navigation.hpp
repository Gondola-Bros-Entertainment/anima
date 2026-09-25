#pragma once
#include <anima/core/math.hpp>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace anima::navigation {
using NodeId = std::uint32_t;
struct Node {
    Vec3 position{};
    bool walkable = true;
};
struct Edge {
    NodeId from{}, to{};
    double cost = 1;
}; // Directed, finite and nonnegative.
// Immutable validated graph; node identifiers are indices in nodes(). No game rules.
class Graph {
  public:
    Graph(std::vector<Node> nodes, std::vector<Edge> edges);
    [[nodiscard]] std::span<const Node> nodes() const { return nodes_; }
    [[nodiscard]] std::span<const Edge> outgoing(NodeId node) const;
    [[nodiscard]] double heuristic_scale() const { return heuristic_scale_; }

  private:
    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
    std::vector<std::size_t> offsets_;
    double heuristic_scale_{};
};
struct Grid {
    std::uint32_t width{}, height{};
    Vec3 origin{}; // Cells lie in the XZ plane at origin.y; IDs are z*width+x.
    float cell_size = 1;
    std::vector<float> costs; // Exactly width*height. Zero blocks; positive is cost per distance entered.
    bool diagonal = true;     // Diagonal edges never cut a blocked cardinal corner.
};
[[nodiscard]] Graph make_grid(const Grid &grid);
enum class PathStatus { found, unreachable, budget_exhausted };
struct SearchSettings {
    std::size_t max_expansions = 100000;
    std::size_t max_edge_visits = 1000000;
};
struct Path {
    PathStatus status = PathStatus::unreachable;
    std::vector<NodeId> nodes;
    double cost{};
    std::size_t expansions{}, edge_visits{};
};
// A* with an automatically admissible Euclidean heuristic; zero-cost links can
// reduce it to Dijkstra. No partial route is returned for an exhausted budget.
[[nodiscard]] Path find_path(const Graph &graph, NodeId start, NodeId goal, SearchSettings settings = {});
[[nodiscard]] std::vector<Vec3> waypoints(const Graph &graph, const Path &path);

// Suggests velocity toward one waypoint, capped to avoid overshooting it in this
// step. The caller applies motion/collision. Only observed arrival advances the
// cursor, so blocked movement does not consume the route. Single caller thread.
class Follower {
  public:
    explicit Follower(std::vector<Vec3> route = {}, std::size_t next = 0);
    void set_route(std::vector<Vec3> route, std::size_t next = 0);
    [[nodiscard]] Vec3 steer(Vec3 position, float speed, double seconds, float arrival_distance = .05F);
    [[nodiscard]] std::span<const Vec3> route() const {
        return route_ ? std::span<const Vec3>(*route_) : std::span<const Vec3>{};
    }
    [[nodiscard]] std::size_t next() const { return next_; }
    [[nodiscard]] bool finished() const { return next_ >= route().size(); }

  private:
    std::shared_ptr<const std::vector<Vec3>> route_;
    std::size_t next_{};
};
} // namespace anima::navigation
