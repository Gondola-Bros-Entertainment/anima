#pragma once
#include "gpu_checks.hpp"
#include "resources.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

// FrameProfile accounts for each draw() with one frame in flight and with two: its CPU fields add up to the call's
// duration, releasing cached meshes counts in prepare_ms, the GPU fields time the frame submitted
// RendererOptions::frames_in_flight submissions earlier, the GPU interval fields add up to gpu_ms, and where the
// renderer measures it, the GPU's idle time between two frames is absent from the first timed frame, which follows no
// timed frame, lies within the calls around the two frames and, with one frame in flight, spans the CPU work that
// separated them, and a present interval is reported only where the renderer measures one, and then by some calls.
// After VulkanRenderer::wait_for_frame(), which an application calls before reading input, draw() waits for no frame.
namespace profile_test {
inline void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
// Most that a draw()'s duration may exceed the sum of its CPU fields, which leave out only the call's entry before
// its first clock reading and its return after presentation.
constexpr double untimed_limit_ms = .1;
// Most that a duration on the GPU's clock may differ from the same duration on the CPU's, as a fraction of it.
constexpr double clock_tolerance = .1;
// Each release trial caches this many meshes, then releases them all before one draw().
constexpr std::size_t released_meshes = 256;
constexpr int steady_frames = 60, release_trials = 3;
// Each lag trial rebuilds every atmosphere table in one frame, which the GPU fields must report frames_in_flight calls
// later as the longest atmosphere time of the calls around it, at least this many times any other, while each of the
// others redraws only the sky view table.
constexpr int lag_trials = 3;
constexpr double least_lag_contrast = 2;
// Heights of the sun's direction, before normalization, between which every lag trial call moves the sun, so that each
// frame redraws at least the sky view table (FrameProfile::gpu_atmosphere_ms).
constexpr std::array<float, 2> lag_sun_heights{.8F, .9F};
// Most that the median fence_wait_ms of a draw() after wait_for_frame() may reach: the call then only checks the
// window before a wait that returns at once.
constexpr double waited_fence_limit_ms = .1;

inline double median(std::vector<double> values) {
    require(!values.empty(), "No frame was measured");
    const auto middle = values.begin() + std::ptrdiff_t(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

class Check {
  public:
    /// Checks the calls of @p renderer, which keeps @p frames in flight.
    Check(anima::VulkanRenderer &renderer, std::uint32_t frames)
        : renderer_(renderer), frames_(frames), started_(std::chrono::steady_clock::now()) {}
    /// Handles window events, then calls draw() once and checks the call; the result is valid until the next call.
    const gpu_check::TimedDraw &draw() {
        require(std::chrono::steady_clock::now() - started_ < gpu_check::watchdog, "Profile watchdog expired");
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            require(event.type != SDL_EVENT_QUIT && event.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED,
                    "Profile check interrupted");
            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                renderer_.request_resize();
        }
        calls_.push_back(gpu_check::timed_draw(renderer_));
        // The call that submitted the frame before the timed one, the calls up to this one, and this one.
        if (calls_.size() > frames_ + 2)
            calls_.pop_front();
        check();
        return calls_.back();
    }
    /// Calls draw() until a frame is presented and returns the presenting call.
    const gpu_check::TimedDraw &present() {
        for (;;) {
            if (const auto &call = draw(); call.presented)
                return call;
            SDL_Delay(5);
        }
    }
    /// The GPU idle time of every call that reported one.
    [[nodiscard]] const std::vector<double> &idle_ms() const noexcept { return idle_ms_; }
    /// The present interval of every call that reported one.
    [[nodiscard]] const std::vector<double> &interval_ms() const noexcept { return interval_ms_; }

  private:
    // Checks the latest call against the frames_ + 1 before it.
    void check() {
        const auto &call = calls_.back();
        const auto &profile = call.profile;
        for (const double field : {profile.fence_wait_ms, profile.prepare_ms, profile.upload_ms, profile.acquire_ms,
                                   profile.record_submit_ms, profile.present_ms})
            require(std::isfinite(field) && field >= 0, "A CPU field is negative or not finite");
        if (profile.present_interval_ms) {
            require(renderer_.measures_present_interval(), "A present interval was reported without display timing");
            require(std::isfinite(*profile.present_interval_ms) && *profile.present_interval_ms >= 0,
                    "A present interval is negative or not finite");
            interval_ms_.push_back(*profile.present_interval_ms);
        }
        // The fields divide the call's own clock readings, so only rounding can carry their sum past its duration.
        constexpr double rounding_ms = 1e-6;
        require(call.cpu_ms() <= call.wall_ms() + rounding_ms, "The CPU fields add up to more than the draw() call");
        // Likewise the GPU interval fields divide gpu_ms at the frame's timestamps between its first and its last.
        if (profile.gpu_available) {
            for (const double field : {profile.gpu_ms, profile.gpu_atmosphere_ms, profile.gpu_shadow_ms,
                                       profile.gpu_scene_ms, profile.gpu_resolve_ms, profile.gpu_transfer_ms})
                require(std::isfinite(field) && field >= 0, "A GPU field is negative or not finite");
            const double intervals = profile.gpu_atmosphere_ms + profile.gpu_shadow_ms + profile.gpu_scene_ms +
                                     profile.gpu_resolve_ms + profile.gpu_transfer_ms;
            require(std::abs(intervals - profile.gpu_ms) <= rounding_ms,
                    "The GPU interval fields add up to " + std::to_string(intervals) + " ms, not the " +
                        std::to_string(profile.gpu_ms) + " ms of gpu_ms");
        }
        // Every draw() of the renderer passes through this check, so the first call with GPU fields reads the first
        // timed frame, which has no timed frame before it to measure idle time from.
        if (profile.gpu_available && !timed_) {
            timed_ = true;
            require(!profile.gpu_idle_ms, "The first timed frame reported GPU idle time");
        }
        // Each call reads the frame that its slot submitted last, and the slots submit in turn, so two calls in a row
        // with GPU fields read two frames submitted in a row, and the second reports its idle time.
        const bool follows_timed = calls_.size() > 1 && calls_[calls_.size() - 2].profile.gpu_available;
        if (!profile.gpu_idle_ms) {
            require(!(renderer_.measures_gpu_idle() && profile.gpu_available && follows_timed),
                    "A timed frame after a timed frame has no GPU idle time");
            return;
        }
        const double idle = *profile.gpu_idle_ms;
        require(renderer_.measures_gpu_idle() && profile.gpu_available,
                "GPU idle time was reported without calibrated GPU timestamps");
        require(std::isfinite(idle) && idle >= 0, "GPU idle time is negative or not finite");
        idle_ms_.push_back(idle);
        // With one frame in flight, the previous call submitted the timed frame after waiting for the frame before
        // it, which had then finished, so the GPU had no frame between them while that call prepared, uploaded and
        // acquired. With two, the timed frame may be queued before the earlier one finishes.
        if (frames_ == 1 && calls_.size() > 1 && calls_[calls_.size() - 2].presented) {
            const auto &submitter = calls_[calls_.size() - 2].profile;
            const double least =
                (submitter.prepare_ms + submitter.upload_ms + submitter.acquire_ms) * (1 - clock_tolerance);
            require(idle >= least, "GPU idle time " + std::to_string(idle) + " ms is shorter than the " +
                                       std::to_string(least) + " ms the submitting draw() spent before submission");
        }
        // When the calls before this one all submitted their frames, the frame before the timed one was submitted
        // during the earliest remembered call, frames_ + 1 calls back, and the timed one finished before this call's
        // fence wait ended.
        if (calls_.size() < frames_ + 2 ||
            !std::all_of(calls_.begin(), calls_.end() - 1, [](const auto &earlier) { return earlier.presented; }))
            return;
        const double most = std::chrono::duration<double, std::milli>(call.ended - calls_.front().began).count() *
                            (1 + clock_tolerance);
        require(idle <= most, "GPU idle time " + std::to_string(idle) + " ms is longer than the " +
                                  std::to_string(most) + " ms of the " + std::to_string(calls_.size()) +
                                  " draw() calls around it");
    }
    anima::VulkanRenderer &renderer_;
    std::uint32_t frames_;
    std::chrono::steady_clock::time_point started_;
    std::deque<gpu_check::TimedDraw> calls_;
    // Whether a call has read a timed frame.
    bool timed_{};
    std::vector<double> idle_ms_, interval_ms_;
};

// What one renderer's checks measured.
struct Summary {
    double steady_untimed{}, release_untimed{}, release_prepare{}, steady_prepare{};
    // Median fence_wait_ms of the steady calls, and of the calls that followed wait_for_frame().
    double steady_fence{}, waited_fence{};
    std::vector<double> idle, release_idle, interval;
    // Median contrast of the rebuilt frame's atmosphere time over the longest other one, where measured.
    std::optional<double> lag_contrast;
};

// Rebuilds every atmosphere table in one frame and requires the call frames_in_flight calls later to report it, as the
// longest atmosphere time of the calls around it by at least least_lag_contrast. Each trial draws as many calls as
// it needs while each presents its frame, so that every call reads the frame of the call frames_in_flight before.
// Every call moves the sun, so that the frames around the rebuild redraw the sky view table.
inline std::optional<double> check_lag(anima::VulkanRenderer &renderer, Check &check, std::uint32_t frames) {
    anima::Environment environment;
    environment.atmosphere.enabled = true;
    std::size_t moves = 0;
    const auto move_sun = [&] {
        environment.sun.direction.y = lag_sun_heights[moves++ % lag_sun_heights.size()];
        renderer.set_environment(environment);
    };
    // Calls before each rebuild, so that the earliest checked call reads a frame that only redraws the sky view table,
    // and not the first frame with the atmosphere, which builds every table.
    const auto before = int(frames) + 1;
    const auto after = int(frames) + 2;
    std::vector<double> contrasts;
    for (int trial = 0; contrasts.size() < lag_trials; ++trial) {
        constexpr int most_trials = 3 * lag_trials;
        require(trial < most_trials, "Too many lag trials had a draw() that presented nothing");
        bool timed = false;
        for (int call = 0; call < before; ++call) {
            move_sun();
            timed = check.present().profile.gpu_available;
        }
        if (!timed)
            return std::nullopt; // The device writes no timestamps.
        // The next move_sun() sets the medium that rebuilds the tables.
        environment.atmosphere.rayleigh_scale_height = trial % 2 ? 8'000.F : 8'001.F;
        std::vector<double> atmosphere_ms;
        bool presented = true;
        for (int call = 0; call < after && presented; ++call) {
            move_sun();
            const auto &next = check.draw();
            presented = next.presented;
            require(!presented || next.profile.gpu_available, "A call after timed calls has no GPU fields");
            atmosphere_ms.push_back(next.profile.gpu_atmosphere_ms);
        }
        if (!presented)
            continue;
        const auto rebuilt = atmosphere_ms[frames];
        atmosphere_ms.erase(atmosphere_ms.begin() + std::ptrdiff_t(frames));
        const auto longest_other = *std::max_element(atmosphere_ms.begin(), atmosphere_ms.end());
        std::string times;
        for (const auto value : atmosphere_ms)
            times += " " + std::to_string(value);
        require(rebuilt >= least_lag_contrast * longest_other,
                "With " + std::to_string(frames) + " frames in flight, the call " + std::to_string(frames) +
                    " after the rebuild reported " + std::to_string(rebuilt) +
                    " ms of atmosphere work, not the longest by a factor of " + std::to_string(least_lag_contrast) +
                    " among the calls around it:" + times);
        // A device whose timestamps are coarser than a dispatch can write equal ones around it.
        contrasts.push_back(longest_other > 0 ? rebuilt / longest_other : std::numeric_limits<double>::infinity());
    }
    return median(contrasts);
}

inline Summary check_renderer(SDL_Window *window, std::uint32_t frames) {
    const auto asset = resource_test::fixture();
    auto scene = std::make_shared<anima::Scene>();
    (void)scene->add(anima::Mesh::compile(*asset));
    anima::RendererOptions options;
    options.validation = true;
    options.profile = true;
    options.frames_in_flight = frames;
    options.scenes = {scene};
    anima::VulkanRenderer renderer(window, options);
    anima::OrbitCamera camera;
    const auto bounds = scene->bounds();
    camera.frame(bounds.minimum, bounds.maximum);
    int width{}, height{};
    require(SDL_GetWindowSizeInPixels(window, &width, &height) && width > 0 && height > 0,
            "Drawable dimensions unavailable");
    renderer.set_view(camera.matrix(float(width) / float(height)));
    // Every slot's fence starts signaled, so a wait before the first frame has nothing to wait for and cannot time out.
    renderer.wait_for_frame();
    Check check(renderer, frames);
    Summary summary;

    std::vector<double> untimed, prepare, fence;
    for (int frame = 0; frame < steady_frames; ++frame) {
        const auto &call = check.present();
        untimed.push_back(call.wall_ms() - call.cpu_ms());
        prepare.push_back(call.profile.prepare_ms);
        fence.push_back(call.profile.fence_wait_ms);
    }
    summary.steady_untimed = median(untimed);
    summary.steady_prepare = median(prepare);
    summary.steady_fence = median(fence);
    require(summary.steady_untimed <= untimed_limit_ms,
            "The CPU fields leave " + std::to_string(summary.steady_untimed) + " ms of a typical draw() untimed");

    // Waiting for the frame slot first, as an application does before reading input, leaves draw() no frame to wait
    // for, so its fence wait returns at once.
    std::vector<double> waited;
    while (waited.size() < std::size_t(steady_frames)) {
        renderer.wait_for_frame();
        if (const auto &call = check.draw(); call.presented)
            waited.push_back(call.profile.fence_wait_ms);
        else
            SDL_Delay(5);
    }
    summary.waited_fence = median(waited);
    require(summary.waited_fence <= waited_fence_limit_ms,
            "After wait_for_frame(), draw() still waited a median " + std::to_string(summary.waited_fence) +
                " ms for its frame, against " + std::to_string(summary.steady_fence) + " ms without it");

    // Releasing cached meshes is frame preparation: the first draw() after they lose their last owner releases them
    // before culling. It destroys them too unless a frame that may draw them is still in flight, as one can be with two
    // frames in flight, and then the next draw() destroys them once that frame has finished.
    std::vector<double> release_untimed, release_prepare;
    for (int trial = 0; trial < release_trials; ++trial) {
        std::vector<std::shared_ptr<const anima::Mesh>> meshes;
        for (std::size_t i = 0; i < released_meshes; ++i)
            meshes.push_back(anima::Mesh::compile(*asset));
        renderer.prepare_meshes(meshes);
        const auto cached = renderer.resource_stats().cached_assets;
        meshes.clear();
        std::optional<double> released_prepare;
        for (;;) {
            const auto &call = check.draw();
            if (renderer.resource_stats().cached_assets == cached) {
                SDL_Delay(5);
                continue;
            }
            require(renderer.resource_stats().cached_assets + released_meshes == cached,
                    "One draw() did not release every unowned mesh");
            // Only a call that went on to present its frame ran through every field.
            if (call.presented) {
                release_untimed.push_back(call.wall_ms() - call.cpu_ms());
                released_prepare = call.profile.prepare_ms;
            }
            break;
        }
        const auto &next = check.draw();
        if (next.presented) {
            if (released_prepare)
                release_prepare.push_back(std::max(*released_prepare, next.profile.prepare_ms));
            // The checks of each call bound the GPU idle time around the release.
            if (next.profile.gpu_idle_ms)
                summary.release_idle.push_back(*next.profile.gpu_idle_ms);
        } else
            (void)check.present();
    }
    require(!release_untimed.empty() && !release_prepare.empty(),
            "No draw() that released or destroyed the meshes presented its frame");
    summary.release_untimed = *std::min_element(release_untimed.begin(), release_untimed.end());
    require(summary.release_untimed <= untimed_limit_ms, "The CPU fields leave " +
                                                             std::to_string(summary.release_untimed) +
                                                             " ms of every draw() that released meshes untimed");
    summary.release_prepare = *std::min_element(release_prepare.begin(), release_prepare.end());
    require(summary.release_prepare > summary.steady_prepare,
            "Destroying " + std::to_string(released_meshes) +
                " meshes did not lengthen prepare_ms beyond its typical " + std::to_string(summary.steady_prepare) +
                " ms");
    summary.lag_contrast = check_lag(renderer, check, frames);
    if (renderer.measures_gpu_idle())
        require(!check.idle_ms().empty(), "No frame reported GPU idle time");
    summary.idle = check.idle_ms();
    if (renderer.measures_present_interval())
        require(!check.interval_ms().empty(), "No draw() reported a present interval");
    summary.interval = check.interval_ms();

    const auto stats = renderer.shutdown();
    require(!stats.validation_errors && !stats.validation_warnings, "Profile check validation failed");
    return summary;
}

inline int run(int argc, char **) {
    require(argc == 2, "Usage: consumer --profile");
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    gpu_check::Video video;
    const auto window = gpu_check::window("Anima profile verification", 640, 480, SDL_WINDOW_HIGH_PIXEL_DENSITY);
    std::ostringstream report;
    for (const std::uint32_t frames : {1U, 2U}) {
        const auto summary = check_renderer(window.get(), frames);
        report << "; with " << frames << " in flight, the CPU fields leave a median " << summary.steady_untimed
               << " ms of draw() untimed, and " << summary.release_untimed << " ms of a draw() that released "
               << released_meshes << " meshes, whose prepare_ms of " << summary.release_prepare
               << " ms or more exceeds the median " << summary.steady_prepare << " ms, a median fence_wait_ms of "
               << summary.waited_fence << " ms after wait_for_frame() and " << summary.steady_fence
               << " ms without it, ";
        // The bound on the waited calls tells a wait_for_frame() that waits from one that does not only where draw()'s
        // own wait exceeds it.
        if (summary.steady_fence <= waited_fence_limit_ms)
            report << "which cannot show that wait_for_frame() waits, since draw() alone waits within the "
                   << waited_fence_limit_ms << " ms bound, ";
        if (summary.lag_contrast) {
            report << "the call " << frames << " after an atmosphere rebuild reports it, ";
            if (std::isinf(*summary.lag_contrast))
                report << "with no atmosphere time on the other calls of most trials, ";
            else
                report << "at a median " << *summary.lag_contrast << " times any other call's atmosphere time, ";
        } else
            report << "no GPU timestamps, ";
        if (!summary.idle.empty()) {
            report << "GPU idle time in " << summary.idle.size() << " frames, median " << median(summary.idle) << " ms";
            if (!summary.release_idle.empty())
                report << ", " << median(summary.release_idle) << " ms around a release";
        } else
            report << "no calibrated timestamps, so no GPU idle time";
        if (!summary.interval.empty())
            report << ", present intervals in " << summary.interval.size() << " draws, median "
                   << median(summary.interval) << " ms";
        else
            report << ", no display timing, so no present intervals";
    }
    std::cout << "PASS profile" << report.str() << '\n';
    return 0;
}
} // namespace profile_test
