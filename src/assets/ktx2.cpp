#include "bc7.hpp"
#include <algorithm>
#include <anima/assets/ktx2.hpp>
#include <array>
#include <fstream>
#include <stdexcept>
#include <string>

// Reads the subset of KTX 2.0 (Khronos, "KTX File Format Specification", version 2.0) that holds one BC7 image:
// the header, the level index, the data format descriptor's basic block and the levels, all little-endian.
namespace anima {
namespace {
constexpr std::size_t maximum_file_bytes = 64 * 1024 * 1024;
constexpr std::uint32_t maximum_edge = 8192;
constexpr std::size_t maximum_texels = 16 * 1024 * 1024;
constexpr std::array<std::uint8_t, 12> identifier{0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};
// Offsets of the header fields that follow the identifier, and of the index that follows them.
constexpr std::size_t format_offset = 12, type_size_offset = 16, width_offset = 20, height_offset = 24,
                      depth_offset = 28, layers_offset = 32, faces_offset = 36, levels_offset = 40,
                      supercompression_offset = 44, dfd_offset = 48, kvd_offset = 56, sgd_offset = 64,
                      level_index_offset = 80, level_index_entry = 24;
constexpr std::uint32_t vk_format_bc7_unorm_block = 145, vk_format_bc7_srgb_block = 146;
// The data format descriptor's basic block: a 4-byte total size, then the block's 24-byte header and 16 bytes of
// each sample.
constexpr std::size_t basic_block_header = 24, sample_bytes = 16;
constexpr std::uint32_t basic_block_version = 2;
constexpr std::uint8_t model_bc7 = 134, transfer_linear = 1, transfer_srgb = 2;
constexpr std::size_t bc7_alignment = 16;

void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
class Reader {
  public:
    explicit Reader(std::span<const std::byte> file) : bytes_(file) {}
    [[nodiscard]] std::uint64_t read(std::size_t offset, std::size_t width) const {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < width; ++i)
            value |= std::uint64_t(std::to_integer<std::uint8_t>(bytes_[offset + i])) << (8 * i);
        return value;
    }
    [[nodiscard]] std::uint32_t u32(std::size_t offset) const { return static_cast<std::uint32_t>(read(offset, 4)); }
    [[nodiscard]] std::uint64_t u64(std::size_t offset) const { return read(offset, 8); }
    [[nodiscard]] std::uint8_t u8(std::size_t offset) const { return static_cast<std::uint8_t>(read(offset, 1)); }
    // Whether the @p length bytes at @p offset lie inside the file.
    [[nodiscard]] bool contains(std::uint64_t offset, std::uint64_t length) const {
        return offset <= bytes_.size() && length <= bytes_.size() - offset;
    }

  private:
    std::span<const std::byte> bytes_;
};
} // namespace

