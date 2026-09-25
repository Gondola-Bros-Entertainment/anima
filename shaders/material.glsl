layout(set = 0, binding = 0) uniform sampler2D baseColorTexture;
layout(set = 0, binding = 1) uniform sampler2D normalTexture;
layout(set = 0, binding = 2) uniform sampler2D metallicRoughnessTexture;
layout(set = 0, binding = 3) uniform sampler2D emissiveTexture;
layout(set = 0, binding = 4) uniform sampler2D occlusionTexture;
layout(set = 0, binding = 5, std140) uniform MaterialData {
    vec4 emissiveAlpha; // RGB radiance, base alpha
    vec4 detail;        // normal scale, mask cutoff (-1 means opaque), occlusion strength, unlit
    vec4 maps;          // normal map present
}
material;
