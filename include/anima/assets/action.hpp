#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

/// @file
/// Presentation clocks for actions: named timed and held phases with cues.
///
/// Part of the `anima::assets` target. The caller supplies elapsed and release times in seconds
/// from its own clock; a timeline never decides whether an action may run or applies its
/// effects. Failures throw `std::invalid_argument` unless stated.

namespace anima {
/// A named point within a phase.
struct ActionCue {
    /// Nonempty and unique within its phase.
    std::string id;
    /// Position in the phase, in [0, 1].
    double at{};
};
/// One phase of an ActionTimeline.
struct ActionPhase {
    /// Nonempty and unique within the timeline.
    std::string id;
    /// Seconds, in (0, 3600]: the length of a timed phase or the loop period of a held phase.
    double duration{};
    /// Loops until release instead of ending after #duration.
    bool held{};
    /// Cues in nondecreasing ActionCue::at order.
    std::vector<ActionCue> cues;
};
/// Where an ActionTimeline is at one moment.
struct ActionTime {
    /// Index of the current phase; the last phase once #complete.
    std::size_t phase{};
    /// Position in the phase, in [0, 1): within the current loop for a held phase. 1 once
    /// #complete.
    double progress{};
    /// Seconds since the phase started; the last phase's full length once #complete.
    double elapsed{};
    /// Timeline time at which the phase started.
    double start{};
    /// Completed loops of a held phase; 0 otherwise.
    std::uint64_t cycle{};
    /// Whether every phase has ended.
    bool complete{};
};
/// A cue at its timeline time.
struct TimedActionCue {
    /// Index of the cue's phase.
    std::size_t phase{};
    std::string id;
    /// Seconds from the start of the timeline.
    double time{};
};
/// Timed and held phases, sampled from caller-supplied elapsed and release times.
///
/// A timed phase lasts its duration. The held phase loops until the release time, or indefinitely
/// while unreleased; a release before it starts skips it. Sampling depends only on its arguments,
/// so seeking needs no history. Const member functions may run concurrently on any thread.
class ActionTimeline {
  public:
    /// Throws unless there are 1 to 64 phases with valid, unique ids and durations, at most one
    /// held phase, and at most 1024 cues in total, each valid and ordered within its phase.
    explicit ActionTimeline(std::vector<ActionPhase> phases) : phases_(std::move(phases)) {
        if (phases_.empty() || phases_.size() > maximum_phases)
            throw std::invalid_argument("Action needs 1..64 phases");
        std::set<std::string> names;
        bool held = false;
        std::size_t cues = 0;
        for (const auto &phase : phases_) {
            if (phase.id.empty() || !names.insert(phase.id).second || !std::isfinite(phase.duration) ||
                phase.duration <= 0 || phase.duration > maximum_phase_seconds || (held && phase.held))
                throw std::invalid_argument("Invalid/duplicate action phase or multiple held phases");
            held |= phase.held;
            double previous = -1;
            std::set<std::string> unique;
            for (const auto &cue : phase.cues) {
                if (cue.id.empty() || !unique.insert(cue.id).second || !std::isfinite(cue.at) || cue.at < 0 ||
                    cue.at > 1 || cue.at < previous || ++cues > maximum_cues)
                    throw std::invalid_argument("Invalid, unordered or excessive action cues");
                previous = cue.at;
            }
        }
        held_ = held;
    }
    const std::vector<ActionPhase> &phases() const { return phases_; }
    /// Whether a phase is held.
    bool held() const { return held_; }
    /// Total seconds of a timeline without a held phase. Throws `std::logic_error` when a phase is
    /// held.
    double duration() const {
        if (held_)
            throw std::logic_error("Held action has no fixed duration");
        double value = 0;
        for (const auto &phase : phases_)
            value += phase.duration;
        return value;
    }
    /// Position at @p elapsed seconds, with the held phase released at @p released_at. Throws for
    /// a negative or nonfinite time, a release time on a timeline without a held phase, or more
    /// than `1e12` loops of the held phase.
    ActionTime sample(double elapsed, std::optional<double> released_at = {}) const {
        validate_time(elapsed, released_at);
        double start = 0, last_start = 0;
        for (std::size_t i = 0; i < phases_.size(); ++i) {
            const auto &p = phases_[i];
            last_start = start;
            const double end = p.held ? (released_at ? std::max(start, *released_at) : INFINITY) : start + p.duration;
            if (elapsed < end) {
                const double local = elapsed - start;
                const double cycles = p.held ? std::floor(local / p.duration) : 0;
                if (cycles > maximum_held_cycles)
                    throw std::invalid_argument("Action clock exceeds cycle range");
                return {i,
                        p.held ? (local - cycles * p.duration) / p.duration : local / p.duration,
                        local,
                        start,
                        static_cast<std::uint64_t>(cycles),
                        false};
            }
            start = end;
        }
        return {phases_.size() - 1, 1, start - last_start, last_start, 0, true};
    }
    /// Every cue at its timeline time, in timeline order, for release time @p released_at.
    ///
    /// Held-phase cues occur once, in the first loop, and only before release; while unreleased,
    /// cues after the held phase are omitted. Continuous effects use explicit start and stop cues.
    /// Throws for a release time that sample() rejects.
    std::vector<TimedActionCue> cues(std::optional<double> released_at = {}) const {
        validate_time(0, released_at);
        std::vector<TimedActionCue> result;
        double start = 0;
        for (std::size_t i = 0; i < phases_.size(); ++i) {
            const auto &p = phases_[i];
            const double end = p.held ? (released_at ? std::max(start, *released_at) : INFINITY) : start + p.duration;
            // Held cues occur once in the first cycle. Continuous effects use
            // explicit start/stop cues; seeking cannot create a catch-up burst.
            for (const auto &cue : p.cues) {
                const double at = start + cue.at * p.duration;
                if ((!p.held || at < end) && std::isfinite(at))
                    result.push_back({i, cue.id, at});
            }
            if (!std::isfinite(end))
                break;
            start = end;
        }
        return result;
    }

