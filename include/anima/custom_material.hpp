#pragma once
#include <anima/assets/asset.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/// @file
/// Custom materials: application shaders that VulkanRenderer runs on scene meshes in place of their Material.
///
/// Part of the `anima::assets` target, which validates custom materials without a display or GPU. A custom
/// material pairs SPIR-V vertex and fragment shaders that the application compiles, for example with the Vulkan
/// SDK's glslc, with a bounded parameter block, textures and a blend mode, as a ShaderMaterial does in Godot or a
/// material with a custom shader does in Unity. The application supplies only shading: the renderer creates the
/// pipelines and owns pass order, depth, sorting, synchronization and every Vulkan object. Scene::set_custom_material
/// assigns a custom material to one material slot of one object, and scene and prefab documents store its name.
///
/// ## Shader interface
///
/// Shaders can read only the inputs below, which `anima/custom_material.glsl`, beside this header, declares for
/// GLSL. Compile each stage for SPIR-V 1.0 to 1.3 (glslc `--target-env=vulkan1.1`) with an entry point named `main`.
/// Every name refers to the GLSL declaration in that file.
///
/// Vertex attributes, read only by vertex shaders, are the mesh's SourceVertex fields in mesh space: location 0
/// `vec3` position, 1 `vec3` normal, 2 `vec3` color, 3 `vec2` uv, 4 `uvec4` joints, 5 `vec4` weights, 6 `vec4`
/// tangent and 7 `float` alpha. A shader may read any subset.
///
/// Descriptors:
/// - Set 0, binding 0, any stage: the `std140` uniform block `AnimaFrame`, the same for every draw of a frame. At
///   offset 0 `mat4 viewProjection`, the camera's column-major Vulkan view-projection that
///   VulkanRenderer::set_view received; 64 `mat4 inverseViewProjection`, its inverse; 128 `vec4 viewOrigin`, the eye
///   position with w 1 in a perspective view, or the unit direction toward the camera with w 0 in an orthographic
///   one; then Environment's lights as `vec4`s with w 0: 144 `sunDirection` (unit, toward the sun), 160
///   `sunRadiance`, 176 `fillDirection`, 192 `fillRadiance`, 208 `ambientSky`, 224 `ambientGround` and 240
///   `ambientSpecular`; 256 `vec4 fog`, the fog color with its density per unit in w (0 for none); 272 `vec4
///   viewport`, the target's width and height in pixels and their reciprocals; and 288 `float time`, the seconds
///   that VulkanRenderer::set_time received.
/// - Set 0, binding 1, fragment shaders of blended and additive materials only: `sampler2D` opaque depth, the
///   depth buffer after every opaque draw of the frame, Vulkan depth from 0 at the near plane to 1 at the far one in
///   `r`, sampled with nearest filtering and clamped at the edges.
/// - Set 0, binding 2, under the same rule: `sampler2D` opaque color, the scene's linear color after every opaque
///   draw, sky included, before exposure and tone mapping, sampled with linear filtering and clamped at the edges.
/// - Set 1, binding 0, vertex shaders only: the `readonly` `std430` storage buffer `AnimaPoses` of `mat4
///   matrices[]`, the posed palettes of the frame (Scene::Instance::palette). A rigid draw uses
///   `matrices[paletteOffset]`; a skinned vertex blends `matrices[paletteOffset + joints[i]]` by `weights[i]`.
/// - Set 2, binding 0, any stage: the parameter block, a uniform block that holds
///   CustomMaterialDefinition::parameters from its start, laid out as the shader declares it.
/// - Set 2, bindings 1 to 4, any stage: `sampler2D` for CustomMaterialDefinition::textures 0 to 3, sampled as their
///   Sampler states, through mip chains built as texture_mips() builds them when Sampler::mipmapped is set.
///
/// Push constants, any stage: the block `AnimaDraw`, per draw. At offset 0 `mat4 viewProjection`, the matrix of
/// the pass being drawn: the camera's, or a shadow region's in the depth-only variant; 64 `uint paletteOffset`,
/// IndexedDraw::palette_offset within the instance's palette; 68 `uint skinned`, 1 for a skinned draw and
/// otherwise 0; and 80 `vec4 factor`, the object's linear RGB factor for the material slot (Scene::set_material_factor)
/// with alpha 1.
///
/// Stages pass values at locations 0 to 15 as 32-bit scalars or vectors, and every fragment shader input must be
/// a vertex shader output of the same type. The fragment shader writes one `vec4` at location 0 into the linear
/// `RGBA16F` scene target, whose display conversion applies exposure and tone mapping; the shadow fragment shader
/// writes no color. `gl_Position` is in Vulkan clip space, with Y down and depth 0 to 1. Built-in variables such as
/// `gl_FragCoord`, `gl_FrontFacing` and `gl_FragDepth` are available. Rasterization culls nothing, and
/// `gl_FrontFacing` reports the winding on screen, which a matrix with a negative determinant reverses.
///
/// ## Validation
///
/// The constructor reads each module's header, instruction framing, capabilities, entry point, types, decorations
/// and global variables, and compares its interface with the one above; it does not apply the whole SPIR-V
/// specification, so supply modules that `spirv-val` accepts, as glslc writes them. A declared resource counts as
/// read even when the shader never uses it. Accepted modules use only the `Shader`, `Matrix`, `ImageQuery` and
/// `DerivativeControl` capabilities, no extensions, only the `GLSL.std.450` extended instructions, and the
/// `Logical` addressing and `GLSL450` memory models, which a Vulkan 1.1 device runs without optional features.
/// The matrices of the frame block, the draw push constants and the pose buffer are column-major; resources need
/// explicit sets and bindings, and inputs and outputs explicit locations without components.

