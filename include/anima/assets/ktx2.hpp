#pragma once
#include <anima/assets/asset.hpp>

/// @file
/// Block-compressed texture images from KTX 2.0 files.
///
/// Part of the `anima::assets` target; the reader is Anima's own and needs no other library. Engines such as Unity
/// and Unreal store textures on the device in block-compressed formats that their content pipelines encode for
/// each platform, so that loading copies blocks instead of decoding and filtering images. Anima reads BC7, the
/// format that desktop GPUs sample, from KTX 2.0 files, the Khronos container that names Vulkan formats. Which
/// textures to compress, the encoder and its quality are the application's choice.
///
/// glTF has no ratified extension that embeds BC7 images, and `KHR_texture_basisu` requires Basis Universal
/// payloads, which need a transcoder; load_asset() keeps decoding PNG and JPEG. To use a BC7 image for an imported
/// texture, replace that Texture's image in a copy of the Asset before compiling it, for example
/// `asset.textures[i].image = anima::load_ktx2(path, asset.textures[i].encoding);`.

namespace anima {
/// Reads a KTX 2.0 image of 1 byte to 64 MiB, as load_ktx2(std::span<const std::byte>, TextureEncoding) does.
/// Throws `std::runtime_error` also when the file cannot be read.
[[nodiscard]] std::shared_ptr<const Image> load_ktx2(const std::filesystem::path &path, TextureEncoding encoding);
/// Reads a KTX 2.0 snapshot of 1 byte to 64 MiB into an ImageFormat::bc7 Image for a texture of @p encoding; @p bytes
/// is not retained.
///
/// The file must hold one 2D image, not an array, cube map or volume, in `VK_FORMAT_BC7_SRGB_BLOCK` for
/// TextureEncoding::srgb or `VK_FORMAT_BC7_UNORM_BLOCK` for TextureEncoding::linear, with a type size of 1, without
/// supercompression, with at most 8192 texels per edge and 16,777,216 texels, and with 1 to the full chain of mip
/// levels stored (a level count of 0, which asks the reader to generate them, is rejected). Each level must start
/// after the level index and have exactly its blocks' bytes, 16-byte aligned and inside the file, with an
/// uncompressed length equal to its length. The data format descriptor must be one basic block describing BC7,
/// with the transfer function that the format names; key/value data must lie inside the file and are otherwise
/// ignored, and supercompression global data must be absent. The Image keeps every stored level.
///
/// Throws `std::invalid_argument` for an unknown @p encoding, and `std::runtime_error` for anything else that the
/// file breaks, including a format that does not match @p encoding, each with a message that names the rule.
[[nodiscard]] std::shared_ptr<const Image> load_ktx2(std::span<const std::byte> bytes, TextureEncoding encoding);
} // namespace anima
