#include "environment_data.glsl"
#include "../include/anima/fog.glsl"
#include "shader_interface.h"
// False in the pipelines that draw uniform fog without sunlight, which then compile without the height fog's code:
// unused, it still costs the registers of its longest path in every fragment.
layout(constant_id = ANIMA_SPEC_HEIGHT_FOG) const bool environmentHeightFog = true;
// @p color at @p position as seen from the eye through the fog, in a perspective view with fog; otherwise @p color.
vec3 environmentFog(vec3 color, vec3 position, vec4 viewOrigin) {
    if (viewOrigin.w > 0.5 && environment.fog.w > 0.0) {
        if (environmentHeightFog)
            color = animaFog(color, viewOrigin.xyz, position, environment.fog, environment.fogShape,
                             environment.fogSun, environment.sunDirection.xyz);
        else
            color = mix(min(environment.fog.rgb, vec3(animaMaximumHalfFloat)), color,
                        exp(-environment.fog.w * length(viewOrigin.xyz - position)));
    }
    return color;
}
// True in the pipelines compiled for ShadowFilter::bilinear_2x2 (VulkanRenderer::set_shadow_filter), which then compile
// the 2x2 filter alone.
layout(constant_id = ANIMA_SPEC_BILINEAR_SHADOW_FILTER) const bool bilinearShadowFilter = false;
layout(set = ANIMA_SET_ENVIRONMENT, binding = ANIMA_ENVIRONMENT_CASCADE_DEPTH) uniform sampler2DArray cascadeDepth;
layout(set = ANIMA_SET_ENVIRONMENT,
       binding = ANIMA_ENVIRONMENT_DETAIL_SHADOW_DEPTH) uniform sampler2DArray detailShadowDepth;
