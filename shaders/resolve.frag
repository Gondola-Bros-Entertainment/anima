#version 450
#extension GL_GOOGLE_include_directive : require
layout(location = 0) in vec2 screen;
layout(location = 0) out vec4 outColor;
#include "shader_interface.h"
layout(set = ANIMA_SET_SCENE_INPUT, binding = ANIMA_SCENE_INPUT_COLOR) uniform sampler2D sceneColor;
layout(constant_id = ANIMA_SPEC_ENCODE_SRGB) const bool encodeSrgb = false;
// Scene target pixels per window pixel along each axis: the render scale, as rounding and the device's limits leave it.
layout(push_constant) uniform Display { vec2 footprint; }
display;
#include "environment.glsl"
// anima::ToneMapping::reinhard's value, as controls.y holds the tone mapping.
const float toneMappingReinhard = 1.0;
// Linear scene color after exposure and tone mapping.
vec3 exposed(vec3 color) {
    color = clamp(color * environment.controls.x, vec3(0), vec3(animaMaximumHalfFloat));
    if (environment.controls.y == toneMappingReinhard)
        color = color / (vec3(1) + color);
    return color;
}
// The mean of the scene target pixels that the window pixel's area covers, each weighted by the area it covers and
// taken as the window shows it: exposed, then clamped to the display's range of 0 to 1. An edge against a surface
// brighter than white then blends in proportion to its coverage, where averaging radiance first would show white
// wherever the bright side covers enough of the area to outweigh the clamp.
vec3 supersampled() {
    vec2 low = floor(gl_FragCoord.xy) * display.footprint, high = low + display.footprint;
    ivec2 first = ivec2(floor(low)), last = min(ivec2(ceil(high)) - 1, textureSize(sceneColor, 0) - 1);
    vec3 sum = vec3(0);
    float total = 0.0;
    for (int y = first.y; y <= last.y; ++y) {
        float covered = min(high.y, float(y + 1)) - max(low.y, float(y));
        for (int x = first.x; x <= last.x; ++x) {
            float weight = covered * (min(high.x, float(x + 1)) - max(low.x, float(x)));
            sum += weight * min(exposed(texelFetch(sceneColor, ivec2(x, y), 0).rgb), vec3(1));
            total += weight;
        }
    }
    return sum / total;
}
void main() {
    // Up to the window's size the target is interpolated bilinearly, before exposure; beyond it each window pixel
    // filters its own area.
    vec3 color = any(greaterThan(display.footprint, vec2(1))) ? supersampled()
                                                              : exposed(texture(sceneColor, screen * 0.5 + 0.5).rgb);
    if (encodeSrgb)
        color = mix(1.055 * pow(color, vec3(1.0 / 2.4)) - 0.055, 12.92 * color, lessThanEqual(color, vec3(0.0031308)));
    outColor = vec4(color, 1);
}
