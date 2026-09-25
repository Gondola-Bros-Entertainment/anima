#version 450
#extension GL_GOOGLE_include_directive : require
layout(location = 0) in vec3 worldNormal;
layout(location = 1) in vec3 baseColor;
layout(location = 2) in vec2 texcoord;
layout(location = 3) in vec3 worldPosition;
layout(location = 4) in vec4 worldTangent;
layout(location = 5) in float vertexAlpha;
#include "material.glsl"
layout(push_constant) uniform Surface {
    layout(offset = 64) vec4 viewOrigin;
    layout(offset = 80) vec4 factors; // metallic, perceptual roughness
}
surface;
#include "environment.glsl"
layout(location = 0) out vec4 outColor;
const float PI = 3.14159265359;
const float minimumRoughness = 0.045;
const float dielectricReflectance = 0.04;
const float maximumHalfFloat = 65504.0;
vec3 unit(vec3 v) { return v * inversesqrt(max(dot(v, v), 1e-12)); }
float receiverPlaneWeight(float curvature, float facing, mat4 view, vec4 settings) {
    vec3 axis = vec3(view[0][0], view[1][0], view[2][0]);
    float texelPitch = 2.0 * settings.y / max(length(axis), 1e-12);
    float curved = smoothstep(0.00001, 0.0001, curvature * texelPitch);
    return mix(1.0, smoothstep(0.0, 0.15, facing), curved);
}
// glTF isotropic GGX, height-correlated Smith visibility and Schlick Fresnel.
vec3 light(vec3 n, vec3 v, vec3 l, vec3 albedo, vec3 f0, float metallic, float alpha) {
    float nl = max(dot(n, l), 0.0), nv = max(dot(n, v), 0.0);
    if (nl <= 0.0 || nv <= 0.0)
        return vec3(0);
    vec3 h = unit(v + l);
    float nh = max(dot(n, h), 0.0), vh = max(dot(v, h), 0.0);
    float a2 = alpha * alpha;
    float d = (1.0 - nh * nh) + a2 * nh * nh;
    float distribution = a2 / max(PI * d * d, 1e-12);
    float visibility = 0.5 / max(nl * sqrt(nv * nv * (1.0 - a2) + a2) + nv * sqrt(nl * nl * (1.0 - a2) + a2), 1e-6);
    vec3 fresnel = f0 + (1.0 - f0) * pow(1.0 - vh, 5.0);
    vec3 diffuse = (1.0 - fresnel) * (1.0 - metallic) * albedo / PI;
    return (diffuse + fresnel * distribution * visibility) * nl;
}
void main() {
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
    geometricNormal *= gl_FrontFacing ? 1.0 : -1.0;
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
    n *= gl_FrontFacing ? 1.0 : -1.0;
    vec4 base = texture(baseColorTexture, texcoord);
    if (material.detail.y >= 0.0 && base.a * material.emissiveAlpha.a * vertexAlpha < material.detail.y)
        discard;
    vec3 albedo = clamp(base.rgb * baseColor, 0.0, 1.0);
    vec3 v = unit(surface.viewOrigin.xyz - worldPosition * surface.viewOrigin.w);
    vec4 mr = texture(metallicRoughnessTexture, texcoord);
    float metallic = surface.factors.x * mr.b;
    float roughness = max(surface.factors.y * mr.g, minimumRoughness);
    vec3 f0 = mix(vec3(dielectricReflectance), albedo, metallic);
    float occlusion = mix(1.0, texture(occlusionTexture, texcoord).r, material.detail.z);
    vec3 ambient = mix(environment.ambientGround.rgb, environment.ambientSky.rgb, n.y * 0.5 + 0.5);
    vec3 color = occlusion * (ambient * (1.0 - metallic) * albedo + environment.ambientSpecular.rgb * f0);
    color += sunVisibility(worldPosition, n, receiverGradient, detailGradient) * environment.sunRadiance.rgb *
             light(n, v, environment.sunDirection.xyz, albedo, f0, metallic, roughness * roughness);
    color += environment.fillRadiance.rgb *
             light(n, v, environment.fillDirection.xyz, albedo, f0, metallic, roughness * roughness);
    color += material.emissiveAlpha.rgb * texture(emissiveTexture, texcoord).rgb;
    if (material.detail.w > 0.5)
        color = albedo;
    if (surface.viewOrigin.w > 0.5 && environment.fog.w > 0.0) {
        float transmittance = exp(-environment.fog.w * length(surface.viewOrigin.xyz - worldPosition));
        color = mix(environment.fog.rgb, color, transmittance);
    }
    outColor = vec4(clamp(color, vec3(0), vec3(maximumHalfFloat)), 1.0);
}
