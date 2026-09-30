#pragma once
#include <anima/assets/asset.hpp>
#include <mutex>

namespace anima {
class Mesh;
}
namespace anima::detail {
// The source images of a Mesh or CustomMaterial compiled with TexelRetention::until_upload, one per texture: held
// until release(), then only weakly. Every member may run on any thread.
class TexelHold {
  public:
    // Holds @p sources; @p released_message is what images() throws once one of them is gone.
    TexelHold(std::vector<std::shared_ptr<const Image>> sources, const char *released_message);
    // A hold of the same images in the same state.
    TexelHold(const TexelHold &other);
    TexelHold &operator=(const TexelHold &) = delete;
    // The held images, or those still alive elsewhere once released. Throws std::logic_error when one is gone.
    [[nodiscard]] std::vector<std::shared_ptr<const Image>> images() const;
    // Drops the hold, keeping weak references. Idempotent.
    void release() noexcept;

  private:
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<const Image>> held_;
    std::vector<std::weak_ptr<const Image>> images_;
    const char *released_message_;
};
// Replaces the image of each of @p textures with one that has its dimensions and no texels, one per distinct
// image, so that textures sharing an image still share one, and returns a hold of the originals in texture order.
[[nodiscard]] TexelHoldPtr hold_texels(std::vector<Texture> &textures, const char *released_message);
// @p source for a @p mesh compiled from it with TexelRetention::keep. For TexelRetention::until_upload, a copy whose
// textures are @p mesh's, which have no texels, so that keeping it beside @p mesh does not keep the images that
// @p mesh lets go.
[[nodiscard]] std::shared_ptr<const Asset> without_texels(std::shared_ptr<const Asset> source, const Mesh &mesh);
} // namespace anima::detail
