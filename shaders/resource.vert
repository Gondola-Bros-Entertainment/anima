#version 450
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec3 color;
layout(location = 3) in vec2 uv;
layout(location = 4) in uvec4 joints;
layout(location = 5) in vec4 weights;
layout(location = 6) in vec4 tangent;
layout(location = 7) in float alpha;
layout(location = 4) out vec4 worldTangent;
layout(location = 5) out float vertexAlpha;
layout(location = 0) out vec3 worldNormal;
layout(location = 1) out vec3 baseColor;
layout(location = 2) out vec2 texcoord;
layout(location = 3) out vec3 worldPosition;
layout(location = 6) flat out float orientation;
layout(set = 2, binding = 0, std430) readonly buffer Poses { mat4 matrices[]; }
poses;
layout(push_constant) uniform Draw {
    layout(offset = 0) mat4 viewProjection;
    layout(offset = 96) uvec4 indices;
    layout(offset = 112) vec4 factor;
}
draw;
void main() {
    mat4 transform = poses.matrices[draw.indices.x];
    if (draw.indices.y != 0) {
        transform = mat4(0.0);
        for (uint i = 0; i < 4; ++i)
            if (weights[i] != 0.0)
                transform += poses.matrices[draw.indices.x + joints[i]] * weights[i];
    }
    // As the CPU's normal(): the cofactor matrix of the blended matrix, with its determinant's sign, has the
    // inverse transpose's direction and stays defined when an axis collapses, leaving the flattened surface's
    // normal. A normal with no direction left, or a nonfinite one, becomes +Y, so lighting never sees NaNs.
    vec3 a = transform[0].xyz, b = transform[1].xyz, c = transform[2].xyz;
    float determinant = dot(a, cross(b, c));
    vec3 v = cross(b, c) * normal.x + cross(c, a) * normal.y + cross(a, b) * normal.z;
    float magnitude = length(v);
    worldNormal = magnitude > 1e-12 && !isinf(magnitude) && !isnan(magnitude)
                      ? v / (determinant < 0.0 ? -magnitude : magnitude)
                      : vec3(0, 1, 0);
    worldTangent = vec4(mat3(transform) * tangent.xyz, tangent.w * (determinant < 0.0 ? -1.0 : 1.0));
    // A negative determinant reverses winding, as glTF specifies for mirrored nodes, so the fragment shader
    // flips gl_FrontFacing by this sign. The provoking (first) vertex supplies it for the whole triangle.
    orientation = determinant < 0.0 ? -1.0 : 1.0;
    vertexAlpha = alpha;
    worldPosition = mat3(transform) * position + transform[3].xyz;
    gl_Position = draw.viewProjection * vec4(worldPosition, 1.0);
    baseColor = color * draw.factor.rgb;
    texcoord = uv;
}
