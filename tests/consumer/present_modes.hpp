#pragma once
// RendererOptions::present_mode and VulkanRenderer::set_present_mode through the public API. A renderer reports no
// mode before its first swapchain, then the requested mode where the surface offers it and fifo otherwise, and fifo
// when fifo is requested. Each request of another mode recreates the swapchain in the next draw(), and only once
// unless a window event or a draw that presents nothing falls in the same span of draws, while a span of draws after a
// repeated and a rejected request that neither disturbs recreates nothing; every mode draws the image that the first
// mode drew. A resize after a mode change keeps the mode. RenderStats::present_waits counts which presents the frames
// wait for: in a new swapchain that a span of draws keeps undisturbed, each draw after the first frames_in_flight + 1
// waits once where the renderer waits for presents and the mode is fifo, with two frames in flight and with one, and no
// draw waits in immediate or on a device without present waits. Neither the count nor the validation layer, which
// checks only that present ids increase and that the features are enabled, shows how long a wait blocks. Where the
// renderer measures present intervals, at least half of the fifo draws report one. Validation stays clean throughout.
#include "gpu_checks.hpp"
#include "rejection.hpp"
#include "resources.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <anima/desktop/vulkan_renderer.hpp>
#include <anima/scene.hpp>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace present_mode_test {
using anima::PresentMode;
using rejection::rejects;
inline void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
// A value that names no PresentMode enumerator.
constexpr auto unknown_mode = static_cast<PresentMode>(-1);
constexpr std::string_view unknown_message = "Unknown present mode";
constexpr std::array all_modes{PresentMode::fifo, PresentMode::immediate, PresentMode::mailbox,
                               PresentMode::fifo_relaxed};
constexpr int window_width = 640, window_height = 480, resized_width = 760, resized_height = 520;
// Frames drawn in a mode before the next step, and frames of the fifo run whose present intervals are measured.
constexpr int settle_frames = 5, fifo_frames = 60;
// Least share of the fifo run's draws that must report a present interval where the renderer measures one; display
// times arrive after a delay, so the first draws of a swapchain report none. The intervals themselves are the driver's:
// for a frame that Core Animation did not display, MoltenVK 1.4.1 reports the time at which it learned that, so a
// window that the system does not show reports intervals as short as the draws, and no bound on them is checked.
constexpr double least_interval_share = .5;

inline const char *name(PresentMode mode) {
    switch (mode) {
    case PresentMode::fifo:
        return "fifo";
    case PresentMode::immediate:
        return "immediate";
    case PresentMode::mailbox:
        return "mailbox";
    case PresentMode::fifo_relaxed:
        return "fifo_relaxed";
    }
    return "unknown";
}
inline std::string name(const std::optional<PresentMode> &mode) { return mode ? name(*mode) : "none"; }

/// Construction rejects a present mode that is not an enumerator, after the frames in flight and before the window.
inline void reject_unknown_mode() {
    const auto construct = [](PresentMode mode, std::uint32_t frames) {
        anima::RendererOptions options;
        options.present_mode = mode;
        options.frames_in_flight = frames;
        anima::VulkanRenderer renderer(nullptr, options);
    };
    rejects<std::invalid_argument>([&] { construct(unknown_mode, 1); }, unknown_message);
    for (const auto mode : all_modes)
        rejects<std::invalid_argument>([&] { construct(mode, 1); }, "Renderer requires an SDL window");
    rejects<std::invalid_argument>([&] { construct(unknown_mode, 3); }, "Frames in flight must be 1 or 2");
}

// The renderer's counters and the check's own at the start of a span of draws, which Run::since() measures.
struct Mark {
    anima::RenderStats stats;
    std::uint32_t resize_requests{}, missed_draws{};
};
// What a span of draws changed.
struct Span {
    std::uint64_t presents{}, present_waits{}, present_wait_timeouts{};
    std::uint32_t swapchains{};
    // Window events that requested a resize and draws that presented nothing, as when the swapchain was out of date;
    // either can recreate the swapchain without a request.
    std::uint32_t disturbances{};
};

