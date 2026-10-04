// The atmosphere of anima::Atmosphere, after Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering
// Technique" (EGSR 2020), with its transmittance table parameterized as in Bruneton, "Precomputed Atmospheric
// Scattering: a New Implementation" (2017). Include it after environment_data.glsl. Lengths are in meters. Altitudes
// are kept apart from the ground's radius, whose float spacing of half a meter would otherwise swallow them: every
// r^2 - R^2 is formed as h (2R + h).
#include "constants.glsl"
#include "shader_interface.h"

// The tables' sizes, which the renderer allocates.
const ivec2 transmittanceSize = ivec2(ANIMA_TRANSMITTANCE_WIDTH, ANIMA_TRANSMITTANCE_HEIGHT);
const ivec2 multipleScatteringSize = ivec2(ANIMA_MULTIPLE_SCATTERING_WIDTH, ANIMA_MULTIPLE_SCATTERING_HEIGHT);
const ivec2 skyViewSize = ivec2(ANIMA_SKY_VIEW_WIDTH, ANIMA_SKY_VIEW_HEIGHT);

float groundRadius() { return environment.atmosphereShape.x; }
float thickness() { return environment.atmosphereShape.y; }
float topRadius() { return environment.atmosphereShape.x + environment.atmosphereShape.y; }

// Texture coordinates whose first and last texel centers hold @p x = 0 and 1, and back.
float textureFromUnit(float x, int size) { return 0.5 / float(size) + x * (1.0 - 1.0 / float(size)); }
float unitFromTexture(float u, int size) { return (u - 0.5 / float(size)) / (1.0 - 1.0 / float(size)); }

// The altitude a distance @p t along a path from @p altitude whose cosine with the zenith is @p mu.
float altitudeAlong(float altitude, float mu, float t) {
    float r = groundRadius() + altitude;
    float rt = sqrt(max(r * r + 2.0 * r * t * mu + t * t, 0.0));
    return altitude + (2.0 * r * t * mu + t * t) / (rt + r);
}
// The cosine with the local zenith, a distance @p t along that path, of the unit direction whose cosine with the
// starting zenith is @p startMu and with the path is @p along: the zenith turns with the path around the planet.
float zenithCosineAlong(float altitude, float mu, float t, float startMu, float along) {
    float r = groundRadius() + altitude;
    float rt = sqrt(max(r * r + 2.0 * r * t * mu + t * t, 0.0));
    return clamp((r * startMu + t * along) / rt, -1.0, 1.0);
}
// Whether the path from @p altitude along zenith cosine @p mu meets the ground.
bool meetsGround(float altitude, float mu) {
    float r = groundRadius() + altitude;
    return mu < 0.0 && r * r * mu * mu - altitude * (2.0 * groundRadius() + altitude) >= 0.0;
}
// The distance to the ground along a path that meets it.
float distanceToGround(float altitude, float mu) {
    float r = groundRadius() + altitude, lowered = altitude * (2.0 * groundRadius() + altitude);
    return lowered / (-r * mu + sqrt(max(r * r * mu * mu - lowered, 0.0)));
}
// The distance to the atmosphere's top along a path from @p altitude, at most the thickness up from it.
float distanceToTop(float altitude, float mu) {
    float r = groundRadius() + altitude, inside = (thickness() - altitude) * (r + topRadius());
    float root = sqrt(max(r * r * mu * mu + inside, 0.0));
    return mu > 0.0 ? inside / (r * mu + root) : root - r * mu;
}
// The distance a path from @p altitude travels through the atmosphere: to the ground where it meets it, else to the
// top.
float pathLength(float altitude, float mu) {
    return meetsGround(altitude, mu) ? distanceToGround(altitude, mu) : distanceToTop(altitude, mu);
}

// The media at @p altitude: extinction, and Rayleigh and Mie scattering, per meter.
vec3 extinctionAt(float altitude) {
    float rayleigh = exp(-altitude / environment.atmosphereRayleigh.w);
    float mie = exp(-altitude / environment.atmosphereMie.w);
    float ozone =
        max(0.0, 1.0 - abs(altitude - environment.atmosphereOzone.w) / (0.5 * environment.atmosphereShape.z));
    return environment.atmosphereRayleigh.rgb * rayleigh +
           (environment.atmosphereMie.rgb + environment.atmosphereMieAbsorption.rgb) * mie +
           environment.atmosphereOzone.rgb * ozone;
}
vec3 rayleighScatteringAt(float altitude) {
    return environment.atmosphereRayleigh.rgb * exp(-altitude / environment.atmosphereRayleigh.w);
}
vec3 mieScatteringAt(float altitude) {
    return environment.atmosphereMie.rgb * exp(-altitude / environment.atmosphereMie.w);
}

