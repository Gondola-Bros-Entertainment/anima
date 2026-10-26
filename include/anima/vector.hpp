#pragma once
#include <anima/assets/asset.hpp>
#include <anima/core/math.hpp>
#include <anima/sprite.hpp>
#include <linea/Drawing.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// @file
/// Vector art rendered at the display's resolution: Linea drawings that a VectorAtlas rasterizes into one sprite sheet
/// at a scale, and again at each new scale.
///
/// Part of the optional `anima::vector` target (`ANIMA_BUILD_VECTOR=ON`, which requires `ANIMA_BUILD_ASSETS=ON`),
/// which renders without a display or GPU. It fetches [Linea](https://github.com/Gondola-Bros-Entertainment/linea), a
/// C++20 vector graphics library with no dependencies, from its main branch, and exposes its headers: an application
/// describes its art as `linea::Drawing` values, layers of filled and stroked paths that Linea rasterizes anti-aliased
/// at any size. The `anima::vector_ui` target registers the same drawings as UI images (anima/vector_ui.hpp).

namespace anima {
/// A drawing that a VectorAtlas renders, found by name, at a size in logical units.
struct VectorArt {
    /// The name that VectorAtlas::frame() finds the drawing by and set_ui_images() registers it under.
    ///
    /// A name is one or more segments joined by `/`, each made of ASCII letters, digits, `-`, `_` and `.`, and none of
    /// them `.` or `..`, as UiContext::set_image() requires. Names are case-sensitive.
    std::string name;
    /// The art. Its canvas, `drawing.width` by `drawing.height` units, each finite and positive, fills the rendered
    /// image, so give it the aspect ratio of #size unless the art should stretch.
    linea::Drawing drawing;
    /// Width and height in logical units, each finite and positive: at a scale of `s` pixels per unit, the drawing
    /// renders at about `size * s` pixels.
    Vec2 size{};
};

/// @p art's drawing rendered at @p scale pixels per logical unit, alone: `linea::render()` of VectorArt::drawing at
/// VectorArt::size times @p scale, a product that `double` holds exactly, each dimension rounded to the nearest whole
/// pixel, halves away from zero, and at least 1. VectorAtlas renders each frame this way.
///
/// The result is an ImageFormat::rgba8 Image whose first row is the drawing's top, with Linea's texels as they are:
/// sRGB-encoded color that is not multiplied by alpha, which is what a TextureEncoding::srgb texture and a UiImage both
/// take, so no conversion applies. Transparent texels hold black. VectorArt::name is not read.
///
/// Throws `std::invalid_argument` for a size that is not finite and positive ("Vector art size must be finite and
/// positive"), a canvas that is not ("Vector art canvas must be finite and positive"), a scale that is not ("Vector art
/// scale must be finite and positive"), and a dimension over max_image_edge pixels ("Vector art must render at most
/// 8192 pixels along each axis"). Calls may run concurrently on any thread; each reads only @p art.
[[nodiscard]] Image render_vector_art(const VectorArt &art, float scale);

/// Named drawings rendered at one scale into one sprite sheet, as a texture packer's atlas holds sprites, and rendered
/// again whenever the scale changes, so that the art stays sharp at the display's resolution: a game's sprites and
/// icons, drawn once as vectors.
///
/// Each drawing renders as render_vector_art() renders it, into the frame of sheet() whose index is the drawing's
/// position in the art the atlas was given, at every scale; frame() finds it by name. The frames are packed in rows,
/// tallest first, from the top left of an ImageFormat::rgba8 image of at most #max_sheet_edge texels a side. Packing
/// depends only on the frames' sizes, so the same art at the same scale gives the same layout on every platform.
///
/// The sheet's texture is TextureEncoding::srgb, filtered linearly without mips and clamped at its edges. Art rendered
/// at the display's resolution draws at about one texel per pixel, which needs no mips, and a mip chain would let
/// VulkanRenderer::set_texture_mip_skip() drop the very level that matches the display. A sprite drawn much smaller
/// than its frame therefore aliases: render the art at the scale it draws at instead.
///
/// Filtering without mips reads at most one texel past a frame's edge, so each frame lies inside a border of #padding
/// texels that no other frame's border overlaps: frames lie at least `2 * padding` texels apart and #padding from the
/// image's edges, and filtering never blends one frame into the next. As sprite.hpp requires, each transparent texel of
/// a frame and of its border holds the color of the nearest texel of the frame that is not transparent, nearest by
/// steps between edge neighbors, as bake_impostor() spreads color, so that filtering draws no dark fringes. Alpha stays
/// as Linea rendered it, 0 in the border. A frame with no texel that is not transparent, and every texel outside the
/// borders, stays transparent black.
///
/// Choose the scale as the framebuffer pixels that one logical unit covers where the art draws: a camera's pixels per
/// world unit for sprites sized in world units, or the SDL display scale for art sized in RCSS `dp`, as UI images are.
///
/// A SpriteSheet never changes, so set_scale() renders a new one, and sheet() returns it from then on. SpriteBatch
/// values hold the previous sheet, which keeps drawing the art at the previous resolution until the application
/// replaces them: after set_scale(), point each batch of the atlas's sprites at sheet() and pass the batches to
/// VulkanRenderer::set_sprites() again. The sprites themselves stay as they are, since each frame keeps its index and
/// a Sprite's size is in world units. The renderer's GPU cache releases the previous sheet once only it would hold
/// it.
///
/// The constructor and set_scale() render every drawing on the calling thread, in time proportional to their pixels.
/// Other members only read, so a const VectorAtlas may be read from several threads, while set_scale() needs the atlas
/// to itself.
class VectorAtlas {
  public:
    /// Texels of spread color around each frame of the sheet, transparent.
    static constexpr std::uint32_t padding = 1;
    /// Most texels along each side of the sheet.
    static constexpr std::uint32_t max_sheet_edge = 8192;

