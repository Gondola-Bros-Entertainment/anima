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
/// tangent and 7 `float` alpha; then per instance, 8 to 10 `vec4`, rows 0 to 2 of the affine matrix of the
/// placement being drawn (Scene::set_placements), whose last row is (0, 0, 0, 1), or of an identity for an object
/// without placements. A shader may read any subset, but only one that reads all three placement rows, in every
/// vertex shader it has, can draw placements (reads_placements()).
///
/// Descriptors:
/// - Set 0, binding 0, any stage: the `std140` uniform block `AnimaFrame`, the same for every draw of a frame. At
///   offset 0 `mat4 viewProjection`, the camera's column-major Vulkan view-projection that
///   VulkanRenderer::set_view received; 64 `mat4 inverseViewProjection`, its inverse; 128 `vec4 viewOrigin`, the eye
///   position with w 1 in a perspective view, or the unit direction toward the camera with w 0 in an orthographic
///   one; then Environment's lights as `vec4`s with w 0: 144 `sunDirection` (unit, toward the sun), 160
///   `sunIrradiance`, the sun's irradiance as it reaches the ground, atmosphere_sunlight(), 176 `fillDirection`, 192
///   `fillIrradiance`, 208 `ambientSky`, 224 `ambientGround` and 240 `ambientSpecular`; 256 `vec4 fog`,
///   EnvironmentSettings::fog_color with fog_density in w, the density per unit at fog_height (0 for no fog); 272 `vec4
///   viewport`, the scene target's width and height in pixels, which are the window's times
///   VulkanRenderer::render_scale() (see VulkanRenderer::set_render_scale), and their reciprocals; 288 `float time`,
///   the seconds that VulkanRenderer::set_time received; 304 `vec4 fogShape`, EnvironmentSettings::fog_height,
///   fog_falloff, fog_sky_distance and fog_sun_anisotropy; and 320 `vec4 fogSun`, fog_sun_scattering times
///   `sunIrradiance`, capped at the largest float, with w 0. `animaFogged()` in custom_material.glsl applies this fog
///   as the standard material does.
/// - Set 0, binding 1, fragment shaders of blended and additive materials only: `sampler2D` opaque depth, the
///   depth buffer after every opaque draw of the frame, at the scene target's size, so that `gl_FragCoord.xy` times
///   the `zw` of `viewport` addresses the fragment's own pixel; reversed Vulkan depth from 1 at the near plane to 0 at
///   the far one in `r`, 0 where nothing opaque drew, sampled with nearest filtering and clamped at the edges. Nearer
///   surfaces have greater depth.
/// - Set 0, binding 2, under the same rule: `sampler2D` opaque color, the scene's linear color after every opaque
///   draw, sky included, before exposure and tone mapping, sampled with linear filtering and clamped at the edges.
/// - Set 1, binding 0, vertex shaders only: the `readonly` `std430` storage buffer `AnimaPoses` of `mat4
///   matrices[]`, the posed palettes of the frame (Scene::Instance::palette). A rigid draw uses
///   `matrices[paletteOffset]`; a skinned vertex blends `matrices[paletteOffset + joints[i]]` by `weights[i]`. For
///   an object with placements, `matrices[paletteOffset]` is instead the node's matrix in the mesh's rest pose,
///   and the vertex's world matrix is `matrices[objectOffset] * placement * matrices[paletteOffset]`. For an object
///   with placements or a visibility range, `matrices[objectOffset]` is its world matrix. The first column of
///   `matrices[objectOffset + 1]` is then its range: begin, begin margin, end and end margin (VisibilityRange), with
///   an endless range ending at the largest finite `float` and no end margin. The `xyz` of its second column is the
///   center of Mesh::rest_bounds(), which the range measures to once the object, and each placement, places it.
/// - Set 2, binding 0, any stage: the parameter block, a uniform block that holds
///   CustomMaterialDefinition::parameters from its start, laid out as the shader declares it.
/// - Set 2, bindings 1 to 4, any stage: `sampler2D` for CustomMaterialDefinition::textures 0 to 3, sampled as their
///   Sampler states, through mip chains built as texture_mips() builds them when Sampler::mipmapped is set, and
///   filtered anisotropically as VulkanRenderer::max_anisotropy() describes.
///
/// Push constants, any stage: the block `AnimaDraw`, per draw. At offset 0 `mat4 viewProjection`, the matrix of
/// the pass being drawn: the camera's, or in the depth-only variant a shadow pass's, a shadow cascade's or the detail
/// region's; 64 `uint paletteOffset`,
/// IndexedDraw::palette_offset within the instance's palette; 68 `uint skinned`, 1 for a skinned draw and
/// otherwise 0; 72 `uint objectOffset`, the index in `AnimaPoses` of the object's world matrix when it has
/// placements or a visibility range; 76 `uint placed`, 1 when it has placements and otherwise 0; 80 `vec4 factor`,
/// the object's linear RGB factor for the material slot (Scene::set_material_factor) with alpha 1; and 96 `uint
/// ranged`, 1 when it has a visibility range other than the default and otherwise 0. `animaModelMatrix()` in
/// `anima/custom_material.glsl` composes these as the standard material does.
///
/// A visibility range (VisibilityRange) is partly the shaders' to apply. The renderer skips an object, or a cluster
/// of placed copies (MeshPlacements::clusters()), in every pass only where it lies wholly outside its range, so copies
/// in the margins, and copies outside the range in a cluster that lies partly inside, reach the shaders whole. In a
/// vertex shader, `animaVisibility()` returns the share of the object or copy that its range draws, as the standard
/// material computes it, negated while it fades in: 0 outside the range, from 0 to -1 across the begin margin, 1
/// between the margins, from 1 to 0 across the end margin, and 1 without a range or in an orthographic view. To
/// draw as the standard material does, the vertex shader passes it as a `flat` output to the fragment shader, which,
/// after any sampling that chooses mip levels from derivatives, since a discard leaves those undefined for the rest of
/// its 2x2 quad, discards the fragments for which `animaDissolved()` returns true: every fragment of a copy whose value
/// is 0, and in the margins those that the standard material's 4x4 ordered dither discards, the pixels complementary in
/// a begin margin to those of an end margin. The depth-only variant's vertex shader moves a copy out of the clip
/// volume, for example to (2, 2, 2, 1), while `abs(animaVisibility())` is at most 0.5. A material whose shaders do
/// neither draws whole every copy that the renderer does not skip, including copies outside the range.
///
/// Stages pass values at locations 0 to 15 as 32-bit scalars or vectors, and every fragment shader input must be
/// a vertex shader output of the same type. The fragment shader writes one `vec4` at location 0 into the linear
/// `RGBA16F` scene target, whose display conversion applies exposure and tone mapping; the shadow fragment shader
/// writes no color. `gl_Position` is in Vulkan clip space, with Y down and depth 0 to 1: reversed in the view, where
/// `viewProjection` puts 1 at the near plane and nearer surfaces pass the depth test, and forward in the shadow
/// variant, with 0 on the side facing the light; `gl_FragDepth` follows the same direction. Built-in variables such as
/// `gl_FragCoord`, `gl_FrontFacing` and `gl_FragDepth` are available, and in the view `gl_FragCoord` counts the scene
/// target's pixels, as `viewport` does. Rasterization culls nothing, and
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
    /// At most CustomMaterial::max_textures textures, valid as validate_scene() requires of a mesh's textures and
    /// with texels, bound in order at set 2, bindings 1 to 4. Their images are shared, not copied (see Image).
    std::vector<Texture> textures;
    /// How long the material holds its textures' texels. With TexelRetention::until_upload, the textures of
    /// CustomMaterial::definition() refer to images without texels, and CustomMaterial::texel_images() returns the
    /// supplied ones while it or anything else holds them.
    TexelRetention texel_retention = TexelRetention::keep;
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

    /// Validates @p definition and keeps it, holding its textures' images as
    /// CustomMaterialDefinition::texel_retention says.
    ///
    /// Throws `std::invalid_argument` before keeping anything: for a name that is empty or longer than #max_name_bytes;
    /// for a blend mode other than the CustomBlend enumerators; for an unknown texel retention; for more than
    /// #max_parameter_bytes parameters or #max_textures textures, or an invalid texture, including one whose image has
    /// no texels; for a missing vertex or fragment shader, or a shadow fragment shader without a shadow vertex shader;
    /// for a module that is not SPIR-V, is larger than #max_shader_bytes, has broken instruction framing, uses a SPIR-V
    /// version, capability, extension, extended instruction set or memory model outside those listed in the file
    /// documentation, or has no entry point named `main` for its stage; and for an interface that differs from the
    /// documented one: an undocumented or mistyped input, output or resource, a resource in a stage that cannot read
    /// it, opaque depth or color in an opaque material or the depth-only variant, a parameter block larger than
    /// CustomMaterialDefinition::parameters, a texture binding beyond the supplied textures, a writable pose buffer, or
    /// a fragment shader input that the preceding vertex shader does not write with the same type. Each message names
    /// the stage and the mismatch.
    explicit CustomMaterial(CustomMaterialDefinition definition);
    /// The validated definition. With TexelRetention::until_upload its textures' images have no texels.
    [[nodiscard]] const CustomMaterialDefinition &definition() const noexcept { return definition_; }
    /// The image of each texture of definition(), in order, with its texels, as Mesh::texel_images() returns a
    /// mesh's; throws `std::logic_error` ("Custom material texture texels were released after upload") once one
    /// that TexelRetention::until_upload let go is gone. Safe to call from any thread.
    [[nodiscard]] std::vector<std::shared_ptr<const Image>> texel_images() const;
    /// Ends the material's own hold on the images of TexelRetention::until_upload, as VulkanRenderer does once it
    /// has uploaded them, and as Mesh::release_texels() does for a mesh; does nothing for TexelRetention::keep.
    void release_texels() const noexcept;
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
    /// Whether the vertex shader, and the depth-only variant's if there is one, read all three placement rows
    /// (locations 8 to 10), which drawing an object's placements requires (Scene::set_placements).
    [[nodiscard]] bool reads_placements() const noexcept {
        constexpr std::uint32_t rows = 0x700;
        return (vertex_attributes_ & rows) == rows && (!casts_shadows() || (shadow_vertex_attributes_ & rows) == rows);
    }
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
    detail::TexelHoldPtr texels_;
};
} // namespace anima
