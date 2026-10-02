#version 450
#extension GL_GOOGLE_include_directive : require
layout(location = 0) in vec3 worldNormal;
layout(location = 1) in vec3 baseColor;
layout(location = 2) in vec2 texcoord;
layout(location = 3) in vec3 worldPosition;
layout(location = 4) in vec4 worldTangent;
layout(location = 5) in float vertexAlpha;
layout(location = 6) flat in float orientation;
layout(location = 7) flat in float visibility;
#include "material.glsl"
#include "visibility.glsl"
layout(push_constant) uniform Surface {
    layout(offset = 64) vec4 viewOrigin;
    layout(offset = 80) vec4 factors; // metallic, perceptual roughness
}
surface;
#include "environment.glsl"
#include "lighting.glsl"
layout(location = 0) out vec4 outColor;
// Set in the blended pipeline, which composites premultiplied color over the target with ONE, ONE_MINUS_SRC_ALPHA.
layout(constant_id = 0) const bool blended = false;
float receiverPlaneWeight(float curvature, float facing, mat4 view, vec4 settings) {
    vec3 axis = vec3(view[0][0], view[1][0], view[2][0]);
    float texelPitch = 2.0 * settings.y / max(length(axis), 1e-12);
    float curved = smoothstep(0.00001, 0.0001, curvature * texelPitch);
    return mix(1.0, smoothstep(0.0, 0.15, facing), curved);
}
void main() {
    // Which side of the surface faces the viewer. A mirrored transform winds its outward faces clockwise, so
    // they rasterize as back faces; its negative orientation restores them to the front.
    float facing = (gl_FrontFacing ? 1.0 : -1.0) * orientation;
    // A single-sided material draws only the side its faces front. Facing is constant across a primitive, so this
    // discards whole quads and leaves derivatives defined.
    if (material.maps.y < 0.5 && facing < 0.0)
        discard;
    vec3 n = unit(worldNormal);
    // Authored tangent frames survive skinning and mirrored instance transforms.
    // Assets without TANGENT use a per-triangle cotangent frame; degenerate UVs
    // retain the geometric normal instead of producing NaNs.
    vec3 dp1 = dFdx(worldPosition), dp2 = dFdy(worldPosition);
    // A smooth normal can face the sun while its polygon is at/beyond the
    // geometric terminator. Extending that nearly edge-on plane across PCF
    // neighbours produces false self-shadow triangles on curved surfaces.
    // Detect curvature from the base normal's variation over a shadow texel,
    // before normal mapping. Constant-normal planes retain their exact depth
    // correction even when their shading normal differs from the polygon.
    vec3 geometricNormal = unit(cross(dp1, dp2));
    geometricNormal *= dot(geometricNormal, worldNormal) < 0.0 ? -1.0 : 1.0;
    geometricNormal *= facing;
    float receiverFacing = dot(geometricNormal, environment.sunDirection.xyz);
    vec3 dn1 = dFdx(n), dn2 = dFdy(n);
    float curvature = sqrt((dot(dn1, dn1) + dot(dn2, dn2)) / max(dot(dp1, dp1) + dot(dp2, dp2), 1e-12));
    // Keep a numerical floor for constant normals. Even small curvature makes
    // near-singular plane extrapolation unreliable across the PCF footprint.
    float planeWeight = receiverPlaneWeight(curvature, receiverFacing, environment.shadowView, environment.shadow);
    float detailWeight =
        receiverPlaneWeight(curvature, receiverFacing, environment.detailShadowView, environment.detailShadow);
    vec2 receiverGradient = shadowReceiverGradient(dp1, dp2, environment.shadowView) * planeWeight;
    vec2 detailGradient = shadowReceiverGradient(dp1, dp2, environment.detailShadowView) * detailWeight;
    vec2 du1 = dFdx(texcoord), du2 = dFdy(texcoord);
    float determinant = du1.x * du2.y - du1.y * du2.x;
    if (material.maps.x > 0.5) {
        vec3 t, b;
        bool valid = true;
        if (abs(worldTangent.w) > 0.5) {
            t = worldTangent.xyz - n * dot(n, worldTangent.xyz);
            valid = dot(t, t) > 1e-12;
            t = unit(t);
            b = cross(n, t) * sign(worldTangent.w);
        } else if (abs(determinant) > 1e-10) {
            t = (dp1 * du2.y - dp2 * du1.y) / determinant;
            t = unit(t - n * dot(n, t));
            vec3 rawB = (dp2 * du1.x - dp1 * du2.x) / determinant;
            b = cross(n, t) * (dot(cross(n, t), rawB) < 0.0 ? -1.0 : 1.0);
        } else {
            t = vec3(0);
            b = vec3(0);
            valid = false;
        }
        vec3 mapped = texture(normalTexture, texcoord).xyz * 2.0 - 1.0;
        mapped.xy *= material.detail.x;
        if (valid)
            n = unit(t * mapped.x + b * mapped.y + n * mapped.z);
    }
    n *= facing;
    vec4 base = texture(baseColorTexture, texcoord);
    vec4 mr = texture(metallicRoughnessTexture, texcoord);
    float occlusion = mix(1.0, texture(occlusionTexture, texcoord).r, material.detail.z);
    vec3 emission = texture(emissiveTexture, texcoord).rgb;
    // The alpha that masking tests and blending composites with.
    float alpha = base.a * material.emissiveAlpha.a * vertexAlpha;
    // Every texture is sampled before a pixel discards, since a discard leaves undefined the derivatives that choose
    // mip levels for the rest of its 2x2 quad. In its visibility range's margins an object dissolves, keeping the share
    // of its pixels that its visibility gives, and a masked material discards what lies below its cutoff.
    if (dissolved(visibility, gl_FragCoord.xy) || (material.detail.y >= 0.0 && alpha < material.detail.y))
        discard;
    vec3 albedo = clamp(base.rgb * baseColor, 0.0, 1.0);
    vec3 color = reflectedLight(n, worldPosition, surface.viewOrigin, albedo, surface.factors.x * mr.b,
                                surface.factors.y * mr.g, occlusion, receiverGradient, detailGradient);
    color += material.emissiveAlpha.rgb * emission;
    if (material.detail.w > 0.5)
        color = albedo;
    color = fogged(color, worldPosition, surface.viewOrigin);
    // Blending scales the whole shaded and fogged color, emission included, by the straight alpha.
    alpha = clamp(alpha, 0.0, 1.0);
    outColor = blended ? vec4(color * alpha, alpha) : vec4(color, 1.0);
}
