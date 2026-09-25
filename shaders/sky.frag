#version 450
#extension GL_GOOGLE_include_directive : require
layout(location = 0) in vec2 screen;
layout(location = 0) out vec4 outColor;
#include "environment.glsl"
#include "sky.glsl"
void main() {
    vec4 position = environment.inverseView * vec4(screen, 0.5, 1.0);
    vec3 ray = environment.viewOrigin.w > 0.5 ? normalize(position.xyz / position.w - environment.viewOrigin.xyz)
                                              : -environment.viewOrigin.xyz;
    vec3 color = skyRadiance(ray);
    outColor = vec4(clamp(color, vec3(0), vec3(65504)), 1.0);
}
