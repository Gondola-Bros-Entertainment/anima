// GLSL declarations of the custom material shader interface that anima/custom_material.hpp documents. Include it
// in shaders compiled for Vulkan 1.1, for example with `glslc --target-env=vulkan1.1 -I <anima>/include`.
//
// Every stage receives the draw push constants (animaDraw) and what frame.glsl, which it includes, declares: the frame
// block (animaFrame), animaWorldPosition() and animaFogged(), and the declarations of fog.glsl and visibility.glsl,
// such as animaDissolved().
// Define these before including the file to declare more:
// - ANIMA_VERTEX in vertex shaders: the vertex attributes, the placement rows, the pose buffer, animaModelMatrix(),
//   animaWorldNormal() and animaVisibility();
// - ANIMA_OPAQUE_DEPTH and ANIMA_OPAQUE_COLOR in the fragment shader of a blended or additive shader: the opaque
//   depth and color, which cost a copy on every frame that draws a material of the shader.
// Parameter blocks and textures belong to each material: declare them at set 2, binding 0, and bindings 1 to 4.
#ifndef ANIMA_CUSTOM_MATERIAL_GLSL
#define ANIMA_CUSTOM_MATERIAL_GLSL

#include "frame.glsl"

layout(push_constant) uniform AnimaDraw {
    // The camera's view-projection, or in the depth-only variant a shadow pass's: a cascade's or the detail region's.
    mat4 viewProjection;
    uint paletteOffset;
    uint skinned;
    // With placements or a visibility range, the index in animaPoses of the object's world matrix, which the matrix
    // that holds its range follows.
    uint objectOffset;
    // 1 with placements; otherwise 0.
    uint placed;
    // The object's linear RGB factor for the material slot, with alpha 1.
    layout(offset = 80) vec4 factor;
    // 1 when the object has a visibility range, whose share animaVisibility() computes; otherwise 0.
    uint ranged;
}
animaDraw;

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
// The share of this object, or of this placed copy, that its visibility range draws at the distance from the eye to
// the center of its mesh's rest bounds as placed, as the standard material computes it with animaVisibilityAt(),
// negated while it fades in: 0 outside the range, from 0 to -1 across the begin margin, 1 between the margins, from 1
// to 0 across the end margin, and 1 without a range or in an orthographic view. abs() of it is the share. Pass it to
// the fragment shader, flat, for animaDissolved(); a depth-only variant casts while abs() of it exceeds
// animaShadowCastShare, and otherwise moves every vertex to animaCulledPosition, as the standard material does.
float animaVisibility() {
    if (animaDraw.ranged == 0u)
        return 1.0;
    mat4 object = animaPoses.matrices[animaDraw.objectOffset];
    if (animaDraw.placed != 0u)
        object = object * animaPlacement();
    mat4 range = animaPoses.matrices[animaDraw.objectOffset + 1u];
    return animaVisibilityAt((object * vec4(range[1].xyz, 1.0)).xyz, range[0], animaFrame.viewOrigin);
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
