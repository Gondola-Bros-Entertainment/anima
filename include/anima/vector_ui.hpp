#pragma once
#include <anima/ui/context.hpp>
#include <anima/vector.hpp>

/// @file
/// Vector art in UI documents: the drawings of a VectorAtlas registered as UI images at the UI's own scale.
///
/// Part of the optional `anima::vector_ui` target, which is built when `ANIMA_BUILD_VECTOR` and `ANIMA_BUILD_UI` are
/// both on.

namespace anima {
/// Registers each drawing of @p atlas with @p ui under its VectorArt::name, as UiContext::set_image() registers an
/// image, rendered by render_vector_art() at the UI's own scale: the ratio that RCSS `dp` lengths have to framebuffer
/// pixels in @p ui's RmlUi context, the SDL display scale, which each image takes as its UiImage::scale.
///
/// Documents draw a drawing with the source `memory:` and its name, as in `<img src="memory:icons/coin"/>`, and
/// measure it at about its VectorArt::size in `dp`: its pixels divided by the scale, each axis rounded to a whole
/// `dp`. An element that takes its size from the image thus draws about one image pixel on each framebuffer pixel. The
/// images do not depend on the atlas's own scale, which only its sheet uses.
///
/// A window that moves to a display of another scale changes the SDL display scale, and UiContext::process_event()
/// and UiContext::update() then give `dp` lengths the new one. Call this again after that, so that the images are
/// rendered at it: until then, each keeps the size it measures and its previous resolution.
///
/// Renders and checks every image before registering any, so a failure registers none. Throws `std::invalid_argument`
/// as render_vector_art() does when a drawing would render over 8,192 pixels along an axis at the UI's scale, or as
/// UiContext::set_image() does when one would measure over 8,192 `dp`, and `std::logic_error` after @p ui shuts down.
void set_ui_images(UiContext &ui, const VectorAtlas &atlas);
} // namespace anima
