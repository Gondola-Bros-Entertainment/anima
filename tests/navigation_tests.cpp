#include <anima/navigation.hpp>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
namespace n = anima::navigation;
using anima::Vec3;
namespace {
void check(bool v, const char *m) {
    if (!v)
        throw std::runtime_error(m);
}
template <class F> void rejects(F f) {
    bool caught = false;
    try {
        f();
    } catch (const std::exception &) {
        caught = true;
    }
    check(caught, "Invalid navigation input accepted");
}
void run() {
    n::Graph graph({{{0, 0, 0}}, {{1, 0, 0}}, {{2, 0, 0}}, {{0, 0, 1}}}, {{0, 1, 10}, {0, 3, 1}, {3, 1, 1}, {1, 2, 1}});
    const auto path = n::find_path(graph, 0, 2);
    check(path.status == n::PathStatus::found && path.cost == 3 && path.nodes == std::vector<n::NodeId>{0, 3, 1, 2},
          "Weighted directed route is not optimal");
    check(n::waypoints(graph, path).size() == 4, "Route conversion lost points");
    check(n::find_path(graph, 2, 0).status == n::PathStatus::unreachable, "Directed edges became reversible");
    auto budget = n::find_path(graph, 0, 2, {1});
    check(budget.status == n::PathStatus::budget_exhausted && budget.nodes.empty() && budget.expansions == 1,
          "Budget exhaustion returned a false route");
    const auto edge_budget = n::find_path(graph, 0, 2, {100, 0});
    check(edge_budget.status == n::PathStatus::budget_exhausted && edge_budget.edge_visits == 0,
          "High-degree graph ignored edge visit budget");
    check(n::find_path(graph, 1, 1, {0}).nodes == std::vector<n::NodeId>{1}, "Same-node zero-budget route failed");
    check(n::find_path(graph, 0, 2, {0}).status == n::PathStatus::budget_exhausted, "Zero expansion budget ignored");
    rejects([&] { (void)n::find_path(graph, 99, 0); });
    rejects([&] { (void)n::waypoints(graph, budget); });
    n::Grid grid{5, 3, {}, 1, std::vector<float>(15, 1), false};
    grid.costs[7] = 100;
    auto map = n::make_grid(grid);
    auto around = n::find_path(map, 5, 9);
    check(around.status == n::PathStatus::found && around.cost == 6, "Grid failed to avoid expensive cell");
    for (auto id : around.nodes)
        check(id != 7, "Weighted route entered expensive shortcut");
    n::Grid corners{2, 2, {}, 1, {1, 0, 0, 1}, true};
    check(n::find_path(n::make_grid(corners), 0, 3).status == n::PathStatus::unreachable,
          "Diagonal cut blocked corner");
    check(n::find_path(n::make_grid(corners), 1, 1).status == n::PathStatus::unreachable, "Blocked endpoint accepted");
    corners.costs = {1, 1, 1, 1};
    auto diagonal = n::find_path(n::make_grid(corners), 0, 3);
    check(std::abs(diagonal.cost - std::sqrt(2.0)) < 1e-12, "Diagonal distance incorrect");
    n::Follower follower({{0, 0, 0}, {1, 0, 0}, {1, 0, 1}});
    auto velocity = follower.steer({}, 100, .1, 0);
    check(follower.next() == 1 && velocity.x == 10 && velocity.z == 0, "Steering overshot route corner");
    velocity = follower.steer({}, 100, .1, 0);
    check(follower.next() == 1 && velocity.x == 10, "Blocked motor consumed its route");
    velocity = follower.steer({1, 0, 0}, 100, .1, 0);
    check(follower.next() == 2 && velocity.z == 10, "Observed arrival failed to advance");
    velocity = follower.steer({1, 0, 1}, 100, .1, 0);
    check(follower.finished() && anima::length(velocity) == 0, "Finished route still steers");
    follower.set_route({{1, 0, 0}});
    auto copy = follower;
    (void)copy.steer({1, 0, 0}, 1, .1);
    check(copy.finished() && !follower.finished(), "Follower copies share mutable cursor");
    rejects([&] { follower.set_route({{NAN, 0, 0}}); });
    check(follower.route().size() == 1 && follower.next() == 0, "Invalid route replacement changed accepted route");
    rejects([&] { (void)follower.steer({}, -1, .1); });
    rejects([&] { (void)follower.steer({}, 1, 0); });
    rejects([&] { (void)n::Graph({}, {}); });
    rejects([&] { (void)n::Graph({{{0, 0, 0}}}, {{0, 1, 1}}); });
    rejects([&] { (void)n::Graph({{{0, 0, 0}}}, {{0, 0, -1}}); });
    rejects([&] {
        auto bad = corners;
        bad.costs[0] = NAN;
        (void)n::make_grid(bad);
    });
    rejects([&] {
        auto bad = corners;
        bad.width = UINT32_MAX;
        (void)n::make_grid(bad);
    });

    // A Bellman-Ford oracle independently qualifies A*, including zero-cost and
    // directed edges, inconsistent geometry/cost units, blocked nodes and cycles.
    std::mt19937 random(1739);
    for (unsigned trial = 0; trial < 300; ++trial) {
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
        auto actual = n::find_path(generated, 0, 1);
        if (std::isfinite(oracle[1])) {
            check(actual.status == n::PathStatus::found && actual.cost == oracle[1],
                  "A* differs from Bellman-Ford optimum");
            double cost = 0;
            for (std::size_t i = 1; i < actual.nodes.size(); ++i) {
                double edge_cost = std::numeric_limits<double>::infinity();
                for (auto e : generated.outgoing(actual.nodes[i - 1]))
                    if (e.to == actual.nodes[i])
                        edge_cost = std::min(edge_cost, e.cost);
                cost += edge_cost;
            }
            check(cost == actual.cost && actual.nodes.front() == 0 && actual.nodes.back() == 1,
                  "Returned route cannot reproduce reported cost");
        } else
            check(actual.status == n::PathStatus::unreachable, "A* invented unreachable route");
    }
}
} // namespace
int main() {
    try {
        run();
        std::cout << "PASS navigation A*, grids, budgets, follower and independent shortest-path oracle\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
