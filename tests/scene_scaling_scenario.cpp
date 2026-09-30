// Runs one scene scaling scenario once, so that Callgrind can count the instructions of its measured
// operation (scene_scaling_instructions.cmake):
//
//   anima_scene_scaling_scenario <scenario> <objects>
//
// The scenarios are chain, destroy_children, destroy_renderers, destroy_wide_subtree,
// destroy_deep_subtree, reuse_slots and query (scene_scaling_scenarios.hpp); the objects are a
// multiple of 64 from 64 to 1,048,576.
#include "scene_scaling_scenarios.hpp"

#include <charconv>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

// Runs @p operation. Callgrind collects instructions only from entering this function to leaving
// it, and its C linkage gives it an exact name to toggle on. Returning a value after the call keeps
// the call from becoming a jump, which would leave the function before the operation runs.
extern "C" int measured_operation(const std::function<void()> &operation) {
    operation();
    return 1;
}

namespace {
constexpr std::size_t maximum_scenario_objects = 1U << 20;

// Every call goes through this volatile pointer, which no compiler can inline or specialize.
int (*volatile measured_call)(const std::function<void()> &) = measured_operation;

std::size_t objects_option(std::string_view text) {
    std::size_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value < 64 ||
        value > maximum_scenario_objects || value % 64)
        throw std::invalid_argument("Expected a multiple of 64 from 64 to 1048576 objects: " + std::string(text));
    return value;
}
} // namespace

int main(int argc, char **argv) {
    try {
        if (argc != 3)
            throw std::invalid_argument("Usage: anima_scene_scaling_scenario <scenario> <objects>");
        const std::string_view scenario = argv[1];
        const auto count = objects_option(argv[2]);
        const auto measure = [](auto &&operation) { return measured_call(std::function<void()>(operation)); };
        if (scenario == "chain")
            (void)scene_scaling::chain(count, measure);
        else if (scenario == "destroy_children")
            (void)scene_scaling::destroy_children(count, measure);
        else if (scenario == "destroy_renderers")
            (void)scene_scaling::destroy_renderers(count, scene_scaling::triangle(), measure);
        else if (scenario == "destroy_wide_subtree")
            (void)scene_scaling::destroy_subtree(count, false, measure);
        else if (scenario == "destroy_deep_subtree")
            (void)scene_scaling::destroy_subtree(count, true, measure);
        else if (scenario == "reuse_slots")
            (void)scene_scaling::reuse_slots(count, measure);
        else if (scenario == "query")
            (void)scene_scaling::query(count, measure);
        else
            throw std::invalid_argument("Unknown scenario: " + std::string(scenario));
    } catch (const std::exception &error) {
        std::fprintf(stderr, "anima_scene_scaling_scenario: %s\n", error.what());
        return 1;
    }
}
