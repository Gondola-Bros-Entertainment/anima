#version 450
// The application's ramp patch for the tone mapping check: writes its object's factor times the scale in its
// parameter block, unlit and unfogged, so that each patch holds a chosen linear color above 1.
#extension GL_GOOGLE_include_directive : require
#include "anima/custom_material.glsl"
layout(location = 0) out vec4 outColor;
layout(set = 2, binding = 0, std140) uniform Ramp {
    // x: the power of 2 that multiplies the factor, which is at most 1.
    vec4 scale;
}
ramp;
void main() { outColor = vec4(animaDraw.factor.rgb * ramp.scale.x, 1.0); }
