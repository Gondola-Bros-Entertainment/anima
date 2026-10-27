#version 450
#extension GL_GOOGLE_include_directive : require
// A level of the bloom chain, downsampled from the image before it with Call of Duty: Advanced Warfare's 13-tap filter.
// The bright pass downsamples the scene target into the first level, keeping of each sample only the light that the
// threshold passes; the other levels downsample the level before them whole.
#include "bloom.glsl"
#include "../include/anima/fog.glsl"
layout(constant_id = ANIMA_SPEC_BLOOM_BRIGHT_PASS) const bool brightPass = false;

// The light of @p color that blooms: the color after exposure, capped as display conversion caps it, times the share
// of its brightness, its largest channel, that the threshold passes. Over the knee, from the threshold less the knee
// width to the threshold plus it, that share rises along a parabola from 0 to meet the hard threshold's.
vec3 bright(vec3 color) {
    color = clamp(color * bloomPass.exposure, vec3(0), vec3(animaMaximumHalfFloat));
    float brightness = max(color.r, max(color.g, color.b));
    if (brightness <= 0.0)
        return vec3(0);
    float knee = bloomPass.knee, soft = 0.0;
    if (knee > 0.0) {
        soft = clamp(brightness - bloomPass.threshold + knee, 0.0, 2.0 * knee);
        soft = soft * soft / (4.0 * knee);
    }
    return color * (max(soft, brightness - bloomPass.threshold) / brightness);
}
// The bilinear sample of source @p offset of its texels from this pixel's center, which lies on a corner between its
// texels, so that the sample is the mean of a 2 by 2 block of them; in the bright pass, only its light that blooms.
vec3 tap(vec2 offset) {
    vec3 color = texture(source, bloomCoordinates() + offset * bloomPass.texel).rgb;
    return brightPass ? bright(color) : color;
}
void main() {
    // Thirteen samples in rows from the top left, which form five overlapping groups of four: the central group d e i
    // j, weighted 1/2, and the groups a b f g, b c g h, f g k l and g h l m, weighted 1/8 each. Each sample's weight is
    // the sum of its groups' over 4.
    vec3 a = tap(vec2(-2, -2)), b = tap(vec2(0, -2)), c = tap(vec2(2, -2));
    vec3 d = tap(vec2(-1, -1)), e = tap(vec2(1, -1));
    vec3 f = tap(vec2(-2, 0)), g = tap(vec2(0, 0)), h = tap(vec2(2, 0));
    vec3 i = tap(vec2(-1, 1)), j = tap(vec2(1, 1));
    vec3 k = tap(vec2(-2, 2)), l = tap(vec2(0, 2)), m = tap(vec2(2, 2));
    const float central = 0.5 / 4.0, outer = 0.125 / 4.0;
    vec3 sum = central * (d + e + i + j) + 4.0 * outer * g + 2.0 * outer * (b + f + h + l) + outer * (a + c + k + m);
    outColor = vec4(sum, 0.0);
}
