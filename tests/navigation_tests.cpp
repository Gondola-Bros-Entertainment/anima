#include <anima/navigation.hpp>
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

namespace n = anima::navigation;
using anima::Vec3;
namespace {
constexpr auto outside_graph = "Navigation node outside graph";
// Directed, weighted edges whose cheapest route from node 0 to node 2 detours through node 3.
n::Graph weighted_graph() {
    return n::Graph({{{0, 0, 0}}, {{1, 0, 0}}, {{2, 0, 0}}, {{0, 0, 1}}},
                    {{0, 1, 10}, {0, 3, 1}, {3, 1, 1}, {1, 2, 1}});
}
} // namespace

TEST_CASE("A* finds the cheapest route over directed, weighted edges") {
    const auto graph = weighted_graph();
    const auto path = n::find_path(graph, 0, 2);
    CHECK(path.status == n::PathStatus::found);
    CHECK(path.cost == 3);
    CHECK(path.nodes == std::vector<n::NodeId>{0, 3, 1, 2});
    CHECK_MESSAGE(n::waypoints(graph, path).size() == 4u, "Route conversion lost points");
    CHECK_MESSAGE(n::find_path(graph, 2, 0).status == n::PathStatus::unreachable, "Directed edges became reversible");
}

TEST_CASE("Search budgets end a search without a route") {
    const auto graph = weighted_graph();
    const auto budget = n::find_path(graph, 0, 2, {1});
    CHECK(budget.status == n::PathStatus::budget_exhausted);
    CHECK_MESSAGE(budget.nodes.empty(), "Budget exhaustion returned a false route");
    CHECK(budget.expansions == 1u);
    CHECK_THROWS_WITH_AS(n::waypoints(graph, budget), "Navigation path is not complete", std::invalid_argument);
    const auto edge_budget = n::find_path(graph, 0, 2, {100, 0});
    CHECK_MESSAGE(edge_budget.status == n::PathStatus::budget_exhausted, "High-degree graph ignored edge visit budget");
    CHECK(edge_budget.edge_visits == 0u);
    CHECK_MESSAGE(n::find_path(graph, 1, 1, {0}).nodes == std::vector<n::NodeId>{1},
                  "Same-node zero-budget route failed");
    CHECK_MESSAGE(n::find_path(graph, 0, 2, {0}).status == n::PathStatus::budget_exhausted,
                  "Zero expansion budget ignored");
}

TEST_CASE("Grid routes avoid expensive cells and never cut a blocked corner") {
    n::Grid grid{5, 3, {}, 1, std::vector<float>(15, 1), false};
    grid.costs[7] = 100;
    const auto map = n::make_grid(grid);
    const auto around = n::find_path(map, 5, 9);
    CHECK(around.status == n::PathStatus::found);
    CHECK_MESSAGE(around.cost == 6, "Grid failed to avoid expensive cell");
    for (auto id : around.nodes)
        CHECK_MESSAGE(id != 7u, "Weighted route entered expensive shortcut");
    n::Grid corners{2, 2, {}, 1, {1, 0, 0, 1}, true};
    CHECK_MESSAGE(n::find_path(n::make_grid(corners), 0, 3).status == n::PathStatus::unreachable,
                  "Diagonal cut blocked corner");
    CHECK_MESSAGE(n::find_path(n::make_grid(corners), 1, 1).status == n::PathStatus::unreachable,
                  "Blocked endpoint accepted");
    corners.costs = {1, 1, 1, 1};
    const auto diagonal = n::find_path(n::make_grid(corners), 0, 3);
    CHECK_MESSAGE(std::abs(diagonal.cost - std::sqrt(2.0)) < 1e-12, "Diagonal distance incorrect");
}

