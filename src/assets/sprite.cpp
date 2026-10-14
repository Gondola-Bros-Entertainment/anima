#include "surface_validation.hpp"
#include <anima/sprite.hpp>

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace anima {
namespace {
void require(bool condition, const char *message) {
    if (!condition)
        throw std::invalid_argument(message);
}
bool finite(Vec2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }
bool finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
// Whether @p length texels from @p first stay within @p edge texels, without overflowing.
bool inside(std::uint32_t first, std::uint32_t length, std::uint32_t edge) {
    return length > 0 && std::uint64_t{first} + length <= edge;
}
} // namespace

SpriteSheet::SpriteSheet(SpriteSheetDefinition definition) : definition_(std::move(definition)) {
    const auto &texture = definition_.texture;
    detail::validate_surfaces({}, std::span(&texture, 1), detail::Texels::required);
    require(!definition_.frames.empty(), "Sprite sheet needs at least one frame");
    for (const auto &frame : definition_.frames)
        require(inside(frame.x, frame.width, texture.image->width) &&
                    inside(frame.y, frame.height, texture.image->height),
                "Sprite frame must lie inside its sheet's image");
}

void validate_sprites(std::span<const SpriteBatch> batches) {
    for (const auto &batch : batches) {
        require(batch.sheet != nullptr, "Sprite batch has no sheet");
        require(batch.blend == SpriteBlend::blended || batch.blend == SpriteBlend::additive, "Unknown sprite blend");
        const auto frames = batch.sheet->definition().frames.size();
        for (const auto &sprite : batch.sprites) {
            require(finite(sprite.position) && finite(sprite.size) && finite(sprite.pivot) &&
                        std::isfinite(sprite.rotation),
                    "Sprite placement must be finite");
            require(sprite.frame < frames, "Sprite frame is not in its sheet");
            require(finite(sprite.color) && sprite.color.x >= 0 && sprite.color.y >= 0 && sprite.color.z >= 0,
                    "Sprite color must be finite and non-negative");
            require(sprite.alpha >= 0 && sprite.alpha <= 1, "Sprite alpha must be in [0, 1]");
        }
    }
}
} // namespace anima