// What the shadow filter knows about a receiver: the screen-space derivatives of its world position, the variation of
// its base normal over a world unit, and how its geometric normal faces the sun. An impostor, which has no receiver
// plane, passes zero derivatives.
struct ShadowReceiver {
    vec3 dx, dy;
    float curvature, facing;
};
// The directional shadow projection is orthographic, so screen derivatives of
// world position transform linearly. Solve dz = gradient dot (du,dv) on the
// actual receiver plane; shaded normals need not describe that plane.
vec2 shadowReceiverGradient(vec3 worldDx, vec3 worldDy, mat4 view) {
    vec3 dx = (view * vec4(worldDx, 0)).xyz;
    vec3 dy = (view * vec4(worldDy, 0)).xyz;
    dx.xy *= 0.5;
    dy.xy *= 0.5;
    vec3 plane = cross(dx, dy);
    // Near edge-on receivers have an ill-conditioned projection. Retain the
    // caller's residual bias instead of amplifying floating-point noise.
    const float minimumProjectedNormal = 1e-4;
    return abs(plane.z) > minimumProjectedNormal * length(plane) ? -plane.xy / plane.z : vec2(0);
}
// A smooth normal can face the sun while its polygon is at/beyond the geometric terminator. Extending that nearly
// edge-on plane across the filter's texels produces false self-shadow triangles on curved surfaces, so the receiver
// plane gives way to the bias alone where the base normal turns noticeably within one texel, @p texel world units.
// Constant-normal planes retain their exact depth correction even when their shading normal differs from the polygon.
vec2 receiverPlaneGradient(ShadowReceiver receiver, mat4 view, float texel) {
    float curved = smoothstep(0.00001, 0.0001, receiver.curvature * texel);
    float weight = mix(1.0, smoothstep(0.0, 0.15, receiver.facing), curved);
    return shadowReceiverGradient(receiver.dx, receiver.dy, view) * weight;
}
// The share of the filter's texels around @p p, in a map's normalized coordinates, that leave the receiver lit: a
// bilinearly weighted 3x3 comparison kernel over 4x4 texels of layer @p layer, or with bilinearShadowFilter the 2x2
// texels around @p p, weighted bilinearly; each compared with the nearer to the light of the receiver's depth and the
// receiver plane's at that texel, from @p gradient, less @p bias. Toward the light the plane keeps a sloped receiver
// from shadowing itself; away from it the receiver's own depth already does, and the plane would put a surface that
// bends toward the light, such as a simplified level's shallow crease, in front of the receiver.
float shadowFilter(sampler2DArray depth, float layer, vec3 p, vec2 gradient, float bias) {
    ivec2 size = textureSize(depth, 0).xy;
    // The texel whose center is the nearest at or below p on both axes, and p's position from it toward the next.
    vec2 pixel = p.xy * vec2(size) - 0.5;
    ivec2 base = ivec2(floor(pixel));
    vec2 fraction = fract(pixel);
    if (bilinearShadowFilter) {
        // One gather of the 2x2 texels from base, at the corner that they share, as for each block below. Their
        // offsets from p across the map and their bilinear weights follow the gather's order: texels (0, 1), (1, 1),
        // (1, 0) and (0, 0) from base.
        vec4 stored = textureGather(depth, vec3(vec2(base + 1) / vec2(size), layer));
        vec2 low = (vec2(clamp(base, ivec2(0), size - 1)) + 0.5) / vec2(size) - p.xy;
        vec2 high = (vec2(clamp(base + 1, ivec2(0), size - 1)) + 0.5) / vec2(size) - p.xy;
        vec4 across = gradient.x * vec4(low.x, high.x, high.x, low.x) + gradient.y * vec4(high.y, high.y, low.y, low.y);
        vec4 receiverDepth = p.z + min(across, 0.0);
        vec4 weight = vec4(1.0 - fraction.x, fraction.x, fraction.x, 1.0 - fraction.x) *
                      vec4(fraction.y, fraction.y, 1.0 - fraction.y, 1.0 - fraction.y);
        return dot(vec4(lessThanEqual(receiverDepth - bias, stored)), weight);
    }
    // Bilinearly interpolate the 3x3 comparison kernel, rather than snapping it
    // to one texel. Its union is 4x4; the separable weights sum to nine. Compare
    // each depth against its own receiver-plane position before interpolation.
    float visible = 0.0;
    // The 4x4 texels' depths, gathered as four 2x2 blocks. Each gather is at the corner that its block's texels share,
    // where choosing another block would take an error of half a texel. The sampler clamps to the edge, as the texel
    // coordinates below are clamped. textureGather returns a block's texels (0, 1), (1, 1), (1, 0) and (0, 0).
    float stored[4][4];
    for (int y = 0; y < 4; y += 2)
        for (int x = 0; x < 4; x += 2) {
            vec4 block = textureGather(depth, vec3(vec2(base + ivec2(x, y)) / vec2(size), layer));
            stored[y][x] = block.w;
            stored[y][x + 1] = block.z;
            stored[y + 1][x] = block.x;
            stored[y + 1][x + 1] = block.y;
        }
    for (int y = -1; y <= 2; ++y)
        for (int x = -1; x <= 2; ++x) {
            vec2 weight = vec2(x == -1  ? 1.0 - fraction.x
                               : x == 2 ? fraction.x
                                        : 1.0,
                               y == -1  ? 1.0 - fraction.y
                               : y == 2 ? fraction.y
                                        : 1.0);
            ivec2 texel = clamp(base + ivec2(x, y), ivec2(0), size - 1);
            vec2 center = (vec2(texel) + 0.5) / vec2(size);
            float receiverDepth = p.z + min(dot(gradient, center - p.xy), 0.0);
            visible += receiverDepth - bias <= stored[y + 1][x + 1] ? weight.x * weight.y : 0.0;
        }
    return visible / 9.0;
}
// The sun's visibility at @p position in cascade @p index, which holds it.
float cascadeVisibility(int index, vec3 position, vec3 normal, ShadowReceiver receiver) {
    mat4 view = environment.cascadeView[index];
    vec4 bias = environment.cascadeBias[index];
    vec3 p = (view * vec4(position, 1)).xyz;
    p.xy = p.xy * 0.5 + 0.5;
    // Fitting keeps every surface that a cascade shades inside it; this guards only against rounding.
    if (any(lessThan(p, vec3(0))) || any(greaterThan(p, vec3(1))))
        return 1.0;
    vec2 gradient = receiverPlaneGradient(receiver, view, bias.z);
    float receiverBias = bias.x + bias.y * (1.0 - max(dot(normal, environment.sunDirection.xyz), 0.0));
    return shadowFilter(cascadeDepth, float(index), p, gradient, receiverBias);
}
// The sun's visibility through the cascades: from the one whose depths hold @p position's view depth, blending into
// the next, or into full light after the last, over the last share of its depths.
float cascadedVisibility(vec3 position, vec3 normal, ShadowReceiver receiver) {
    int count = int(environment.cascades.x);
    float depth = dot(position - environment.cascadeOrigin.xyz, environment.cascadeAxis.xyz);
    float begin = environment.cascadeOrigin.w;
    for (int i = 0; i < count; ++i) {
        float end = environment.cascadeEnd[i];
        if (depth < end) {
            float visible = cascadeVisibility(i, position, normal, receiver);
            float band = environment.cascadeAxis.w * (end - begin);
            float blend = band > 0.0 ? clamp((depth - (end - band)) / band, 0.0, 1.0) : 0.0;
            if (blend > 0.0) {
                float next = i + 1 < count ? cascadeVisibility(i + 1, position, normal, receiver) : 1.0;
                visible = mix(visible, next, blend);
            }
            return visible;
        }
        begin = end;
    }
    return 1.0;
}
// The share of the detail region's box on each axis, in from each face, over which the region's result fades into the
// cascades', so that crossing its boundary neither removes their casters nor double-darkens them.
const float detailShadowFade = 0.04;
// The sun's visibility at @p position: inside an enabled detail region from its map, fading into the cascades over
// its outer detailShadowFade, and elsewhere from the cascades. Only the fade and the world outside the region sample
// the cascades, through a single call, so that compilers inline the cascades' filters once.
float sunVisibility(vec3 position, vec3 normal, ShadowReceiver receiver) {
    float detail = 1.0;
    float weight = 0.0;
    if (environment.detailShadow.x >= 0.5) {
        mat4 view = environment.detailShadowView;
        vec3 p = (view * vec4(position, 1)).xyz;
        p.xy = p.xy * 0.5 + 0.5;
        bool outside = any(lessThan(p, vec3(0))) || any(greaterThan(p, vec3(1)));
        if (!outside) {
            vec4 settings = environment.detailShadow;
            // The texel's world size: the box's width, 2 over the projection's X scale, over the resolution.
            float texel = 2.0 * settings.y / max(length(vec3(view[0][0], view[1][0], view[2][0])), 1e-12);
            float bias = settings.z + settings.w * (1.0 - max(dot(normal, environment.sunDirection.xyz), 0.0));
            detail = shadowFilter(detailShadowDepth, 0.0, p, receiverPlaneGradient(receiver, view, texel), bias);
            float edge = min(min(p.x, 1.0 - p.x), min(p.y, 1.0 - p.y));
            edge = min(edge, min(p.z, 1.0 - p.z));
            // smoothstep() is defined as 1 from its upper edge on, where the region's map alone shades. This tests
            // the edge rather than smoothstep()'s result, which Vulkan's precision lets fall just short of 1 there.
            if (edge >= detailShadowFade)
                return detail;
            weight = smoothstep(0.0, detailShadowFade, edge);
        }
    }
    // A weight of 0, with the region disabled or outside its box, returns the cascades' result exactly.
    return mix(cascadedVisibility(position, normal, receiver), detail, weight);
}
