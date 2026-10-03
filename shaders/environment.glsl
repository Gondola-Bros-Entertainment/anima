layout(set = 1, binding = 0, std140) uniform EnvironmentData {
    vec4 sunDirection, sunRadiance, fillDirection, fillRadiance;
    vec4 ambientSky, ambientGround, ambientSpecular;
    vec4 skyZenith, skyHorizon, skyGround, fog, controls;
    mat4 inverseView;
    vec4 viewOrigin;
    // The sun's shadow cascades fitted to the view: each one's view-projection,
    mat4 cascadeView[4];
    // the view depth where each one ends,
    vec4 cascadeEnd;
    // and its constant and slope biases in its normalized depth, and the world size of its texels.
    vec4 cascadeBias[4];
    // The point that view depths are measured from, with the depth where the first cascade begins in w.
    vec4 cascadeOrigin;
    // The unit view direction along which they are measured, with the share of each cascade that blends into the next
    // in w.
    vec4 cascadeAxis;
    // The number of cascades, none while they are disabled.
    vec4 cascades;
    mat4 detailShadowView;
    vec4 detailShadow; // enabled, inverse resolution, constant bias, slope bias
}
environment;
layout(set = 1, binding = 1) uniform sampler2DArray cascadeDepth;
layout(set = 1, binding = 2) uniform sampler2DArray detailShadowDepth;
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
// bilinearly weighted 3x3 comparison kernel over 4x4 texels of layer @p layer, each compared with the nearer to the
// light of the receiver's depth and the receiver plane's at that texel, from @p gradient, less @p bias. Toward the
// light the plane keeps a sloped receiver from shadowing itself; away from it the receiver's own depth already does,
// and the plane would put a surface that bends toward the light, such as a simplified level's shallow crease, in front
// of the receiver.
float shadowFilter(sampler2DArray depth, float layer, vec3 p, vec2 gradient, float bias) {
    float visible = 0.0;
    ivec2 size = textureSize(depth, 0).xy;
    // Bilinearly interpolate the 3x3 comparison kernel, rather than snapping it
    // to one texel. Its union is 4x4; the separable weights sum to nine. Compare
    // each depth against its own receiver-plane position before interpolation.
    vec2 pixel = p.xy * vec2(size) - 0.5;
    ivec2 base = ivec2(floor(pixel));
    vec2 fraction = fract(pixel);
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
float sunVisibility(vec3 position, vec3 normal, ShadowReceiver receiver) {
    float visible = cascadedVisibility(position, normal, receiver);
    if (environment.detailShadow.x < 0.5)
        return visible;
    mat4 view = environment.detailShadowView;
    vec3 p = (view * vec4(position, 1)).xyz;
    p.xy = p.xy * 0.5 + 0.5;
    if (any(lessThan(p, vec3(0))) || any(greaterThan(p, vec3(1))))
        return visible;
    vec4 settings = environment.detailShadow;
    // The texel's world size: the box's width, 2 over the projection's X scale, over the resolution.
    float texel = 2.0 * settings.y / max(length(vec3(view[0][0], view[1][0], view[2][0])), 1e-12);
    float bias = settings.z + settings.w * (1.0 - max(dot(normal, environment.sunDirection.xyz), 0.0));
    float detail = shadowFilter(detailShadowDepth, 0.0, p, receiverPlaneGradient(receiver, view, texel), bias);
    // Fade at the region's boundary into the cascades, so crossing it neither removes their casters nor
    // double-darkens them.
    float edge = min(min(p.x, 1.0 - p.x), min(p.y, 1.0 - p.y));
    edge = min(edge, min(p.z, 1.0 - p.z));
    return mix(visible, detail, smoothstep(0.0, 0.04, edge));
}