namespace anima {
/// How a custom material's fragments combine with the scene target. VulkanRenderer states the passes and order.
enum class CustomBlend {
    /// Replaces the target and writes depth, drawing with the opaque and masked meshes.
    opaque,
    /// The "over" operator on premultiplied color: the fragment writes its color times its alpha with that
    /// alpha, blended as `ONE, ONE_MINUS_SRC_ALPHA`. Draws after every opaque surface, sorted back to front with
    /// blended meshes, and writes no depth.
    blended,
    /// Adds the fragment's color and alpha to the target (`ONE, ONE`). Draws after every opaque surface, sorted
    /// with blended draws, and writes no depth.
    additive
};

/// Everything that defines a custom material; CustomMaterial validates it.
struct CustomMaterialDefinition {
    /// The name that scene and prefab documents store, 1 to 4,096 bytes. Documents resolve it through a
    /// CustomMaterialResolver, and one document cannot name two different materials alike.
    std::string name;
    /// SPIR-V words of the vertex shader, in host byte order, such as glslc writes with `-mfmt=c` or a `.spv`
    /// file holds on a little-endian host.
    std::vector<std::uint32_t> vertex_shader;
    /// SPIR-V words of the fragment shader.
    std::vector<std::uint32_t> fragment_shader;
    /// SPIR-V words of the depth-only variant's vertex shader. Empty, the default, means the material casts no
    /// shadows; otherwise the renderer draws it into the shadow maps of objects that cast shadows
    /// (Scene::Instance::casts_shadows), whatever the blend mode, and what it covers casts a full shadow.
    std::vector<std::uint32_t> shadow_vertex_shader;
    /// SPIR-V words of the depth-only variant's fragment shader, which may discard fragments and writes no color.
    /// Empty, the default, writes the depth of every covered pixel. It requires #shadow_vertex_shader.
    std::vector<std::uint32_t> shadow_fragment_shader;
    /// How the fragments combine with the target, which also selects the pass that draws them.
    CustomBlend blend = CustomBlend::opaque;
    /// Contents of the parameter block, at most CustomMaterial::max_parameter_bytes bytes, and at least as many
    /// as any stage's declaration of the block spans. Encode them as the shaders lay the block out, `std140` in
    /// GLSL.
    std::vector<std::byte> parameters;
    /// At most CustomMaterial::max_textures textures, valid as validate_scene() requires of a mesh's textures,
    /// bound in order at set 2, bindings 1 to 4. Their images are shared, not copied (see Image).
    std::vector<Texture> textures;
};

/// A validated, immutable custom material.
///
/// Share it as `std::shared_ptr<const CustomMaterial>`: objects that use it hold that pointer, and VulkanRenderer
/// caches its pipelines and GPU resources per CustomMaterial object, so assign one material to many objects rather
/// than creating copies. Nothing changes after construction, so a CustomMaterial may be read from several
/// threads.
class CustomMaterial {
  public:
    /// Largest CustomMaterialDefinition::name, in bytes.
    static constexpr std::size_t max_name_bytes = 4096;
    /// Largest parameter block, in bytes; the minimum `maxUniformBufferRange` of Vulkan devices is far larger.
    static constexpr std::size_t max_parameter_bytes = 256;
    /// Most textures a material binds.
    static constexpr std::size_t max_textures = 4;
    /// Largest shader module, in bytes.
    static constexpr std::size_t max_shader_bytes = 16 * 1024 * 1024;

