#pragma once
#include <algorithm>
#include <anima/assets/asset.hpp>
#include <cmath>
#include <span>

namespace anima::detail {
// Largest absolute element difference from identity of any joint's world matrix in @p rest times its inverse bind
// matrix, as MeshDescription and MeshSnapshot report it; 0 without skins. Throws std::out_of_range when a joint is
// outside @p rest or lacks an inverse bind matrix.
inline float bind_deviation(const Pose &rest, std::span<const AssetSkin> skins) {
    float deviation = 0;
    const auto unit = identity();
    for (const auto &skin : skins)
        for (std::size_t j = 0; j < skin.joints.size(); ++j) {
            const auto bind = rest.world.at(skin.joints[j]) * skin.inverse_bind.at(j);
            for (unsigned k = 0; k < 16; ++k)
                deviation = std::max(deviation, std::abs(bind[k] - unit[k]));
        }
    return deviation;
}
} // namespace anima::detail
