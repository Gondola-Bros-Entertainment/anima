#include <anima/assets/preview.hpp>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace anima {
void Playback::select(const Animation &animation, const ClipMetadata &metadata, bool play) {
    if (animation.name != metadata.name || !std::isfinite(animation.duration) || animation.duration <= 0)
        throw std::invalid_argument("Playback clip/metadata mismatch");
    for (const auto &event : metadata.events)
        if (!std::isfinite(event.time) || event.time < 0 || event.time > animation.duration)
            throw std::invalid_argument("Preview event outside animation duration");
    animation_ = &animation;
    metadata_ = metadata;
    restart();
    playing_ = play;
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
    elapsed_ = metadata_.loop ? std::fmod(time, animation_->duration) : std::min(time, animation_->duration);
    finished_ = !metadata_.loop && elapsed_ == animation_->duration;
    fresh_ = false;
    if (finished_)
        playing_ = false;
}
double Playback::time() const noexcept {
    if (!animation_)
        return 0;
    return metadata_.loop ? std::fmod(elapsed_, animation_->duration) : elapsed_;
}
std::vector<ClipEvent> Playback::advance(double elapsed) {
    if (!std::isfinite(elapsed) || elapsed < 0)
        throw std::invalid_argument("Playback elapsed time must be finite and nonnegative");
    std::vector<ClipEvent> events;
    if (!playing_ || !animation_ || elapsed == 0)
        return events;
    const auto duration = animation_->duration;
    // A looping step crosses every event once per loop; a clip that does not loop stops at its end.
    constexpr unsigned maximum_loops_per_step = 10'000;
    if (metadata_.loop && elapsed / duration > maximum_loops_per_step)
        throw std::invalid_argument("Playback step exceeds " + std::to_string(maximum_loops_per_step) +
                                    " loops; split large offline advances");
    const auto from = elapsed_, to = metadata_.loop ? from + elapsed : std::min(from + elapsed, duration);
    if (!std::isfinite(to))
        throw std::runtime_error("Playback timeline overflow");
    std::vector<std::pair<double, ClipEvent>> crossed;
    for (const auto &event : metadata_.events) {
        if (fresh_ && event.time == 0)
            crossed.emplace_back(0, event);
        if (metadata_.loop) {
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
    elapsed_ = metadata_.loop ? std::fmod(to, duration) : to;
    fresh_ = false;
    if (!metadata_.loop && to == duration) {
        playing_ = false;
        finished_ = true;
    }
    return events;
}
namespace {
// compatible_skin's documented bound on inverse-bind and rest world matrix differences.
constexpr float skin_match_tolerance = 1e-4F;
float difference(const Mat4 &a, const Mat4 &b) {
    float d = 0;
    for (unsigned i = 0; i < 16; ++i)
        d = std::max(d, std::abs(a[i] - b[i]));
    return d;
}
std::string ancestors(const Asset &asset, std::size_t node) {
    std::string result;
    for (auto parent = asset.nodes.at(node).parent; parent >= 0; parent = asset.nodes.at(parent).parent)
        result += "/" + asset.nodes.at(parent).name;
    return result;
}
} // namespace
std::vector<std::pair<std::size_t, std::size_t>> compatible_skin(const Asset &body, const Asset &fitted) {
    if (body.skins.size() != 1 || fitted.skins.size() != 1)
        throw std::runtime_error("Fitted skin check requires one skin in the body and in the fitted model");
    const auto &body_skin = body.skins[0];
    const auto &fitted_skin = fitted.skins[0];
    if (body_skin.joints.size() != fitted_skin.joints.size())
        throw std::runtime_error("Fitted joint count differs from the body; export the fitted model against its rig");
    std::map<std::string, std::size_t> body_names;
    std::set<std::string> fitted_names;
    for (std::size_t i = 0; i < body_skin.joints.size(); ++i)
        if (!body_names.emplace(body.nodes.at(body_skin.joints[i]).name, i).second)
            throw std::runtime_error("Duplicate body joint name");
    const auto body_pose = sample_pose(body), fitted_pose = sample_pose(fitted);
    std::vector<std::pair<std::size_t, std::size_t>> mapping;
    for (std::size_t i = 0; i < fitted_skin.joints.size(); ++i) {
        const auto node = fitted_skin.joints[i];
        const auto &name = fitted.nodes.at(node).name;
        if (!fitted_names.insert(name).second)
            throw std::runtime_error("Duplicate fitted joint: " + name);
        const auto found = body_names.find(name);
        if (found == body_names.end())
            throw std::runtime_error("Fitted joint missing from the body: " + name);
        const auto body_node = body_skin.joints[found->second];
        if (ancestors(body, body_node) != ancestors(fitted, node))
            throw std::runtime_error("Fitted hierarchy mismatch at " + name + "; export against the body's rig");
        const auto bind_error = difference(body_skin.inverse_bind.at(found->second), fitted_skin.inverse_bind.at(i));
        if (bind_error > skin_match_tolerance)
            throw std::runtime_error("Fitted inverse-bind mismatch at " + name + " (max error " +
                                     std::to_string(bind_error) + "); export the fitted model for this body");
        if (difference(body_pose.world.at(body_node), fitted_pose.world.at(node)) > skin_match_tolerance)
            throw std::runtime_error("Fitted rest-pose mismatch at " + name + "; use the body's rest transforms");
        mapping.emplace_back(node, body_node);
    }
    for (const auto &primitive : fitted.primitives)
        if (primitive.skin != 0)
            throw std::runtime_error("Fitted model contains an unskinned mesh; bind it to the body's rig");
    return mapping;
}
AssetPreview::AssetPreview(const std::filesystem::path &manifest) : manifest_(read_manifest(manifest)) {
    model_ = load_asset(manifest_.directory / manifest_.model);
    validate_manifest(manifest_, *model_);
    animator_ = scene_->create(manifest_.asset_id, Mesh::compile(*model_)).add_component<Animator>(model_);
    if (manifest_.clips.empty())
        bind_pose();
    else
        select(manifest_.clips.front().name);
}
void AssetPreview::select(const std::string &clip, bool play) {
    const auto found = std::find_if(manifest_.clips.begin(), manifest_.clips.end(),
                                    [&](const auto &value) { return value.name == clip; });
    if (found == manifest_.clips.end())
        throw std::out_of_range("Clip has no manifest playback policy: " + clip);
    animator_->select(*found, play);
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
