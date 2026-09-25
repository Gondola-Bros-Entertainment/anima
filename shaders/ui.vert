#version 450
layout(location = 0) in vec2 position;
layout(location = 1) in vec4 color;
layout(location = 2) in vec2 uv;
layout(location = 0) out vec4 vertex_color;
layout(location = 1) out vec2 texture_uv;
layout(push_constant) uniform View {
    mat4 transform;
    vec2 translation;
    vec2 dimensions;
}
view;
void main() {
    vec4 p = view.transform * vec4(position + view.translation, 0.0, 1.0);
    gl_Position = vec4(2.0 * p.x / view.dimensions.x - p.w, 2.0 * p.y / view.dimensions.y - p.w, 0.0, p.w);
    vertex_color = color;
    texture_uv = uv;
}
