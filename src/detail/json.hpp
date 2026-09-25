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
    // On syntax errors the normal parser below preserves its exception type and
    // diagnostic. It can only reach the prefix already checked by the SAX pass.
    (void)Json::sax_parse(source, &validation);
    return Json::parse(source);
}
} // namespace anima::detail
