#include <anima/assets/ktx2.hpp>
#include <anima/assets/material_textures.hpp>
#include <anima/assets/mesh_preparation.hpp>
#include <anima/assets/scene_validation.hpp>
#include <anima/mesh.hpp>
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// The reference decodes are PNG files; this copy of the decoder is private to the test.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include <stb_image.h>

using namespace anima;
namespace {
const std::filesystem::path fixtures = ANIMA_TEXTURE_FIXTURES;

std::vector<std::byte> read_file(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    REQUIRE(file);
    std::vector<std::byte> bytes(static_cast<std::size_t>(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(file);
    return bytes;
}
// An RGBA8 PNG fixture: its width, height and texels.
struct Decoded {
    int width{}, height{};
    std::vector<std::uint8_t> rgba;
};
Decoded read_png(const std::filesystem::path &path) {
    const auto bytes = read_file(path);
    Decoded result;
    int channels = 0;
    auto *texels = stbi_load_from_memory(reinterpret_cast<const stbi_uc *>(bytes.data()),
                                         static_cast<int>(bytes.size()), &result.width, &result.height, &channels, 4);
    REQUIRE(texels);
    result.rgba.assign(texels, texels + std::size_t(result.width) * result.height * 4);
    stbi_image_free(texels);
    return result;
}
// Little-endian fields of a KTX2 file, for building rejected variants of a valid one.
void put32(std::vector<std::byte> &bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        bytes[offset + i] = static_cast<std::byte>(value >> (8 * i));
}
void put64(std::vector<std::byte> &bytes, std::size_t offset, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        bytes[offset + i] = static_cast<std::byte>(value >> (8 * i));
}
std::uint32_t get32(const std::vector<std::byte> &bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
        value |= std::to_integer<std::uint32_t>(bytes[offset + i]) << (8 * i);
    return value;
}
// The KTX 2.0 layout, written out here independently of the reader. Header fields: format, type size, width,
// height, depth, layers, faces, levels, supercompression; then the data format descriptor's offset and length, the
// key/value data's offset and length, the supercompression global data's length, and the first level index entry.
constexpr std::size_t format_field = 12, type_size_field = 16, width_field = 20, height_field = 24, depth_field = 28,
                      layers_field = 32, faces_field = 36, levels_field = 40, supercompression_field = 44,
                      dfd_field = 48, dfd_length_field = 52, kvd_field = 56, kvd_length_field = 60,
                      sgd_length_field = 72, first_level = 80;
// Each level index entry: byteOffset, byteLength and uncompressedByteLength.
constexpr std::size_t level_entry = 24, level_length = 8, level_uncompressed_length = 16;
// Fields of the data format descriptor, from its dfdTotalSize: the basic block's vendor and descriptor type, its
// version number and size, color model, transfer function, first texel block dimension and bytesPlane0.
constexpr std::size_t dfd_type = 4, dfd_version = 8, dfd_model = 12, dfd_transfer = 14, dfd_dimension = 16,
                      dfd_bytes_plane0 = 20;
// The fixtures' basic block: a 24-byte header and one 16-byte sample, after the 4-byte dfdTotalSize. Its size
// shares a word with the version number, in the upper 16 bits.
constexpr std::uint32_t basic_block_bytes = 24 + 16, block_size_shift = 16;
// Values from the Vulkan and Khronos data format headers.
constexpr std::uint32_t vk_format_undefined = 0, r8g8b8a8_unorm = 37, khr_df_versionnumber_1_2 = 1,
                        khr_df_versionnumber_1_3 = 2;
constexpr std::uint8_t khr_df_model_bc1a = 128, khr_df_transfer_linear = 1;
constexpr std::uint32_t basislz = 1, zstandard = 2, zlib = 3;
// A texel block dimension of 8, stored minus one; BC1's 8-byte plane; the type size of 32-bit components; the faces
// of a cube map; a descriptor block from a vendor other than Khronos.
constexpr std::uint8_t eight_texel_dimension = 7, bc1_bytes_plane0 = 8;
constexpr std::uint32_t r32_type_size = 4, cube_faces = 6, other_vendor = 1;
// The reader's limits.
constexpr std::size_t maximum_file_bytes = 64 * 1024 * 1024;
constexpr std::uint32_t maximum_edge = 8192, maximum_texels = 16 * 1024 * 1024;
// Bytes of one BC7 block.
constexpr std::uint32_t bc7_block_bytes = 16;

// Every level of @p image against the reference decode beside the fixture: the levels decoded by basisu, side by
// side from the base level, whose whole blocks ImageMagick decodes identically (see tests/textures/README.md).
void check_against_reference(const Image &image, const std::string &name) {
    const auto reference = read_png(fixtures / (name + "-bc7-decoded.png"));
    const auto levels = decode_image(image);
    REQUIRE(levels.size() == image.levels);
    int left = 0;
    for (std::size_t level = 0; level < levels.size(); ++level) {
        const auto &decoded = levels[level];
        CAPTURE(name);
        CAPTURE(level);
        CHECK(decoded.width == std::max(image.width >> level, 1U));
        CHECK(decoded.height == std::max(image.height >> level, 1U));
        REQUIRE(left + int(decoded.width) <= reference.width);
        REQUIRE(int(decoded.height) <= reference.height);
        std::size_t mismatches = 0;
        for (std::uint32_t y = 0; y < decoded.height; ++y)
            for (std::uint32_t x = 0; x < decoded.width; ++x) {
                const auto *texel = decoded.rgba.data() + (std::size_t(y) * decoded.width + x) * 4;
                mismatches += !std::equal(texel, texel + 4,
                                          reference.rgba.data() + (std::size_t(y) * reference.width + left + x) * 4);
            }
        CHECK(mismatches == 0);
        left += int(decoded.width);
    }
    CHECK(left == reference.width);
}
// The modes of the blocks of @p image, and its rotations, index selections and partitions by subset count.
struct Coverage {
    std::set<unsigned> modes, rotations, selections;
    std::map<unsigned, std::set<unsigned>> partitions;
    void add(const Image &image) {
        for (std::size_t i = 0; i < image.blocks.size(); i += 16) {
            const auto *block = &image.blocks[i];
            unsigned mode = 0;
            while (mode < 8 && !(block[0] & (1U << mode)))
                ++mode;
            modes.insert(mode);
            const auto field = [&](unsigned first, unsigned bits) {
                unsigned value = 0;
                for (unsigned bit = 0; bit < bits; ++bit)
                    value |= unsigned((block[(first + bit) / 8] >> ((first + bit) % 8)) & 1U) << bit;
                return value;
            };
            if (mode == 0 || mode == 2)
                partitions[3].insert(field(mode + 1, mode == 0 ? 4 : 6));
            else if (mode == 1 || mode == 3 || mode == 7)
                partitions[2].insert(field(mode + 1, 6));
            else if (mode == 4 || mode == 5)
                rotations.insert(field(mode + 1, 2));
            if (mode == 4)
                selections.insert(field(7, 1));
        }
    }
};
// A texture over @p image with the default sampler.
Texture texture_of(std::shared_ptr<const Image> image, TextureEncoding encoding = TextureEncoding::srgb) {
    return {std::move(image), {}, encoding};
}
// A snapshot of @p textures alone.
MeshSnapshot snapshot_of(std::vector<Texture> textures) {
    MeshSnapshot result;
    result.textures = std::move(textures);
    return result;
}
// One triangle whose material samples texture 0 as its base color, masked so that RGBA8 mips would need their own
// coverage-preserving copy.
Asset textured_triangle(std::shared_ptr<const Image> image) {
    Asset source;
    source.nodes.resize(1);
    Material material;
    material.texture = 0;
    material.alpha_mode = AlphaMode::mask;
    source.materials.push_back(material);
    source.textures.push_back(texture_of(std::move(image)));
    SourcePrimitive primitive;
    primitive.material = 0;
    for (const auto corner : {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}}) {
        SourceVertex vertex;
        vertex.position = corner;
        vertex.normal = {0, 0, 1};
        primitive.vertices.push_back(vertex);
    }
    source.primitives.push_back(primitive);
    return source;
}
std::shared_ptr<const Image> pattern() { return load_ktx2(fixtures / "pattern-bc7.ktx2", TextureEncoding::srgb); }
void rejects(const std::vector<std::byte> &bytes, const char *message,
             TextureEncoding encoding = TextureEncoding::srgb) {
    CHECK_THROWS_WITH_AS((void)load_ktx2(bytes, encoding), message, std::runtime_error);
}
} // namespace

TEST_CASE("KTX2 files load as BC7 images with every stored level") {
    const auto image = pattern();
    CHECK(image->format == ImageFormat::bc7);
    CHECK(image->width == 64);
    CHECK(image->height == 64);
    CHECK(image->levels == 7);
    CHECK(image->rgba.empty());
    // 256 + 64 + 16 + 4 + 1 + 1 + 1 blocks of 16 bytes.
    CHECK(image->blocks.size() == 343 * 16);
    const auto odd = load_ktx2(fixtures / "pattern-odd-bc7.ktx2", TextureEncoding::srgb);
    CHECK(odd->width == 30);
    CHECK(odd->height == 18);
    CHECK(odd->levels == 5);
    // Levels of 30x18, 15x9, 7x4, 3x2 and 1x1 texels cover 8x5, 4x3, 2x1, 1x1 and 1x1 blocks.
    CHECK(odd->blocks.size() == (40 + 12 + 2 + 1 + 1) * 16);
    const auto linear = load_ktx2(read_file(fixtures / "pattern-linear-bc7.ktx2"), TextureEncoding::linear);
    CHECK(linear->levels == 7);
    CHECK_NOTHROW(
        validate_scene(snapshot_of({texture_of(image), texture_of(odd), texture_of(linear, TextureEncoding::linear)})));
}

TEST_CASE("BC7 blocks decode exactly as two independent decoders decode them") {
    Coverage coverage;
    for (const auto &[name, encoding] :
         {std::pair{"pattern", TextureEncoding::srgb}, std::pair{"pattern-odd", TextureEncoding::srgb},
          std::pair{"pattern-linear", TextureEncoding::linear}, std::pair{"opaque", TextureEncoding::srgb},
          std::pair{"opaque-linear", TextureEncoding::linear}, std::pair{"partitions", TextureEncoding::linear}}) {
        const auto image = load_ktx2(fixtures / (std::string(name) + "-bc7.ktx2"), encoding);
        coverage.add(*image);
        check_against_reference(*image, name);
    }
    // The fixtures reach every mode, rotation and index selection, and every partition of two and three subsets.
    CHECK(coverage.modes == std::set<unsigned>{0, 1, 2, 3, 4, 5, 6, 7});
    CHECK(coverage.rotations == std::set<unsigned>{0, 1, 2, 3});
    CHECK(coverage.selections == std::set<unsigned>{0, 1});
    CHECK(coverage.partitions[2].size() == 64);
    CHECK(coverage.partitions[3].size() == 64);
}

TEST_CASE("Decoding the first levels of an image decodes them as decoding every level does") {
    const auto image = load_ktx2(fixtures / "pattern-bc7.ktx2", TextureEncoding::srgb);
    const auto every = decode_image(*image);
    for (std::uint32_t levels = 1; levels <= image->levels; ++levels) {
        CAPTURE(levels);
        const auto first = decode_image(*image, levels);
        REQUIRE(first.size() == levels);
        for (std::uint32_t level = 0; level < levels; ++level)
            CHECK(first[level].rgba == every[level].rgba);
    }
    constexpr auto message = "Decoded level count must be from 1 to the image's levels";
    CHECK_THROWS_WITH_AS((void)decode_image(*image, 0), message, std::invalid_argument);
    CHECK_THROWS_WITH_AS((void)decode_image(*image, image->levels + 1), message, std::invalid_argument);
}

TEST_CASE("A reserved BC7 block decodes to zero, and RGBA8 images decode as they are") {
    const Image reserved{4, 4, {}, ImageFormat::bc7, 1, std::vector<std::uint8_t>(16)};
    const auto levels = decode_image(reserved);
    REQUIRE(levels.size() == 1);
    CHECK(levels[0].rgba == std::vector<std::uint8_t>(64));
    const Image plain{1, 2, {1, 2, 3, 4, 5, 6, 7, 8}};
    const auto same = decode_image(plain);
    REQUIRE(same.size() == 1);
    CHECK(same[0].rgba == plain.rgba);
    CHECK_THROWS_WITH_AS((void)decode_image(Image{4, 4, {}, ImageFormat::bc7, 1, {}}), "Texture image has no texels",
                         std::invalid_argument);
}

TEST_CASE("Block-compressed images are validated against their dimensions and levels") {
    const auto image = pattern();
    const auto edited = [&](auto edit) {
        auto copy = *image;
        edit(copy);
        return snapshot_of({texture_of(std::make_shared<const Image>(std::move(copy)))});
    };
    CHECK_THROWS_WITH_AS(validate_scene(edited([](Image &i) { i.levels = 0; })),
                         "BC7 texture image must store 1 to all of its mip levels", std::invalid_argument);
    CHECK_THROWS_WITH_AS(validate_scene(edited([](Image &i) { i.levels = 8; })),
                         "BC7 texture image must store 1 to all of its mip levels", std::invalid_argument);
    CHECK_THROWS_WITH_AS(validate_scene(edited([](Image &i) { i.blocks.pop_back(); })),
                         "BC7 texture image byte count does not match its dimensions and levels",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(validate_scene(edited([](Image &i) { i.rgba = {0, 0, 0, 0}; })),
                         "BC7 texture image must not store RGBA8 texels", std::invalid_argument);
    CHECK_THROWS_WITH_AS(validate_scene(edited([](Image &i) { i.format = static_cast<ImageFormat>(7); })),
                         "Unknown texture image format", std::invalid_argument);
    const auto layered = std::make_shared<const Image>(Image{1, 1, {1, 2, 3, 4}, ImageFormat::rgba8, 2});
    CHECK_THROWS_WITH_AS(validate_scene(snapshot_of({texture_of(layered)})),
                         "RGBA8 texture image must store one level and no blocks", std::invalid_argument);
    // A description without texels is valid until something needs them.
    CHECK_NOTHROW(validate_scene(edited([](Image &i) { i.blocks.clear(); })));
    CHECK_THROWS_WITH_AS((void)texture_mips(texture_of(image)), "Mip filtering requires an RGBA8 image",
                         std::invalid_argument);
}

TEST_CASE("BC7 textures upload their stored levels, so their uses share one planned image") {
    const auto mesh = Mesh::compile(textured_triangle(pattern()));
    const auto plan = material_texture_plan(mesh->materials()->material_data, mesh->materials()->textures);
    // The white fallback and the one BC7 image, which the masked material's coverage rules leave alone.
    REQUIRE(plan.images.size() == 2);
    CHECK(plan.images[1].mips == TextureMipOptions{});
    const MeshPreparation prepared(mesh);
    REQUIRE(prepared.images().size() == 2);
    CHECK(prepared.images()[0].size() == 1); // The fallback's one texel.
    CHECK(prepared.images()[1].empty());     // Nothing to prepare.
}

TEST_CASE("A texture limit keeps a BC7 image's first stored level that fits") {
    const auto image = pattern();
    const auto meshes = Mesh::compile_static(textured_triangle(image), {.max_texture_edge = 16});
    REQUIRE(meshes.size() == 1);
    const auto &shrunk = *meshes[0]->materials()->textures.at(0).image;
    CHECK(shrunk.format == ImageFormat::bc7);
    CHECK(shrunk.width == 16);
    CHECK(shrunk.height == 16);
    CHECK(shrunk.levels == 5);
    // Levels 2 to 6 of the source: after 256 + 64 blocks.
    REQUIRE(shrunk.blocks.size() == (16 + 4 + 1 + 1 + 1) * 16);
    CHECK(std::equal(shrunk.blocks.begin(), shrunk.blocks.end(), image->blocks.begin() + (256 + 64) * 16));
    auto base_only = *image;
    base_only.levels = 1;
    base_only.blocks.resize(256 * 16);
    CHECK_THROWS_WITH_AS(Mesh::compile_static(textured_triangle(std::make_shared<const Image>(std::move(base_only))),
                                              {.max_texture_edge = 16}),
                         "Block-compressed texture stores no mip level within the texture limit",
                         std::invalid_argument);
}

TEST_CASE("A mesh compiled until upload describes a BC7 image by its format and levels") {
    std::weak_ptr<const Image> source;
    std::shared_ptr<const Mesh> mesh;
    {
        const auto image = pattern();
        source = image;
        mesh = Mesh::compile(textured_triangle(image), TexelRetention::until_upload);
    }
    const auto &description = *mesh->materials()->textures.at(0).image;
    CHECK(description.format == ImageFormat::bc7);
    CHECK(description.levels == 7);
    CHECK(description.blocks.empty());
    CHECK(mesh->texel_images().at(0) == source.lock());
    mesh->release_texels();
    CHECK(source.expired());
}

TEST_CASE("KTX2 files outside the supported subset are rejected with the rule they break") {
    const auto valid = read_file(fixtures / "pattern-bc7.ktx2");
    const auto with = [&](auto edit) {
        auto bytes = valid;
        edit(bytes);
        return bytes;
    };
    CHECK_THROWS_WITH_AS((void)load_ktx2(valid, static_cast<TextureEncoding>(2)), "Unknown texture encoding",
                         std::invalid_argument);
    rejects({}, "KTX2 must be between 1 byte and 64 MiB");
    // Bytes after the levels are allowed, up to the file limit.
    auto largest = valid;
    largest.resize(maximum_file_bytes);
    CHECK(load_ktx2(largest, TextureEncoding::srgb)->blocks.size() ==
          load_ktx2(valid, TextureEncoding::srgb)->blocks.size());
    largest.push_back({});
    rejects(largest, "KTX2 must be between 1 byte and 64 MiB");
    rejects(std::vector<std::byte>(valid.begin(), valid.begin() + 79), "Not a KTX 2.0 file");
    rejects(with([](auto &b) { b[5] = std::byte{'1'}; }), "Not a KTX 2.0 file");
    rejects(with([](auto &b) { put32(b, format_field, vk_format_undefined); }),
            "KTX2 format must be VK_FORMAT_BC7_SRGB_BLOCK or VK_FORMAT_BC7_UNORM_BLOCK");
    rejects(with([](auto &b) { put32(b, format_field, r8g8b8a8_unorm); }),
            "KTX2 format must be VK_FORMAT_BC7_SRGB_BLOCK or VK_FORMAT_BC7_UNORM_BLOCK");
    rejects(valid, "KTX2 format VK_FORMAT_BC7_SRGB_BLOCK does not match a linear texture", TextureEncoding::linear);
    rejects(read_file(fixtures / "pattern-linear-bc7.ktx2"),
            "KTX2 format VK_FORMAT_BC7_UNORM_BLOCK does not match an sRGB texture");
    rejects(with([](auto &b) { put32(b, type_size_field, r32_type_size); }),
            "KTX2 type size must be 1 for a block-compressed format");
    for (const auto &[field, value] :
         {std::pair{height_field, 0U}, std::pair{width_field, 0U}, std::pair{depth_field, 1U},
          std::pair{layers_field, 2U}, std::pair{faces_field, cube_faces}}) {
        CAPTURE(field);
        rejects(with([&](auto &b) { put32(b, field, value); }),
                "KTX2 must hold one 2D image, not an array, cube map or volume");
    }
    rejects(with([](auto &b) { put32(b, width_field, maximum_edge + 1); }),
            "KTX2 image exceeds import dimensions or texel limit");
    rejects(with([](auto &b) { put32(b, height_field, maximum_edge + 1); }),
            "KTX2 image exceeds import dimensions or texel limit");
    rejects(with([](auto &b) {
                put32(b, width_field, maximum_edge);
                put32(b, height_field, maximum_texels / maximum_edge + 1);
            }),
            "KTX2 image exceeds import dimensions or texel limit");
    rejects(with([](auto &b) { put32(b, levels_field, 0); }),
            "KTX2 level count 0 asks for generated mip levels, which BC7 images cannot have");
    const auto levels = get32(valid, levels_field);
    rejects(with([&](auto &b) { put32(b, levels_field, levels + 1); }),
            "KTX2 level count exceeds the image's mip chain");
    for (const auto scheme : {basislz, zstandard, zlib}) {
        CAPTURE(scheme);
        rejects(with([&](auto &b) { put32(b, supercompression_field, scheme); }),
                "KTX2 supercompression is unsupported");
    }
    rejects(with([](auto &b) { put64(b, sgd_length_field, 1); }),
            "KTX2 has supercompression global data without supercompression");
    rejects(std::vector<std::byte>(valid.begin(), valid.begin() + first_level + level_entry * (levels - 1)),
            "KTX2 level index exceeds the file");
    const auto dfd = get32(valid, dfd_field);
    rejects(with([](auto &b) { put32(b, dfd_length_field, 0); }), "Invalid KTX2 data format descriptor");
    // A dfdTotalSize that leaves out its own field.
    rejects(with([&](auto &b) { put32(b, dfd, basic_block_bytes); }), "Invalid KTX2 data format descriptor");
    rejects(with([&](auto &b) { put32(b, dfd_field, std::uint32_t(b.size())); }),
            "Invalid KTX2 data format descriptor");
    const auto version = [](std::uint32_t number, std::uint32_t size) { return number | (size << block_size_shift); };
    rejects(with([&](auto &b) { put32(b, dfd + dfd_version, version(khr_df_versionnumber_1_2, basic_block_bytes)); }),
            "KTX2 data format descriptor must be one basic block");
    rejects(with([&](auto &b) { put32(b, dfd + dfd_type, other_vendor); }),
            "KTX2 data format descriptor must be one basic block");
    // A basic block that claims a second sample the descriptor does not hold.
    rejects(with([&](auto &b) {
                put32(b, dfd + dfd_version, version(khr_df_versionnumber_1_3, basic_block_bytes + bc7_block_bytes));
            }),
            "KTX2 data format descriptor must be one basic block");
    // A descriptor of its dfdTotalSize and one more word, too short for a basic block: at the end of the file,
    // reading the block would pass the end.
    rejects(with([](auto &b) {
                constexpr std::uint32_t short_descriptor = 2 * sizeof(std::uint32_t);
                const auto end = std::uint32_t(b.size());
                b.resize(b.size() + short_descriptor);
                put32(b, end, short_descriptor);
                put32(b, dfd_field, end);
                put32(b, dfd_length_field, short_descriptor);
            }),
            "Invalid KTX2 data format descriptor");
    rejects(with([&](auto &b) { b[dfd + dfd_model] = std::byte{khr_df_model_bc1a}; }),
            "KTX2 data format descriptor does not describe BC7");
    rejects(with([&](auto &b) { b[dfd + dfd_dimension] = std::byte{eight_texel_dimension}; }),
            "KTX2 data format descriptor does not describe BC7");
    rejects(with([&](auto &b) { b[dfd + dfd_bytes_plane0] = std::byte{bc1_bytes_plane0}; }),
            "KTX2 data format descriptor does not describe BC7");
    rejects(with([&](auto &b) { b[dfd + dfd_transfer] = std::byte{khr_df_transfer_linear}; }),
            "KTX2 data format descriptor's transfer function does not match its format");
    // Key/value data from the level index past the end of the file.
    rejects(with([](auto &b) {
                put32(b, kvd_field, first_level);
                put32(b, kvd_length_field, std::uint32_t(b.size()));
            }),
            "KTX2 key/value data exceed the file");
    const auto level_offset = [&](std::size_t level) { return first_level + level_entry * level; };
    rejects(with([&](auto &b) { put64(b, level_offset(0), get32(b, level_offset(0)) + bc7_block_bytes / 2); }),
            "KTX2 mip level 0 is not aligned to 16 bytes");
    rejects(with([&](auto &b) { put64(b, level_offset(3), first_level); }), "KTX2 mip level 3 overlaps the header");
    rejects(with([&](auto &b) { put64(b, level_offset(1) + level_length, bc7_block_bytes); }),
            "KTX2 mip level 1 does not hold exactly its BC7 blocks");
    rejects(with([&](auto &b) {
                put64(b, level_offset(6) + level_uncompressed_length,
                      get32(b, level_offset(6) + level_length) + bc7_block_bytes);
            }),
            "KTX2 mip level 6 has an uncompressed length other than its length");
    rejects(with([&](auto &b) { put64(b, level_offset(0), b.size()); }), "KTX2 mip level 0 exceeds the file");
    CHECK_THROWS_WITH_AS((void)load_ktx2(fixtures / "absent.ktx2", TextureEncoding::srgb), "Cannot open KTX2 file",
                         std::runtime_error);
}
