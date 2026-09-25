layout(set = 1, binding = 0, std140) uniform EnvironmentData {
    vec4 sunDirection, sunRadiance, fillDirection, fillRadiance;
    vec4 ambientSky, ambientGround, ambientSpecular;
    vec4 skyZenith, skyHorizon, skyGround, fog, controls;
    mat4 inverseView;
    vec4 viewOrigin;
    mat4 shadowView;
    vec4 shadow; // enabled, inverse resolution, constant bias, slope bias
    mat4 detailShadowView;
    vec4 detailShadow;
}
environment;
layout(set = 1, binding = 1) uniform sampler2D shadowDepth;
layout(set = 1, binding = 2) uniform sampler2D detailShadowDepth;
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
float shadowRegionVisibility(sampler2D depth, mat4 view, vec4 settings, vec3 position, vec3 normal,
                             vec2 receiverGradient, out float coverage) {
    coverage = 0.0;
    if (settings.x < 0.5)
        return 1.0;
    vec4 clip = view * vec4(position, 1);
    vec3 p = clip.xyz / clip.w;
    p.xy = p.xy * 0.5 + 0.5;
    if (any(lessThan(p, vec3(0))) || any(greaterThan(p, vec3(1))))
        return 1.0;
    float bias = settings.z + settings.w * (1.0 - max(dot(normal, environment.sunDirection.xyz), 0.0));
    float visible = 0.0;
    ivec2 size = textureSize(depth, 0);
    // Bilinearly interpolate the 3x3 comparison kernel, rather than snapping it
    // to one texel. Its union is 4x4; the separable weights sum to nine. Compare
    // each depth against its own receiver-plane position before interpolation.
    vec2 pixel = p.xy * vec2(size) - 0.5;
    ivec2 base = ivec2(floor(pixel));
    vec2 fraction = fract(pixel);
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
            float receiverDepth = p.z + dot(receiverGradient, center - p.xy);
            visible += receiverDepth - bias <= texelFetch(depth, texel, 0).r ? weight.x * weight.y : 0.0;
        }
    // Fade at the explicit region boundary instead of drawing a hard shadow cutoff.
    float edge = min(min(p.x, 1.0 - p.x), min(p.y, 1.0 - p.y));
    edge = min(edge, min(p.z, 1.0 - p.z));
    coverage = smoothstep(0.0, 0.04, edge);
    return visible / 9.0;
}
float sunVisibility(vec3 position, vec3 normal, vec2 receiverGradient, vec2 detailGradient) {
    float coverage;
    float visible = shadowRegionVisibility(shadowDepth, environment.shadowView, environment.shadow, position, normal,
                                           receiverGradient, coverage);
    visible = mix(1.0, visible, coverage);
    if (environment.detailShadow.x < 0.5)
        return visible;
    float detail = shadowRegionVisibility(detailShadowDepth, environment.detailShadowView, environment.detailShadow,
                                          position, normal, detailGradient, coverage);
    // The detailed region hands off to the already sampled world shadow, so
    // crossing its boundary neither removes world casters nor double-darkens them.
    return mix(visible, detail, coverage);
}
