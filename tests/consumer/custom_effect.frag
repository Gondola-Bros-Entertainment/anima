#version 450
// The application's effect: a textured color whose intensity pulses with the caller's time.
#extension GL_GOOGLE_include_directive : require
#include "anima/custom_material.glsl"
layout(location = 0) in vec3 worldPosition;
layout(location = 1) in vec2 texcoord;
layout(location = 0) out vec4 outColor;
layout(set = 2, binding = 0, std140) uniform Effect {
    // Linear color, and in w the coverage that a blended material composites with.
    vec4 color;
    // Intensity x + y * sin(z * time).
    vec4 pulse;
}
effect;
layout(set = 2, binding = 1) uniform sampler2D pattern;
void main() {
    float intensity = effect.pulse.x + effect.pulse.y * sin(effect.pulse.z * animaFrame.time);
    vec3 color = effect.color.rgb * texture(pattern, texcoord).rgb * animaDraw.factor.rgb * intensity;
    outColor = vec4(color * effect.color.w, effect.color.w);
}
