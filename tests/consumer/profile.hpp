#pragma once
#include "gpu_checks.hpp"
#include "resources.hpp"
#include <algorithm>
#include <cmath>
#include <deque>

// FrameProfile accounts for each draw(): its CPU fields add up to the call's duration, releasing cached meshes counts
// in prepare_ms, and where the renderer measures it, the GPU's idle time between two frames spans the CPU work that
// separated them and is absent from the first timed frame, which follows no timed frame.
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

inline double median(std::vector<double> values) {
    require(!values.empty(), "No frame was measured");
    const auto middle = values.begin() + std::ptrdiff_t(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

class Check {
  public:
    explicit Check(anima::VulkanRenderer &renderer) : renderer_(renderer), started_(std::chrono::steady_clock::now()) {}
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
        constexpr std::size_t remembered = 3;
        if (calls_.size() > remembered)
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

  private:
    // Checks the latest call against the two before it.
    void check() {
        const auto &call = calls_.back();
        const auto &profile = call.profile;
        for (const double field : {profile.fence_wait_ms, profile.prepare_ms, profile.upload_ms, profile.acquire_ms,
                                   profile.record_submit_ms, profile.present_ms})
            require(std::isfinite(field) && field >= 0, "A CPU field is negative or not finite");
        // The fields divide the call's own clock readings, so only rounding can carry their sum past its duration.
        constexpr double rounding_ms = 1e-6;
        require(call.cpu_ms() <= call.wall_ms() + rounding_ms, "The CPU fields add up to more than the draw() call");
        // Every draw() of the renderer passes through this check, so the first call with GPU fields reads the first
        // timed frame, which has no timed frame before it to measure idle time from.
        if (profile.gpu_available && !timed_) {
            timed_ = true;
            require(!profile.gpu_idle_ms, "The first timed frame reported GPU idle time");
        }
        if (!profile.gpu_idle_ms) {
            // Each timed frame after a timed frame that the call before submitted reports its idle time.
            const bool follows_timed = calls_.size() > 1 && calls_[calls_.size() - 2].presented &&
                                       calls_[calls_.size() - 2].profile.gpu_available;
            require(!(renderer_.measures_gpu_idle() && profile.gpu_available && follows_timed),
                    "A timed frame after a timed frame has no GPU idle time");
            return;
        }
        const double idle = *profile.gpu_idle_ms;
        require(renderer_.measures_gpu_idle() && profile.gpu_available,
                "GPU idle time was reported without calibrated GPU timestamps");
        require(std::isfinite(idle) && idle >= 0, "GPU idle time is negative or not finite");
        idle_ms_.push_back(idle);
        if (calls_.size() < 2 || !calls_[calls_.size() - 2].presented)
            return;
        // The previous call submitted the timed frame after waiting for the frame before it, which had then
        // finished, so the GPU had no frame between them while that call prepared, uploaded and acquired.
        const auto &submitter = calls_[calls_.size() - 2].profile;
        const double least =
            (submitter.prepare_ms + submitter.upload_ms + submitter.acquire_ms) * (1 - clock_tolerance);
        require(idle >= least, "GPU idle time " + std::to_string(idle) + " ms is shorter than the " +
                                   std::to_string(least) + " ms the submitting draw() spent before submission");
        if (calls_.size() < 3 || !calls_.front().presented)
            return;
        // The frame before the timed one was submitted during the earliest remembered call, and the timed one
        // finished before this call's fence wait ended.
        const double most = std::chrono::duration<double, std::milli>(call.ended - calls_.front().began).count() *
                            (1 + clock_tolerance);
        require(idle <= most, "GPU idle time " + std::to_string(idle) + " ms is longer than the " +
                                  std::to_string(most) + " ms of the three draw() calls around it");
    }
    anima::VulkanRenderer &renderer_;
    std::chrono::steady_clock::time_point started_;
    std::deque<gpu_check::TimedDraw> calls_;
    // Whether a call has read a timed frame.
    bool timed_{};
    std::vector<double> idle_ms_;
};

inline int run(int argc, char **) {
    require(argc == 2, "Usage: consumer --profile");
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    gpu_check::Video video;
    const auto window = gpu_check::window("Anima profile verification", 640, 480, SDL_WINDOW_HIGH_PIXEL_DENSITY);
    const auto asset = resource_test::fixture();
    auto scene = std::make_shared<anima::Scene>();
    (void)scene->add(anima::Mesh::compile(*asset));
    anima::RendererOptions options;
    options.validation = true;
    options.profile = true;
    options.scenes = {scene};
    anima::VulkanRenderer renderer(window.get(), options);
    anima::OrbitCamera camera;
    const auto bounds = scene->bounds();
    camera.frame(bounds.minimum, bounds.maximum);
    int width{}, height{};
    require(SDL_GetWindowSizeInPixels(window.get(), &width, &height) && width > 0 && height > 0,
            "Drawable dimensions unavailable");
    renderer.set_view(camera.matrix(float(width) / float(height)));
    Check check(renderer);

    std::vector<double> untimed, prepare;
    for (int frame = 0; frame < steady_frames; ++frame) {
        const auto &call = check.present();
        untimed.push_back(call.wall_ms() - call.cpu_ms());
        prepare.push_back(call.profile.prepare_ms);
    }
    const double steady_untimed = median(untimed), steady_prepare = median(prepare);
    require(steady_untimed <= untimed_limit_ms,
            "The CPU fields leave " + std::to_string(steady_untimed) + " ms of a typical draw() untimed");

    // Releasing cached meshes is frame preparation: the draw() after the release destroys them before culling.
    std::vector<double> release_untimed, release_prepare, release_idle;
    for (int trial = 0; trial < release_trials; ++trial) {
        std::vector<std::shared_ptr<const anima::Mesh>> meshes;
        for (std::size_t i = 0; i < released_meshes; ++i)
            meshes.push_back(anima::Mesh::compile(*asset));
        renderer.prepare_meshes(meshes);
        const auto cached = renderer.resource_stats().cached_assets;
        meshes.clear();
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
                release_prepare.push_back(call.profile.prepare_ms);
            }
            break;
        }
        // The next frame's GPU idle time spans the release, which the checks of each call bound from below.
        if (const auto &next = check.present(); next.profile.gpu_idle_ms)
            release_idle.push_back(*next.profile.gpu_idle_ms);
    }
    require(!release_untimed.empty(), "No draw() that released the meshes presented its frame");
    const double least_untimed = *std::min_element(release_untimed.begin(), release_untimed.end());
    require(least_untimed <= untimed_limit_ms, "The CPU fields leave " + std::to_string(least_untimed) +
                                                   " ms of every draw() that released meshes untimed");
    const double least_release = *std::min_element(release_prepare.begin(), release_prepare.end());
    require(least_release > steady_prepare, "Releasing " + std::to_string(released_meshes) +
                                                " meshes did not lengthen prepare_ms beyond its typical " +
                                                std::to_string(steady_prepare) + " ms");
    if (renderer.measures_gpu_idle())
        require(!check.idle_ms().empty(), "No frame reported GPU idle time");

    const auto stats = renderer.shutdown();
    require(!stats.validation_errors && !stats.validation_warnings, "Profile check validation failed");
    std::cout << "PASS profile: the CPU fields leave a median " << steady_untimed << " ms of draw() untimed, and "
              << least_untimed << " ms of a draw() that released " << released_meshes << " meshes, whose prepare_ms of "
              << least_release << " ms or more exceeds the median " << steady_prepare << " ms; ";
    if (renderer.measures_gpu_idle()) {
        std::cout << "GPU idle time in " << check.idle_ms().size() << " frames, median " << median(check.idle_ms())
                  << " ms";
        if (!release_idle.empty())
            std::cout << ", " << median(release_idle) << " ms around a release";
        std::cout << '\n';
    } else
        std::cout << "this device has no calibrated timestamps, so no GPU idle time\n";
    return 0;
}
} // namespace profile_test
