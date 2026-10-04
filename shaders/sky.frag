#version 450
#extension GL_GOOGLE_include_directive : require
// The sky behind the scene, while the atmosphere is enabled: the sky view table in the direction of the view ray,
// times the sun's irradiance above the atmosphere, with the sun's disc, fogged as a surface fog_sky_distance along the
// ray would be.
layout(location = 0) in vec2 screen;
layout(location = 0) out vec4 outColor;
#include "environment.glsl"
#include "atmosphere.glsl"
layout(set = 1, binding = 3) uniform sampler2D skyViewTable;
layout(set = 1, binding = 4) uniform sampler2D transmittanceTable;
void main() {
    vec3 ray = environment.viewOrigin.w > 0.5 ? normalize((environment.viewRays * vec4(screen, 0.0, 1.0)).xyz)
                                              : -environment.viewOrigin.xyz;
    vec3 sun = environment.sunDirection.xyz;
    float altitude = environment.atmosphereGround.w;
    float elevation = asin(clamp(ray.y, -1.0, 1.0));
    // The angle between the ray's and the sun's horizontal directions; the table does not vary with it where either
    // is vertical.
    float lengths = length(ray.xz) * length(sun.xz);
    float azimuth = lengths > 1e-12 ? acos(clamp(dot(ray.xz, sun.xz) / lengths, -1.0, 1.0)) : 0.0;
    vec3 color = textureLod(skyViewTable, skyViewCoordinates(elevation, azimuth, altitude), 0.0).rgb *
                 environment.atmosphereSun.rgb;
    // The disc's radiance, the sun's irradiance spread over its solid angle, 2 pi (1 - cos r) = 4 pi sin^2(r / 2),
    // through the atmosphere along the ray.
    float radius = environment.atmosphereShape.w, halfSine = sin(0.5 * radius);
    if (dot(ray, sun) >= cos(radius))
        color += environment.atmosphereSun.rgb / (4.0 * atmospherePi * halfSine * halfSine) *
                 transmittanceToTop(transmittanceTable, altitude, ray.y);
    if (environment.fogShape.z > 0.0)
        color = environmentFog(color, environment.viewOrigin.xyz + ray * environment.fogShape.z, environment.viewOrigin);
    outColor = vec4(clamp(color, vec3(0), vec3(65504)), 1.0);
}
