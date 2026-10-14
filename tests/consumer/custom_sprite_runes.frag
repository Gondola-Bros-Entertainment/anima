#version 450
// The application's sprite shader: a ring of runes that turn with the caller's time, drawn in the sprite's frame. Each
// sprite's parameters give the ring's radius and half width, as fractions of half the sprite's size, how fast the runes
// turn, in radians per second, and their phase.
#extension GL_GOOGLE_include_directive : require
#include "anima/sprite.glsl"
layout(location = 0) out vec4 outColor;
// Runes around the ring.
const float runes = 6.0;
void main() {
    vec4 ring = animaSpriteParameters;
    vec2 point = animaSpriteUv * 2.0 - 1.0;
    float band = clamp(1.0 - abs(length(point) - ring.x) / ring.y, 0.0, 1.0);
    float angle = atan(point.y, point.x) + ring.z * animaFrame.time + ring.w;
    float glyph = 0.5 + 0.5 * cos(runes * angle);
    vec4 texel = texture(animaSpriteSheet, animaSheetUv);
    float alpha = texel.a * animaSpriteColor.a * band * glyph;
    outColor = vec4(texel.rgb * animaSpriteColor.rgb * alpha, alpha);
}
