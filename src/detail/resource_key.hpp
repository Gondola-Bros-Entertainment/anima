#pragma once
#include <cstddef>
#include <stdexcept>
#include <string_view>

namespace anima::detail {
inline constexpr std::size_t maximum_resource_key_bytes = 4096;

// Throws std::invalid_argument(message) unless @p key is a nonempty string of at most 4,096 bytes
// of well-formed UTF-8 without NUL. Well-formed excludes overlong encodings, UTF-16 surrogates
// (U+D800 to U+DFFF), code points above U+10FFFF and truncated sequences, so a valid key always
// encodes as a JSON string.
inline void validate_resource_key(std::string_view key, const char *message) {
    if (key.empty() || key.size() > maximum_resource_key_bytes)
        throw std::invalid_argument(message);
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(key[i]); };
    for (std::size_t i = 0; i < key.size();) {
        const auto lead = byte(i);
        if (lead == 0)
            throw std::invalid_argument(message);
        if (lead < 0x80) {
            ++i;
            continue;
        }
        // The length of the sequence and the range of its second byte, which rules out overlong
        // forms, surrogates and code points past U+10FFFF; later bytes are plain continuations.
        std::size_t length = 0;
        unsigned char low = 0x80, high = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF)
            length = 2;
        else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            if (lead == 0xE0)
                low = 0xA0;
            else if (lead == 0xED)
                high = 0x9F;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            if (lead == 0xF0)
                low = 0x90;
            else if (lead == 0xF4)
                high = 0x8F;
        } else
            throw std::invalid_argument(message);
        if (key.size() - i < length || byte(i + 1) < low || byte(i + 1) > high)
            throw std::invalid_argument(message);
        for (std::size_t k = 2; k < length; ++k)
            if ((byte(i + k) & 0xC0U) != 0x80U)
                throw std::invalid_argument(message);
        i += length;
    }
}
} // namespace anima::detail
