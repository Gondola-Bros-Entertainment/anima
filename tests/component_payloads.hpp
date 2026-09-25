#pragma once
#include <string>
#include <string_view>
#include <vector>

// Mutate an otherwise valid component payload. In particular, the first field
// must not be silently overwritten by a later duplicate, even with JSON escapes.
inline std::vector<std::string> invalid_component_payloads(std::string_view valid, std::string_view field) {
    const auto rest = std::string(valid.substr(1));
    auto escaped = std::string(field);
    constexpr char hex[] = "0123456789abcdef";
    const auto first = static_cast<unsigned char>(escaped.front());
    escaped.replace(0, 1, std::string("\\u00") + hex[first >> 4] + hex[first & 15]);
    auto renamed = std::string(valid);
    const auto field_key = "\"" + std::string(field) + "\"";
    renamed.replace(renamed.find(field_key), field_key.size(), "\"unexpected\"");
    return {"{}",
            renamed,
            "{\"unexpected\":null," + rest,
            "{\"" + std::string(field) + "\":null," + rest,
            "{\"" + escaped + "\":null," + rest,
            "{\"unexpected\":" + std::string(64, '[') + "0" + std::string(64, ']') + "," + rest};
}
