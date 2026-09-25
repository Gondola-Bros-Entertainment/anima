#include <anima/core/fixed_step.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace anima {

FixedStepClock::FixedStepClock(std::chrono::nanoseconds step, std::uint32_t max_steps) : step_{step} {
    const auto divisor = static_cast<std::int64_t>(max_steps) + 1;
    if (step.count() <= 0 || max_steps == 0 || step.count() > std::numeric_limits<std::int64_t>::max() / divisor) {
        throw std::invalid_argument("Invalid fixed-step interval or catch-up limit");
    }
    max_elapsed_ = step * max_steps;
}

StepBatch FixedStepClock::advance(std::chrono::nanoseconds elapsed) {
    if (elapsed.count() < 0) {
        throw std::invalid_argument("Elapsed simulation time cannot be negative");
    }
    const auto accepted = std::min(elapsed, max_elapsed_);
    remainder_ += accepted;
    const auto steps = remainder_ / step_;
    remainder_ %= step_;
    return {static_cast<std::uint32_t>(steps),
            static_cast<double>(remainder_.count()) / static_cast<double>(step_.count()), elapsed - accepted};
}

void FixedStepClock::reset() noexcept { remainder_ = std::chrono::nanoseconds::zero(); }

} // namespace anima
