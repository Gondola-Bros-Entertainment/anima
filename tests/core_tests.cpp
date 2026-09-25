#include <anima/core/fixed_step.hpp>

#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <class F> void invalid(F action) {
    try {
        action();
    } catch (const std::invalid_argument &) {
        return;
    }
    throw std::runtime_error("Expected invalid_argument");
}
} // namespace

int main() {
    using namespace std::chrono_literals;
    try {
        // Different render cadences must advance the same simulation time.
        anima::FixedStepClock steady{10ms}, uneven{10ms};
        unsigned steady_ticks = 0, uneven_ticks = 0;
        for (int i = 0; i < 100; ++i)
            steady_ticks += steady.advance(10ms).steps;
        for (int i = 0; i < 20; ++i) {
            uneven_ticks += uneven.advance(7ms).steps;
            uneven_ticks += uneven.advance(43ms).steps;
        }
        require(steady_ticks == 100 && uneven_ticks == steady_ticks, "Cadence changed simulation ticks");
        require(uneven.advance(0ns).interpolation == 0.0, "Cadence left a time remainder");

        anima::FixedStepClock bounded{10ms, 4};
        require(bounded.advance(5ms).steps == 0, "Fractional step advanced simulation");
        const auto stalled = bounded.advance(1s);
        require(stalled.steps == 4, "Long pause caused an unbounded catch-up");
        require(stalled.dropped == 960ms, "Dropped-time accounting is wrong");
        require(std::abs(stalled.interpolation - 0.5) < 1e-12, "Pause lost fractional time");
        bounded.reset();
        require(bounded.advance(0ns).interpolation == 0.0, "Reset retained old time");
        const auto extreme = bounded.advance(std::chrono::nanoseconds::max());
        require(extreme.steps == 4 && extreme.interpolation == 0.0, "Extreme elapsed time overflowed");
        invalid([&] { (void)bounded.advance(-1ns); });
        invalid([] { anima::FixedStepClock bad{0ns}; });
        invalid([] { anima::FixedStepClock bad{10ms, 0}; });
        invalid([] { anima::FixedStepClock bad{std::chrono::nanoseconds::max(), 2}; });
        std::cout << "PASS: cadence independence, stall budget, remainder/reset, overflow and invalid inputs\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
