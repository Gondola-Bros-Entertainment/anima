// GLSL declarations of the sprite shader interface that anima/sprite.hpp documents. Include it in a sprite shader, a
// fragment shader compiled for Vulkan 1.1, for example with `glslc --target-env=vulkan1.1 -I <anima>/include`.
//
// It declares what the renderer's sprite vertex shader passes for each sprite, the batch's sheet, and what frame.glsl,
// which it includes, declares: the frame block (animaFrame) that custom materials read too, its helpers, and the
// declarations of fog.glsl and visibility.glsl. Write the premultiplied color at location 0.
#ifndef ANIMA_SPRITE_GLSL
#define ANIMA_SPRITE_GLSL

#include "frame.glsl"

// The texture coordinates in the sheet of the point being shaded, which sample the sprite's frame there.
layout(location = 0) in vec2 animaSheetUv;
// Sprite::color, with Sprite::alpha in w.
layout(location = 1) in vec4 animaSpriteColor;
// The point's coordinates across the sprite, from (0, 0) at its frame's first column and first row to (1, 1) at the
// opposite corner, as the frame's texture coordinates run.
layout(location = 2) in vec2 animaSpriteUv;
// Sprite::parameters, the same at every point of the sprite.
layout(location = 3) flat in vec4 animaSpriteParameters;

// The batch's sheet, sampled as its texture's Sampler says.
layout(set = 1, binding = 0) uniform sampler2D animaSpriteSheet;

#endif
