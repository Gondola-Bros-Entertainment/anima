#pragma once
#include <anima/assets/material_textures.hpp>
#include <anima/scene.hpp>

namespace anima {
// Immutable CPU upload data bound to its exact compiled asset. Construction may
// run on a worker; it uses the same colour/alpha filtering as synchronous upload.
// Keep only the preparations currently needed: mip chains include base pixels.
class MeshPreparation {
  public:
    explicit MeshPreparation(std::shared_ptr<const Mesh> asset);
    MeshPreparation(MeshPreparation &&) noexcept = default;
    MeshPreparation &operator=(MeshPreparation &&) noexcept = default;
    MeshPreparation(const MeshPreparation &) = delete;
    MeshPreparation &operator=(const MeshPreparation &) = delete;
    const std::shared_ptr<const Mesh> &asset() const { return asset_; }
    const MaterialTexturePlan &plan() const { return plan_; }
    const std::vector<std::vector<MipLevel>> &images() const { return images_; }

  private:
    std::shared_ptr<const Mesh> asset_;
    MaterialTexturePlan plan_;
    std::vector<std::vector<MipLevel>> images_;
};
} // namespace anima
