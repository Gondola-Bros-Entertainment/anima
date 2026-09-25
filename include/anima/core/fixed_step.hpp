#pragma once
/// @file
/// Dependency-free simulation timing; the caller owns the clock and simulation.

#include <chrono>
#include <cstdint>

namespace anima {

/// Result of accepting one elapsed interval into a FixedStepClock.
struct StepBatch {
    std::uint32_t steps{};              ///< Number of whole simulation steps to execute.
    double interpolation{};             ///< Remaining fraction of one step, in [0, 1).
    std::chrono::nanoseconds dropped{}; ///< Elapsed time discarded by the catch-up limit.
};

/// A bounded accumulator using integer nanoseconds.
///
/// The caller supplies elapsed time, executes returned steps and chooses how to
/// render interpolation. No OS clock, event pump or simulation callback is owned.
/// Instances have independent state. Concurrent access to one instance requires
/// caller synchronization.
class FixedStepClock {
  public:
    /// Construct an empty accumulator.
    /// @param step Positive duration of each simulation step.
    /// @param max_steps Positive catch-up limit per advance() call.
    /// @throws std::invalid_argument If either argument is invalid or their
    /// bounded accumulation can overflow the duration representation.
    explicit FixedStepClock(std::chrono::nanoseconds step = std::chrono::nanoseconds{16'666'667},
                            std::uint32_t max_steps = 8);
    /// Accept elapsed time and retain the remainder for the next call.
    /// @param elapsed Nonnegative duration since the previous application update.
    /// @return Whole steps, interpolation fraction and discarded time. At most
    /// the configured catch-up limit of elapsed time is accepted in one call.
    /// @throws std::invalid_argument For negative elapsed time, preserving state.
    [[nodiscard]] StepBatch advance(std::chrono::nanoseconds elapsed);
    /// Discard the remainder without changing the step or catch-up limit.
    void reset() noexcept;
    /// Return the configured simulation step duration.
    [[nodiscard]] std::chrono::nanoseconds step() const noexcept { return step_; }

  private:
    std::chrono::nanoseconds step_;
    std::chrono::nanoseconds max_elapsed_;
    std::chrono::nanoseconds remainder_{};
};

} // namespace anima
