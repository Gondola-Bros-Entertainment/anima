#version 450
#extension GL_GOOGLE_include_directive : require
#include "shader_interface.h"
layout(set = ANIMA_SET_SPRITE_SHEET, binding = ANIMA_SPRITE_SHEET_TEXTURE) uniform sampler2D sheet;
layout(location = 0) in vec2 frame_uv;
layout(location = 1) in vec4 tint;
layout(location = 0) out vec4 output_color;
// Straight texels times the sprite's factors, written premultiplied for the blended and additive pipelines alike.
void main() {
    vec4 texel = texture(sheet, frame_uv);
    float alpha = texel.a * tint.a;
    output_color = vec4(texel.rgb * tint.rgb * alpha, alpha);
}
