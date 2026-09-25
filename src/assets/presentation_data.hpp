#pragma once
#include "../detail/json.hpp"
#include <anima/assets/preview.hpp>
#include <cstdint>
#include <fstream>
#include <map>
#include <set>
namespace anima::presentation_data {
inline constexpr std::size_t maximum_document_bytes = 4 * 1024 * 1024;
using Json = nlohmann::json;
inline std::string text(const Json &value) {
    const auto result = value.get<std::string>();
    if (result.empty())
        throw std::invalid_argument("Empty presentation identity/reference");
    return result;
}
inline Json parse(std::string_view source, int maximum_depth = 64) {
    return detail::parse_json(source, maximum_document_bytes, maximum_depth);
}
inline Json read(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto size = input.tellg();
    if (!input || size < 0 || static_cast<std::uintmax_t>(size) > maximum_document_bytes)
        throw std::invalid_argument("Missing/oversized presentation document: " + path.string());
    std::string source(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    if (!input.read(source.data(), static_cast<std::streamsize>(source.size())))
        throw std::runtime_error("Cannot read presentation document");
    return parse(source);
}
inline anima::Mat4 matrix(const Json &value, bool rigid) {
    if (!value.is_array() || value.size() != 16)
        throw std::invalid_argument("Presentation transform requires 16 column-major values");
    auto result = value.get<anima::Mat4>();
    if (std::any_of(result.begin(), result.end(), [](float x) { return !std::isfinite(x); }) ||
        std::abs(result[3]) > 1e-6F || std::abs(result[7]) > 1e-6F || std::abs(result[11]) > 1e-6F ||
        std::abs(result[15] - 1) > 1e-6F)
        throw std::invalid_argument("Presentation transform must be finite and affine");
    (void)anima::inverse(result);
    if (rigid) {
        const anima::Vec3 x{result[0], result[1], result[2]}, y{result[4], result[5], result[6]},
            z{result[8], result[9], result[10]};
        if (std::abs(anima::length(x) - 1) > 1e-4F || std::abs(anima::length(y) - 1) > 1e-4F ||
            std::abs(anima::length(z) - 1) > 1e-4F || std::abs(anima::dot(x, y)) > 1e-4F ||
            std::abs(anima::dot(x, z)) > 1e-4F || std::abs(anima::dot(y, z)) > 1e-4F ||
            anima::dot(anima::cross(x, y), z) < .9999F)
            throw std::invalid_argument("Grip markers require a rigid right-handed frame; apply model scale first");
    }
    return result;
}
template <class T> inline const auto &lookup(const T &values, std::string_view id) {
    const auto found = values.find(id);
    if (found == values.end())
        throw std::invalid_argument("Missing presentation reference: " + std::string(id));
    return found->second;
}
template <class T> inline void insert(T &values, typename T::mapped_type entry) {
    const auto id = entry.id;
    if (!values.emplace(id, std::move(entry)).second)
        throw std::invalid_argument("Duplicate presentation identity: " + id);
}
} // namespace anima::presentation_data