  private:
    static constexpr std::size_t maximum_phases = 64;
    static constexpr double maximum_phase_seconds = 3600;
    static constexpr std::size_t maximum_cues = 1024;
    // Bounds a held phase's cycle count before it is converted to an integer, whatever the platform's cast does
    // beyond that range.
    static constexpr double maximum_held_cycles = 1e12;
    void validate_time(double elapsed, std::optional<double> released_at) const {
        if (!std::isfinite(elapsed) || elapsed < 0 ||
            (released_at && (!held_ || !std::isfinite(*released_at) || *released_at < 0)))
            throw std::invalid_argument("Invalid action presentation/release time");
    }
    std::vector<ActionPhase> phases_;
    bool held_{};
};
/// Reports the cues that the frames of one action instance reach, each at most once until a rewind.
///
/// Use one cursor per observer of an action instance. A new instance starts at time 0, so its first
/// advance() reports every cue it has reached, even when that first frame lands after 0. An
/// observer that joins an instance already under way calls seek() first. A rewind, any frame
/// earlier than the one before it, seeks silently: later frames report the cues after its time
/// again, but none at or before it. Repeated frames never report a cue twice. A release that
/// arrives late still reports the cues it places behind the previous frame, unless they lie at or
/// before the latest seek() or rewind, however small that rewind's step back. Call reset() on
/// cancellation. Not synchronized: use each cursor from one thread at a time.
class ActionCueCursor {
  public:
    /// Returns, in timeline order, the cues of @p timeline at or before @p elapsed that the cursor
    /// has not yet passed for @p action and @p instance, except cues at or before the time of the
    /// instance's latest seek() or rewind. Each call passes the cues it reaches, reported or not, and
    /// identifies a cue by its phase index and id. A rewind, a call with an earlier @p elapsed than
    /// the previous call's, forgets the cues passed before it. A call for another action or instance
    /// than the previous call's starts that instance anew, with no cue passed and no seek or rewind
    /// time. So a frame reports the cues reached since the previous call; when advance() starts an
    /// instance, it reports every cue from time 0 through @p elapsed; and a cue that a late or
    /// changed @p released_at, or a @p timeline unlike the previous call's, places at or before the
    /// previous call's time is reported by the first advance() that reaches it. Throws for an empty
    /// @p action, a zero @p instance, or times that ActionTimeline::sample rejects.
    std::vector<TimedActionCue> advance(std::string_view action, std::uint64_t instance, const ActionTimeline &timeline,
                                        double elapsed, std::optional<double> released_at = {}) {
        return move(action, instance, timeline, elapsed, released_at, true);
    }
    /// Moves to @p elapsed without reporting cues, as for an observer that joins @p instance after it
    /// started. Until a rewind or a call for another instance, later advance() calls report no cue at
    /// or before @p elapsed, whatever release time they pass. Throws as advance() does.
    void seek(std::string_view action, std::uint64_t instance, const ActionTimeline &timeline, double elapsed,
              std::optional<double> released_at = {}) {
        (void)move(action, instance, timeline, elapsed, released_at, false);
    }
    /// Forgets the current instance, so the next advance() starts fresh.
    void reset() { *this = {}; }

  private:
    std::vector<TimedActionCue> move(std::string_view action, std::uint64_t instance, const ActionTimeline &timeline,
                                     double elapsed, std::optional<double> released_at, bool report) {
        (void)timeline.sample(elapsed, released_at);
        if (action.empty() || !instance)
            throw std::invalid_argument("Action cue cursor needs an instance identity");
        const bool fresh = action_ != action || instance_ != instance;
        const bool rewound = !fresh && elapsed < elapsed_;
        if (fresh || rewound) {
            emitted_.clear();
            action_ = action;
            instance_ = instance;
        }
        elapsed_ = elapsed;
        // Only a seek or a rewind passes over time silently, and an instance that advance() starts has no
        // silent time. A later advance keeps the bound, so a cue placed behind the previous advance, as by a
        // late release, is still reported once.
        if (!report || rewound)
            silent_through_ = elapsed;
        else if (fresh)
            silent_through_.reset();
        std::vector<TimedActionCue> result;
        for (auto cue : timeline.cues(released_at)) {
            if (cue.time > elapsed)
                continue;
            const bool first = emitted_.insert({cue.phase, cue.id}).second;
            if (first && (!silent_through_ || cue.time > *silent_through_))
                result.push_back(std::move(cue));
        }
        return result;
    }
    std::string action_;
    std::uint64_t instance_{};
    double elapsed_{};
    // Time of the latest seek or rewind of this instance; advance() reports no cue at or before it.
    std::optional<double> silent_through_;
    std::set<std::pair<std::size_t, std::string>> emitted_;
};
} // namespace anima
