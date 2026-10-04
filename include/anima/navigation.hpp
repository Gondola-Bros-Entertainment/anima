#pragma once
#include <anima/core/math.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

/// @file
/// Weighted directed graphs, grid construction and lookup, A* route queries and a route follower.
///
/// Part of the `anima::core` target, which has no third-party dependencies. Positions are world coordinates in
/// caller-consistent units, and edge costs may use any unit. Every position component must be finite and within
/// 1,000,000 of zero; this is a validation bound, not a precision guarantee. Invalid arguments throw
/// `std::invalid_argument`, and node identifiers outside a graph throw `std::out_of_range`.
///
/// A Graph is immutable and each query keeps its working memory local, so threads may query one graph concurrently.
/// The application supplies traversability, costs, endpoints and movement; there is no navigation-mesh baking,
/// obstacle update, local avoidance or background search.

/// Navigation graphs, route queries and route following, and their scene integration; see navigation.hpp and
/// navigation_scene.hpp.
namespace anima::navigation {
/// Index of a node in Graph::nodes().
using NodeId = std::uint32_t;
/// One place in a Graph.
struct Node {
    /// World position, each component finite and within 1,000,000 of zero; the A* estimate measures straight-line
    /// distance between positions.
    Vec3 position{};
    /// Routes never start at, end at or pass through a node that is not walkable.
    bool walkable = true;
};
/// Directed link between two nodes; travel the other way needs its own edge. Parallel edges, self-links and
/// zero-cost edges are allowed.
struct Edge {
    /// Node the edge leaves, an index into the graph's nodes.
    NodeId from{};
    /// Node the edge enters, an index into the graph's nodes.
    NodeId to{};
    /// Traversal cost, in [0, 1e12].
    double cost = 1;
};
/// Validated, immutable directed graph. Node identifiers are indices into nodes().
class Graph {
  public:
    /// Builds a graph from 1 to 1,000,000 @p nodes and at most 8,000,000 @p edges.
    Graph(std::vector<Node> nodes, std::vector<Edge> edges);
    /// The nodes as given to the constructor, in order, so a NodeId indexes this span.
    [[nodiscard]] std::span<const Node> nodes() const { return nodes_; }
    /// Edges leaving @p node, in input order.
    [[nodiscard]] std::span<const Edge> outgoing(NodeId node) const;
    /// Cost per unit of straight-line distance in the A* estimate: the largest double below the lowest cost-to-length
    /// ratio of any edge joining distinct positions, or zero when that ratio is zero or no such edge exists. A zero
    /// estimate reduces find_path() to Dijkstra's algorithm.
    [[nodiscard]] double heuristic_scale() const { return heuristic_scale_; }

