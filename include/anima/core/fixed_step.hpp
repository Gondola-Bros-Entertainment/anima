#pragma once
/// @file
/// Fixed-step simulation timing without an OS clock. Part of the `anima::core` target; the caller
/// owns the clock and the simulation.

#include <chrono>
#include <cstdint>

namespace anima {

/// Result of accepting one elapsed interval into a FixedStepClock.
struct StepBatch {
    std::uint32_t steps{};              ///< Whole simulation steps to execute, at most the catch-up limit.
    double interpolation{};             ///< Remaining fraction of one step, in [0, 1).
    std::chrono::nanoseconds dropped{}; ///< Elapsed time discarded by the catch-up limit.
};

/// Accumulator that turns caller-supplied elapsed time into whole fixed steps, in integer
/// nanoseconds.
///
/// The caller executes the returned steps and chooses how to render the interpolation fraction;
/// the clock owns no OS clock, event pump or simulation callback. Instances have independent
/// state; concurrent use of one instance needs caller synchronization.
class FixedStepClock {
  public:
    /// Creates an empty accumulator with steps of @p step and a catch-up limit of @p max_steps steps
    /// per advance() call. Throws `std::invalid_argument` unless both are positive and @p step times
    /// (@p max_steps + 1) fits in `std::chrono::nanoseconds`.
    explicit FixedStepClock(std::chrono::nanoseconds step = std::chrono::nanoseconds{16'666'667},
                            std::uint32_t max_steps = 8);
    /// Adds @p elapsed, capped at step() times the catch-up limit, and returns the whole steps now
    /// due; the remainder carries into the next call. Throws `std::invalid_argument` for negative
    /// @p elapsed, leaving the state unchanged.
    [[nodiscard]] StepBatch advance(std::chrono::nanoseconds elapsed);
    /// Discards the carried remainder without changing the step or the catch-up limit.
    void reset() noexcept;
    /// Duration of one simulation step.
    [[nodiscard]] std::chrono::nanoseconds step() const noexcept { return step_; }

  private:
    std::chrono::nanoseconds step_;
    std::chrono::nanoseconds max_elapsed_;
    std::chrono::nanoseconds remainder_{};
};

} // namespace anima
