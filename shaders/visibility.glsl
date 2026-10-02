// Visibility ranges, as the standard material's shaders apply them. anima/custom_material.glsl repeats both functions
// for custom materials as animaVisibility() and animaDissolved(), and the consumer's custom materials check compares
// their pixels with these.

// The share of an object, or of a placed copy, that its visibility range draws at the distance from @p origin, the
// eye, to @p center, the center of its mesh's rest bounds as placed: 1 whole, 0 gone, dissolving across the margins,
// and negated in the begin margin, where it fades in, so that dissolved() keeps the complementary pixels there.
// @p range holds its begin, begin margin, end and end margin; the margins never overlap, so at most one of them is
// partial. An orthographic view, whose @p origin has w 0, draws everything whole.
float visibilityAt(vec3 center, vec4 range, vec4 origin) {
    if (origin.w == 0.0)
        return 1.0;
    float d = distance(center, origin.xyz);
    float rise = range.y > 0.0 ? clamp((d - range.x) / range.y, 0.0, 1.0) : (d >= range.x ? 1.0 : 0.0);
    float fall = range.w > 0.0 ? clamp((range.z - d) / range.w, 0.0, 1.0) : (d < range.z ? 1.0 : 0.0);
    return rise < 1.0 ? -rise : fall;
}

// Whether the fragment at framebuffer coordinates @p pixel falls in the share of an object that @p visibility, from
// visibilityAt(), dissolves with a 4x4 ordered dither, without blending or sorting. Fading out it keeps the pixels
// whose threshold lies below the share, and fading in, where @p visibility is negative, those whose threshold lies at
// or above 1 minus it, so an object fading in over the distances that another fades out over draws exactly the pixels
// the other leaves, as dithered LOD transitions do.
bool dissolved(float visibility, vec2 pixel) {
    const float pattern[16] =
        float[](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
    ivec2 cell = ivec2(pixel) & 3;
    float threshold = (pattern[cell.y * 4 + cell.x] + 0.5) / 16.0;
    return abs(visibility) < 1.0 && (visibility >= 0.0 ? threshold >= visibility : threshold < 1.0 + visibility);
}
