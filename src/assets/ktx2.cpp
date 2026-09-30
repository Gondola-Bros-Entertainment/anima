#include "bc7.hpp"
#include <algorithm>
#include <anima/assets/ktx2.hpp>
#include <array>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

// Reads the subset of KTX 2.0 (Khronos, "KTX File Format Specification", version 2.0) that holds one BC7 image:
// the header, the level index, the data format descriptor's basic block and the levels, all little-endian.
namespace anima {
namespace {
constexpr std::size_t maximum_file_bytes = 64 * 1024 * 1024;
constexpr std::uint32_t maximum_edge = 8192;
constexpr std::size_t maximum_texels = 16 * 1024 * 1024;
constexpr std::array<std::uint8_t, 12> identifier{0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};
// Offsets of the header fields that follow the identifier, and of the level index that follows them.
constexpr std::size_t format_offset = 12, type_size_offset = 16, width_offset = 20, height_offset = 24,
                      depth_offset = 28, layers_offset = 32, faces_offset = 36, levels_offset = 40,
                      supercompression_offset = 44, dfd_offset = 48, dfd_length_offset = 52, kvd_offset = 56,
                      kvd_length_offset = 60, sgd_length_offset = 72, level_index_offset = 80;
// Each level index entry: byteOffset, byteLength and uncompressedByteLength.
constexpr std::size_t level_index_entry = 24, level_length = 8, level_uncompressed_length = 16;
constexpr std::uint32_t vk_format_bc7_unorm_block = 145, vk_format_bc7_srgb_block = 146;
// The data format descriptor (Khronos Data Format Specification 1.3): its 4-byte dfdTotalSize, then the basic
// block, whose 24-byte header holds, at these offsets from the block, the vendor and descriptor type, the version
// number and block size, the color model, the transfer function, the texel block dimensions and bytesPlane0, and
// which describes each sample in 16 bytes.
constexpr std::size_t total_size_bytes = 4, basic_block_header = 24, sample_bytes = 16;
constexpr std::size_t block_type = 0, block_version = 4, block_model = 8, block_transfer = 10, block_dimensions = 12,
                      block_bytes_plane0 = 16;
// KHR_DF_VENDORID_KHRONOS with KHR_DF_KHR_DESCRIPTORTYPE_BASICFORMAT, and KHR_DF_VERSIONNUMBER_1_3, which shares its
// word with the block size in the upper 16 bits.
constexpr std::uint32_t khronos_basic_format = 0, version_1_3 = 2, version_mask = 0xFFFF, block_size_shift = 16;
constexpr std::uint8_t model_bc7 = 134, transfer_linear = 1, transfer_srgb = 2;
// The texel block dimensions of BC7, each stored minus one in its own byte, and its one plane of
// detail::bc7_block_bytes.
constexpr std::uint32_t bc7_dimensions = (detail::bc7_block_edge - 1) | ((detail::bc7_block_edge - 1) << 8);
// Levels of a format whose blocks take 16 bytes start at multiples of 16.
constexpr std::size_t bc7_alignment = detail::bc7_block_bytes;

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
    require(!file.u64(sgd_length_offset), "KTX2 has supercompression global data without supercompression");
    require(file.contains(level_index_offset, std::uint64_t{levels} * level_index_entry),
            "KTX2 level index exceeds the file");
    // The data format descriptor: its total size, then exactly one basic block describing BC7 in the format's
    // transfer function.
    const auto descriptor = file.u32(dfd_offset), descriptor_bytes = file.u32(dfd_length_offset);
    require(file.contains(descriptor, descriptor_bytes) &&
                descriptor_bytes >= total_size_bytes + basic_block_header + sample_bytes &&
                file.u32(descriptor) == descriptor_bytes,
            "Invalid KTX2 data format descriptor");
    const auto block = std::size_t{descriptor} + total_size_bytes;
    const auto version = file.u32(block + block_version);
    require(file.u32(block + block_type) == khronos_basic_format && (version & version_mask) == version_1_3 &&
                version >> block_size_shift == descriptor_bytes - total_size_bytes,
            "KTX2 data format descriptor must be one basic block");
    require(file.u8(block + block_model) == model_bc7 && file.u32(block + block_dimensions) == bc7_dimensions &&
                file.u8(block + block_bytes_plane0) == detail::bc7_block_bytes,
            "KTX2 data format descriptor does not describe BC7");
    require(file.u8(block + block_transfer) == (srgb ? transfer_srgb : transfer_linear),
            "KTX2 data format descriptor's transfer function does not match its format");
    const auto key_values = file.u32(kvd_offset), key_value_bytes = file.u32(kvd_length_offset);
    require(!key_value_bytes || file.contains(key_values, key_value_bytes), "KTX2 key/value data exceed the file");
    // Check every level before allocating the image, whose size the header alone would otherwise decide.
    std::vector<std::span<const std::byte>> stored;
    stored.reserve(levels);
    for (std::uint32_t level = 0; level < levels; ++level) {
        const auto entry = level_index_offset + std::size_t{level} * level_index_entry;
        const auto offset = file.u64(entry), length = file.u64(entry + level_length),
                   uncompressed = file.u64(entry + level_uncompressed_length);
        const auto name = "KTX2 mip level " + std::to_string(level);
        require(offset % bc7_alignment == 0, name + " is not aligned to 16 bytes");
        require(offset >= level_index_offset + std::uint64_t{levels} * level_index_entry,
                name + " overlaps the header");
        require(length == detail::bc7_level_bytes(std::max(width >> level, 1U), std::max(height >> level, 1U)),
                name + " does not hold exactly its BC7 blocks");
        require(uncompressed == length, name + " has an uncompressed length other than its length");
        require(file.contains(offset, length), name + " exceeds the file");
        stored.push_back(bytes.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(length)));
    }
    Image image{width, height, {}, ImageFormat::bc7, levels, {}};
    image.blocks.resize(detail::bc7_image_bytes(width, height, levels));
    auto out = image.blocks.begin();
    for (const auto level : stored)
        out = std::ranges::transform(level, out, [](std::byte value) {
                  return std::to_integer<std::uint8_t>(value);
              }).out;
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
    file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(bool(file), "Cannot read KTX2 file");
    return load_ktx2(bytes, encoding);
}
} // namespace anima
