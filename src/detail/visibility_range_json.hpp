#pragma once
#include "json.hpp"
#include <anima/scene.hpp>
#include <cmath>
#include <stdexcept>

// The `visibility_range` field of scene, prefab and prefab variant documents: null for the default range, or an object
// with exactly `begin`, `end` (null for no end), `begin_margin` and `end_margin`.
namespace anima::detail {
inline nlohmann::json encode_visibility_range(const VisibilityRange &range) {
    if (range == VisibilityRange{})
        return nullptr;
    return {{"begin", range.begin},
            {"end", std::isinf(range.end) ? nlohmann::json(nullptr) : nlohmann::json(range.end)},
            {"begin_margin", range.begin_margin},
            {"end_margin", range.end_margin}};
}
// Reads @p value, which must be null or the object encode_visibility_range() writes, and validates the range; throws
// `std::invalid_argument` with @p message for a malformed value and as validate_visibility_range() does.
inline VisibilityRange decode_visibility_range(const nlohmann::json &value, const char *message) {
    VisibilityRange range;
    if (value.is_null())
        return range;
    if (!value.is_object())
        throw std::invalid_argument(message);
    json_fields(value, {"begin", "end", "begin_margin", "end_margin"}, {});
    const auto number = [&](const nlohmann::json &field) {
        if (!field.is_number())
            throw std::invalid_argument(message);
        return json_float(field);
    };
    range.begin = number(value.at("begin"));
    if (!value.at("end").is_null())
        range.end = number(value.at("end"));
    range.begin_margin = number(value.at("begin_margin"));
    range.end_margin = number(value.at("end_margin"));
    validate_visibility_range(range);
    return range;
}
} // namespace anima::detail
