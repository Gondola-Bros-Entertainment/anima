#version 450
#extension GL_GOOGLE_include_directive : require
// The level after this one in the bloom chain, upsampled with a 3 by 3 tent filter of bilinear samples one of its
// texels apart and scaled by its weight, which the pipeline's blending adds to this level.
#include "bloom.glsl"

// The bilinear sample of source @p offset of its texels from this pixel's center.
vec3 tap(vec2 offset) { return texture(source, bloomCoordinates() + offset * bloomPass.texel).rgb; }
void main() {
    // Weights 1 2 1, 2 4 2 and 1 2 1, over 16.
    vec3 sum = 4.0 * tap(vec2(0, 0)) + 2.0 * (tap(vec2(-1, 0)) + tap(vec2(1, 0)) + tap(vec2(0, -1)) + tap(vec2(0, 1))) +
               tap(vec2(-1, -1)) + tap(vec2(1, -1)) + tap(vec2(-1, 1)) + tap(vec2(1, 1));
    outColor = vec4(sum * (bloomPass.weight / 16.0), 0.0);
}
