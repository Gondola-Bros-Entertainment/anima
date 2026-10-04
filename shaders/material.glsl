#include "shader_interface.h"
layout(set = ANIMA_SET_MATERIAL, binding = ANIMA_MATERIAL_BASE_COLOR) uniform sampler2D baseColorTexture;
layout(set = ANIMA_SET_MATERIAL, binding = ANIMA_MATERIAL_NORMAL) uniform sampler2D normalTexture;
layout(set = ANIMA_SET_MATERIAL,
       binding = ANIMA_MATERIAL_METALLIC_ROUGHNESS) uniform sampler2D metallicRoughnessTexture;
layout(set = ANIMA_SET_MATERIAL, binding = ANIMA_MATERIAL_EMISSIVE) uniform sampler2D emissiveTexture;
layout(set = ANIMA_SET_MATERIAL, binding = ANIMA_MATERIAL_OCCLUSION) uniform sampler2D occlusionTexture;
layout(set = ANIMA_SET_MATERIAL, binding = ANIMA_MATERIAL_UNIFORM, std140) uniform MaterialData {
    vec4 emissiveAlpha; // RGB radiance, base alpha
    vec4 detail;        // normal scale, mask cutoff (-1 means opaque), occlusion strength, unlit
    vec4 maps;          // normal map present, double-sided
}
material;
