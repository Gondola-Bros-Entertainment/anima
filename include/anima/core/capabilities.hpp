#pragma once
#include <set>
#include <span>
#include <string>
#include <vector>

/// @file
/// Selection among alternative actions by the capabilities they require. Part of the
/// `anima::core` target.

namespace anima {
/// Names of capabilities, required by an action or available to perform it.
using Capabilities = std::set<std::string, std::less<>>;
/// One way to perform an action, usable when all of its #requirements are available.
struct ActionVariant {
    /// Action name returned by resolve_action.
    std::string action;
    Capabilities requirements;
};
/// Returns the action of the most specific variant in @p variants whose requirements are all in
/// @p available, where specificity is the number of requirements. The result refers into
/// @p variants. Throws `std::invalid_argument` when no variant qualifies or when several
/// qualifying variants tie for the most requirements.
const std::string &resolve_action(std::span<const ActionVariant> variants, const Capabilities &available);
} // namespace anima
