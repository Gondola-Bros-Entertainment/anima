#version 450
#extension GL_GOOGLE_include_directive : require
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec3 color;
layout(location = 3) in vec2 uv;
layout(location = 4) in uvec4 joints;
layout(location = 5) in vec4 weights;
layout(location = 6) in vec4 tangent;
layout(location = 7) in float alpha;
// Rows 0 to 2 of the placement's affine matrix, per instance; an identity for an object without placements.
layout(location = 8) in vec4 placement0;
layout(location = 9) in vec4 placement1;
layout(location = 10) in vec4 placement2;
layout(location = 4) out vec4 worldTangent;
layout(location = 5) out float vertexAlpha;
layout(location = 0) out vec3 worldNormal;
layout(location = 1) out vec3 baseColor;
layout(location = 2) out vec2 texcoord;
layout(location = 3) out vec3 worldPosition;
layout(location = 6) flat out float orientation;
layout(location = 7) flat out float visibility;
layout(set = 2, binding = 0, std430) readonly buffer Poses { mat4 matrices[]; }
poses;
layout(push_constant) uniform Draw {
    layout(offset = 0) mat4 viewProjection;
    // The eye with w 1, or in an orthographic view the direction toward the camera with w 0.
    layout(offset = 64) vec4 origin;
    layout(offset = 96) uvec4 indices;
    layout(offset = 112) vec4 factor;
}
draw;
#include "visibility.glsl"
// The point @p p placed by this copy's placement, whose rows dot it as a homogeneous point.
vec3 placePoint(vec3 p) {
    vec4 h = vec4(p, 1.0);
    return vec3(dot(placement0, h), dot(placement1, h), dot(placement2, h));
}
void main() {
    // indices: the draw's first palette matrix; whether it is skinned; with placements or a visibility range, the
    // object's world matrix, followed by a matrix whose first column is its range and whose second holds the center
    // of its mesh's rest bounds; and flags, 1 with placements, when the palette matrix is the node's in the rest pose,
    // and 2 with a range.
    mat4 transform = poses.matrices[draw.indices.x];
    bool placed = (draw.indices.w & 1u) != 0u, ranged = (draw.indices.w & 2u) != 0u;
    if (!placed && draw.indices.y != 0) {
        transform = mat4(0.0);
        for (uint i = 0; i < 4; ++i)
            if (weights[i] != 0.0)
                transform += poses.matrices[draw.indices.x + joints[i]] * weights[i];
    }
    // Every matrix here is affine, so the world matrix composes as a mat3 and a translation; mat4 products would
    // spend nearly twice the multiply-adds per vertex.
    mat3 linear = mat3(transform);
    vec3 translation = transform[3].xyz;
    // The object's world matrix; with placements it places the mesh after the copy's placement.
    mat4 object = placed || ranged ? poses.matrices[draw.indices.z] : mat4(1.0);
    if (placed) {
        linear = mat3(object) * transpose(mat3(placement0.xyz, placement1.xyz, placement2.xyz)) * linear;
        translation = mat3(object) * placePoint(translation) + object[3].xyz;
    }
    // As the CPU's normal(): the cofactor matrix of the composed matrix, with its determinant's sign, has the
    // inverse transpose's direction and stays defined when an axis collapses, leaving the flattened surface's
    // normal. A normal with no direction left, or a nonfinite one, becomes +Y, so lighting never sees NaNs.
    vec3 a = linear[0], b = linear[1], c = linear[2];
    float determinant = dot(a, cross(b, c));
    vec3 v = cross(b, c) * normal.x + cross(c, a) * normal.y + cross(a, b) * normal.z;
    float magnitude = length(v);
    worldNormal = magnitude > 1e-12 && !isinf(magnitude) && !isnan(magnitude)
                      ? v / (determinant < 0.0 ? -magnitude : magnitude)
                      : vec3(0, 1, 0);
    worldTangent = vec4(linear * tangent.xyz, tangent.w * (determinant < 0.0 ? -1.0 : 1.0));
    // A negative determinant reverses winding, as glTF specifies for mirrored nodes, so the fragment shader
    // flips gl_FrontFacing by this sign. The provoking (first) vertex supplies it for the whole triangle.
    orientation = determinant < 0.0 ? -1.0 : 1.0;
    vertexAlpha = alpha;
    worldPosition = linear * position + translation;
    gl_Position = draw.viewProjection * vec4(worldPosition, 1.0);
    visibility = 1.0;
    if (ranged) {
        mat4 range = poses.matrices[draw.indices.z + 1u];
        vec3 center = placed ? placePoint(range[1].xyz) : range[1].xyz;
        visibility = visibilityAt(mat3(object) * center + object[3].xyz, range[0], draw.origin);
    }
    // A copy that its range hides draws nothing: every corner leaves the view volume.
    if (visibility == 0.0)
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    baseColor = color * draw.factor.rgb;
    texcoord = uv;
}
