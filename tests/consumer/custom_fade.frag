#version 450
// The application's fading surface: the object's material factor as its one color, of which the ordered dither
// discards the share that the object's visibility range hides, as the standard material discards it.
#extension GL_GOOGLE_include_directive : require
#include "anima/custom_material.glsl"
layout(location = 0) flat in float visibility;
layout(location = 0) out vec4 outColor;
void main() {
    if (animaDissolved(visibility, gl_FragCoord.xy))
        discard;
    outColor = vec4(animaDraw.factor.rgb, 1.0);
}
