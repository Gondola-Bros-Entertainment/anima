// Default gradient background and directional light disc.
vec3 skyRadiance(vec3 ray) {
    const float horizonExponent = 0.45;
    const float groundBlendHeight = -0.25;
    const vec2 sunDiscCosines = vec2(.99994, .99998);
    const float sunGlowIntensity = .025;
    const float sunGlowExponent = 96.0;
    vec3 color = mix(environment.skyHorizon.rgb, environment.skyZenith.rgb, pow(max(ray.y, 0.0), horizonExponent));
    color = mix(color, environment.skyGround.rgb, 1.0 - smoothstep(groundBlendHeight, 0.0, ray.y));
    float sun = max(dot(ray, environment.sunDirection.xyz), 0.0);
    color += environment.sunRadiance.rgb *
             (smoothstep(sunDiscCosines.x, sunDiscCosines.y, sun) + sunGlowIntensity * pow(sun, sunGlowExponent));
    return color;
}