// One renderer of the check, drawing as an application's loop does: wait_for_frame(), then events, then draw().
class Run {
  public:
    Run(SDL_Window *window, anima::RendererOptions options)
        : window_(window), scene_(std::make_shared<anima::Scene>()), started_(std::chrono::steady_clock::now()),
          frames_in_flight_(options.frames_in_flight) {
        (void)scene_->add(anima::Mesh::compile(*resource_test::fixture()));
        options.validation = true;
        options.scenes = {scene_};
        renderer_.emplace(window, options);
        view();
    }
    anima::VulkanRenderer &renderer() { return *renderer_; }
    [[nodiscard]] std::uint32_t frames_in_flight() const noexcept { return frames_in_flight_; }
    /// Draws until @p count frames are presented and returns the profile of each presenting draw().
    std::vector<anima::FrameProfile> present(int count) {
        std::vector<anima::FrameProfile> profiles;
        while (int(profiles.size()) < count) {
            require(std::chrono::steady_clock::now() - started_ < gpu_check::watchdog, "Present mode watchdog expired");
            renderer_->wait_for_frame();
            SDL_Event event{};
            while (SDL_PollEvent(&event)) {
                require(event.type != SDL_EVENT_QUIT && event.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED,
                        "Present mode check interrupted");
                if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                    renderer_->request_resize();
                    ++resize_requests_;
                    view();
                }
            }
            if (renderer_->draw())
                profiles.push_back(renderer_->frame_profile());
            else {
                ++missed_draws_;
                SDL_Delay(5);
            }
        }
        return profiles;
    }
    /// Reads back the next presented frame.
    gpu_check::Image capture() {
        renderer_->request_capture();
        (void)present(1);
        return gpu_check::take(*renderer_);
    }
    /// The counters at the start of a span of draws.
    [[nodiscard]] Mark mark() const { return {renderer_->stats(), resize_requests_, missed_draws_}; }
    /// What the draws since @p start changed.
    [[nodiscard]] Span since(const Mark &start) const {
        const auto now = renderer_->stats();
        return {now.presented_frames - start.stats.presented_frames, now.present_waits - start.stats.present_waits,
                now.present_wait_timeouts - start.stats.present_wait_timeouts,
                now.swapchain_generations - start.stats.swapchain_generations,
                resize_requests_ - start.resize_requests + missed_draws_ - start.missed_draws};
    }
    /// Points the view at the scene through the window's current aspect.
    void view() {
        int width = 0, height = 0;
        require(SDL_GetWindowSizeInPixels(window_, &width, &height) && width > 0 && height > 0,
                "Present mode window has no drawable size");
        anima::OrbitCamera camera;
        const auto bounds = scene_->bounds();
        camera.frame(bounds.minimum, bounds.maximum);
        renderer_->set_view(camera.matrix(float(width) / float(height)));
    }

  private:
    SDL_Window *window_;
    std::shared_ptr<anima::Scene> scene_;
    std::chrono::steady_clock::time_point started_;
    std::uint32_t frames_in_flight_;
    std::optional<anima::VulkanRenderer> renderer_;
    // Resizes that window events requested, and draws that presented nothing.
    std::uint32_t resize_requests_{}, missed_draws_{};
};

