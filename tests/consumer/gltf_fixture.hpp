#pragma once
// Builds binary glTF 2.0 fixtures in memory for the GPU checks: accessors over one binary buffer, embedded PNG
// images and a JSON document whose other members the caller writes.
#include "gpu_checks.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gltf_fixture {
/// A float formatted for JSON, independently of the global locale.
inline std::string number(double value) {
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text.precision(9);
    text << value;
    return text.str();
}
inline std::size_t components(std::string_view type) {
    if (type == "SCALAR")
        return 1;
    if (type == "VEC2")
        return 2;
    if (type == "VEC3")
        return 3;
    if (type == "VEC4")
        return 4;
    if (type == "MAT4")
        return 16;
    throw std::invalid_argument("Unknown glTF accessor type");
}
class Builder {
  public:
    /// Appends @p values as one buffer view and a float accessor of glTF @p type. With @p bounds it records the
    /// minimum and maximum that glTF requires of positions and animation inputs. Returns the accessor index.
    int floats(const std::vector<float> &values, std::string_view type, bool bounds = false) {
        const auto width = components(type);
        const auto view = append(values.data(), values.size() * sizeof(float), 0);
        std::string accessor = R"({"bufferView":)" + std::to_string(view) + R"(,"componentType":5126,"count":)" +
                               std::to_string(values.size() / width) + R"(,"type":")" + std::string(type) + '"';
        if (bounds)
            accessor += limits(values, width, 0, width);
        accessors_.push_back(accessor + '}');
        return int(accessors_.size() - 1);
    }
    /// Appends @p values as one buffer view and an unsigned short accessor of glTF @p type; returns its index.
    int shorts(const std::vector<std::uint16_t> &values, std::string_view type) {
        const auto view = append(values.data(), values.size() * sizeof(std::uint16_t), 0);
        accessors_.push_back(R"({"bufferView":)" + std::to_string(view) + R"(,"componentType":5123,"count":)" +
                             std::to_string(values.size() / components(type)) + R"(,"type":")" + std::string(type) +
                             "\"}");
        return int(accessors_.size() - 1);
    }
    /// Appends vertices of @p stride floats each as one buffer view with that byte stride, and a float accessor
    /// for each attribute in @p layout, given as its glTF type and its offset in floats. The first attribute is
    /// the position and records its bounds. Returns the first attribute's accessor; the others follow in order.
    int interleaved(const std::vector<float> &vertices, std::size_t stride,
                    std::initializer_list<std::pair<std::string_view, std::size_t>> layout) {
        const auto view = append(vertices.data(), vertices.size() * sizeof(float), stride * sizeof(float));
        const auto first = int(accessors_.size());
        for (const auto &[type, offset] : layout) {
            std::string accessor = R"({"bufferView":)" + std::to_string(view) + R"(,"byteOffset":)" +
                                   std::to_string(offset * sizeof(float)) + R"(,"componentType":5126,"count":)" +
                                   std::to_string(vertices.size() / stride) + R"(,"type":")" + std::string(type) + '"';
            if (int(accessors_.size()) == first)
                accessor += limits(vertices, stride, offset, components(type));
            accessors_.push_back(accessor + '}');
        }
        return first;
    }
    /// Embeds 8-bit RGB (@p channels 3) or RGBA (4) pixels as a PNG image; returns the image index.
    int png(std::uint32_t width, std::uint32_t height, unsigned channels, const std::vector<std::uint8_t> &pixels) {
        const auto file = gpu_check::png::encode(width, height, channels, pixels);
        const auto view = append(file.data(), file.size(), 0);
        images_.push_back(R"({"bufferView":)" + std::to_string(view) + R"(,"mimeType":"image/png"})");
        return int(images_.size() - 1);
    }
    /// The GLB file. @p members are the document's other top-level members, such as scenes, nodes, meshes,
    /// materials, textures, samplers, skins and animations, written as JSON without enclosing braces.
    [[nodiscard]] std::vector<std::byte> glb(const std::string &members) const {
        std::string json = R"({"asset":{"version":"2.0"},)" + members + R"(,"buffers":[{"byteLength":)" +
                           std::to_string(binary_.size()) + "}]" + list("bufferViews", views_) +
                           list("accessors", accessors_) + list("images", images_) + '}';
        json.append((4 - json.size() % 4) % 4, ' ');
        auto binary = binary_;
        binary.resize((binary.size() + 3) / 4 * 4);
        std::vector<std::byte> file;
        const auto word = [&](std::uint32_t value) {
            for (int shift = 0; shift < 32; shift += 8)
                file.push_back(std::byte(value >> shift));
        };
        constexpr std::uint32_t magic = 0x46546C67, version = 2, header_bytes = 12, chunk_header_bytes = 8;
        constexpr std::uint32_t json_chunk = 0x4E4F534A, binary_chunk = 0x004E4942;
        word(magic);
        word(version);
        word(std::uint32_t(header_bytes + 2 * chunk_header_bytes + json.size() + binary.size()));
        word(std::uint32_t(json.size()));
        word(json_chunk);
        for (const char c : json)
            file.push_back(std::byte(c));
        word(std::uint32_t(binary.size()));
        word(binary_chunk);
        for (const auto byte : binary)
            file.push_back(std::byte(byte));
        return file;
    }

  private:
    std::vector<std::uint8_t> binary_;
    std::vector<std::string> views_, accessors_, images_;
    std::size_t append(const void *data, std::size_t bytes, std::size_t stride) {
        binary_.resize((binary_.size() + 3) / 4 * 4);
        const auto offset = binary_.size();
        binary_.resize(offset + bytes);
        std::memcpy(binary_.data() + offset, data, bytes);
        std::string view =
            R"({"buffer":0,"byteOffset":)" + std::to_string(offset) + R"(,"byteLength":)" + std::to_string(bytes);
        if (stride)
            view += R"(,"byteStride":)" + std::to_string(stride);
        views_.push_back(view + '}');
        return views_.size() - 1;
    }
    static std::string limits(const std::vector<float> &values, std::size_t stride, std::size_t offset,
                              std::size_t width) {
        std::string minimum = R"(,"min":[)", maximum = R"(,"max":[)";
        for (std::size_t c = 0; c < width; ++c) {
            float low = values[offset + c], high = low;
            for (std::size_t i = offset + c; i < values.size(); i += stride) {
                low = std::min(low, values[i]);
                high = std::max(high, values[i]);
            }
            minimum += (c ? "," : "") + number(low);
            maximum += (c ? "," : "") + number(high);
        }
        return minimum + ']' + maximum + ']';
    }
    static std::string list(const char *name, const std::vector<std::string> &items) {
        if (items.empty())
            return {};
        std::string text = std::string(",\"") + name + "\":[";
        for (std::size_t i = 0; i < items.size(); ++i)
            text += (i ? "," : "") + items[i];
        return text + ']';
    }
};
} // namespace gltf_fixture
