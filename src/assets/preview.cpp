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
    if (elapsed / duration > 10000)
        throw std::runtime_error("Playback step exceeds 10000 loops; split large offline advances");
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
std::vector<std::pair<std::size_t, std::size_t>> compatible_skin(const Asset &character, const Asset &equipment) {
    if (character.skins.size() != 1 || equipment.skins.size() != 1)
        throw std::runtime_error("Equipment preview requires one skin per character/item");
    const auto &base = character.skins[0];
    const auto &gear = equipment.skins[0];
    if (base.joints.size() != gear.joints.size())
        throw std::runtime_error(
            "Equipment joint count differs from the target body; export fitted equipment against its rig");
    std::map<std::string, std::size_t> base_names;
    std::set<std::string> gear_names;
    for (std::size_t i = 0; i < base.joints.size(); ++i)
        if (!base_names.emplace(character.nodes.at(base.joints[i]).name, i).second)
            throw std::runtime_error("Duplicate character joint name");
    const auto base_pose = sample_pose(character), gear_pose = sample_pose(equipment);
    std::vector<std::pair<std::size_t, std::size_t>> mapping;
    for (std::size_t i = 0; i < gear.joints.size(); ++i) {
        const auto node = gear.joints[i];
        const auto &name = equipment.nodes.at(node).name;
        if (!gear_names.insert(name).second)
            throw std::runtime_error("Duplicate equipment joint: " + name);
        const auto found = base_names.find(name);
        if (found == base_names.end())
            throw std::runtime_error("Equipment joint missing from target body: " + name);
        const auto base_node = base.joints[found->second];
        if (ancestors(character, base_node) != ancestors(equipment, node))
            throw std::runtime_error("Equipment hierarchy mismatch at " + name +
                                     "; export against the target body's rig");
        const auto bind_error = difference(base.inverse_bind.at(found->second), gear.inverse_bind.at(i));
        if (bind_error > skin_match_tolerance)
            throw std::runtime_error("Equipment inverse-bind mismatch at " + name + " (max error " +
                                     std::to_string(bind_error) + "); refit/export for this body profile");
        if (difference(base_pose.world.at(base_node), gear_pose.world.at(node)) > skin_match_tolerance)
            throw std::runtime_error("Equipment rest-pose mismatch at " + name +
                                     "; use the target body's rest transforms");
        mapping.emplace_back(node, base_node);
    }
    for (const auto &primitive : equipment.primitives)
        if (primitive.skin != 0)
            throw std::runtime_error("Chest equipment contains an unskinned mesh; bind it to the target rig");
    return mapping;
}
AssetPreview::AssetPreview(const std::filesystem::path &manifest) : manifest_(read_manifest(manifest)) {
    character_ = load_asset(manifest_.directory / manifest_.model);
    validate_manifest(manifest_, *character_);
    animator_ = scene_->create(manifest_.asset_id, Mesh::compile(*character_)).add_component<Animator>(character_);
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
