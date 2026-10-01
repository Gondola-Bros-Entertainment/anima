// GLSL declarations of the custom material shader interface that anima/custom_material.hpp documents. Include it
// in shaders compiled for Vulkan 1.1, for example with `glslc --target-env=vulkan1.1 -I <anima>/include`.
//
// Every stage receives the frame block (animaFrame), the draw push constants (animaDraw) and
// animaWorldPosition(). Define these before including the file to declare more:
// - ANIMA_VERTEX in vertex shaders: the vertex attributes, the placement rows, the pose buffer, animaModelMatrix()
//   and animaWorldNormal();
// - ANIMA_OPAQUE_DEPTH and ANIMA_OPAQUE_COLOR in the fragment shader of a blended or additive material: the
//   opaque depth and color, which cost a copy on every frame that draws the material.
// Parameter blocks and textures belong to each material: declare them at set 2, binding 0, and bindings 1 to 4.
#ifndef ANIMA_CUSTOM_MATERIAL_GLSL
#define ANIMA_CUSTOM_MATERIAL_GLSL

layout(set = 0, binding = 0, std140) uniform AnimaFrame {
    mat4 viewProjection;
    mat4 inverseViewProjection;
    // The eye with w 1, or in an orthographic view the unit direction toward the camera with w 0.
    vec4 viewOrigin;
    vec4 sunDirection;
    vec4 sunRadiance;
    vec4 fillDirection;
    vec4 fillRadiance;
    vec4 ambientSky;
    vec4 ambientGround;
    vec4 ambientSpecular;
    // Fog color, and its density per unit in w.
    vec4 fog;
    // Width and height in pixels, and their reciprocals.
    vec4 viewport;
    float time;
}
animaFrame;

layout(push_constant) uniform AnimaDraw {
    // The camera's view-projection, or a shadow region's in the depth-only variant.
    mat4 viewProjection;
    uint paletteOffset;
    uint skinned;
    // With placements, the index of the object's world matrix in animaPoses, and 1; otherwise 0.
    uint objectOffset;
    uint placed;
    // The object's linear RGB factor for the material slot, with alpha 1.
    layout(offset = 80) vec4 factor;
}
animaDraw;

// The world position that Vulkan depth @p depth shows at the framebuffer coordinates @p pixel, such as
// gl_FragCoord.xy.
vec3 animaWorldPosition(vec2 pixel, float depth) {
    vec4 world = animaFrame.inverseViewProjection * vec4(pixel * animaFrame.viewport.zw * 2.0 - 1.0, depth, 1.0);
    return world.xyz / world.w;
}

#ifdef ANIMA_VERTEX
layout(location = 0) in vec3 animaPosition;
layout(location = 1) in vec3 animaNormal;
layout(location = 2) in vec3 animaColor;
layout(location = 3) in vec2 animaUv;
layout(location = 4) in uvec4 animaJoints;
layout(location = 5) in vec4 animaWeights;
layout(location = 6) in vec4 animaTangent;
layout(location = 7) in float animaAlpha;
// Rows 0 to 2 of the affine matrix of the placement being drawn, or of an identity without placements.
layout(location = 8) in vec4 animaPlacement0;
layout(location = 9) in vec4 animaPlacement1;
layout(location = 10) in vec4 animaPlacement2;
layout(set = 1, binding = 0, std430) readonly buffer AnimaPoses { mat4 matrices[]; }
animaPoses;

// The placement being drawn, relative to its object.
mat4 animaPlacement() {
    return transpose(mat4(animaPlacement0, animaPlacement1, animaPlacement2, vec4(0.0, 0.0, 0.0, 1.0)));
}
// The matrix from mesh space to world space for this vertex: its node's, or in a skinned draw its joints' matrices
// blended by weight, or for a placed copy the object's matrix times the placement times the node's rest matrix, as
// the standard material computes it.
mat4 animaModelMatrix() {
    if (animaDraw.placed != 0u)
        return animaPoses.matrices[animaDraw.objectOffset] * animaPlacement() *
               animaPoses.matrices[animaDraw.paletteOffset];
    if (animaDraw.skinned == 0u)
        return animaPoses.matrices[animaDraw.paletteOffset];
    mat4 transform = mat4(0.0);
    for (uint i = 0u; i < 4u; ++i)
        if (animaWeights[i] != 0.0)
            transform += animaPoses.matrices[animaDraw.paletteOffset + animaJoints[i]] * animaWeights[i];
    return transform;
}
// animaNormal in world space under @p transform, as the standard material computes it: the direction of the
// cofactor matrix, which stays defined when an axis collapses, or +Y when no direction is left.
vec3 animaWorldNormal(mat4 transform) {
    vec3 a = transform[0].xyz, b = transform[1].xyz, c = transform[2].xyz;
    vec3 v = cross(b, c) * animaNormal.x + cross(c, a) * animaNormal.y + cross(a, b) * animaNormal.z;
    float magnitude = length(v);
    if (!(magnitude > 1e-12) || isinf(magnitude))
        return vec3(0, 1, 0);
    return v / (dot(a, cross(b, c)) < 0.0 ? -magnitude : magnitude);
}
#endif

#ifdef ANIMA_OPAQUE_DEPTH
layout(set = 0, binding = 1) uniform sampler2D animaOpaqueDepth;
#endif
#ifdef ANIMA_OPAQUE_COLOR
layout(set = 0, binding = 2) uniform sampler2D animaOpaqueColor;
#endif

#endif
