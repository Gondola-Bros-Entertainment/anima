#pragma once
#include <anima/assets/staging.hpp>
#include <cstdint>

namespace anima::detail {
// One staging call's view of its StagingOptions: the stop checks and progress counts it reports.
class StagingSteps {
  public:
    StagingSteps() = default;
    explicit StagingSteps(const StagingOptions &options) : stop_(options.stop), progress_(options.progress) {}
    // Throws StagingCancelled when a stop was requested.
    void check() const {
        if (stop_.stop_requested())
            throw StagingCancelled();
    }
    // Whether the call reports progress, so counting its steps has a reader.
    [[nodiscard]] bool counting() const noexcept { return progress_ != nullptr; }
    // Adds @p steps to the total. Callers count a step before completing it, so completed never passes total.
    void add(std::uint64_t steps) const {
        if (progress_ && steps)
            progress_->total_ += steps;
    }
    // Completes one step that add() counted.
    void complete() const {
        if (progress_)
            ++progress_->completed_;
    }

  private:
    StopToken stop_;
    StagingProgress *progress_{};
};
} // namespace anima::detail
