// The standard material's lighting, which mesh.frag and impostor.frag share. Include it after environment.glsl.
const float PI = 3.14159265359;
const float minimumRoughness = 0.045;
const float dielectricReflectance = 0.04;
const float maximumHalfFloat = 65504.0;
vec3 unit(vec3 v) { return v * inversesqrt(max(dot(v, v), 1e-12)); }
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
// The light that a surface at @p position with unit normal @p n reflects toward @p viewOrigin, the eye with w 1 or the
// direction toward the camera with w 0: ambient light darkened by @p occlusion, and the sun, shadowed as
// sunVisibility() finds for @p receiver, and the fill light, each through light().
vec3 reflectedLight(vec3 n, vec3 position, vec4 viewOrigin, vec3 albedo, float metallic, float perceptualRoughness,
                    float occlusion, ShadowReceiver receiver) {
    vec3 v = unit(viewOrigin.xyz - position * viewOrigin.w);
    float roughness = max(perceptualRoughness, minimumRoughness);
    vec3 f0 = mix(vec3(dielectricReflectance), albedo, metallic);
    vec3 ambient = mix(environment.ambientGround.rgb, environment.ambientSky.rgb, n.y * 0.5 + 0.5);
    vec3 color = occlusion * (ambient * (1.0 - metallic) * albedo + environment.ambientSpecular.rgb * f0);
    // The shadow filter runs only where the sun lights the surface, since it only scales that light.
    vec3 sun = light(n, v, environment.sunDirection.xyz, albedo, f0, metallic, roughness * roughness);
    if (any(greaterThan(sun, vec3(0))))
        color += sunVisibility(position, n, receiver) * environment.sunRadiance.rgb * sun;
    color += environment.fillRadiance.rgb *
             light(n, v, environment.fillDirection.xyz, albedo, f0, metallic, roughness * roughness);
    return color;
}
// @p color seen through the fog between @p viewOrigin and @p position in a perspective view, clamped to the half floats
// of the scene target.
vec3 fogged(vec3 color, vec3 position, vec4 viewOrigin) {
    if (viewOrigin.w > 0.5 && environment.fog.w > 0.0) {
        float transmittance = exp(-environment.fog.w * length(viewOrigin.xyz - position));
        color = mix(environment.fog.rgb, color, transmittance);
    }
    return clamp(color, vec3(0), vec3(maximumHalfFloat));
}
