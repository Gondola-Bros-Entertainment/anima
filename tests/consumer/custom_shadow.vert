#version 450
// The depth-only variant of the application's effect: the same placement, through the shadow region's matrix.
#extension GL_GOOGLE_include_directive : require
#define ANIMA_VERTEX
#include "anima/custom_material.glsl"
void main() { gl_Position = animaDraw.viewProjection * (animaModelMatrix() * vec4(animaPosition, 1.0)); }