// Step @p i of the @p steps that divide a path of length @p span at span (k / steps)^2 for each k, finer near the
// path's start: the distance @p t of its middle along the path, and its length @p dt.
void squaredStep(int i, int steps, float span, out float t, out float dt) {
    float near = span * pow(float(i) / float(steps), 2.0), far = span * pow(float(i + 1) / float(steps), 2.0);
    t = 0.5 * (near + far);
    dt = far - near;
}
// The integral over a step of length @p dt of a constant source dimmed by the step's own medium, @p extinction per
// meter, which passes @p transmittance, exp(-extinction dt), across the step: (1 - transmittance) / extinction, or dt
// in a channel whose medium is empty.
vec3 stepIntegral(vec3 extinction, vec3 transmittance, float dt) {
    // The extinction per meter below which a channel's medium counts as empty.
    const float emptyExtinction = 1e-20;
    return mix((1.0 - transmittance) / max(extinction, vec3(emptyExtinction)), vec3(dt),
               lessThan(extinction, vec3(emptyExtinction)));
}

float rayleighPhase(float cosine) { return 3.0 / (16.0 * PI) * (1.0 + cosine * cosine); }
// Cornette-Shanks, with the asymmetry environment.atmosphereMieAbsorption.w; its base never falls below its least
// value, (1 - |g|)^2.
float miePhase(float cosine) {
    float g = environment.atmosphereMieAbsorption.w, a = abs(g);
    float base = max(1.0 + g * g - 2.0 * g * cosine, (1.0 - a) * (1.0 - a));
    return 3.0 / (8.0 * PI) * (1.0 - g * g) * (1.0 + cosine * cosine) / ((2.0 + g * g) * base * sqrt(base));
}

// The transmittance table's coordinates for a path from @p altitude along zenith cosine @p mu that stays above the
// ground: the share of the way from the nearest to the farthest distance to the top, and the distance to the horizon
// over its greatest.
vec2 transmittanceCoordinates(float altitude, float mu) {
    float horizonTop = sqrt(thickness() * (groundRadius() + topRadius()));
    float horizon = sqrt(max(altitude * (2.0 * groundRadius() + altitude), 0.0));
    float nearest = thickness() - altitude, farthest = horizon + horizonTop;
    float share = (distanceToTop(altitude, mu) - nearest) / max(farthest - nearest, 1e-3);
    return vec2(textureFromUnit(clamp(share, 0.0, 1.0), transmittanceSize.x),
                textureFromUnit(horizon / horizonTop, transmittanceSize.y));
}
// The share of light that crosses the atmosphere from @p altitude to its top along zenith cosine @p mu, read from
// @p table; zero along a path that meets the ground.
vec3 transmittanceToTop(sampler2D table, float altitude, float mu) {
    if (meetsGround(altitude, mu))
        return vec3(0.0);
    return textureLod(table, transmittanceCoordinates(altitude, mu), 0.0).rgb;
}

// The multiple scattering table's coordinates: the sun's zenith cosine, and the altitude over the thickness.
vec2 multipleScatteringCoordinates(float altitude, float sunMu) {
    return vec2(textureFromUnit(clamp(sunMu * 0.5 + 0.5, 0.0, 1.0), multipleScatteringSize.x),
                textureFromUnit(clamp(altitude / thickness(), 0.0, 1.0), multipleScatteringSize.y));
}
// The table holds the logarithm of Psi_ms, never below that of leastMultipleScattering, so that linear filtering
// interpolates it geometrically: across the terminator it falls by orders of magnitude over a few columns, which linear
// interpolation of the term itself overestimates, and its least values lie below half precision's normal range.
const float leastMultipleScattering = 1e-30;
// Psi_ms at @p altitude for the sun's zenith cosine @p sunMu, from @p table.
vec3 multipleScattering(sampler2D table, float altitude, float sunMu) {
    return exp(textureLod(table, multipleScatteringCoordinates(altitude, sunMu), 0.0).rgb);
}

// The angle below the horizontal of the horizon seen from @p altitude.
float horizonDip(float altitude) {
    float r = groundRadius() + altitude;
    return asin(clamp(sqrt(max(altitude * (2.0 * groundRadius() + altitude), 0.0)) / r, 0.0, 1.0));
}
// The sky view table's coordinates for a direction at @p elevation radians above the horizontal and @p azimuth
// radians from the sun's, from 0 to pi, seen from @p altitude: the azimuth's square root, which spends texels near the
// sun, and the elevation's distance from the horizon, square rooted on each side of it, which spends them near the
// horizon.
vec2 skyViewCoordinates(float elevation, float azimuth, float altitude) {
    float dip = horizonDip(altitude), v;
    if (elevation >= -dip)
        v = 0.5 + 0.5 * sqrt(clamp((elevation + dip) / (0.5 * PI + dip), 0.0, 1.0));
    else
        v = 0.5 - 0.5 * sqrt(clamp((-dip - elevation) / (0.5 * PI - dip), 0.0, 1.0));
    float u = sqrt(clamp(azimuth / PI, 0.0, 1.0));
    return vec2(textureFromUnit(u, skyViewSize.x), textureFromUnit(v, skyViewSize.y));
}
