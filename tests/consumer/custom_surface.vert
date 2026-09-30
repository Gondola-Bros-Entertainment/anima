#version 450
// The application's vertex shader for its water and effect materials: places each vertex, skinned or rigid, as
// the standard material does.
#extension GL_GOOGLE_include_directive : require
#define ANIMA_VERTEX
#include "anima/custom_material.glsl"
layout(location = 0) out vec3 worldPosition;
layout(location = 1) out vec2 texcoord;
void main() {
    mat4 model = animaModelMatrix();
    worldPosition = (model * vec4(animaPosition, 1.0)).xyz;
    texcoord = animaUv;
    gl_Position = animaDraw.viewProjection * vec4(worldPosition, 1.0);
}