    /// Keeps @p art and renders it at @p scale pixels per logical unit, as set_scale() does.
    ///
    /// Throws `std::invalid_argument` for no drawings ("Vector atlas needs at least one drawing"), a name that
    /// VectorArt::name rejects ("Invalid vector art name: " and the name), a name that two drawings share ("Duplicate
    /// vector art name: " and the name), and a size or canvas that render_vector_art() rejects, before rendering any;
    /// and otherwise as set_scale() does.
    VectorAtlas(std::vector<VectorArt> art, float scale);
    /// Renders every drawing again at @p scale pixels per logical unit into a new sheet, keeping each drawing's frame
    /// index, unless @p scale equals scale(), when it does nothing.
    ///
    /// Throws `std::invalid_argument`, keeping the previous scale and sheet, as render_vector_art() does for the scale
    /// or a drawing, and when the frames and their borders do not fit one sheet of #max_sheet_edge texels a side
    /// ("Vector art must fit one sheet of 8192 by 8192 texels").
    void set_scale(float scale);
    /// Pixels per logical unit of the art in sheet().
    [[nodiscard]] float scale() const noexcept { return scale_; }
    /// The art rendered at scale(); a new sheet after each set_scale() that changes the scale.
    [[nodiscard]] const std::shared_ptr<const SpriteSheet> &sheet() const noexcept { return sheet_; }
    /// The index of the frame of the drawing named @p name, its position in the art the atlas was given, the same at
    /// every scale. Throws `std::out_of_range` when no drawing has that name ("Missing vector art: " and the name).
    [[nodiscard]] std::uint32_t frame(std::string_view name) const;
    /// The art the atlas was given, in frame order.
    [[nodiscard]] std::span<const VectorArt> art() const noexcept { return art_; }

  private:
    std::vector<VectorArt> art_;
    // Indices into art_, sorted by name, which frame() searches.
    std::vector<std::size_t> by_name_;
    float scale_{};
    std::shared_ptr<const SpriteSheet> sheet_;
};
} // namespace anima
