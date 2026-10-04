#version 450
#extension GL_GOOGLE_include_directive : require
// One corner of an impostor's quad, which faces the viewpoint across the front of the sphere that holds its mesh,
// covering the sphere's silhouette, in the mesh's space so that any affine placement keeps it exact. The fragment
// shaders find where each pixel's ray crosses the planes of the three frames nearest the viewing direction.
#include "shader_interface.h"
// The corner, from 0 to 1 along the quad's right and up axes.
layout(location = ANIMA_ATTRIBUTE_UV) in vec2 uv;
// Rows 0 to 2 of the placement's affine matrix, per instance; an identity for an object without placements.
layout(location = ANIMA_ATTRIBUTE_PLACEMENT_ROW0) in vec4 placement0;
layout(location = ANIMA_ATTRIBUTE_PLACEMENT_ROW1) in vec4 placement1;
layout(location = ANIMA_ATTRIBUTE_PLACEMENT_ROW2) in vec4 placement2;
// The ray through this corner in the mesh's space: from the eye, or along a parallel view from the corner itself.
layout(location = 0) out vec3 rayOrigin;
layout(location = 1) out vec3 rayDirection;
layout(location = 2) flat out vec3 frameWeights;
// The three frames, packed as column | row << 8.
layout(location = 3) flat out uvec3 frames;
layout(location = 4) flat out float visibility;
// The matrix from the mesh's space to the world.
layout(location = 5) flat out mat4 model;
layout(set = ANIMA_SET_POSES, binding = ANIMA_POSE_MATRICES, std430) readonly buffer Poses { mat4 matrices[]; }
poses;
layout(push_constant) uniform Draw {
    layout(offset = 0) mat4 viewProjection;
    // The eye with w 1, or in an orthographic view the direction toward the camera with w 0.
    layout(offset = 64) vec4 origin;
    // The center and radius of the sphere that the frames cover (ImpostorFrames).
    layout(offset = 80) vec4 sphere;
    // As in resource.vert, except that y holds the frames per side and the arrangement (ANIMA_IMPOSTOR_COUNT_MASK and
    // ANIMA_IMPOSTOR_LAYOUT_SHIFT).
    layout(offset = 96) uvec4 indices;
}
draw;
// Set in the shadow pipeline, which views along the sun, whose direction the shadow pass's projection gives.
layout(constant_id = ANIMA_SPEC_SHADOW_PASS) const bool shadowPass = false;
#include "../include/anima/visibility.glsl"
#include "impostor.glsl"
void main() {
    mat4 transform = poses.matrices[draw.indices.x];
    bool placed = (draw.indices.w & ANIMA_DRAW_PLACED) != 0u, ranged = (draw.indices.w & ANIMA_DRAW_RANGED) != 0u;
    mat4 object = placed || ranged ? poses.matrices[draw.indices.z] : mat4(1.0);
    if (placed) {
        object = object * transpose(mat4(placement0, placement1, placement2, vec4(0, 0, 0, 1)));
        transform = object * transform;
    }
    model = transform;
    mat4 inverseModel = inverse(transform);
    vec3 center = draw.sphere.xyz;
    float radius = draw.sphere.w;
    uint count = draw.indices.y & ANIMA_IMPOSTOR_COUNT_MASK,
         arrangement = draw.indices.y >> ANIMA_IMPOSTOR_LAYOUT_SHIFT;
    // The direction toward the viewpoint in the mesh's space. A shadow pass's projection is orthographic with forward
    // depth, so its third row points away from the sun.
    vec3 toward;
    bool parallel = shadowPass || draw.origin.w == 0.0;
    vec3 eye = vec3(0);
    float span = radius, distanceToCenter = 0.0;
    if (shadowPass)
        toward = -vec3(draw.viewProjection[0][2], draw.viewProjection[1][2], draw.viewProjection[2][2]);
    else if (parallel)
        toward = draw.origin.xyz;
    if (parallel)
        toward = normalize(mat3(inverseModel) * toward);
    else {
        eye = (inverseModel * vec4(draw.origin.xyz, 1)).xyz;
        toward = eye - center;
        distanceToCenter = length(toward);
        toward /= max(distanceToCenter, 1e-30);
        // At the sphere's front, the cone from the eye that touches the sphere is this wide.
        if (distanceToCenter > radius)
            span = (distanceToCenter - radius) * radius / sqrt(distanceToCenter * distanceToCenter - radius * radius);
    }
    vec3 right = abs(toward.x) + abs(toward.z) > 0.0 ? normalize(vec3(toward.z, 0, -toward.x)) : vec3(1, 0, 0);
    vec3 up = cross(toward, right);
    vec3 corner = center + toward * radius + (right * (uv.x * 2.0 - 1.0) + up * (uv.y * 2.0 - 1.0)) * span;
    rayOrigin = parallel ? corner : eye;
    rayDirection = parallel ? -toward : corner - eye;
    impostorFrames(arrangement, count, toward, frames, frameWeights);
    gl_Position = draw.viewProjection * (transform * vec4(corner, 1.0));
    visibility = 1.0;
    if (ranged) {
        mat4 range = poses.matrices[draw.indices.z + ANIMA_POSE_HEADER_RANGE];
        visibility = animaVisibilityAt((object * vec4(range[1].xyz, 1.0)).xyz, range[0], draw.origin);
    }
    // A copy that its range hides draws nothing, nor does one seen from inside its sphere; in the shadow pass, a copy
    // casts while more than animaShadowCastShare of it draws, as the standard material's does.
    if ((shadowPass ? abs(visibility) <= animaShadowCastShare : visibility == 0.0) ||
        (!parallel && distanceToCenter <= radius))
        gl_Position = animaCulledPosition;
}
