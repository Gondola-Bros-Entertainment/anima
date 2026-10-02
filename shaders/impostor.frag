#version 450
#extension GL_GOOGLE_include_directive : require
// An impostor's pixel: where its ray crosses the plane of each of the three frames nearest the viewing direction, the
// atlas's texels blended by the frames' weights, at the depth of the surface point that they store, lit as the
// standard material lights a surface.
layout(location = 0) in vec3 rayOrigin;
layout(location = 1) in vec3 rayDirection;
layout(location = 2) flat in vec3 frameWeights;
layout(location = 3) flat in uvec3 frames;
layout(location = 4) flat in float visibility;
layout(location = 5) flat in mat4 model;
#include "material.glsl"
layout(push_constant) uniform Surface {
    layout(offset = 0) mat4 viewProjection;
    layout(offset = 64) vec4 viewOrigin;
    layout(offset = 80) vec4 sphere;
    layout(offset = 96) uvec4 indices;
    layout(offset = 112) vec4 factor;
}
surface;
#include "environment.glsl"
#include "lighting.glsl"
#include "visibility.glsl"
#include "impostor.glsl"
#include "impostor_sample.glsl"
layout(location = 0) out vec4 outColor;
// The quad lies across the front of the sphere that holds every surface point, and the depth written is clamped to the
// quad's own where the parallax step carries the blended point in front of it, so in reversed depth it is never
// greater than the quad's: the depth test may reject hidden pixels before they are shaded, as it does for surfaces that
// write no depth.
layout(depth_less) out float gl_FragDepth;
void main() {
    // impostorHit() chooses its mip level before any pixel of the quad discards, and every sample after reads it
    // explicitly, so the discards leave nothing undefined and the pixels they remove skip the rest.
    ImpostorHit hit = impostorHit(surface.indices.y & 255u, surface.indices.y >> 8, surface.sphere, rayOrigin,
                                  rayDirection, frames, frameWeights);
    if (dissolved(visibility, gl_FragCoord.xy) || hit.color.a < material.detail.y)
        discard;
    vec3 normal, emission;
    vec4 maps;
    impostorShade(hit, any(greaterThan(material.emissiveAlpha.rgb, vec3(0))), normal, maps, emission);
    vec3 position = (model * vec4(hit.point, 1.0)).xyz;
    vec4 clip = surface.viewProjection * vec4(position, 1.0);
    gl_FragDepth = min(clip.z / clip.w, gl_FragCoord.z);
    // As the CPU's normal(): the cofactor matrix keeps the normal's direction under any affine matrix.
    vec3 a = model[0].xyz, b = model[1].xyz, c = model[2].xyz;
    vec3 n = cross(b, c) * normal.x + cross(c, a) * normal.y + cross(a, b) * normal.z;
    n = unit(dot(a, cross(b, c)) < 0.0 ? -n : n);
    vec3 albedo = clamp(hit.color.rgb * surface.factor.rgb, 0.0, 1.0);
    float occlusion = mix(1.0, maps.r, material.detail.z);
    // Impostor surfaces have no plane to extend across the shadow kernel, so they keep the constant and slope biases.
    vec3 color =
        reflectedLight(n, position, surface.viewOrigin, albedo, maps.b, maps.g, occlusion, vec2(0), vec2(0));
    color += material.emissiveAlpha.rgb * emission;
    outColor = vec4(fogged(color, position, surface.viewOrigin), 1.0);
}
