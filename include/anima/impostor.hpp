#pragma once
#include <anima/mesh.hpp>
#include <cstdint>
#include <optional>

/// @file
/// Octahedral impostors, after Ryan Brucks's, on which Unreal's Impostor Baker is built: views of a mesh from a grid
/// of directions, baked into one atlas, which VulkanRenderer draws as a single quad per copy for distant foliage and
/// other detailed objects. Part of the `anima::assets` target, which bakes without a display or GPU.
///
/// Bake an atlas with bake_impostor() and compile it with Mesh::compile_impostor(). An application that compresses
/// its textures bakes offline instead, stores the images in a block-compressed format with the ImpostorFrames beside
/// them, and compiles the atlas it loads. Give the impostor the source's placements and a visibility range whose
/// begin margin spans the source's end margin: both measure to the same center, so the two keep complementary pixels
/// as one hands over to the other (VisibilityRange).

namespace anima {
/// Options for bake_impostor().
struct ImpostorOptions {
    /// The directions that the frames cover.
    ImpostorLayout layout = ImpostorLayout::hemisphere;
    /// Frames along each side of the atlas, from 2 to 32.
    std::uint32_t frames = 12;
    /// Texels along each side of each frame, from 8 to 1024; the atlas spans `frames * frame_size` texels, at most
    /// 8192.
    std::uint32_t frame_size = 128;
    /// Samples along each side of each texel, from 1 to 8, whose covered share sets the texel's coverage.
    std::uint32_t samples = 4;
};

/// The frames and images of an impostor, which Mesh::compile_impostor() compiles.
///
/// Each image holds ImpostorFrames::count by ImpostorFrames::count frames of equal size, as ImpostorFrames lays them
/// out. A texel describes the surface nearest the frame's viewpoint along the frame's direction through the texel's
/// center, its values averaged over the samples that a surface covers. A texel that no surface covers holds the values
/// of the nearest covered texel of its frame, with coverage 0, so that filtering, and the mips that texture_mips()
/// builds from it, do not darken silhouettes; a frame that nothing covers holds zeros.
struct ImpostorAtlas {
    ImpostorFrames frames;
    /// TextureEncoding::srgb. RGB is the base color, the material's factor times its map times the vertex color; alpha
    /// is the share of the texel's samples that a surface covers.
    Texture color;
    /// TextureEncoding::linear. RGB is the unit normal in mesh space, after normal mapping and turned toward the
    /// frame's viewpoint on a double-sided material's back, as `0.5 n + 0.5`; alpha is the surface's height toward the
    /// viewpoint, as `0.5 dot(p - center, d) / radius + 0.5` for the surface point `p` and the frame direction `d`.
    Texture normal_depth;
    /// TextureEncoding::linear. R is ambient occlusion, `1 + strength * (map - 1)`; G is perceptual roughness and B is
    /// metallic, each the factor times its map channel; alpha is 1.
    Texture surface;
    /// TextureEncoding::srgb. RGB is emitted light, the material's emissive factor times its map, divided by
    /// #emission_scale; alpha is 1. bake_impostor() leaves it empty when no material emits.
    std::optional<Texture> emissive;
    /// The factor by which emitted light exceeds #emissive's RGB, finite and at least 1: bake_impostor() gives the
    /// largest channel of any material's emissive factor, or 1 when none exceeds 1.
    float emission_scale = 1;
};

/// Bakes @p mesh, in its rest pose, into the frames of an impostor with @p options.
///
/// The frames cover the sphere around the center of Mesh::rest_bounds() that holds every vertex. Each samples the
/// surfaces as the standard material does: the base-color map's alpha times the material's alpha and the vertex alpha
/// against a masked material's cutoff, single-sided materials from their front only, filtered and wrapped as each
/// texture's sampler says, from the mip level whose texels best match a sample's footprint in each triangle.
/// Every draw bakes, whatever an instance's visibility. The images are ImageFormat::rgba8 and mipmapped with linear
/// filtering, clamped at their edges.
///
/// Throws `std::invalid_argument` for options out of range ("Impostor options must give 2 to 32 frames of 8 to 1024
/// texels, at most 8192 in all, and 1 to 8 samples"), an unknown layout ("Unknown impostor layout"), a mesh with a
/// skinned draw ("Impostors bake only rigid meshes"), a blended or unlit material ("Impostors bake only lit opaque and
/// masked materials"), or no triangles, or vertices that all lie at the center of its bounds or too far from it for a
/// float ("An impostor requires a mesh with triangles"), and `std::logic_error` as Mesh::texel_images() does once the
/// texels of a Mesh compiled with TexelRetention::until_upload are gone. Takes time proportional to the frames times
/// `T log T` for `T` triangles, which each frame sorts by depth, plus the frames times the samples.
///
/// Calls may run concurrently on any thread. Each reads @p mesh, which never changes, and its images.
[[nodiscard]] ImpostorAtlas bake_impostor(const Mesh &mesh, const ImpostorOptions &options = {});
} // namespace anima
