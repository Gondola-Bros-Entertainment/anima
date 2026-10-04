#pragma once
#include <cstdint>

namespace anima::detail {
// The layouts of the custom material interface's frame block and draw push constants, which
// include/anima/custom_material.hpp documents and anima/custom_material.glsl declares. The validator
// (src/assets/custom_material.cpp) accepts a member that a shader declares only at one of these offsets, with the
// documented type, and the renderer (src/desktop/custom_renderer.inc) asserts that the frame block that it uploads and
// the push constants that it pushes place their members at them. The vertex attributes' locations are the built-in
// shaders' (shaders/shader_interface.h).

// The offset of each member of the frame block, AnimaFrame, in std140, and the block's size.
inline constexpr std::uint32_t custom_frame_view_projection = 0, custom_frame_inverse_view_projection = 64,
                               custom_frame_view_origin = 128, custom_frame_sun_direction = 144,
                               custom_frame_sun_irradiance = 160, custom_frame_fill_direction = 176,
                               custom_frame_fill_irradiance = 192, custom_frame_ambient_sky = 208,
                               custom_frame_ambient_ground = 224, custom_frame_ambient_specular = 240,
                               custom_frame_fog = 256, custom_frame_viewport = 272, custom_frame_time = 288,
                               custom_frame_fog_shape = 304, custom_frame_fog_sun = 320, custom_frame_bytes = 336;
// The offset of each member of the draw push constants, AnimaDraw, and their size.
inline constexpr std::uint32_t custom_draw_view_projection = 0, custom_draw_palette_offset = 64,
                               custom_draw_skinned = 68, custom_draw_object_offset = 72, custom_draw_placed = 76,
                               custom_draw_factor = 80, custom_draw_ranged = 96, custom_draw_bytes = 100;
} // namespace anima::detail