TEST_CASE("A follower steers toward its next waypoint and advances only on observed arrival") {
    n::Follower follower({{0, 0, 0}, {1, 0, 0}, {1, 0, 1}});
    auto velocity = follower.steer({}, 100, .1, 0);
    CHECK(follower.next() == 1u);
    CHECK_MESSAGE(velocity.x == 10, "Steering overshot route corner");
    CHECK(velocity.z == 0);
    velocity = follower.steer({}, 100, .1, 0);
    CHECK_MESSAGE(follower.next() == 1u, "Blocked motor consumed its route");
    CHECK(velocity.x == 10);
    velocity = follower.steer({1, 0, 0}, 100, .1, 0);
    CHECK_MESSAGE(follower.next() == 2u, "Observed arrival failed to advance");
    CHECK(velocity.z == 10);
    velocity = follower.steer({1, 0, 1}, 100, .1, 0);
    CHECK(follower.finished());
    CHECK_MESSAGE(anima::length(velocity) == 0, "Finished route still steers");
    follower.set_route({{1, 0, 0}});
    auto copy = follower;
    (void)copy.steer({1, 0, 0}, 1, .1);
    CHECK(copy.finished());
    CHECK_FALSE_MESSAGE(follower.finished(), "Follower copies share mutable cursor");
    CHECK_THROWS_WITH_AS(follower.set_route({{NAN, 0, 0}}), "Navigation position outside finite supported range",
                         std::invalid_argument);
    // The rejected replacement keeps the accepted route and cursor.
    CHECK(follower.route().size() == 1u);
    CHECK(follower.next() == 0u);
    CHECK_THROWS_WITH_AS(follower.steer({}, -1, .1), "Invalid navigation speed/arrival distance",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(follower.steer({}, 1, 0), "Navigation step must be in [0.000001, 0.1] seconds",
                         std::invalid_argument);
}

TEST_CASE("Invalid graphs, grids and endpoints are rejected") {
    CHECK_THROWS_WITH_AS(n::find_path(weighted_graph(), 99, 0), outside_graph, std::out_of_range);
    CHECK_THROWS_WITH_AS(n::Graph({}, {}), "Navigation graph size outside supported range", std::invalid_argument);
    CHECK_THROWS_WITH_AS(n::Graph({{{0, 0, 0}}}, {{0, 1, 1}}), outside_graph, std::out_of_range);
    CHECK_THROWS_WITH_AS(n::Graph({{{0, 0, 0}}}, {{0, 0, -1}}), "Navigation edge cost outside [0, 1e12]",
                         std::invalid_argument);
    const n::Grid valid{2, 2, {}, 1, {1, 1, 1, 1}, true};
    auto bad = valid;
    bad.costs[0] = NAN;
    CHECK_THROWS_WITH_AS(n::make_grid(bad), "Invalid navigation cell cost", std::invalid_argument);
    // One cell past the documented limit, with a cost for every cell, so only the cell count check rejects it.
    constexpr std::uint32_t oversized = 1'000'001;
    bad = {oversized, 1, {}, 1, std::vector<float>(oversized, 1), true};
    CHECK_THROWS_WITH_AS(n::make_grid(bad), "Invalid navigation grid dimensions/cost count", std::invalid_argument);
}

TEST_CASE("A* matches a Bellman-Ford oracle on random graphs") {
    // A Bellman-Ford oracle independently qualifies A*, including zero-cost and
    // directed edges, inconsistent geometry/cost units, blocked nodes and cycles.
    std::mt19937 random(1739);
    for (unsigned trial = 0; trial < 300; ++trial) {
        CAPTURE(trial);
        constexpr n::NodeId count = 16;
        std::vector<n::Node> nodes;
        std::vector<n::Edge> edges;
        for (n::NodeId i = 0; i < count; ++i)
            nodes.push_back(
                {{float(random() % 100), float(random() % 10), float(random() % 100)}, i < 2 || random() % 5 != 0});
        for (n::NodeId a = 0; a < count; ++a)
            for (n::NodeId b = 0; b < count; ++b)
                if (random() % 5 == 0)
                    edges.push_back({a, b, double(random() % 9 + trial % 2)});
        std::vector<double> oracle(count, std::numeric_limits<double>::infinity());
        oracle[0] = 0;
        for (n::NodeId iteration = 1; iteration < count; ++iteration)
            for (const auto &e : edges)
                if (nodes[e.from].walkable && nodes[e.to].walkable)
                    oracle[e.to] = std::min(oracle[e.to], oracle[e.from] + e.cost);
        n::Graph generated(nodes, edges);
        const auto actual = n::find_path(generated, 0, 1);
        if (std::isfinite(oracle[1])) {
            REQUIRE(actual.status == n::PathStatus::found);
            CHECK_MESSAGE(actual.cost == oracle[1], "A* differs from Bellman-Ford optimum");
            double cost = 0;
            for (std::size_t i = 1; i < actual.nodes.size(); ++i) {
                double edge_cost = std::numeric_limits<double>::infinity();
                for (auto e : generated.outgoing(actual.nodes[i - 1]))
                    if (e.to == actual.nodes[i])
                        edge_cost = std::min(edge_cost, e.cost);
                cost += edge_cost;
            }
            CHECK_MESSAGE(cost == actual.cost, "Returned route cannot reproduce reported cost");
            CHECK(actual.nodes.front() == 0u);
            CHECK(actual.nodes.back() == 1u);
        } else
            CHECK_MESSAGE(actual.status == n::PathStatus::unreachable, "A* invented unreachable route");
    }
}
