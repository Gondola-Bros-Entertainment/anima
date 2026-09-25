#version 450
layout(location = 0) in vec3 position;
layout(location = 3) in vec2 uv;
layout(location = 4) in uvec4 joints;
layout(location = 5) in vec4 weights;
layout(location = 7) in float alpha;
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
    mat4 transform = poses.matrices[draw.indices.x];
    if (draw.indices.y != 0) {
        transform = mat4(0);
        for (uint i = 0; i < 4; ++i)
            if (weights[i] != 0.0)
                transform += poses.matrices[draw.indices.x + joints[i]] * weights[i];
    }
    gl_Position = draw.viewProjection * transform * vec4(position, 1);
    texcoord = uv;
    vertexAlpha = alpha;
}
