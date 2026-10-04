#pragma once
#include <anima/assets/material_textures.hpp>
#include <anima/scene.hpp>

/// @file
/// CPU preparation of a Mesh's texture uploads, for VulkanRenderer::prepare_mesh. Part of the
/// `anima::assets` target.

namespace anima {
/// Mip chains and material bindings for one compiled Mesh, computed on the CPU.
///
/// Construction reads the Mesh, its texels through Mesh::texel_images(), which is safe on any thread, and
/// shares no other mutable state, so constructions may run concurrently on any thread, and it filters
/// exactly as a synchronous upload does. The object is move-only, shares ownership of the Mesh and of its
/// block-compressed images, and owns its mip chains, including base texels, so keep only the preparations
/// still needed.
class MeshPreparation {
  public:
    /// Plans and filters the textures of @p asset, reading their texels through Mesh::texel_images(); see
    /// material_texture_plan and texture_mips. Throws `std::invalid_argument` for a null asset or an invalid
    /// material or texture, and `std::logic_error` as Mesh::texel_images() does once the texels of a Mesh compiled
    /// with TexelRetention::until_upload are gone.
    explicit MeshPreparation(std::shared_ptr<const Mesh> asset);
    MeshPreparation(MeshPreparation &&) noexcept = default;
    MeshPreparation &operator=(MeshPreparation &&) noexcept = default;
    MeshPreparation(const MeshPreparation &) = delete;
    MeshPreparation &operator=(const MeshPreparation &) = delete;
    /// The Mesh the data belongs to.
    [[nodiscard]] const std::shared_ptr<const Mesh> &asset() const noexcept { return asset_; }
    /// The plan that material_texture_plan builds for the Mesh's materials and textures.
    [[nodiscard]] const MaterialTexturePlan &plan() const noexcept { return plan_; }
    /// One mip chain per MaterialTexturePlan::images entry, base level first; textures without
    /// Sampler::mipmapped have only the base level, and those whose image is block-compressed have none,
    /// since they upload the levels that the image stores.
    [[nodiscard]] const std::vector<std::vector<MipLevel>> &images() const noexcept { return images_; }
    /// One image per MaterialTexturePlan::images entry: the block-compressed image, with its texels, that the entry
    /// uploads, or null for an entry whose mip chain images() holds. Holding these lets a preparation upload a Mesh
    /// compiled with TexelRetention::until_upload after an earlier upload let the Mesh's texels go.
    [[nodiscard]] const std::vector<std::shared_ptr<const Image>> &compressed_images() const noexcept {
        return compressed_images_;
    }

  private:
    std::shared_ptr<const Mesh> asset_;
    MaterialTexturePlan plan_;
    std::vector<std::vector<MipLevel>> images_;
    std::vector<std::shared_ptr<const Image>> compressed_images_;
};
} // namespace anima
