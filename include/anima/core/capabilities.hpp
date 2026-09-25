#pragma once
#include <set>
#include <span>
#include <string>
#include <vector>
namespace anima {
using Capabilities = std::set<std::string, std::less<>>;
struct ActionVariant {
    std::string action;
    Capabilities requirements;
};
// The most specific available variant wins; an equal-specificity tie is an error.
const std::string &resolve_action(std::span<const ActionVariant> variants, const Capabilities &available);
} // namespace anima
