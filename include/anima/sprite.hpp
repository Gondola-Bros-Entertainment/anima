#pragma once
#include <anima/assets/asset.hpp>
#include <anima/core/math.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

/// @file
/// Sprites: textured quads in planes of constant Z that VulkanRenderer draws, for 2D games and the 2D parts of 3D ones.
///
/// Part of the `anima::assets` target, which validates sprite sheets and sprites without a display or GPU. A
/// SpriteSheet is a texture and the rectangles of its frames, as a texture atlas is in Unity's Sprite Editor or an
/// AtlasTexture is in Godot. VulkanRenderer::set_sprites() takes SpriteBatch values, each of which draws its sprites
/// from one sheet with one blend mode and one shader in one draw call, in the order the batches and their sprites are
/// listed.
///
/// Texels hold straight (unpremultiplied) alpha, as a mesh's do, and filtering within a mip level interpolates straight
/// color, so the color of a transparent texel reaches the edges between it and opaque ones: an image whose transparent
/// texels hold black draws dark fringes. Spread each frame's edge colors into the transparent texels around it, as
/// bake_impostor() does for its frames, and leave a gap of transparent texels between frames, so that filtering and the
/// coarser mip levels, which average neighboring texels, do not blend one frame into the next.
///
/// ## Sprite shaders
///
/// A batch may draw its sprites with a SpriteShader instead of the built-in shader: a fragment shader that the
/// application compiles to SPIR-V, as for a CustomShader, for effects that are shapes rather than pictures, such as
/// rings, energy fields and dissolves. The renderer's vertex shader still places each sprite's quad, and the batch's
/// SpriteBlend still composites what the shader writes. A sprite shader can read only the inputs below, which
/// `anima/sprite.glsl`, beside this header, declares for GLSL, and it shares the frame block, the SPIR-V rules and the
/// validation of custom shaders (custom_material.hpp): compile it for SPIR-V 1.0 to 1.3 (glslc
/// `--target-env=vulkan1.1`) with a fragment entry point named `main`.
///
/// - Set 0, binding 0: the `std140` uniform block `AnimaFrame`, the block that custom material shaders read, at the
/// same
///   offsets and with the same values, such as `time`, the seconds that VulkanRenderer::set_time received. Its helpers
///   in `anima/frame.glsl`, which `anima/sprite.glsl` includes, are available too.
/// - Set 1, binding 0: `sampler2D`, the batch's sheet, sampled as SpriteSheetDefinition::texture says, as the built-in
///   shader samples it.
/// - Inputs, interpolated across the quad: location 0 `vec2`, the texture coordinates in the sheet that sample the
///   sprite's frame at the point being shaded; 1 `vec4`, Sprite::color with Sprite::alpha in w; 2 `vec2`, the point's
///   coordinates across the sprite, from (0, 0) at the corner of its frame's first column and first row to (1, 1) at
///   the opposite corner, so that they run as the frame's texture coordinates do, mirroring included; and 3 `vec4`,
///   Sprite::parameters, the same at every point of the sprite, which `anima/sprite.glsl` declares `flat` so that
///   interpolation cannot round them. A shader may read any subset.
/// - Output: one `vec4` at location 0, premultiplied: a color times its alpha, with that alpha, as the built-in shader
///   writes it. A blended batch composites it with `ONE, ONE_MINUS_SRC_ALPHA`, and an additive one adds it.
///
/// Sprite shaders read no push constants and no other descriptors. Built-in variables such as `gl_FragCoord` are
/// available, `gl_FragCoord` counting the scene target's pixels, as the frame block's `viewport` does, and a shader may
/// discard fragments.

namespace anima {
/// A rectangle of a sprite sheet's image, in texels: #width texels across from column #x and #height texels down from
/// row #y, where column 0 lies at texture coordinate `u = 0` and row 0, the first row of Image::rgba, at `v = 0`.
struct SpriteFrame {
    /// First column.
    std::uint32_t x{};
    /// First row.
    std::uint32_t y{};
    /// Columns, at least 1.
    std::uint32_t width{};
    /// Rows, at least 1.
    std::uint32_t height{};
};

/// Everything that defines a sprite sheet; SpriteSheet validates it.
struct SpriteSheetDefinition {
    /// The texture that holds the frames, valid as validate_scene() requires of a mesh's textures and with texels. It
    /// samples through its Sampler and decodes through its TextureEncoding as a mesh's texture does, and its image is
    /// shared, not copied (see Image).
    Texture texture;
    /// The frames that Sprite::frame indexes, at least one, each inside the texture's image.
    std::vector<SpriteFrame> frames;
};

/// A validated, immutable sprite sheet.
///
/// Share it as `std::shared_ptr<const SpriteSheet>`: batches hold that pointer, and VulkanRenderer caches the sheet's
/// texture on the GPU per SpriteSheet object, so draw every batch of one atlas from one sheet rather than from copies.
/// A sheet keeps its texture's image, texels included, for its whole lifetime. Nothing changes after construction, so a
/// SpriteSheet may be read from several threads.
class SpriteSheet {
  public:
    /// Validates @p definition and keeps it.
    ///
    /// Throws `std::invalid_argument` for a texture that validate_scene() rejects or whose image has no texels, for a
    /// sheet without frames ("Sprite sheet needs at least one frame"), and for a frame with no width or height or that
    /// extends past the image's edges ("Sprite frame must lie inside its sheet's image").
    explicit SpriteSheet(SpriteSheetDefinition definition);
    /// The validated definition.
    [[nodiscard]] const SpriteSheetDefinition &definition() const noexcept { return definition_; }

