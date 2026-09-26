#pragma once
#include <anima/assets/asset.hpp>
#include <optional>

namespace anima::detail {
// Texture-space cutoff whose coverage the base-color mips of @p material preserve, or none when its
// visibility does not depend on texture alpha. A masked fragment survives where texture alpha times the
// alpha factor (and vertex alpha) reaches alpha_cutoff, so texture alpha decides only when
// `alpha_cutoff / alpha` is in (0, 1]: a zero cutoff keeps every fragment, and one above the alpha factor
// discards every one. The upload plan and static texture shrinking share this rule, so a shrunk level keeps
// the coverage that its upload's mips preserve. Any result is a valid TextureMipOptions cutoff, even for a
// material that validation would reject.
inline std::optional<float> alpha_coverage_cutoff(const Material &material) {
    if (material.alpha_mode != AlphaMode::mask || !(material.alpha > 0))
        return std::nullopt;
    const auto cutoff = material.alpha_cutoff / material.alpha;
    if (cutoff > 0 && cutoff <= 1)
        return cutoff;
    return std::nullopt;
}
} // namespace anima::detail
