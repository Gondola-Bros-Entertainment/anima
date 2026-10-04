#pragma once
#include "../detail/json.hpp"
#include "../detail/utf8_path.hpp"
#include <algorithm>
#include <anima/assets/manifest.hpp>
#include <cstdint>
#include <fstream>
#include <map>
#include <set>
namespace anima::presentation_data {
inline constexpr std::size_t maximum_document_bytes = 4 * 1024 * 1024;
// Nesting levels a presentation document may use.
inline constexpr int maximum_document_depth = 64;
// The bottom row of an affine presentation transform is (0, 0, 0, 1) within this.
inline constexpr float affine_tolerance = 1e-6F;
// A rigid frame's axes are unit length and orthogonal within this, and its handedness triple product is
// within this of 1.
inline constexpr float rigid_tolerance = 1e-4F;
// Numbers in a document's 3D vector.
inline constexpr std::size_t vector_components = 3;
// The one action catalog version ActionRuntime accepts, which InteractionRuntime also writes for each role.
inline constexpr int action_catalog_version = 2;
using Json = nlohmann::json;
inline std::string text(const Json &value) {
    const auto result = value.get<std::string>();
    if (result.empty())
        throw std::invalid_argument("Empty presentation identity/reference");
    return result;
}
// The path that a document's file reference @p utf8 names, relative to the directory that the document resolves it
// against. Throws std::invalid_argument with @p message unless @p utf8 is nonempty without a NUL, colon or backslash,
// and the path has no root and no ".." component and ends in @p extension when that is nonempty. Colons and
// backslashes are rejected on every platform, since Windows reads them as a drive and as separators, so a document
// accepted anywhere names a file inside its directory on Windows too.
inline std::filesystem::path relative_document_path(std::string_view utf8, std::string_view extension,
                                                    const char *message) {
    if (utf8.empty() || utf8.find('\0') != std::string_view::npos || utf8.find(':') != std::string_view::npos ||
        utf8.find('\\') != std::string_view::npos)
        throw std::invalid_argument(message);
    auto result = detail::utf8_path(utf8);
    if (result.has_root_path() ||
        std::any_of(result.begin(), result.end(), [](const auto &part) { return part == ".."; }) ||
        (!extension.empty() && result.extension() != detail::utf8_path(extension)))
        throw std::invalid_argument(message);
    return result;
}
inline Json parse(std::string_view source, int maximum_depth = maximum_document_depth) {
    return detail::parse_json(source, maximum_document_bytes, maximum_depth);
}
inline Json read(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto size = input.tellg();
    if (!input || size < 0 || static_cast<std::uintmax_t>(size) > maximum_document_bytes)
        throw std::invalid_argument("Missing/oversized presentation document: " + detail::utf8_text(path));
    std::string source(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    if (!input.read(source.data(), static_cast<std::streamsize>(source.size())))
        throw std::runtime_error("Cannot read presentation document");
    return parse(source);
}
// Reads a column-major frame; json_floats leaves every element finite.
inline anima::Mat4 matrix(const Json &value, bool rigid) {
    const anima::Mat4 result{
        detail::json_floats<anima::Mat4::size()>(value, "Presentation transform requires 16 column-major values")};
    if (std::abs(result[3]) > affine_tolerance || std::abs(result[7]) > affine_tolerance ||
        std::abs(result[11]) > affine_tolerance || std::abs(result[15] - 1) > affine_tolerance)
        throw std::invalid_argument("Presentation transform must be affine");
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
// Reads a vector of three numbers, each finite as json_floats reads it. Throws std::invalid_argument with message
// unless value is an array of three.
inline anima::Vec3 vec3(const Json &value, const char *message) {
    const auto [x, y, z] = detail::json_floats<vector_components>(value, message);
    return {x, y, z};
}
template <class T> inline const auto &lookup(const T &values, std::string_view id) {
    const auto found = values.find(id);
    if (found == values.end())
        throw std::out_of_range("Missing presentation reference: " + std::string(id));
    return found->second;
}
template <class T> inline void insert(T &values, typename T::mapped_type entry) {
    const auto id = entry.id;
    if (!values.emplace(id, std::move(entry)).second)
        throw std::invalid_argument("Duplicate presentation identity: " + id);
}
// Runs a step that decodes a presentation document. Lookups report a missing name as std::out_of_range; while
// decoding, that means the document names something that does not exist, so it becomes Error, as json_step
// turns JSON errors into Error.
template <class Error = std::invalid_argument, class Step> decltype(auto) decode_step(Step &&step) {
    try {
        return anima::detail::json_step<Error>(std::forward<Step>(step));
    } catch (const std::out_of_range &error) {
        throw Error(error.what());
    }
}
} // namespace anima::presentation_data
