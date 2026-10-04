#include "texel_hold.hpp"
#include <anima/mesh.hpp>
#include <map>
#include <stdexcept>
#include <utility>

namespace anima::detail {
TexelHold::TexelHold(std::vector<std::shared_ptr<const Image>> sources, const char *released_message)
    : held_(std::move(sources)), images_(held_.begin(), held_.end()), released_message_(released_message) {}
TexelHold::TexelHold(const TexelHold &other) : released_message_(other.released_message_) {
    const std::scoped_lock lock(other.mutex_);
    held_ = other.held_;
    images_ = other.images_;
}
std::vector<std::shared_ptr<const Image>> TexelHold::images() const {
    const std::scoped_lock lock(mutex_);
    if (!held_.empty())
        return held_;
    std::vector<std::shared_ptr<const Image>> result;
    result.reserve(images_.size());
    for (const auto &image : images_) {
        auto alive = image.lock();
        if (!alive)
            throw std::logic_error(released_message_);
        result.push_back(std::move(alive));
    }
    return result;
}
void TexelHold::release() noexcept {
    // Destroy the last references outside the lock, so that freeing large images does not block readers.
    std::vector<std::shared_ptr<const Image>> released;
    {
        const std::scoped_lock lock(mutex_);
        released.swap(held_);
    }
}
TexelHoldPtr hold_texels(std::vector<Texture> &textures, const char *released_message) {
    std::vector<std::shared_ptr<const Image>> sources;
    sources.reserve(textures.size());
    // Keys hold the source pointers, which order by identity.
    std::map<std::shared_ptr<const Image>, std::shared_ptr<const Image>> descriptions;
    for (auto &texture : textures) {
        auto &description = descriptions[texture.image];
        if (!description) {
            const auto &source = *texture.image;
            description =
                std::make_shared<const Image>(Image{source.width, source.height, {}, source.format, source.levels, {}});
        }
        sources.push_back(std::exchange(texture.image, description));
    }
    return TexelHoldPtr(std::make_unique<TexelHold>(std::move(sources), released_message));
}

std::shared_ptr<const Asset> without_texels(std::shared_ptr<const Asset> source, const Mesh &mesh) {
    if (mesh.texel_retention() == TexelRetention::keep)
        return source;
    auto copy = std::make_shared<Asset>(*source);
    copy->textures = mesh.description()->textures;
    return copy;
}

TexelHoldPtr::TexelHoldPtr() noexcept = default;
TexelHoldPtr::TexelHoldPtr(std::unique_ptr<TexelHold> hold) noexcept : hold_(std::move(hold)) {}
TexelHoldPtr::TexelHoldPtr(const TexelHoldPtr &other)
    : hold_(other.hold_ ? std::make_unique<TexelHold>(*other.hold_) : nullptr) {}
TexelHoldPtr &TexelHoldPtr::operator=(const TexelHoldPtr &other) {
    if (this != &other)
        hold_ = other.hold_ ? std::make_unique<TexelHold>(*other.hold_) : nullptr;
    return *this;
}
TexelHoldPtr::TexelHoldPtr(TexelHoldPtr &&other) noexcept = default;
TexelHoldPtr &TexelHoldPtr::operator=(TexelHoldPtr &&other) noexcept = default;
TexelHoldPtr::~TexelHoldPtr() = default;
} // namespace anima::detail
