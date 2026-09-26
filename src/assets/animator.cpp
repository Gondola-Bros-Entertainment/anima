#include "mesh_limits.hpp"
#include <anima/animation.hpp>

namespace anima {
bool Mesh::accepts_animation_source(const Asset &source) const {
    if (source.nodes.size() != nodes_.size())
        return false;
    for (std::size_t i = 0; i < nodes_.size(); ++i)
        if (nodes_[i].first != source.nodes[i].name || nodes_[i].second != source.nodes[i].parent)
            return false;
    const auto rest = sample_pose(source);
    for (std::size_t i = 0; i < rest.world.size(); ++i)
        for (std::size_t k = 0; k < 16; ++k)
            if (!std::isfinite(rest.world[i][k]) ||
                std::abs(rest.world[i][k] - rest_.world[i][k]) > mesh_limits::rest_pose_tolerance)
                return false;
    return true;
}
Animator::Animator(GameObject object, std::shared_ptr<const Asset> source)
    : object_(std::move(object)), mesh_(object_.renderer().mesh()), source_(std::move(source)) {
    if (!source_ || !mesh_->accepts_animation_source(*source_))
        throw std::invalid_argument("Animator source does not match the object's mesh hierarchy and bind");
    publish(playback_, true);
}
void Animator::publish(const Playback &playback, bool bind) {
    if (object_.renderer().mesh() != mesh_)
        throw std::logic_error("Animator mesh was replaced; bind a new Animator explicitly");
    auto pose = sample_pose(*source_, bind ? nullptr : playback.animation(), playback.time());
    object_.renderer().set_pose(pose);
    pose_ = std::move(pose);
}
void Animator::play(std::string_view clip, bool loop) { select({std::string(clip), loop, {}, {}}); }
void Animator::select(const ClipMetadata &clip, bool play) {
    auto next = playback_;
    next.select(find_animation(*source_, clip.name), clip, play);
    publish(next);
    playback_ = std::move(next);
    bind_ = false;
}
void Animator::bind_pose() {
    publish(playback_, true);
    playback_.pause();
    bind_ = true;
}
void Animator::pause() { playback_.pause(); }
void Animator::resume() {
    auto next = playback_;
    next.resume();
    publish(next);
    playback_ = std::move(next);
    bind_ = !playback_.animation();
}
void Animator::restart() {
    auto next = playback_;
    next.restart();
    publish(next);
    playback_ = std::move(next);
    bind_ = !playback_.animation();
}
void Animator::seek(double seconds) {
    auto next = playback_;
    next.seek(seconds);
    publish(next);
    playback_ = std::move(next);
    bind_ = false;
}
std::vector<ClipEvent> Animator::update(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0)
        throw std::invalid_argument("Invalid Animator time step");
    // Validate even while paused, so expired/replaced components cannot hide.
    if (object_.renderer().mesh() != mesh_)
        throw std::logic_error("Animator mesh was replaced");
    if (bind_ || !playback_.playing())
        return {};
    auto next = playback_;
    auto events = next.advance(seconds);
    publish(next);
    playback_ = std::move(next);
    return events;
}
} // namespace anima
