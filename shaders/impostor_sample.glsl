// Sampling an impostor's atlas, which impostor.frag and impostor-shadow.frag share. Include it after material.glsl and
// impostor.glsl: the material's base color is the atlas's color, its normal map the normals and depth, and its
// metallic-roughness map the surface values. Sampling runs in two steps so that a pixel the dither or the coverage
// discards pays only for the first: impostorHit() for every pixel, and impostorShade() for the pixels that stay.

// The atlas coordinates of the point @p offset from the sphere's center on the plane of @p frame, whose axes are
// @p right and @p up, kept @p inset, a share of the frame, inside it.
vec2 impostorAtlas(uvec2 frame, uint count, vec3 offset, vec3 right, vec3 up, float radius, float inset) {
    vec2 local = vec2(dot(offset, right), dot(offset, up)) / (2.0 * radius) + 0.5;
    local = clamp(local, vec2(inset), vec2(1.0 - inset));
    return (vec2(frame) + local) / float(count);
}
// Where a pixel's ray meets the surfaces of its three frames, in the mesh's space.
struct ImpostorHit {
    // Each frame's atlas coordinates, after the parallax step, and its share of the blend, 0 for a frame whose plane
    // the ray runs along.
    vec2 atlas[3];
    vec3 weights;
    // The mip level that every sample reads.
    float lod;
    // Linear base color and coverage, blended.
    vec4 color;
    // The surface point, blended.
    vec3 point;
};
// The frames @p frames of an atlas of @p count per side in @p arrangement around @p sphere's center and radius, met by
// the ray from @p origin along @p direction, blended by @p weights. In each, the ray crosses the frame's plane, steps
// once along itself to the height stored there, as parallax mapping steps, which keeps the frames' features aligned as
// they blend, and reads the color where that leaves it; the surface point is the ray's at that height. The mip level is
// chosen once, from the derivatives of the heaviest frame's crossing while every pixel of the quad still runs, and
// every sample reads it explicitly, so that a later discard leaves no sample undefined.
ImpostorHit impostorHit(uint count, uint arrangement, vec4 sphere, vec3 origin, vec3 direction, uvec3 frames,
                        vec3 weights) {
    ImpostorHit hit;
    float frameTexels = float(textureSize(baseColorTexture, 0).x) / float(count);
    vec3 crossings[3], directions[3], rights[3], ups[3];
    float steps[3];
    float total = 0.0;
    for (int k = 0; k < 3; ++k) {
        impostorFrame(arrangement, count, impostorUnpack(frames[k]), directions[k], rights[k], ups[k]);
        float along = dot(direction, directions[k]);
        hit.weights[k] = abs(along) > 1e-6 ? weights[k] : 0.0;
        steps[k] = hit.weights[k] > 0.0 ? 1.0 / along : -1.0;
        crossings[k] = origin + direction * (dot(sphere.xyz - origin, directions[k]) * steps[k]);
        hit.atlas[k] = impostorAtlas(impostorUnpack(frames[k]), count, crossings[k] - sphere.xyz, rights[k], ups[k],
                                     sphere.w, 0.5 / frameTexels);
        total += hit.weights[k];
    }
    hit.weights /= max(total, 1e-6);
    int heaviest = weights.x >= weights.y && weights.x >= weights.z ? 0 : weights.y >= weights.z ? 1 : 2;
    // The coarsest level keeps four texels across a frame, and frames that split evenly into its texels, so that no
    // texel of a level straddles two frames.
    float coarsest = max(min(log2(frameTexels) - 2.0, float(findLSB(uint(frameTexels)))), 0.0);
    hit.lod = clamp(textureQueryLod(baseColorTexture, hit.atlas[heaviest]).y, 0.0, coarsest);
    // Linear filtering reads half a texel around a sample, of the coarser of the two levels that it blends, so samples
    // keep that far inside their frame.
    float inset = 0.5 * exp2(ceil(hit.lod)) / frameTexels;
    hit.color = vec4(0);
    hit.point = vec3(0);
    for (int k = 0; k < 3; ++k) {
        uvec2 frame = impostorUnpack(frames[k]);
        vec2 crossing = impostorAtlas(frame, count, crossings[k] - sphere.xyz, rights[k], ups[k], sphere.w, inset);
        float height = (textureLod(normalTexture, crossing, hit.lod).a * 2.0 - 1.0) * sphere.w;
        vec3 surface = crossings[k] + direction * (height * steps[k]);
        hit.atlas[k] =
            impostorAtlas(frame, count, surface - directions[k] * height - sphere.xyz, rights[k], ups[k], sphere.w, inset);
        hit.color += textureLod(baseColorTexture, hit.atlas[k], hit.lod) * hit.weights[k];
        hit.point += surface * hit.weights[k];
    }
    return hit;
}
// What @p hit's frames store beside color, blended: the normal in the mesh's space, not normalized, occlusion,
// roughness and metallic in @p surface's r, g and b, and, when @p emits, the emissive map's color.
void impostorShade(ImpostorHit hit, bool emits, out vec3 normal, out vec4 surface, out vec3 emission) {
    normal = vec3(0);
    surface = vec4(0);
    emission = vec3(0);
    for (int k = 0; k < 3; ++k) {
        normal += (textureLod(normalTexture, hit.atlas[k], hit.lod).xyz * 2.0 - 1.0) * hit.weights[k];
        surface += textureLod(metallicRoughnessTexture, hit.atlas[k], hit.lod) * hit.weights[k];
        if (emits)
            emission += textureLod(emissiveTexture, hit.atlas[k], hit.lod).rgb * hit.weights[k];
    }
}
