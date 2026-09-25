#version 450
layout(location = 0) out vec3 color;
layout(push_constant) uniform View { vec2 scale; }
view;
const vec2 positions[3] = vec2[](vec2(0.0, -0.65), vec2(0.6, 0.45), vec2(-0.6, 0.45));
const vec3 colors[3] = vec3[](vec3(0.95, 0.70, 0.30), vec3(0.05, 0.65, 0.60), vec3(0.85, 0.90, 0.85));
void main() {
    gl_Position = vec4(positions[gl_VertexIndex] * view.scale, 0.0, 1.0);
    color = colors[gl_VertexIndex];
}
