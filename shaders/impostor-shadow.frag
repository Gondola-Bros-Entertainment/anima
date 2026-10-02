#version 450
#extension GL_GOOGLE_include_directive : require
// An impostor's depth in a shadow region, which impostor.vert views along the sun: covered where the frames nearest
// the sun's direction cover the ray, at the depth of the surface point that they store.
layout(location = 0) in vec3 rayOrigin;
layout(location = 1) in vec3 rayDirection;
layout(location = 2) flat in vec3 frameWeights;
layout(location = 3) flat in uvec3 frames;
layout(location = 4) flat in float visibility;
layout(location = 5) flat in mat4 model;
#include "material.glsl"
layout(push_constant) uniform Draw {
    layout(offset = 0) mat4 viewProjection;
    layout(offset = 80) vec4 sphere;
    layout(offset = 96) uvec4 indices;
}
draw;
#include "impostor.glsl"
#include "impostor_sample.glsl"
// The quad lies across the side of the sphere facing the sun, and the depth written is clamped to the quad's own where
// the parallax step carries the blended point in front of it, so in the shadow regions' forward depth it is never less
// than the quad's, and the depth test may reject hidden pixels before they are shaded.
layout(depth_greater) out float gl_FragDepth;
void main() {
    ImpostorHit hit = impostorHit(draw.indices.y & 255u, draw.indices.y >> 8, draw.sphere, rayOrigin, rayDirection,
                                  frames, frameWeights);
    if (hit.color.a < material.detail.y)
        discard;
    vec4 clip = draw.viewProjection * (model * vec4(hit.point, 1.0));
    gl_FragDepth = max(clip.z / clip.w, gl_FragCoord.z);
}
