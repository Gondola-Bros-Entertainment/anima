#version 450
#extension GL_GOOGLE_include_directive : require
// The background behind the scene while the atmosphere is disabled, fogged as a surface HeightFog::sky_distance along
// the view ray would be. The renderer draws it only in a perspective view whose fog reaches the background; elsewhere
// the world pass's clear color shows the background unfogged.
layout(location = 0) in vec2 screen;
layout(location = 0) out vec4 outColor;
#include "environment.glsl"
void main() {
    vec3 ray = normalize((environment.viewRays * vec4(screen, 0.0, 1.0)).xyz);
    vec3 color = environmentFog(environment.background.rgb, environment.viewOrigin.xyz + ray * environment.fogShape.z,
                                environment.viewOrigin);
    outColor = vec4(clamp(color, vec3(0), vec3(animaMaximumHalfFloat)), 1.0);
}
