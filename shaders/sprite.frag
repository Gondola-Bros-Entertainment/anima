#version 450
#extension GL_GOOGLE_include_directive : require
#include "shader_interface.h"
layout(set = ANIMA_SET_SPRITE_SHEET, binding = ANIMA_SPRITE_SHEET_TEXTURE) uniform sampler2D sheet;
layout(location = ANIMA_SPRITE_OUT_SHEET_UV) in vec2 sheet_uv;
layout(location = ANIMA_SPRITE_OUT_COLOR) in vec4 tint;
layout(location = 0) out vec4 output_color;
// Straight texels times the sprite's factors, written premultiplied for the blended and additive pipelines alike.
void main() {
    vec4 texel = texture(sheet, sheet_uv);
    float alpha = texel.a * tint.a;
    output_color = vec4(texel.rgb * tint.rgb * alpha, alpha);
}
