#version 450
// The application's probe of the frame inputs: writes the one that its parameter selects as a color.
#extension GL_GOOGLE_include_directive : require
#include "anima/custom_material.glsl"
layout(location = 0) in vec3 worldPosition;
layout(location = 1) in vec2 texcoord;
layout(location = 0) out vec4 outColor;
layout(set = 2, binding = 0, std140) uniform Probe {
    // x: the input to write.
    uvec4 select;
}
probe;
void main() {
    vec3 color = vec3(1, 0, 1);
    switch (probe.select.x) {
    case 0u:
        color = animaFrame.sunRadiance.rgb;
        break;
    case 1u:
        color = animaFrame.fillRadiance.rgb;
        break;
    case 2u:
        color = animaFrame.ambientSky.rgb;
        break;
    case 3u:
        color = animaFrame.ambientGround.rgb;
        break;
    case 4u:
        color = animaFrame.ambientSpecular.rgb;
        break;
    case 5u:
        color = animaFrame.fog.rgb;
        break;
    case 6u:
        color = vec3(animaFrame.fog.w, animaFrame.viewport.x * animaFrame.viewport.w / 2.0,
                     animaFrame.viewport.y * animaFrame.viewport.z);
        break;
    case 7u:
        color = animaFrame.sunDirection.xyz * 0.5 + 0.5;
        break;
    case 8u:
        color = animaFrame.fillDirection.xyz * 0.5 + 0.5;
        break;
    case 9u:
        color = animaFrame.viewOrigin.xyz * 0.1 + animaFrame.viewOrigin.w * 0.25;
        break;
    case 10u:
        color = vec3(animaFrame.viewProjection == animaDraw.viewProjection ? 0.25 : 0.75,
                     length(animaFrame.inverseViewProjection * animaFrame.viewProjection * vec4(1, 2, 3, 1) -
                            vec4(1, 2, 3, 1)),
                     0.5);
        break;
    }
    outColor = vec4(color, 1.0);
}
