#version 450
// The application's fading surface: places each vertex as the standard material does and passes on the share of the
// object, or of the placed copy, that its visibility range draws, for the fragment shader to dissolve.
#extension GL_GOOGLE_include_directive : require
#define ANIMA_VERTEX
#include "anima/custom_material.glsl"
layout(location = 0) flat out float visibility;
void main() {
    mat4 model = animaModelMatrix();
    vec3 world = mat3(model) * animaPosition + model[3].xyz;
    gl_Position = animaDraw.viewProjection * vec4(world, 1.0);
    visibility = animaVisibility();
}
