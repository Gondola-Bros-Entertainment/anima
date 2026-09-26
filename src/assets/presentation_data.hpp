#pragma once
#include "../detail/json.hpp"
#include <anima/assets/preview.hpp>
#include <cstdint>
#include <fstream>
#include <map>
#include <set>
namespace anima::presentation_data {
inline constexpr std::size_t maximum_document_bytes = 4 * 1024 * 1024;
// The bottom row of an affine presentation transform is (0, 0, 0, 1) within this.
inline constexpr float affine_tolerance = 1e-6F;
// A rigid frame's axes are unit length and orthogonal within this, and its handedness triple product is
// within this of 1.
inline constexpr float rigid_tolerance = 1e-4F;
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
        std::abs(result[3]) > affine_tolerance || std::abs(result[7]) > affine_tolerance ||
        std::abs(result[11]) > affine_tolerance || std::abs(result[15] - 1) > affine_tolerance)
        throw std::invalid_argument("Presentation transform must be finite and affine");
    (void)anima::inverse(result);
    if (rigid) {
        const auto x = anima::axis_x(result), y = anima::axis_y(result), z = anima::axis_z(result);
        if (std::abs(anima::length(x) - 1) > rigid_tolerance || std::abs(anima::length(y) - 1) > rigid_tolerance ||
            std::abs(anima::length(z) - 1) > rigid_tolerance || std::abs(anima::dot(x, y)) > rigid_tolerance ||
            std::abs(anima::dot(x, z)) > rigid_tolerance || std::abs(anima::dot(y, z)) > rigid_tolerance ||
            anima::dot(anima::cross(x, y), z) < 1 - rigid_tolerance)
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
