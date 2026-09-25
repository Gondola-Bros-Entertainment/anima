#version 450
layout(set = 0, binding = 0) uniform sampler2D image;
layout(location = 0) in vec4 vertex_color;
layout(location = 1) in vec2 texture_uv;
layout(location = 0) out vec4 output_color;
vec4 linear_premultiplied(vec4 c) {
    vec3 s = c.a > 0.0 ? clamp(c.rgb / c.a, 0.0, 1.0) : vec3(0.0);
    vec3 linear = mix(pow((s + 0.055) / 1.055, vec3(2.4)), s / 12.92, lessThanEqual(s, vec3(0.04045)));
    return vec4(linear * c.a, c.a);
}
void main() { output_color = linear_premultiplied(vertex_color) * linear_premultiplied(texture(image, texture_uv)); }
