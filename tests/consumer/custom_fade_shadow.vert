#version 450
// The depth-only variant of the application's fading surface: the same placement through the shadow region's matrix,
// casting while more than half of the object draws, as the standard material casts. animaVisibility() is negative
// while the object fades in, so its absolute value is the share that draws.
#extension GL_GOOGLE_include_directive : require
#define ANIMA_VERTEX
#include "anima/custom_material.glsl"
void main() {
    gl_Position = animaDraw.viewProjection * animaModelMatrix() * vec4(animaPosition, 1.0);
    if (abs(animaVisibility()) <= 0.5)
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
}
