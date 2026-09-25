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

namespace anima {
// A presentation clock. The caller supplies authoritative elapsed/release time;
// this does not decide whether a skill is legal or apply gameplay effects.
struct ActionCue {
    std::string id;
    double phase{};
};
struct ActionPhase {
    std::string id;
    double duration{}; // Seconds for a timed phase, loop period for a held phase.
    bool held{};
    std::vector<ActionCue> cues;
};
struct ActionTime {
    std::size_t phase{};
    double progress{}, elapsed{}, start{};
    std::uint64_t cycle{};
    bool complete{};
};
struct TimedActionCue {
    std::size_t phase{};
    std::string id;
    double time{};
};
class ActionTimeline {
  public:
    explicit ActionTimeline(std::vector<ActionPhase> phases) : phases_(std::move(phases)) {
        if (phases_.empty() || phases_.size() > 64)
            throw std::invalid_argument("Action needs 1..64 phases");
        std::set<std::string> names;
        bool held = false;
        std::size_t cues = 0;
        for (const auto &phase : phases_) {
            if (phase.id.empty() || !names.insert(phase.id).second || !std::isfinite(phase.duration) ||
                phase.duration <= 0 || phase.duration > 3600 || (held && phase.held))
                throw std::invalid_argument("Invalid/duplicate action phase or multiple held phases");
            held |= phase.held;
            double previous = -1;
            std::set<std::string> unique;
            for (const auto &cue : phase.cues) {
                if (cue.id.empty() || !unique.insert(cue.id).second || !std::isfinite(cue.phase) || cue.phase < 0 ||
                    cue.phase > 1 || cue.phase < previous || ++cues > 1024)
                    throw std::invalid_argument("Invalid, unordered or excessive action cues");
                previous = cue.phase;
            }
        }
        held_ = held;
    }
    const std::vector<ActionPhase> &phases() const { return phases_; }
    bool held() const { return held_; }
    double duration() const {
        if (held_)
            throw std::logic_error("Held action has no fixed duration");
        double value = 0;
        for (const auto &phase : phases_)
            value += phase.duration;
        return value;
    }
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
                // Bound conversion independently of the platform's integer cast.
                if (cycles > 1e12)
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
                const double at = start + cue.phase * p.duration;
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
    void validate_time(double elapsed, std::optional<double> released_at) const {
        if (!std::isfinite(elapsed) || elapsed < 0 ||
            (released_at && (!held_ || !std::isfinite(*released_at) || *released_at < 0)))
            throw std::invalid_argument("Invalid action presentation/release time");
    }
    std::vector<ActionPhase> phases_;
    bool held_{};
};
// One cursor per actor/action instance. New late observers and rewinds seek
// silently; repeated frames cannot re-emit a cue. Cancellation resets the cursor.
class ActionCueCursor {
  public:
    std::vector<TimedActionCue> advance(std::string_view action, std::uint64_t instance, const ActionTimeline &timeline,
                                        double elapsed, std::optional<double> released_at = {}) {
        (void)timeline.sample(elapsed, released_at);
        if (action.empty() || !instance)
            throw std::invalid_argument("Action cue cursor needs an instance identity");
        const bool fresh = action_ != action || instance_ != instance || elapsed < elapsed_;
        const auto previous = elapsed_;
        if (fresh) {
            emitted_.clear();
            action_ = action;
            instance_ = instance;
        }
        elapsed_ = elapsed;
        std::vector<TimedActionCue> result;
        for (auto cue : timeline.cues(released_at)) {
            const auto key = std::make_pair(cue.phase, cue.id);
            if (cue.time > elapsed)
                continue;
            const bool first = emitted_.insert(key).second;
            if (first && ((fresh && elapsed == 0 && cue.time == 0) || (!fresh && cue.time > previous)))
                result.push_back(std::move(cue));
        }
        return result;
    }
    void reset() { *this = {}; }

  private:
    std::string action_;
    std::uint64_t instance_{};
    double elapsed_{};
    std::set<std::pair<std::size_t, std::string>> emitted_;
};
} // namespace anima
