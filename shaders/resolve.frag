#version 450
#extension GL_GOOGLE_include_directive : require
layout(location = 0) in vec2 screen;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D sceneColor;
layout(constant_id = 0) const bool encodeSrgb = false;
#include "environment.glsl"
const float maximumHalfFloat = 65504.0;
vec3 displayColor(vec3 color) {
    color = clamp(color * environment.controls.x, vec3(0), vec3(maximumHalfFloat));
    if (environment.controls.y > 0.5)
        color = color / (vec3(1) + color);
    if (encodeSrgb)
        color = mix(1.055 * pow(color, vec3(1.0 / 2.4)) - 0.055, 12.92 * color, lessThanEqual(color, vec3(0.0031308)));
    return color;
}
void main() { outColor = vec4(displayColor(texture(sceneColor, screen * 0.5 + 0.5).rgb), 1); }
