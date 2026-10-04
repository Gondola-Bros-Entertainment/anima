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
// The values of anima::ToneMapping's reinhard, agx and pbr_neutral, as controls.y holds the tone mapping.
const float toneMappingReinhard = 1.0, toneMappingAgx = 2.0, toneMappingPbrNeutral = 3.0;

// Khronos PBR Neutral, from its specification (https://github.com/KhronosGroup/ToneMapping, PBR_Neutral/README.md):
// an offset that removes a dielectric's reflection at normal incidence, F90, then compression of a peak above Ks
// toward white, desaturating at the rate Kd.
vec3 pbrNeutral(vec3 color) {
    const float f90 = 0.04, ks = 0.8 - f90, kd = 0.15;
    float x = min(color.r, min(color.g, color.b));
    color -= x <= 2.0 * f90 ? x - x * x / (4.0 * f90) : f90;
    float p = max(color.r, max(color.g, color.b));
    if (p <= ks)
        return color;
    float pn = 1.0 - (1.0 - ks) * (1.0 - ks) / (p + 1.0 - 2.0 * ks);
    float g = 1.0 / (kd * (p - pn) + 1.0);
    return color * (pn / p) * g + vec3(pn * (1.0 - g));
}

// The HSV hue of @p color, from 0 to 1, and 0 for a neutral color.
float hue(vec3 color) {
    float high = max(color.r, max(color.g, color.b)), range = high - min(color.r, min(color.g, color.b));
    if (range <= 0.0)
        return 0.0;
    float sixths = high == color.r   ? (color.g - color.b) / range
                   : high == color.g ? 2.0 + (color.b - color.r) / range
                                     : 4.0 + (color.r - color.g) / range;
    return fract(sixths / 6.0);
}
// AgX as Blender's AgX view forms the image for an sRGB display (anima::ToneMapping::agx), in closed form. GLSL's mat3
// takes columns. agxInset is Blender's AgX inset matrix times the Rec.709 to Rec.2020 matrix, derived from the two
// standards' primaries and D65 white; agxOutset is the Rec.2020 to Rec.709 matrix times the outset, the inverse of
// Filament's AgXOutsetMatrixInv.
const mat3 agxInset = mat3(0.5448147465, 0.1404169485, 0.0888104196, 0.3737873984, 0.7541375546, 0.1788717564,
                           0.0813978551, 0.1054454970, 0.7323178240);
const mat3 agxOutset = mat3(1.9648874117, -0.2993133649, -0.1643527425, -0.8559884957, 1.3263979646, -0.2381839694,
                            -0.1088989160, -0.0270845997, 1.4025367120);
// The sigmoid's pivot, (10 / 16.5, 0.18^(1 / 2.4)), and slope there; and the scales of its toe and shoulder, which put
// it through (0, 0) and (1, 1) with powers of 1.5.
const float agxPivotX = 10.0 / 16.5, agxPivotY = 0.4894370896, agxSlope = 2.4;
const float agxToeScale = -0.5656756067, agxShoulderScale = 0.7151982863;
vec3 agx(vec3 color) {
    vec3 inset = agxInset * color;
    float before = hue(inset);
    // log2(0.18 * 2^-10) is -12.4739311883; the logarithm spans 16.5 stops from there.
    vec3 x = (log2(max(inset, vec3(1e-10))) + 12.4739311883) / 16.5;
    vec3 scale = mix(vec3(agxToeScale), vec3(agxShoulderScale), greaterThanEqual(x, vec3(agxPivotX)));
    vec3 u = agxSlope * (x - agxPivotX) / scale;
    vec3 y = scale * u * pow(vec3(1) + u * sqrt(u), vec3(-2.0 / 3.0)) + agxPivotY;
    vec3 curved = pow(max(y, vec3(0)), vec3(2.4));
    // Keep 40% of the curve's change in hue: 40% of the way from the hue before it to the hue after it, along the
    // shorter way around.
    float turn = hue(curved) - before;
    turn -= float(turn > 0.5) - float(turn < -0.5);
    float h = fract(before + 0.4 * turn);
    vec3 shares = clamp(abs(fract(vec3(h) + vec3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
    float low = min(curved.r, min(curved.g, curved.b)), high = max(curved.r, max(curved.g, curved.b));
    // Clamped per channel, where Blender's table keeps the luminance of a color outside Rec.709.
    return clamp(agxOutset * (low + (high - low) * shares), 0.0, 1.0);
}

// Linear scene color after exposure and tone mapping.
vec3 exposed(vec3 color) {
    color = clamp(color * environment.controls.x, vec3(0), vec3(animaMaximumHalfFloat));
    if (environment.controls.y == toneMappingReinhard)
        color = color / (vec3(1) + color);
    else if (environment.controls.y == toneMappingAgx)
        color = agx(color);
    else if (environment.controls.y == toneMappingPbrNeutral)
        color = pbrNeutral(color);
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
