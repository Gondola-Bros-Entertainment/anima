#version 450
// The application's water: light that crosses the water between its surface and the opaque surface behind it is
// absorbed with distance, and what is absorbed is replaced by the water's deep color.
#extension GL_GOOGLE_include_directive : require
#define ANIMA_OPAQUE_DEPTH
#define ANIMA_OPAQUE_COLOR
#include "anima/custom_material.glsl"
layout(location = 0) in vec3 worldPosition;
layout(location = 1) in vec2 texcoord;
layout(location = 0) out vec4 outColor;
layout(set = 2, binding = 0, std140) uniform Water {
    // Linear color of infinitely deep water.
    vec4 deep;
    // x: absorption per unit of distance through the water.
    vec4 absorption;
}
water;
void main() {
    vec2 screen = gl_FragCoord.xy * animaFrame.viewport.zw;
    vec3 behind = animaWorldPosition(gl_FragCoord.xy, texture(animaOpaqueDepth, screen).r);
    float transmittance = exp(-water.absorption.x * distance(worldPosition, behind));
    // Premultiplied, with alpha 1: the water replaces what it covers with its own blend of it.
    outColor = vec4(mix(water.deep.rgb, texture(animaOpaqueColor, screen).rgb, transmittance), 1.0);
}
