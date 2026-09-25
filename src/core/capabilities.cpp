#include <algorithm>
#include <anima/core/capabilities.hpp>
#include <stdexcept>
namespace anima {
const std::string &resolve_action(std::span<const ActionVariant> variants, const Capabilities &available) {
    const ActionVariant *best = nullptr;
    bool ambiguous = false;
    for (const auto &value : variants) {
        if (!std::includes(available.begin(), available.end(), value.requirements.begin(), value.requirements.end()))
            continue;
        if (!best || value.requirements.size() > best->requirements.size()) {
            best = &value;
            ambiguous = false;
        } else if (value.requirements.size() == best->requirements.size())
            ambiguous = true;
    }
    if (!best || ambiguous)
        throw std::invalid_argument("No unique compatible action variant");
    return best->action;
}
} // namespace anima