inline double median(std::vector<double> values) {
    require(!values.empty(), "No value was measured");
    const auto middle = values.begin() + std::ptrdiff_t(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

// The present waits of a span of draws in one new swapchain, as VulkanRenderer::wait_for_frame describes them: one for
// each draw after the first frames_in_flight + 1, for the present of the frame frames_in_flight + 1 submissions
// earlier, where the renderer waits for presents and the swapchain's mode is a FIFO mode, and none otherwise.
inline std::uint64_t expected_waits(Run &run, int draws) {
    auto &renderer = run.renderer();
    const auto mode = renderer.present_mode();
    const bool queued = mode == PresentMode::fifo || mode == PresentMode::fifo_relaxed;
    const auto unwaited = std::int64_t(run.frames_in_flight()) + 1;
    if (!renderer.waits_for_presents() || !queued)
        return 0;
    return std::uint64_t(std::max<std::int64_t>(draws - unwaited, 0));
}

// Draws @p count frames in a new swapchain, which a resize request creates, and requires the present waits that
// expected_waits() names. A span that a window event or a draw that presented nothing disturbs, or that ends in another
// swapchain, is drawn again. The wait_for_frame() before the span makes any wait for a present of the previous
// swapchain, which the span's first draw would otherwise make. Returns the profiles of the span's draws and what it
// changed.
inline std::pair<std::vector<anima::FrameProfile>, Span> check_waits(Run &run, int count) {
    auto &renderer = run.renderer();
    for (;;) {
        renderer.wait_for_frame();
        const auto start = run.mark();
        renderer.request_resize();
        auto profiles = run.present(count);
        const auto span = run.since(start);
        if (span.disturbances != 0 || span.swapchains != 1)
            continue;
        const auto expected = expected_waits(run, count);
        require(span.present_waits == expected,
                std::to_string(count) + " draws in " + name(renderer.present_mode()) + " with " +
                    std::to_string(run.frames_in_flight()) + " frames in flight waited for " +
                    std::to_string(span.present_waits) + " presents, not " + std::to_string(expected));
        return {std::move(profiles), span};
    }
}

// What the requests drew and reported.
struct Summary {
    std::vector<std::pair<PresentMode, PresentMode>> chosen;
    bool waits_for_presents{}, measures_interval{};
    std::uint32_t frames_in_flight{};
    // Present waits of the draws in the first mode and in fifo, and the waits that timed out.
    std::uint64_t first_waits{}, fifo_waits{}, timeouts{};
    std::optional<double> fifo_interval_ms;
};

// Whether two snapshots of the renderer's counters agree.
inline bool same_counters(const anima::RenderStats &a, const anima::RenderStats &b) {
    return a.presented_frames == b.presented_frames && a.swapchain_generations == b.swapchain_generations &&
           a.validation_warnings == b.validation_warnings && a.validation_errors == b.validation_errors &&
           a.captured == b.captured && a.capture_count == b.capture_count &&
           a.scene_generations == b.scene_generations && a.present_waits == b.present_waits &&
           a.present_wait_timeouts == b.present_wait_timeouts;
}

// Requests immediate, then every other mode in turn, and checks what each reports and draws, how many swapchains the
// requests create, and which presents the draws in the first mode and in fifo wait for.
inline Summary check_requests(SDL_Window *window, gpu_check::Captures &images) {
    anima::RendererOptions options;
    options.log = gpu_check::log;
    options.present_mode = PresentMode::immediate;
    options.profile = true;
    Run run(window, options);
    auto &renderer = run.renderer();
    Summary summary;
    summary.waits_for_presents = renderer.waits_for_presents();
    summary.measures_interval = renderer.measures_present_interval();
    summary.frames_in_flight = run.frames_in_flight();
    require(!renderer.present_mode(), "A present mode was reported before the first swapchain");
    // The first swapchain is made copyable for this capture, so the capture recreates nothing.
    images.add("immediate", run.capture());
    auto previous = renderer.present_mode();
    require(previous == PresentMode::immediate || previous == PresentMode::fifo,
            "Requesting immediate presented in " + name(previous));
    summary.chosen.emplace_back(PresentMode::immediate, *previous);
    summary.first_waits = check_waits(run, settle_frames).second.present_waits;
    // Neither the current request nor a rejected one recreates the swapchain, which only a span of draws that nothing
    // else disturbs can show.
    for (;;) {
        const auto start = run.mark();
        renderer.set_present_mode(PresentMode::immediate);
        rejects<std::invalid_argument>([&] { renderer.set_present_mode(unknown_mode); }, unknown_message);
        (void)run.present(settle_frames);
        const auto span = run.since(start);
        if (span.disturbances != 0)
            continue;
        require(span.swapchains == 0, "Repeating the request and requesting an unknown mode created " +
                                          std::to_string(span.swapchains) + " swapchains");
        break;
    }
    for (const auto mode : {PresentMode::mailbox, PresentMode::fifo_relaxed, PresentMode::fifo}) {
        renderer.set_present_mode(mode);
        require(renderer.present_mode() == previous, "A present mode request changed the mode before a draw");
        // The request and the capture recreate the swapchain together, in the next draw(), and a window event or a
        // draw that presents nothing can recreate it once more.
        const auto start = run.mark();
        images.add(name(mode), run.capture());
        const auto span = run.since(start);
        require(span.swapchains >= 1 && span.swapchains <= 1 + span.disturbances,
                std::string("Requesting ") + name(mode) + " created " + std::to_string(span.swapchains) +
                    " swapchains, not 1 and at most " + std::to_string(span.disturbances) +
                    " more for window events and draws that presented nothing");
        const auto chosen = renderer.present_mode();
        require(chosen == mode || chosen == PresentMode::fifo,
                std::string("Requesting ") + name(mode) + " presented in " + name(chosen));
        summary.chosen.emplace_back(mode, *chosen);
        images.require_same("immediate", name(mode), std::string("Presenting in ") + name(mode) + " changed the image");
        previous = chosen;
    }
    require(previous == PresentMode::fifo,
            "Requesting fifo, which every surface offers, presented in " + name(previous));
    const auto fifo = check_waits(run, fifo_frames);
    summary.fifo_waits = fifo.second.present_waits;
    std::vector<double> intervals;
    for (const auto &profile : fifo.first)
        if (profile.present_interval_ms) {
            require(summary.measures_interval, "A present interval was reported without display timing");
            intervals.push_back(*profile.present_interval_ms);
        }
    if (summary.measures_interval) {
        require(double(intervals.size()) >= least_interval_share * fifo_frames,
                "Only " + std::to_string(intervals.size()) + " of " + std::to_string(fifo_frames) +
                    " fifo draws reported a present interval");
        summary.fifo_interval_ms = median(intervals);
    }
    const auto stats = renderer.shutdown();
    summary.timeouts = stats.present_wait_timeouts;
    require(!stats.validation_errors && !stats.validation_warnings,
            "Present mode requests drew with validation warnings or errors");
    require(same_counters(renderer.stats(), stats), "The counters after shutdown differ from what shutdown returned");
    rejects<std::logic_error>([&] { renderer.set_present_mode(PresentMode::fifo); }, "Renderer is shut down");
    require(renderer.present_mode() == PresentMode::fifo, "Shutdown changed the reported present mode");
    return summary;
}

// What the renderer with one frame in flight chose for immediate, and the present waits of its fifo draws.
struct ResizeSummary {
    PresentMode chosen{};
    std::uint64_t fifo_waits{};
};

// Draws fifo frames with one frame in flight, changes the mode to immediate, then resizes the window, and requires the
// resize to keep the mode and the image to follow the window. The fifo and immediate draws wait for the presents that
// check_waits() requires.
inline ResizeSummary check_resize(SDL_Window *window, gpu_check::Captures &images) {
    anima::RendererOptions options;
    options.log = gpu_check::log;
    options.frames_in_flight = 1;
    Run run(window, options);
    auto &renderer = run.renderer();
    ResizeSummary summary;
    summary.fifo_waits = check_waits(run, settle_frames).second.present_waits;
    require(renderer.present_mode() == PresentMode::fifo,
            "The default request presented in " + name(renderer.present_mode()));
    renderer.set_present_mode(PresentMode::immediate);
    images.add("before-resize", run.capture());
    const auto chosen = renderer.present_mode();
    require(chosen == PresentMode::immediate || chosen == PresentMode::fifo,
            "Requesting immediate presented in " + name(chosen));
    summary.chosen = *chosen;
    (void)check_waits(run, settle_frames);
    require(SDL_SetWindowSize(window, resized_width, resized_height), "Resize failed");
    require(SDL_SyncWindow(window), "Resize did not settle");
    renderer.request_resize();
    run.view();
    images.add("resized", run.capture());
    require(renderer.present_mode() == chosen,
            "Resizing changed the present mode from " + name(chosen) + " to " + name(renderer.present_mode()));
    (void)run.present(settle_frames);
    const auto stats = renderer.shutdown();
    // The swapchains of the fifo draws, the mode change, the immediate draws and the resize.
    constexpr std::uint32_t least_swapchains = 4;
    require(stats.swapchain_generations >= least_swapchains && !stats.validation_errors && !stats.validation_warnings,
            "A resize after a mode change created " + std::to_string(stats.swapchain_generations) +
                " swapchains, or drew with validation warnings or errors");
    images.require(!gpu_check::same_size(images["before-resize"], images["resized"]),
                   "The resized window kept its capture size", {"before-resize", "resized"});
    for (const auto *drawn : {"before-resize", "resized"})
        images.require_foreground(drawn, "The scene is not visible");
    return summary;
}

inline int run(int argc, char **argv) {
    require(argc == 3, "Usage: consumer --present-modes OUTPUT");
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    gpu_check::Video video;
    const auto window =
        gpu_check::window("Anima present mode verification", window_width, window_height, SDL_WINDOW_RESIZABLE);
    gpu_check::Captures images(argv[2]);
    const auto summary = check_requests(window.get(), images);
    const auto resized = check_resize(window.get(), images);
    std::ostringstream report;
    report << "PASS present modes:";
    for (const auto &[requested, chosen] : summary.chosen)
        report << ' ' << name(requested) << " presents in " << name(chosen) << ';';
    report << " each change recreated the swapchain and drew the same image, a repeated and a rejected request "
              "recreated none, a resize after a change to immediate kept "
           << name(resized.chosen) << ", ";
    if (summary.waits_for_presents)
        report << fifo_frames << " fifo draws with " << summary.frames_in_flight << " frames in flight waited for "
               << summary.fifo_waits << " presents, " << settle_frames << " with 1 for " << resized.fifo_waits
               << " and " << settle_frames << " in " << name(summary.chosen.front().second) << " for "
               << summary.first_waits << ", and " << summary.timeouts << " waits of the first renderer timed out, ";
    else
        report << "the device has no present waits, so no draw waited for one, ";
    if (summary.fifo_interval_ms)
        report << "fifo frames reported display times a median " << *summary.fifo_interval_ms << " ms apart";
    else
        report << "the device reports no display times";
    std::cout << report.str() << ", with clean validation\n";
    return 0;
}
} // namespace present_mode_test
