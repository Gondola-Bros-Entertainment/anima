#version 450
#extension GL_GOOGLE_include_directive : require
layout(location = 0) in vec2 texcoord;
layout(location = 1) in float vertexAlpha;
#include "material.glsl"
void main() {
    if (material.detail.y >= 0.0 &&
        texture(baseColorTexture, texcoord).a * material.emissiveAlpha.a * vertexAlpha < material.detail.y)
        discard;
}
