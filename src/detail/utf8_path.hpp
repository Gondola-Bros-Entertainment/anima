#pragma once
#include <filesystem>
#include <string>
#include <string_view>

// Paths in documents and messages are UTF-8 on every platform. A narrow std::string converts through the active code
// page on Windows, which names another file for non-ASCII text, and path::string() throws std::system_error for a
// character that code page lacks.
namespace anima::detail {
// The path that the UTF-8 text @p utf8 names.
inline std::filesystem::path utf8_path(std::string_view utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}
// @p path as UTF-8 text, for exception messages.
inline std::string utf8_text(const std::filesystem::path &path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}
} // namespace anima::detail
