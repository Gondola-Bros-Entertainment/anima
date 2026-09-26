#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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

// Private document reader shared by scene and component formats. Validate
// nesting and decoded keys before building the DOM. The DOM callback parser
// scans the parent array after every object, making large object arrays quadratic.
inline nlohmann::json parse_json(std::string_view source, std::size_t maximum_bytes, int maximum_depth = 16) {
    if (source.size() > maximum_bytes)
        throw std::invalid_argument("JSON document exceeds byte limit");
    using Json = nlohmann::json;
    struct Validation final : nlohmann::json_sax<Json> {
        explicit Validation(int limit) : maximum_depth(limit) {}

        bool check_depth() const {
            if (depth > maximum_depth)
                throw std::invalid_argument("JSON document exceeds nesting limit");
            return true;
        }
        bool null() override { return check_depth(); }
        bool boolean(bool) override { return check_depth(); }
        bool number_integer(number_integer_t) override { return check_depth(); }
        bool number_unsigned(number_unsigned_t) override { return check_depth(); }
        bool number_float(number_float_t, const string_t &) override { return check_depth(); }
        bool string(string_t &) override { return check_depth(); }
        bool binary(binary_t &) override { return check_depth(); }
        bool start_object(std::size_t) override {
            check_depth();
            ++depth;
            keys.emplace_back();
            return true;
        }
        bool key(string_t &value) override {
            check_depth();
            if (!keys.back().insert(value).second)
                throw std::invalid_argument("Duplicate JSON document field");
            return true;
        }
        bool end_object() override {
            --depth;
            keys.pop_back();
            return true;
        }
        bool start_array(std::size_t) override {
            check_depth();
            ++depth;
            return true;
        }
        bool end_array() override {
            --depth;
            return true;
        }
        bool parse_error(std::size_t, const std::string &, const Json::exception &) override { return false; }

        int maximum_depth;
        int depth = 0;
        std::vector<std::set<std::string>> keys;
    } validation(maximum_depth);
    // On syntax errors the normal parser below reports the diagnostic. It can only reach the prefix
    // already checked by the SAX pass.
    (void)Json::sax_parse(source, &validation);
    try {
        return Json::parse(source);
    } catch (const Json::parse_error &error) {
        throw std::invalid_argument(error.what());
    }
}

// Reads a JSON number as a float. A value outside the finite float range throws instead of narrowing,
// which would be undefined behavior.
inline float json_float(const nlohmann::json &value) {
    if (!value.is_number())
        throw std::invalid_argument("JSON value must be a number");
    const auto number = value.get<double>();
    if (!(std::abs(number) <= std::numeric_limits<float>::max()))
        throw std::invalid_argument("JSON number outside the float range");
    return static_cast<float>(number);
}

// Runs one document encode or decode step. The JSON library's own failures (a mistyped value, a missing
// key, or text that is not UTF-8 on output) are rethrown as Error, the type the caller's contract uses
// for a rejected document; nlohmann is private, so callers could not otherwise name them.
template <class Error = std::invalid_argument, class Step> decltype(auto) json_step(Step &&step) {
    try {
        return std::forward<Step>(step)();
    } catch (const nlohmann::json::exception &error) {
        throw Error(error.what());
    }
}
} // namespace anima::detail
