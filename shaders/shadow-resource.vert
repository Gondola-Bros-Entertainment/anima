#version 450
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
    layout(offset = 96) uvec4 indices;
}
draw;
void main() {
    // indices: the draw's first palette matrix, whether it is skinned, and with placements, the object's world matrix
    // and 1, when the palette matrix is the node's in the rest pose.
    mat4 transform = poses.matrices[draw.indices.x];
    if (draw.indices.w != 0u)
        transform = poses.matrices[draw.indices.z] *
                    transpose(mat4(placement0, placement1, placement2, vec4(0, 0, 0, 1))) * transform;
    else if (draw.indices.y != 0) {
        transform = mat4(0);
        for (uint i = 0; i < 4; ++i)
            if (weights[i] != 0.0)
                transform += poses.matrices[draw.indices.x + joints[i]] * weights[i];
    }
    gl_Position = draw.viewProjection * transform * vec4(position, 1);
    texcoord = uv;
    vertexAlpha = alpha;
}
