// What the bloom passes share (anima::Bloom): the position that sky.vert's fullscreen triangle passes, the image that a
// pass reads, its output and its push constants, which the renderer's BloomPush lays out. bloom-downsample.frag and
// bloom-upsample.frag include it.
#ifndef ANIMA_BLOOM_GLSL
#define ANIMA_BLOOM_GLSL
#include "shader_interface.h"
// The pixel's place in the level drawn, from -1 to 1 along each axis.
layout(location = 0) in vec2 screen;
layout(location = 0) out vec4 outColor;
// The scene target, in the bright pass, or the level of the chain that the pass reads.
layout(set = ANIMA_SET_SCENE_INPUT, binding = ANIMA_SCENE_INPUT_COLOR) uniform sampler2D source;
layout(push_constant) uniform BloomPass {
    // The size of a texel of source, in texture coordinates.
    vec2 texel;
    // The bright pass's EnvironmentSettings::exposure, Bloom::threshold, and knee width, the threshold times
    // Bloom::knee.
    float exposure;
    float threshold;
    float knee;
    // The weight of the level that an upsample adds.
    float weight;
}
bloomPass;
// The pixel's texture coordinates, in source as in the level drawn, since both cover the whole view.
vec2 bloomCoordinates() { return screen * 0.5 + 0.5; }
#endif
