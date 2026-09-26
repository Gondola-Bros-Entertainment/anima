#pragma once
#include <anima/assets/material_textures.hpp>
#include <anima/scene.hpp>

/// @file
/// CPU preparation of a Mesh's texture uploads, for VulkanRenderer::prepare_mesh. Part of the
/// `anima::assets` target.

namespace anima {
/// Mip chains and material bindings for one compiled Mesh, computed on the CPU.
///
/// Construction reads only the immutable Mesh, so it may run on a worker thread, and it filters
/// exactly as a synchronous upload does. The object is move-only, shares ownership of the Mesh and
/// owns its mip chains, including base texels, so keep only the preparations still needed.
class MeshPreparation {
  public:
    /// Plans and filters the textures of @p asset; see material_texture_plan and texture_mips.
    /// Throws `std::invalid_argument` for a null asset or an invalid material or texture.
    explicit MeshPreparation(std::shared_ptr<const Mesh> asset);
    MeshPreparation(MeshPreparation &&) noexcept = default;
    MeshPreparation &operator=(MeshPreparation &&) noexcept = default;
    MeshPreparation(const MeshPreparation &) = delete;
    MeshPreparation &operator=(const MeshPreparation &) = delete;
    /// The Mesh the data belongs to.
    const std::shared_ptr<const Mesh> &asset() const { return asset_; }
    const MaterialTexturePlan &plan() const { return plan_; }
    /// One mip chain per MaterialTexturePlan::images entry, base level first; textures without
    /// Sampler::mipmapped have only the base level.
    const std::vector<std::vector<MipLevel>> &images() const { return images_; }

  private:
    std::shared_ptr<const Mesh> asset_;
    MaterialTexturePlan plan_;
    std::vector<std::vector<MipLevel>> images_;
};
} // namespace anima
