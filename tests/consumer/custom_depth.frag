#version 450
// The application's probe of opaque depth: writes the depth behind each fragment, as the renderer stored it, times 20 in
// red, and 0.5 in green where it is exactly 0.
#extension GL_GOOGLE_include_directive : require
#define ANIMA_OPAQUE_DEPTH
#include "anima/custom_material.glsl"
layout(location = 0) in vec3 worldPosition;
layout(location = 1) in vec2 texcoord;
layout(location = 0) out vec4 outColor;
void main() {
    float depth = texture(animaOpaqueDepth, gl_FragCoord.xy * animaFrame.viewport.zw).r;
    // Premultiplied, with alpha 1: the probe replaces what it covers.
    outColor = vec4(depth * 20.0, depth == 0.0 ? 0.5 : 0.0, 0.0, 1.0);
}
