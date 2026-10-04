#pragma once
#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <utility>

/// @file
/// Cancellation and progress for loads that an application stages on its own threads. Part of the
/// `anima::assets` target.
///
/// GLB import (load_asset, load_motion_asset) and document staging (stage_scene, stage_scene_set) run on
/// the calling thread and take StagingOptions. They start no thread, and the engine keeps no queue for
/// them: the application calls them from threads, a job system or `std::async`, as it schedules, and
/// commits the results on the thread that owns its scenes. A call checks its StopToken between bounded steps and
/// reports those steps to a StagingProgress.
///
/// The loaders that import GLB files pass their StagingOptions to those imports: AttachmentLibrary::load,
/// AttachmentSet::prepare, FittedLibrary::load, MotionRuntime and MotionRuntime::load, ActorPresentation and
/// AssetPreview. Their contracts say which of their work the options cover, and how loads that share one import
/// treat a stop.

namespace anima {
namespace detail {
class StagingSteps;
} // namespace detail

/// Observer of a StopSource's stop state, as `std::stop_token` observes a `std::stop_source`, without
/// stop callbacks.
///
/// Copies share the state, and any thread may copy one or read stop_requested(). A default token has no
/// state and never reports a stop.
class StopToken {
  public:
    /// Token without a state, which never reports a stop.
    StopToken() noexcept = default;
    /// Whether a stop was requested of the source this token came from. A true result synchronizes with
    /// that request (acquire and release).
    [[nodiscard]] bool stop_requested() const noexcept;

  private:
    friend class StopSource;
    explicit StopToken(std::shared_ptr<const std::atomic<bool>> state) noexcept : state_(std::move(state)) {}
    std::shared_ptr<const std::atomic<bool>> state_;
};

/// Owner of a stop state that StopToken copies observe, as `std::stop_source` is; copies share the state.
///
/// Any thread may request a stop. The request is permanent: the state cannot be reset, so a new load
/// needs a new source.
class StopSource {
  public:
    /// Creates a state with no stop requested. Throws `std::bad_alloc` when memory runs out. A moved-from
    /// source has no state: tokens it returns afterwards never report a stop, and request_stop() and
    /// stop_requested() return false. Tokens taken before the move observe the source it moved to.
    StopSource();
    /// Token that observes this source's state.
    [[nodiscard]] StopToken get_token() const noexcept { return StopToken(state_); }
    /// Requests a stop. Returns true when this call made the request and false when one was made
    /// before.
    bool request_stop() noexcept;
    /// Whether a stop was requested.
    [[nodiscard]] bool stop_requested() const noexcept;

  private:
    std::shared_ptr<std::atomic<bool>> state_;
};

/// Thrown by a staging call whose StopToken reported a stop.
///
/// It derives only from `std::exception`, so handlers of `std::invalid_argument` or `std::runtime_error`
/// for rejected content do not catch it. what() returns `"Staging was cancelled"`. The call returns no
/// result and leaves no side effects other than the application callbacks it already made, such as
/// MeshResolver calls, and the steps it already added to a StagingProgress.
class StagingCancelled : public std::exception {
  public:
    /// `"Staging was cancelled"`, a string with static storage duration.
    [[nodiscard]] const char *what() const noexcept override;
};

/// Completed and total steps of staging calls, held in atomic counters that any thread may read while
/// the calls run.
///
/// The steps are the read of a GLB file, the parse of each GLB or document, and each decoded image,
/// imported primitive, decoded document object, resolved mesh key and resolved custom material name. A
/// call adds a read or a parse to the total as it begins it, and its other steps once it has parsed the
/// file's structure. Both counters only increase, and completed() never exceeds a total() read after it.
/// Calls that share one object add to the same counters, whether they run one after another or at once.
/// After a failed or cancelled call, completed() may stay below total(). The object is borrowed by each
/// call and must outlive it.
class StagingProgress {
  public:
    /// Counters at zero.
    StagingProgress() = default;
    StagingProgress(const StagingProgress &) = delete;
    StagingProgress &operator=(const StagingProgress &) = delete;
    /// Steps finished so far.
    [[nodiscard]] std::uint64_t completed() const noexcept { return completed_.load(); }
    /// Steps known so far.
    [[nodiscard]] std::uint64_t total() const noexcept { return total_.load(); }

  private:
    friend class detail::StagingSteps;
    std::atomic<std::uint64_t> completed_{}, total_{};
};

/// Cancellation and progress for one staging call.
struct StagingOptions {
    /// Checked before the call's first step, between steps and before it returns; a stop throws
    /// StagingCancelled. The default token never stops.
    StopToken stop;
    /// Counters that the call advances, or null for none.
    StagingProgress *progress{};
};
} // namespace anima