std::shared_ptr<const Image> load_ktx2(std::span<const std::byte> bytes, TextureEncoding encoding) {
    if (encoding != TextureEncoding::srgb && encoding != TextureEncoding::linear)
        throw std::invalid_argument("Unknown texture encoding");
    require(!bytes.empty() && bytes.size() <= maximum_file_bytes, "KTX2 must be between 1 byte and 64 MiB");
    const Reader file(bytes);
    require(file.contains(0, level_index_offset) && std::equal(identifier.begin(), identifier.end(), bytes.begin(),
                                                               [](std::uint8_t expected, std::byte actual) {
                                                                   return std::to_integer<std::uint8_t>(actual) ==
                                                                          expected;
                                                               }),
            "Not a KTX 2.0 file");
    const auto format = file.u32(format_offset);
    require(format == vk_format_bc7_srgb_block || format == vk_format_bc7_unorm_block,
            "KTX2 format must be VK_FORMAT_BC7_SRGB_BLOCK or VK_FORMAT_BC7_UNORM_BLOCK");
    const bool srgb = format == vk_format_bc7_srgb_block;
    require(srgb == (encoding == TextureEncoding::srgb),
            srgb ? "KTX2 format VK_FORMAT_BC7_SRGB_BLOCK does not match a linear texture"
                 : "KTX2 format VK_FORMAT_BC7_UNORM_BLOCK does not match an sRGB texture");
    require(file.u32(type_size_offset) == 1, "KTX2 type size must be 1 for a block-compressed format");
    const auto width = file.u32(width_offset), height = file.u32(height_offset);
    require(width && height && !file.u32(depth_offset) && !file.u32(layers_offset) && file.u32(faces_offset) == 1,
            "KTX2 must hold one 2D image, not an array, cube map or volume");
    require(width <= maximum_edge && height <= maximum_edge && std::size_t{width} * height <= maximum_texels,
            "KTX2 image exceeds import dimensions or texel limit");
    const auto levels = file.u32(levels_offset);
    require(levels != 0, "KTX2 level count 0 asks for generated mip levels, which BC7 images cannot have");
    require(levels <= detail::full_mip_levels(width, height), "KTX2 level count exceeds the image's mip chain");
    require(file.u32(supercompression_offset) == 0, "KTX2 supercompression is unsupported");
    require(!file.u64(sgd_offset + 8), "KTX2 has supercompression global data without supercompression");
    require(file.contains(level_index_offset, std::uint64_t{levels} * level_index_entry),
            "KTX2 level index exceeds the file");
    // The data format descriptor: its total size, then exactly one basic block describing BC7 in the format's
    // transfer function.
    const auto descriptor = file.u32(dfd_offset), descriptor_bytes = file.u32(dfd_offset + 4);
    require(file.contains(descriptor, descriptor_bytes) && descriptor_bytes >= 4 + basic_block_header + sample_bytes &&
                file.u32(descriptor) == descriptor_bytes,
            "Invalid KTX2 data format descriptor");
    const auto block = std::size_t{descriptor} + 4;
    require(file.u32(block) == 0 && (file.u32(block + 4) & 0xFFFF) == basic_block_version &&
                (file.u32(block + 4) >> 16) == descriptor_bytes - 4,
            "KTX2 data format descriptor must be one basic block");
    require(file.u8(block + 8) == model_bc7 && file.u32(block + 12) == 0x0303 && file.u8(block + 16) == 16,
            "KTX2 data format descriptor does not describe BC7");
    require(file.u8(block + 10) == (srgb ? transfer_srgb : transfer_linear),
            "KTX2 data format descriptor's transfer function does not match its format");
    const auto key_values = file.u32(kvd_offset), key_value_bytes = file.u32(kvd_offset + 4);
    require(!key_value_bytes || file.contains(key_values, key_value_bytes), "KTX2 key/value data exceed the file");
    Image image{width, height, {}, ImageFormat::bc7, levels, {}};
    image.blocks.reserve(detail::bc7_image_bytes(width, height, levels));
    for (std::uint32_t level = 0; level < levels; ++level) {
        const auto entry = level_index_offset + std::size_t{level} * level_index_entry;
        const auto offset = file.u64(entry), length = file.u64(entry + 8), uncompressed = file.u64(entry + 16);
        const auto name = "KTX2 mip level " + std::to_string(level);
        require(offset % bc7_alignment == 0, name + " is not aligned to 16 bytes");
        require(offset >= level_index_offset + std::uint64_t{levels} * level_index_entry,
                name + " overlaps the header");
        require(length == detail::bc7_level_bytes(std::max(width >> level, 1U), std::max(height >> level, 1U)),
                name + " does not hold exactly its BC7 blocks");
        require(uncompressed == length, name + " has an uncompressed length other than its length");
        require(file.contains(offset, length), name + " exceeds the file");
        const auto data = bytes.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(length));
        for (const auto value : data)
            image.blocks.push_back(std::to_integer<std::uint8_t>(value));
    }
    return std::make_shared<const Image>(std::move(image));
}
std::shared_ptr<const Image> load_ktx2(const std::filesystem::path &path, TextureEncoding encoding) {
    if (encoding != TextureEncoding::srgb && encoding != TextureEncoding::linear)
        throw std::invalid_argument("Unknown texture encoding");
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    require(bool(file), "Cannot open KTX2 file");
    const auto length = file.tellg();
    require(length > 0 && length <= static_cast<std::streamoff>(maximum_file_bytes),
            "KTX2 must be between 1 byte and 64 MiB");
    std::vector<std::byte> bytes(static_cast<std::size_t>(length));
    file.seekg(0);
    file.read(reinterpret_cast<char *>(bytes.data()), length);
    require(bool(file), "Cannot read KTX2 file");
    return load_ktx2(bytes, encoding);
}
} // namespace anima
