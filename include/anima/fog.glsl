// The fog of anima::EnvironmentSettings, anima::HeightFog, as the renderer's shaders and custom materials
// (custom_material.glsl) apply it. Its inputs are the frame's fog vectors: @p fog holds the fog's color with its
// density in w, @p shape holds its height, falloff, sky_distance and sun_anisotropy, and @p sun holds its
// sun_scattering times the sun as it reaches the ground, anima::atmosphere_sunlight().
#ifndef ANIMA_FOG_GLSL
#define ANIMA_FOG_GLSL

// The largest finite half float, (2 - 2^-10) * 2^15, and so the largest finite value that a channel of the scene's
// default 16-bit float color target holds (anima::SceneColorFormat::rgba16f).
const float animaMaximumHalfFloat = 65504.0;

// The share of light that reaches @p eye from @p position through fog of positive density: exp(-t), where t
// integrates the density along the straight path between them, in closed form.
float animaFogTransmittance(vec3 eye, vec3 position, vec4 fog, vec4 shape) {
    float distance = length(eye - position);
    float falloff = shape.y;
    if (falloff <= 0.0)
        return exp(-fog.w * distance);
    float rise = falloff * abs(position.y - eye.y);
    // The mean density over the path relative to the density at its lower end, (1 - exp(-rise)) / rise, which tends
    // to 1 - rise / 2 as rise does to zero. At most 1, so the path's length times it cannot overflow.
    float mean = rise > 1e-4 ? (1.0 - exp(-rise)) / rise : 1.0 - 0.5 * rise;
    float span = distance * mean;
    // An empty path, or one shorter than a float can carry through, keeps all the light.
    if (span <= 0.0)
        return 1.0;
    // t in log space, so that neither a vanishing density nor a large exponent decides it alone; past e^80, t leaves
    // no light that a float can hold.
    float logDepth = log(fog.w) + log(span) - falloff * (min(eye.y, position.y) - shape.x);
    return exp(-exp(min(logDepth, 80.0)));
}

// The light that the fog scatters toward @p eye along the path to @p position: the ambient fog color, plus sunlight
// toward unit @p sunDirection through a Henyey-Greenstein phase function, each channel capped at
// animaMaximumHalfFloat, with or without sunlight.
vec3 animaFogLight(vec3 eye, vec3 position, vec4 fog, vec4 shape, vec4 sun, vec3 sunDirection) {
    vec3 path = position - eye;
    float length2 = dot(path, path);
    if (all(equal(sun.rgb, vec3(0.0))) || length2 <= 0.0)
        return min(fog.rgb, vec3(animaMaximumHalfFloat));
    const float pi = 3.14159265359;
    float g = shape.w, a = abs(g);
    // 1 + g^2 - 2 g c = (1 - |g|)^2 + |g| |u - sign(g) s|^2 for unit u and s with cosine c: a sum of nonnegative
    // terms, without the cancellation of the first form toward the sun, and at least (1 - |g|)^2.
    vec3 offset = path * inversesqrt(length2) - (g < 0.0 ? -sunDirection : sunDirection);
    float base = (1.0 - a) * (1.0 - a) + a * dot(offset, offset);
    float phase = (1.0 - a) * (1.0 + a) / (4.0 * pi * base * sqrt(base));
    return min(fog.rgb + sun.rgb * phase, vec3(animaMaximumHalfFloat));
}

// @p color at @p position as seen from @p eye through fog of positive density.
vec3 animaFog(vec3 color, vec3 eye, vec3 position, vec4 fog, vec4 shape, vec4 sun, vec3 sunDirection) {
    return mix(animaFogLight(eye, position, fog, shape, sun, sunDirection), color,
               animaFogTransmittance(eye, position, fog, shape));
}

#endif
