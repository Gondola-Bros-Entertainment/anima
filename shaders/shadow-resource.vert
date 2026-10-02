#version 450
#extension GL_GOOGLE_include_directive : require
layout(location = 0) in vec3 position;
layout(location = 3) in vec2 uv;
layout(location = 4) in uvec4 joints;
layout(location = 5) in vec4 weights;
layout(location = 7) in float alpha;
// Rows 0 to 2 of the placement's affine matrix, per instance; an identity for an object without placements.
layout(location = 8) in vec4 placement0;
layout(location = 9) in vec4 placement1;
layout(location = 10) in vec4 placement2;
layout(location = 0) out vec2 texcoord;
layout(location = 1) out float vertexAlpha;
layout(set = 2, binding = 0, std430) readonly buffer Poses { mat4 matrices[]; }
poses;
layout(push_constant) uniform Draw {
    layout(offset = 0) mat4 viewProjection;
    // The camera's eye with w 1, or its direction with w 0 in an orthographic view; ranges are measured from it.
    layout(offset = 64) vec4 origin;
    layout(offset = 96) uvec4 indices;
}
draw;
#include "visibility.glsl"
void main() {
    // indices as in resource.vert.
    mat4 transform = poses.matrices[draw.indices.x];
    bool placed = (draw.indices.w & 1u) != 0u, ranged = (draw.indices.w & 2u) != 0u;
    // The object's world matrix, or with placements this copy's.
    mat4 object = placed || ranged ? poses.matrices[draw.indices.z] : mat4(1.0);
    if (placed) {
        object = object * transpose(mat4(placement0, placement1, placement2, vec4(0, 0, 0, 1)));
        transform = object * transform;
    } else if (draw.indices.y != 0) {
        transform = mat4(0);
        for (uint i = 0; i < 4; ++i)
            if (weights[i] != 0.0)
                transform += poses.matrices[draw.indices.x + joints[i]] * weights[i];
    }
    gl_Position = draw.viewProjection * transform * vec4(position, 1);
    // A copy casts while more than half of it draws; a dithered shadow map would speckle.
    if (ranged) {
        mat4 range = poses.matrices[draw.indices.z + 1u];
        if (abs(visibilityAt((object * vec4(range[1].xyz, 1.0)).xyz, range[0], draw.origin)) <= 0.5)
            gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    }
    texcoord = uv;
    vertexAlpha = alpha;
}