  private:
    SpriteSheetDefinition definition_;
};

/// How a batch's sprites combine with the scene target.
enum class SpriteBlend {
    /// The "over" operator on premultiplied color, as CustomBlend::blended: each pixel writes its color times its alpha
    /// with that alpha, blended as `ONE, ONE_MINUS_SRC_ALPHA`.
    blended,
    /// Adds each pixel's color times its alpha, and its alpha, to the target (`ONE, ONE`), as CustomBlend::additive
    /// does, for glows, sparks and light.
    additive
};

/// A validated, immutable sprite shader: the fragment shader of the sprite shader interface that this file documents.
///
/// Share it as `std::shared_ptr<const SpriteShader>`: batches hold that pointer, and VulkanRenderer creates its shader
/// module and pipelines once per SpriteShader object, so draw every batch of one effect with one shader rather than
/// with copies. Nothing changes after construction, so a SpriteShader may be read from several threads.
class SpriteShader {
  public:
    /// Validates @p fragment_shader, SPIR-V words in host byte order, as load_spirv() reads them, and keeps them.
    ///
    /// Throws `std::invalid_argument` as CustomShader's constructor does for a stage, with messages that name "the
    /// sprite shader": for a module that is not SPIR-V, is larger than CustomShader::max_shader_bytes, has broken
    /// instruction framing, uses a SPIR-V version, capability, extension, extended instruction set or memory model
    /// outside those that custom_material.hpp lists, or has no fragment entry point named `main`; and for an interface
    /// that differs from the documented one: a resource other than the frame block and the sheet, as in "The sprite
    /// shader declares set 2 binding 0, which the sprite shader interface does not provide", push constants ("The
    /// sprite shader declares push constants, which the sprite shader interface does not provide"), a mistyped frame
    /// block member or sheet, an input other than the four documented ones or of another type, as in "The sprite shader
    /// reads location 0 as vec3, but the sprite vertex shader writes a vec2 there", or an output other than one `vec4`
    /// at location 0.
    explicit SpriteShader(std::vector<std::uint32_t> fragment_shader);
    /// The validated SPIR-V words.
    [[nodiscard]] const std::vector<std::uint32_t> &fragment_shader() const noexcept { return fragment_shader_; }

  private:
    std::vector<std::uint32_t> fragment_shader_;
};

/// One textured quad of a SpriteBatch, in the plane of constant Z through #position.
///
/// The quad's own X and Y axes are the world's +X and +Y turned by #rotation about +Z, and unless #size mirrors them
/// the frame's first row lies along its +Y edge and its first column along its -X edge, so a view that looks down -Z
/// with +Y up, such as anima::orthographic() times anima::look_at() from `{0, 0, 1}` toward the origin, shows the frame
/// upright and unmirrored at a rotation of 0.
struct Sprite {
    /// Where the pivot lies, in world space.
    Vec3 position{};
    /// Extent along the quad's X and Y axes, in world units. A negative component mirrors the frame along that axis,
    /// and a zero component draws nothing.
    Vec2 size{1, 1};
    /// The point of the quad that #position places and #rotation turns about, as fractions of #size from the edges of
    /// the frame's first column and last row, its -X and -Y edges unless #size mirrors them: `{0.5, 0.5}` is the center
    /// and `{0, 0}` the corner where those edges meet.
    Vec2 pivot{.5F, .5F};
    /// Counterclockwise turn about +Z, in radians.
    float rotation{};
    /// Index into SpriteSheetDefinition::frames.
    std::uint32_t frame{};
    /// Linear RGB factor of the texels' color, which may exceed 1 to brighten them.
    Vec3 color{1, 1, 1};
    /// Factor of the texels' alpha, in [0, 1].
    float alpha = 1;
    /// Values that the batch's SpriteShader reads, each finite, the same at every point of the sprite; the built-in
    /// shader reads none.
    std::array<float, 4> parameters{};
};

/// Sprites from one sheet that draw with one blend mode and one shader, in one draw call.
struct SpriteBatch {
    /// The sheet whose frames the sprites show, which VulkanRenderer holds while it draws the batch.
    std::shared_ptr<const SpriteSheet> sheet;
    /// How the sprites combine with the scene target.
    SpriteBlend blend = SpriteBlend::blended;
    /// The sprites, drawn in order, each over those before it.
    std::vector<Sprite> sprites;
    /// The shader that draws the sprites, which VulkanRenderer holds while it draws the batch, or null, the default,
    /// for the built-in one, which writes each texel times Sprite::color and Sprite::alpha.
    std::shared_ptr<const SpriteShader> shader{};
};

/// Throws `std::invalid_argument` for a batch without a sheet ("Sprite batch has no sheet") or with a blend mode
/// outside SpriteBlend ("Unknown sprite blend"), and for a sprite whose position, size, pivot or rotation is not finite
/// ("Sprite placement must be finite"), whose frame is not one of its sheet's ("Sprite frame is not in its sheet"),
/// whose color is not finite and at least 0 ("Sprite color must be finite and non-negative"), whose alpha is not in
/// [0, 1] ("Sprite alpha must be in [0, 1]") or whose parameters are not finite ("Sprite parameters must be finite").
/// VulkanRenderer::set_sprites() checks its batches with it.
void validate_sprites(std::span<const SpriteBatch> batches);
} // namespace anima