    /// Validates @p definition and keeps it.
    ///
    /// Throws `std::invalid_argument` before keeping anything: for a name that is empty or longer than
    /// #max_name_bytes; for a blend mode other than the CustomBlend enumerators; for more than
    /// #max_parameter_bytes parameters or #max_textures textures, or an invalid texture; for a missing vertex or
    /// fragment shader, or a shadow fragment shader without a shadow vertex shader; for a module that is not
    /// SPIR-V, is larger than #max_shader_bytes, has broken instruction framing, uses a SPIR-V version, capability,
    /// extension, extended instruction set or memory model outside those listed in the file documentation, or has
    /// no entry point named `main` for its stage; and for an interface that differs from the documented one: an
    /// undocumented or mistyped input, output or resource, a resource in a stage that cannot read it, opaque depth
    /// or color in an opaque material or the depth-only variant, a parameter block larger than
    /// CustomMaterialDefinition::parameters, a texture binding beyond the supplied textures, a writable pose
    /// buffer, or a fragment shader input that the preceding vertex shader does not write with the same type.
    /// Each message names the stage and the mismatch.
    explicit CustomMaterial(CustomMaterialDefinition definition);
    /// The validated definition.
    [[nodiscard]] const CustomMaterialDefinition &definition() const noexcept { return definition_; }
    /// CustomMaterialDefinition::name.
    [[nodiscard]] const std::string &name() const noexcept { return definition_.name; }
    /// CustomMaterialDefinition::blend.
    [[nodiscard]] CustomBlend blend() const noexcept { return definition_.blend; }
    /// Whether the material has a depth-only variant, and so casts shadows.
    [[nodiscard]] bool casts_shadows() const noexcept { return !definition_.shadow_vertex_shader.empty(); }
    /// Vertex attributes that the vertex shader declares: bit `i` for location `i`.
    [[nodiscard]] std::uint32_t vertex_attributes() const noexcept { return vertex_attributes_; }
    /// Vertex attributes that the shadow vertex shader declares, or 0 without one.
    [[nodiscard]] std::uint32_t shadow_vertex_attributes() const noexcept { return shadow_vertex_attributes_; }
    /// Whether the fragment shader declares opaque depth; either input makes the renderer copy opaque depth and
    /// color on frames that draw the material.
    [[nodiscard]] bool reads_opaque_depth() const noexcept { return reads_opaque_depth_; }
    /// Whether the fragment shader declares opaque color; either input makes the renderer copy opaque depth and
    /// color on frames that draw the material.
    [[nodiscard]] bool reads_opaque_color() const noexcept { return reads_opaque_color_; }

  private:
    CustomMaterialDefinition definition_;
    std::uint32_t vertex_attributes_{}, shadow_vertex_attributes_{};
    bool reads_opaque_depth_{}, reads_opaque_color_{};
};
} // namespace anima
