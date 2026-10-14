#version 450
#extension GL_GOOGLE_include_directive : require
#include "shader_interface.h"
// One sprite per instance, as two counterclockwise triangles over the corners of its quad, which gl_VertexIndex picks.
layout(location = ANIMA_SPRITE_PLACEMENT) in vec4 placement; // Position, then rotation in radians.
layout(location = ANIMA_SPRITE_SHAPE) in vec4 shape;         // Size, then pivot as fractions of it.
layout(location = ANIMA_SPRITE_FRAME) in vec4 frame;         // Texture coordinates at the first texel, then past the last.
layout(location = ANIMA_SPRITE_COLOR) in vec4 color;
layout(push_constant) uniform View {
    mat4 view_projection;
}
view;
layout(location = 0) out vec2 frame_uv;
layout(location = 1) out vec4 tint;
// Corners in the quad's own axes, from its -X, -Y corner.
const vec2 corners[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(1, 1), vec2(0, 0), vec2(1, 1), vec2(0, 1));
void main() {
    vec2 corner = corners[gl_VertexIndex];
    vec2 local = (corner - shape.zw) * shape.xy;
    float c = cos(placement.w), s = sin(placement.w);
    vec2 turned = vec2(c * local.x - s * local.y, s * local.x + c * local.y);
    gl_Position = view.view_projection * vec4(placement.xy + turned, placement.z, 1.0);
    // The frame's first row lies along the quad's +Y edge.
    frame_uv = vec2(mix(frame.x, frame.z, corner.x), mix(frame.w, frame.y, corner.y));
    tint = color;
}