  private:
    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
    std::vector<std::size_t> offsets_;
    double heuristic_scale_{};
};
/// Cells for make_grid(), in the XZ plane at `origin.y`.
struct Grid {
    /// Cells along X.
    std::uint32_t columns{};
    /// Cells along Z; `columns * rows` must be in [1, 1,000,000].
    std::uint32_t rows{};
    /// Position of cell (0, 0); cell (x, z) lies at `origin + (x, 0, z) * cell_size`.
    Vec3 origin{};
    /// Distance between neighbouring cells, in [0.001, 10,000].
    float cell_size = 1;
    /// One cost per cell, indexed `z * columns + x`, each in [0, 1,000,000]. Zero blocks the cell. An edge entering a
    /// cell costs its length times that cell's cost, so the two directions between cells can differ.
    std::vector<float> costs;
    /// Adds diagonal edges of length `cell_size * sqrt(2)` where both cardinal neighbours they pass between are
    /// walkable, so routes never cut a blocked corner.
    bool diagonal = true;
};
/// Builds a Graph with node `z * columns + x` for cell (x, z) and edges both ways between walkable neighbours. A
/// blocked cell keeps its node, marked not walkable, with no edges.
[[nodiscard]] Graph make_grid(const Grid &grid);
/// Node that make_grid() builds for the cell whose center is nearest @p position in the XZ plane, or empty when
/// @p position lies outside every cell.
///
/// Cell (x, z) covers the positions whose X and Z each lie in [-cell_size / 2, cell_size / 2) of its center, so a
/// position on an edge between cells belongs to the cell on its +X or +Z side, and the grid reaches half a cell
/// beyond its outer centers. Y and walkability are ignored: a blocked cell still returns its node. Throws for a
/// position outside the supported range, or for an origin, cell counts, cost count or cell size that make_grid()
/// rejects; the cost values are not read, so a lookup takes constant time.
[[nodiscard]] std::optional<NodeId> grid_node(const Grid &grid, Vec3 position);
/// Outcome of find_path().
enum class PathStatus {
    found,           ///< Path::nodes holds a least-cost route.
    unreachable,     ///< No route exists, or the start or goal is not walkable.
    budget_exhausted ///< A SearchSettings limit stopped the search; no partial route is returned.
};
/// Work limits for one find_path() call.
struct SearchSettings {
    /// Nodes the search may expand, at most 1,000,000.
    std::size_t max_expansions = 100000;
    /// Edges the search may examine, at most 8,000,000; this bounds work even at high-degree nodes.
    std::size_t max_edge_visits = 1000000;
};
/// Result of find_path().
struct Path {
    /// Whether #nodes holds a route, and why not otherwise.
    PathStatus status = PathStatus::unreachable;
    /// Route from start to goal inclusive when PathStatus::found; otherwise empty.
    std::vector<NodeId> nodes;
    /// Sum of the route's edge costs, with no charge for the start node; zero unless found.
    double cost{};
    /// Nodes expanded.
    std::size_t expansions{};
    /// Edges examined.
    std::size_t edge_visits{};
};
/// Finds a least-cost route from @p start to @p goal with A*, estimating the remaining cost as
/// Graph::heuristic_scale() times the straight-line distance.
///
/// Ties in estimated total cost expand the lower node ID first; cross-platform reproducibility is not guaranteed. A
/// walkable @p start equal to @p goal returns a one-node, zero-cost route even with zero budgets.
[[nodiscard]] Path find_path(const Graph &graph, NodeId start, NodeId goal, SearchSettings settings = {});
/// Positions of the nodes of @p path, in route order. @p path must come from @p graph; only its node indices and
/// walkability are checked. Throws unless @p path is PathStatus::found with at least one node, or when it visits a
/// node that is not walkable.
[[nodiscard]] std::vector<Vec3> waypoints(const Graph &graph, const Path &path);

/// Arrival distance of a default SteerSettings.
inline constexpr float default_arrival_distance = .05F;
/// Space in which Follower::steer measures arrival and steers.
enum class SteerPlane {
    xyz, ///< Full 3D distance and velocity, for flight or routes over several levels.
    xz   ///< Distance and velocity in the XZ plane only: heights are ignored and the velocity's Y is zero, for an
         ///< agent whose position is above or below its route, such as a character's center over a grid.
};
/// Steering parameters of Follower::steer and Agent.
struct SteerSettings {
    /// Speed in units per second, in [0, 10,000].
    float speed = 1;
    /// Distance within which a waypoint counts as reached, in [0, 10,000], measured in #plane.
    float arrival_distance = default_arrival_distance;
    /// Space of the arrival distance and the velocity.
    SteerPlane plane = SteerPlane::xyz;
};

/// Tracks progress along a route and suggests a velocity toward the next waypoint.
///
/// Copies share one immutable route but keep separate cursors. Only observed arrival advances the cursor, so
/// movement the caller blocks never consumes the route. The caller applies motion and collision. A rejected call
/// leaves the follower unchanged. Use each follower from one thread at a time.
class Follower {
  public:
    /// Starts at waypoint @p next of @p route, as set_route() does.
    explicit Follower(std::vector<Vec3> route = {}, std::size_t next = 0);
    /// Replaces the route and cursor. @p route holds at most 1,000,000 waypoints, and @p next is at most its size,
    /// where equal means finished.
    void set_route(std::vector<Vec3> route, std::size_t next = 0);
    /// Advances past each successive waypoint within SteerSettings::arrival_distance of @p position, then returns a
    /// velocity toward the next one, or zero once finished.
    ///
    /// The velocity has magnitude SteerSettings::speed, in units per second, capped so that moving for @p seconds
    /// reaches the waypoint without passing it. With SteerPlane::xz, distances, the cap and the velocity use only X
    /// and Z, and the velocity's Y is zero. @p seconds is in [0.000001, 0.1]. The cursor advances even at zero speed.
    /// Throws for @p settings outside the ranges SteerSettings states, or a plane that is not a SteerPlane enumerator.
    [[nodiscard]] Vec3 steer(Vec3 position, double seconds, SteerSettings settings = {});
    /// Route waypoints, valid while this follower keeps its route.
    [[nodiscard]] std::span<const Vec3> route() const {
        return route_ ? std::span<const Vec3>(*route_) : std::span<const Vec3>{};
    }
    /// Index of the next unreached waypoint; the route size once finished.
    [[nodiscard]] std::size_t next() const { return next_; }
    /// Whether every waypoint has been reached; true for an empty route.
    [[nodiscard]] bool finished() const { return next_ >= route().size(); }

  private:
    std::shared_ptr<const std::vector<Vec3>> route_;
    std::size_t next_{};
};
} // namespace anima::navigation
