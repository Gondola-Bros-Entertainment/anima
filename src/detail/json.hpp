#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
// Anima's copy of nlohmann/json carries a local change (third_party/README.md), so it has an inline namespace
// of its own: an application that links its own copy of the same release shares no definition with it.
#define NLOHMANN_JSON_NAMESPACE nlohmann::json_anima_v3_12_0
#define NLOHMANN_JSON_NAMESPACE_BEGIN                                                                                  \
    namespace nlohmann {                                                                                               \
    inline namespace json_anima_v3_12_0 {
#define NLOHMANN_JSON_NAMESPACE_END                                                                                    \
    }                                                                                                                  \
    }
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
// Throws std::invalid_argument with message unless field of the object value is the integer
// expected; a value that is not an object or lacks the field fails as json_fields reports it.
// Readers call it before json_fields, so a document of another version reports its version
// rather than a field that version lacks or adds.
inline void json_version(const nlohmann::json &value, const char *field, std::int64_t expected, const char *message) {
    if (!value.is_object())
        throw std::invalid_argument("JSON document requires an object");
    if (!value.contains(field))
        throw std::invalid_argument("Missing JSON field: " + std::string(field));
    if (!value.at(field).is_number_integer() || value.at(field) != expected)
        throw std::invalid_argument(message);
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

// Reads a JSON array of exactly N numbers, each as json_float reads it, so every element is finite. Throws
// std::invalid_argument with message for a value that is not an array of N elements. nlohmann's own conversion to
// std::array would also accept a boolean, narrow without the range check and ignore elements past N.
template <std::size_t N> std::array<float, N> json_floats(const nlohmann::json &value, const char *message) {
    if (!value.is_array() || value.size() != N)
        throw std::invalid_argument(message);
    std::array<float, N> result{};
    for (std::size_t i = 0; i < N; ++i)
        result[i] = json_float(value[i]);
    return result;
}

// The array or object that field of the object value holds. A field of another JSON type throws
// std::invalid_argument naming it: iterating would otherwise read an object's values as a list, items() would name
// an array's elements by their indices, and both would read null as empty. A missing field fails as
// nlohmann::json::at reports it, so readers check fields with json_fields first.
inline const nlohmann::json &json_array(const nlohmann::json &object, const char *field) {
    const auto &value = object.at(field);
    if (!value.is_array())
        throw std::invalid_argument("JSON field must be an array: " + std::string(field));
    return value;
}
inline const nlohmann::json &json_object(const nlohmann::json &object, const char *field) {
    const auto &value = object.at(field);
    if (!value.is_object())
        throw std::invalid_argument("JSON field must be an object: " + std::string(field));
    return value;
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

// The name of each enumerator of Enum, for documents that store enumerators by name: reordering or inserting an
// enumerator then cannot change what a saved document means.
template <class Enum, std::size_t N> using JsonNames = std::array<std::pair<Enum, std::string_view>, N>;

// Returns value's name in names. Throws std::invalid_argument with message when names has no entry for value.
template <class Enum, std::size_t N>
std::string_view json_name(const JsonNames<Enum, N> &names, Enum value, const char *message) {
    for (const auto &[enumerator, name] : names)
        if (enumerator == value)
            return name;
    throw std::invalid_argument(message);
}

// Returns the enumerator that value names in names. Throws std::invalid_argument with message for any value that is
// not one of those names, including an enumerator's number.
template <class Enum, std::size_t N>
Enum json_enumerator(const JsonNames<Enum, N> &names, const nlohmann::json &value, const char *message) {
    if (const auto *text = value.get_ptr<const std::string *>())
        for (const auto &[enumerator, name] : names)
            if (*text == name)
                return enumerator;
    throw std::invalid_argument(message);
}
} // namespace anima::detail
