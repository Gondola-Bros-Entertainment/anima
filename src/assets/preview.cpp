#include <anima/assets/preview.hpp>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace anima {
namespace {
// Whether playback wraps at the end of @p animation. A looping clip of zero duration holds time 0 instead, since
// wrapping it would divide by zero.
bool wraps(const Animation &animation, const ClipMetadata &metadata) noexcept {
    return metadata.loop && animation.duration > 0;
}
} // namespace
void Playback::select(const Animation &animation, const ClipMetadata &metadata, PlaybackStart start) {
    if (animation.name != metadata.name)
        throw std::invalid_argument("Clip metadata names another clip: " + metadata.name);
    if (!std::isfinite(animation.duration) || animation.duration < 0)
        throw std::invalid_argument("Clip duration must be finite and nonnegative: " + animation.name);
    for (const auto &event : metadata.events)
        if (!std::isfinite(event.time) || event.time < 0 || event.time > animation.duration)
            throw std::invalid_argument("Preview event outside animation duration");
    animation_ = &animation;
    metadata_ = metadata;
    restart();
    playing_ = start == PlaybackStart::playing;
}
void Playback::restart() {
    if (!animation_)
        return;
    elapsed_ = 0;
    playing_ = true;
    finished_ = false;
    fresh_ = true;
}
void Playback::resume() {
    if (!animation_)
        return;
    if (finished_)
        restart();
    else
        playing_ = true;
}
void Playback::toggle() {
    if (playing_)
        pause();
    else
        resume();
}
void Playback::seek(double time) {
    if (!animation_ || !std::isfinite(time) || time < 0)
        throw std::invalid_argument("Invalid playback seek");
    elapsed_ =
        wraps(*animation_, metadata_) ? std::fmod(time, animation_->duration) : std::min(time, animation_->duration);
    finished_ = !metadata_.loop && elapsed_ == animation_->duration;
    fresh_ = false;
    if (finished_)
        playing_ = false;
}
double Playback::time() const noexcept {
    if (!animation_)
        return 0;
    return wraps(*animation_, metadata_) ? std::fmod(elapsed_, animation_->duration) : elapsed_;
}
std::vector<ClipEvent> Playback::advance(double elapsed) {
    if (!std::isfinite(elapsed) || elapsed < 0)
        throw std::invalid_argument("Playback elapsed time must be finite and nonnegative");
    std::vector<ClipEvent> events;
    if (!playing_ || !animation_ || elapsed == 0)
        return events;
    const auto duration = animation_->duration;
    // A looping step crosses every event once per loop; a clip that does not loop stops at its end. A looping clip of
    // zero duration holds time 0 instead of wrapping, so it crosses its events, all at 0, only after a fresh start.
    const bool wrapping = wraps(*animation_, metadata_);
    constexpr unsigned maximum_loops_per_step = 10'000;
    if (wrapping && elapsed / duration > maximum_loops_per_step)
        throw std::invalid_argument("Playback step exceeds " + std::to_string(maximum_loops_per_step) +
                                    " loops; split large offline advances");
    const auto from = elapsed_, to = wrapping ? from + elapsed : std::min(from + elapsed, duration);
    if (!std::isfinite(to))
        throw std::runtime_error("Playback timeline overflow");
    std::vector<std::pair<double, ClipEvent>> crossed;
    for (const auto &event : metadata_.events) {
        if (fresh_ && event.time == 0)
            crossed.emplace_back(0, event);
        if (wrapping) {
            const auto first = static_cast<long long>(std::floor((from - event.time) / duration)) + 1;
            const auto last = static_cast<long long>(std::floor((to - event.time) / duration));
            for (auto cycle = first; cycle <= last; ++cycle)
                crossed.emplace_back(event.time + double(cycle) * duration, event);
        } else if (event.time > from && event.time <= to)
            crossed.emplace_back(event.time, event);
    }
    std::stable_sort(crossed.begin(), crossed.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    for (const auto &event : crossed)
        events.push_back(event.second);
    // Discard whole loop counts after event crossing to retain fractional precision indefinitely.
    elapsed_ = wrapping ? std::fmod(to, duration) : to;
    fresh_ = false;
    if (!metadata_.loop && to == duration) {
        playing_ = false;
        finished_ = true;
    }
    return events;
}
AssetPreview::AssetPreview(const std::filesystem::path &manifest, TexelRetention texel_retention,
                           const StagingOptions &options)
    : manifest_(read_manifest(manifest)) {
    const auto model = load_asset(manifest_.directory / manifest_.model, options);
    validate_manifest(manifest_, *model);
    auto mesh = Mesh::compile(*model, texel_retention);
    auto motion = std::make_shared<Asset>();
    motion->nodes = model->nodes;
    motion->animations = model->animations;
    model_ = std::move(motion);
    animator_ = scene_->create(manifest_.asset_id, std::move(mesh)).add_component<Animator>(model_);
    if (manifest_.clips.empty())
        bind_pose();
    else
        select(manifest_.clips.front().name);
}
void AssetPreview::select(std::string_view clip, PlaybackStart start) {
    const auto found = std::find_if(manifest_.clips.begin(), manifest_.clips.end(),
                                    [&](const auto &value) { return value.name == clip; });
    if (found == manifest_.clips.end())
        throw std::out_of_range("Clip has no manifest playback policy: " + std::string(clip));
    animator_->select(*found, start);
    bind_ = false;
}
void AssetPreview::bind_pose() {
    animator_->bind_pose();
    bind_ = true;
}
void AssetPreview::toggle_play() {
    if (!playback().animation())
        return;
    if (bind_ || !playback().playing())
        animator_->resume();
    else
        animator_->pause();
    bind_ = false;
}
void AssetPreview::restart() {
    if (!playback().animation())
        return;
    animator_->restart();
    bind_ = false;
}
void AssetPreview::seek(double time) {
    animator_->seek(time);
    bind_ = false;
}
std::vector<ClipEvent> AssetPreview::advance(double elapsed) {
    scene_->update(elapsed);
    const auto events = animator_->events();
    return {events.begin(), events.end()};
}
std::string AssetPreview::status() const {
    std::ostringstream text;
    if (bind_)
        text << "Bind pose";
    else
        text << playback().animation()->name << ' ' << std::fixed << std::setprecision(2) << playback().time() << 's';
    text << " | "
         << (bind_                   ? "static"
             : playback().playing()  ? "playing"
             : playback().finished() ? "finished"
                                     : "paused");
    return text.str();
}
} // namespace anima
