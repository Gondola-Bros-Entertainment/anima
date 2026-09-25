#pragma once
#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace anima::detail {
inline void json_fields(const nlohmann::json &value, std::initializer_list<std::string_view> required,
                        std::initializer_list<std::string_view> optional = {}) {
    if (!value.is_object())
        throw std::invalid_argument("JSON document requires an object");
    for (const auto name : required)
        if (!value.contains(name))
            throw std::invalid_argument("Missing JSON field: " + std::string(name));
    for (auto it = value.begin(); it != value.end(); ++it)
        if (std::find(required.begin(), required.end(), it.key()) == required.end() &&
            std::find(optional.begin(), optional.end(), it.key()) == optional.end())
            throw std::invalid_argument("Unknown JSON field: " + it.key());
}

// Private document reader shared by scene and component formats. Bound nesting
// during parsing, before constructing an arbitrarily deep JSON value. Compare
// decoded keys so escaped spellings cannot bypass duplicate-field rejection.
inline nlohmann::json parse_json(std::string_view source, std::size_t maximum_bytes, int maximum_depth = 16) {
    if (source.size() > maximum_bytes)
        throw std::invalid_argument("JSON document exceeds byte limit");
    using Json = nlohmann::json;
    std::vector<std::set<std::string>> keys;
    return Json::parse(source, [&](int depth, Json::parse_event_t event, Json &value) {
        if (depth > maximum_depth)
            throw std::invalid_argument("JSON document exceeds nesting limit");
        if (event == Json::parse_event_t::object_start)
            keys.emplace_back();
        else if (event == Json::parse_event_t::object_end)
            keys.pop_back();
        else if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
            throw std::invalid_argument("Duplicate JSON document field");
        return true;
    });
}
} // namespace anima::detail
