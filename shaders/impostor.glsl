// Impostor frames as ImpostorFrames in anima/mesh.hpp lays them out, which impostor.vert, impostor.frag and
// impostor-shadow.frag share with the baker in src/assets/impostor.cpp. An arrangement of ANIMA_IMPOSTOR_HEMISPHERE is
// the hemisphere, and any other the sphere.
#include "shader_interface.h"

// The direction from which frame @p frame of a grid of @p count per side views, unnormalized, from the grid point's
// offsets from the center in units of half a cell, which are integers: their zeros are exact on every device, as the
// documented fallback for the axes at a pole requires.
ivec3 impostorFrameOffsets(uint arrangement, uint count, uvec2 frame) {
    int span = int(count) - 1;
    ivec2 g = 2 * ivec2(frame) - span;
    if (arrangement == ANIMA_IMPOSTOR_HEMISPHERE) {
        // x = (u + v) / 2 and z = (u - v) / 2 of the hemi-octahedral point, scaled by 2 span.
        ivec3 d = ivec3(g.x + g.y, 0, g.x - g.y);
        d.y = 2 * span - abs(d.x) - abs(d.z);
        return d;
    }
    ivec3 d = ivec3(g.x, span - abs(g.x) - abs(g.y), g.y);
    if (d.y < 0)
        d.xz = ivec2((span - abs(g.y)) * (g.x < 0 ? -1 : 1), (span - abs(g.x)) * (g.y < 0 ? -1 : 1));
    return d;
}
// The unit direction and texture axes of frame @p frame.
void impostorFrame(uint arrangement, uint count, uvec2 frame, out vec3 direction, out vec3 right, out vec3 up) {
    ivec3 d = impostorFrameOffsets(arrangement, count, frame);
    direction = normalize(vec3(d));
    right = d.x == 0 && d.z == 0 ? vec3(1, 0, 0) : normalize(vec3(direction.z, 0, -direction.x));
    up = cross(direction, right);
}
// The position of unit direction @p v on the grid, from 0 to count - 1 along each axis; a hemisphere's grid holds the
// directions below its horizon at the horizon.
vec2 impostorGrid(uint arrangement, uint count, vec3 v) {
    if (arrangement == ANIMA_IMPOSTOR_HEMISPHERE) {
        v.y = max(v.y, 0.0);
        if (abs(v.x) + abs(v.y) + abs(v.z) < 1e-6)
            v = vec3(1, 0, 0);
    }
    vec3 p = v / (abs(v.x) + abs(v.y) + abs(v.z));
    vec2 uv = arrangement == ANIMA_IMPOSTOR_HEMISPHERE ? vec2(p.x + p.z, p.x - p.z) : p.xz;
    if (arrangement != ANIMA_IMPOSTOR_HEMISPHERE && p.y < 0.0)
        uv = (1.0 - abs(uv.yx)) * vec2(uv.x < 0.0 ? -1.0 : 1.0, uv.y < 0.0 ? -1.0 : 1.0);
    return clamp((uv * 0.5 + 0.5) * float(count - 1u), vec2(0), vec2(float(count - 1u)));
}
// The three frames whose directions surround @p v on the grid, packed as column | row << 8, and their barycentric
// weights.
void impostorFrames(uint arrangement, uint count, vec3 v, out uvec3 frames, out vec3 weights) {
    vec2 g = impostorGrid(arrangement, count, v);
    vec2 cell = min(floor(g), vec2(float(count - 2u)));
    vec2 f = g - cell;
    uvec2 c = uvec2(cell);
    uint corner = c.x | c.y << 8;
    if (f.x + f.y < 1.0) {
        frames = uvec3(corner, corner + 1u, corner + 256u);
        weights = vec3(1.0 - f.x - f.y, f.x, f.y);
    } else {
        frames = uvec3(corner + 257u, corner + 1u, corner + 256u);
        weights = vec3(f.x + f.y - 1.0, 1.0 - f.y, 1.0 - f.x);
    }
}
uvec2 impostorUnpack(uint frame) { return uvec2(frame & 255u, frame >> 8); }
