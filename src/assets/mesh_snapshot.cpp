#include "../detail/image_limits.hpp"
#include "../detail/staging.hpp"
#include "bind_pose.hpp"
#include "mesh_limits.hpp"
#include "rotation_matrix.hpp"
#include "winding.hpp"
#include <anima/assets/mesh_snapshot.hpp>
#include <anima/assets/scene_validation.hpp>
#include <cgltf.h>
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace anima {
namespace {
static_assert(max_image_edge == detail::maximum_image_edge && max_image_texels == detail::maximum_image_texels,
              "asset.hpp must publish the image limits that the decoder and the UI share");
// Bound decoded allocation and hierarchy traversal before accepting a GLB.
constexpr std::size_t maximum_source_bytes = 64 * 1024 * 1024;
// Decoded bytes of every image that a texture uses, each counted once: 16 images at the pixel limit.
constexpr std::size_t maximum_decoded_bytes = std::size_t{1} << 30;
constexpr std::size_t maximum_nodes = 4096, maximum_materials = 4096;
constexpr std::size_t maximum_textures = 4096, maximum_images = 4096;
constexpr std::size_t maximum_accessor_elements = 2'000'000, maximum_expanded_vertices = 2'000'000;
// Keys over every channel of every clip: 192 MB at 24 bytes a key, as much as expanded vertices take at 96 bytes.
constexpr std::size_t maximum_animation_keys = 8'000'000;
constexpr unsigned maximum_hierarchy_depth = 256;
// A skinned vertex whose joint weights sum to this or less is unweighted.
constexpr float minimum_skin_weight_sum = 1e-6F;

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
// Both snapshot functions index the pose by node, so they check its size before reading it.
void require_pose_for(const Asset &asset, const Pose &pose) {
    require(pose.world.size() == asset.nodes.size(), "Pose does not match asset nodes");
}
// glTF requires a node matrix to be TRS without shear. Its decomposition becomes the node's rest transform, so
// consumers of local TRS, such as motion transfer, see the node's actual placement. A mirrored matrix gets a
// negative X scale, and a matrix with a zero-scale axis keeps the identity rotation.
Transform decompose(const Mat4 &m) {
    Transform result;
    result.translation = translation_of(m);
    const std::array axes{axis_x(m), axis_y(m), axis_z(m)};
    result.scale = {length(axes[0]), length(axes[1]), length(axes[2])};
    if (dot(cross(axes[0], axes[1]), axes[2]) < 0)
        result.scale.x = -result.scale.x;
    const std::array scales{result.scale.x, result.scale.y, result.scale.z};
    if (std::ranges::any_of(scales, [](float s) { return s == 0; }))
        return result;
    detail::Matrix3 rotation{};
    for (unsigned c = 0; c < 3; ++c) {
        const auto column = axes[c] * (1 / scales[c]);
        rotation[0][c] = column.x;
        rotation[1][c] = column.y;
        rotation[2][c] = column.z;
    }
    if (const auto q = detail::rotation_quaternion(rotation))
        result.rotation = *q;
    return result;
}
std::string name(const char *value) {
    if (!value)
        return "(unnamed)";
    std::string result{value};
    result.resize(cgltf_decode_string(result.data()));
    return result;
}
void check(cgltf_result result, const char *operation) {
    if (result != cgltf_result_success)
        throw std::runtime_error(std::string(operation) + " failed (cgltf " + std::to_string(result) + ")");
}
std::array<float, 16> read(const cgltf_accessor *accessor, std::size_t index, cgltf_type type) {
    require(accessor && accessor->type == type && index < accessor->count, "Missing/mismatched glTF accessor");
    std::array<float, 16> result{};
    require(cgltf_accessor_read_float(accessor, index, result.data(), result.size()), "Cannot read glTF accessor");
    for (const auto value : result)
        require(std::isfinite(value), "Non-finite glTF attribute");
    return result;
}
const cgltf_accessor *attribute(const cgltf_primitive &primitive, cgltf_attribute_type type, int set = 0) {
    for (std::size_t i = 0; i < primitive.attributes_count; ++i) {
        const auto &value = primitive.attributes[i];
        if (value.type == type && value.index == set)
            return value.data;
    }
    return nullptr;
}
// An embedded image's encoded bytes and the dimensions that its header declares.
struct EncodedImage {
    const std::uint8_t *bytes{};
    int size{};
    int width{};
    int height{};
    // Bytes of the image decoded to RGBA.
    std::size_t decoded_bytes() const {
        return std::size_t(width) * std::size_t(height) * detail::rgba_bytes_per_pixel;
    }
};
// Reads @p image's header, without decoding it, and checks the limits of one image.
EncodedImage measure(const cgltf_image *image) {
    require(image && image->buffer_view && !image->uri, "Only embedded PNG/JPEG images are supported");
    const auto *view = image->buffer_view;
    require(view->size <= maximum_source_bytes, "Embedded image exceeds import byte limit");
    EncodedImage result;
    result.bytes = cgltf_buffer_view_data(view);
    require(result.bytes, "Missing embedded image data");
    result.size = static_cast<int>(view->size);
    int channels = 0;
    require(stbi_info_from_memory(result.bytes, result.size, &result.width, &result.height, &channels),
            "Invalid embedded image");
    require(result.width > 0 && result.height > 0 && std::cmp_less_equal(result.width, max_image_edge) &&
                std::cmp_less_equal(result.height, max_image_edge) &&
                std::size_t(result.width) * result.height <= max_image_texels,
            "Decoded image exceeds import dimensions or pixel limit");
    return result;
}
std::shared_ptr<const Image> decode(const EncodedImage &image) {
    int width = 0, height = 0, channels = 0;
    std::unique_ptr<unsigned char, decltype(&stbi_image_free)> pixels{
        stbi_load_from_memory(image.bytes, image.size, &width, &height, &channels, STBI_rgb_alpha), stbi_image_free};
    // The import's byte limit counted the dimensions from the header, so the pixels must have them too.
    require(pixels && width == image.width && height == image.height, "PNG/JPEG decode failed");
    return std::make_shared<Image>(Image{static_cast<std::uint32_t>(width),
                                         static_cast<std::uint32_t>(height),
                                         {pixels.get(), pixels.get() + image.decoded_bytes()}});
}
Sampler sampler(const cgltf_sampler *source) {
    Sampler result;
    if (!source)
        return result;
    const auto wrap = [](cgltf_wrap_mode mode) {
        if (mode == cgltf_wrap_mode_repeat)
            return Wrap::repeat;
        if (mode == cgltf_wrap_mode_clamp_to_edge)
            return Wrap::clamp;
        if (mode == cgltf_wrap_mode_mirrored_repeat)
            return Wrap::mirror;
        throw std::runtime_error("Invalid texture wrap mode");
    };
    result.u = wrap(source->wrap_s);
    result.v = wrap(source->wrap_t);
    require(source->mag_filter == cgltf_filter_type_undefined || source->mag_filter == cgltf_filter_type_linear ||
                source->mag_filter == cgltf_filter_type_nearest,
            "Invalid magnification filter");
    result.mag = source->mag_filter == cgltf_filter_type_nearest ? Filter::nearest : Filter::linear;
    switch (source->min_filter) {
    case cgltf_filter_type_nearest:
        result.min = Filter::nearest;
        result.mipmapped = false;
        break;
    case cgltf_filter_type_linear:
        result.min = Filter::linear;
        result.mipmapped = false;
        break;
    case cgltf_filter_type_nearest_mipmap_nearest:
        result.min = result.mip = Filter::nearest;
        break;
    case cgltf_filter_type_linear_mipmap_nearest:
        result.min = Filter::linear;
        result.mip = Filter::nearest;
        break;
    case cgltf_filter_type_nearest_mipmap_linear:
        result.min = Filter::nearest;
        result.mip = Filter::linear;
        break;
    case cgltf_filter_type_linear_mipmap_linear:
    case cgltf_filter_type_undefined:
        break;
    default:
        throw std::runtime_error("Invalid minification filter");
    }
    return result;
}
} // namespace

static std::vector<std::byte> read_glb(const std::filesystem::path &path, const detail::StagingSteps &steps) {
    steps.add(1);
    steps.check();
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    require(bool(file), "Cannot open GLB file");
    const auto length = file.tellg();
    require(length > 0 && length <= static_cast<std::streamoff>(maximum_source_bytes),
            "GLB must be between 1 byte and 64 MiB");
    std::vector<std::byte> bytes(static_cast<std::size_t>(length));
    file.seekg(0);
    file.read(reinterpret_cast<char *>(bytes.data()), length);
    require(bool(file), "Cannot read GLB file");
    steps.complete();
    return bytes;
}
static std::shared_ptr<const Asset> read_asset(std::span<const std::byte> bytes, bool motion_only,
                                               const detail::StagingSteps &steps) {
    steps.add(1);
    steps.check();
    require(!bytes.empty() && bytes.size() <= maximum_source_bytes, "GLB must be between 1 byte and 64 MiB");
    cgltf_options options{};
    options.type = cgltf_file_type_glb;
    cgltf_data *parsed = nullptr;
    check(cgltf_parse(&options, bytes.data(), bytes.size(), &parsed), "Parse GLB");
    std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data{parsed, cgltf_free};
    for (std::size_t i = 0; i < data->extensions_required_count; ++i)
        require(std::string_view(data->extensions_required[i]) == "KHR_materials_unlit",
                "Required glTF extension is unsupported");
    require(data->nodes_count <= maximum_nodes && data->materials_count <= maximum_materials,
            "GLB exceeds import node/material limit");
    require(data->textures_count <= maximum_textures, "GLB exceeds import texture limit");
    require(data->images_count <= maximum_images, "GLB exceeds import image limit");
    require(data->buffers_count == 1 && !data->buffers[0].uri, "Only a single embedded GLB buffer is supported");
    for (std::size_t i = 0; i < data->buffer_views_count; ++i) {
        const auto &view = data->buffer_views[i];
        require(!view.has_meshopt_compression, "Meshopt compression is unsupported");
        require(view.buffer && view.offset <= view.buffer->size && view.size <= view.buffer->size - view.offset,
                "Buffer view exceeds embedded buffer bounds");
    }
    for (std::size_t i = 0; i < data->accessors_count; ++i) {
        const auto &accessor = data->accessors[i];
        require(!accessor.is_sparse, "Sparse accessors are unsupported");
        require(accessor.count <= maximum_accessor_elements, "Accessor exceeds import element limit");
        if (accessor.buffer_view) {
            const auto size = accessor.buffer_view->size;
            const auto element = cgltf_calc_size(accessor.type, accessor.component_type);
            require(element && accessor.stride >= element && accessor.offset <= size &&
                        element <= size - accessor.offset && accessor.count > 0 &&
                        accessor.count - 1 <= (size - accessor.offset - element) / accessor.stride,
                    "Accessor exceeds buffer view bounds");
        }
    }
    // Each channel copies its keys, and channels can share accessors, so bound the keys of every channel together
    // before reading any. glTF 2.0 ignores a channel without a target node, so it reads none.
    std::size_t animation_keys = 0;
    for (std::size_t a = 0; a < data->animations_count; ++a)
        for (std::size_t c = 0; c < data->animations[a].channels_count; ++c) {
            const auto &channel = data->animations[a].channels[c];
            if (!channel.target_node)
                continue;
            const auto keys = channel.sampler->input->count;
            require(keys <= maximum_animation_keys - animation_keys, "Animation keys exceed import limit");
            animation_keys += keys;
        }
    check(cgltf_load_buffers(&options, data.get(), nullptr), "Load embedded GLB buffer");
    check(cgltf_validate(data.get()), "Validate GLB structure/accessor bounds");
    // Bound hierarchy work before cgltf's parent-chain traversal.
    for (std::size_t i = 0; i < data->nodes_count; ++i) {
        unsigned depth = 0;
        for (auto *node = &data->nodes[i]; node; node = node->parent)
            require(++depth <= maximum_hierarchy_depth, "Node hierarchy exceeds depth limit or contains a cycle");
    }
    steps.complete();
    auto asset = std::make_shared<Asset>();
    asset->nodes.reserve(data->nodes_count);
    for (std::size_t i = 0; i < data->nodes_count; ++i) {
        const auto &node = data->nodes[i];
        AssetNode value;
        value.name = name(node.name);
        value.parent = node.parent ? static_cast<int>(node.parent - data->nodes) : -1;
        value.rest.translation = {node.translation[0], node.translation[1], node.translation[2]};
        std::copy_n(node.rotation, 4, value.rest.rotation.begin());
        value.rest.rotation = unit_quaternion(value.rest.rotation);
        value.rest.scale = {node.scale[0], node.scale[1], node.scale[2]};
        value.has_matrix = node.has_matrix;
        cgltf_node_transform_local(&node, value.rest_matrix.data());
        if (value.has_matrix)
            value.rest = decompose(value.rest_matrix);
        asset->nodes.push_back(value);
    }
    for (std::size_t i = 0; i < data->skins_count; ++i) {
        const auto &skin = data->skins[i];
        AssetSkin value;
        require(skin.joints_count > 0 && skin.joints_count <= Mesh::max_skin_joints,
                "Invalid/oversized skin joint palette");
        for (std::size_t j = 0; j < skin.joints_count; ++j) {
            value.joints.push_back(static_cast<std::size_t>(skin.joints[j] - data->nodes));
            value.inverse_bind.push_back(
                skin.inverse_bind_matrices ? read(skin.inverse_bind_matrices, j, cgltf_type_mat4) : identity());
        }
        asset->skins.push_back(std::move(value));
    }
    // Each image that a texture uses, measured from its header and bounded in total before any is decoded, and
    // decoded once; every texture made from it shares the pixels.
    struct TextureImage {
        EncodedImage encoded;
        std::shared_ptr<const Image> decoded;
    };
    std::unordered_map<const cgltf_image *, TextureImage> images;
    std::size_t total_decoded_bytes = 0;
    for (std::size_t i = 0; i < data->textures_count; ++i) {
        const auto *image = data->textures[i].image;
        // A Basis or WebP source is optional unless its extension is required, which is rejected above;
        // the texture's standard PNG or JPEG source then serves as the fallback.
        require(image, "Texture needs a PNG or JPEG source");
        if (images.contains(image))
            continue;
        const auto encoded = measure(image);
        require(encoded.decoded_bytes() <= maximum_decoded_bytes - total_decoded_bytes,
                "Decoded images exceed import byte limit");
        total_decoded_bytes += encoded.decoded_bytes();
        images.emplace(image, TextureImage{encoded, nullptr});
    }
    // Geometry comes from the default scene, else the first scene, else every root node.
    const auto *selected = data->scene ? data->scene : (data->scenes_count ? &data->scenes[0] : nullptr);
    std::vector<const cgltf_node *> roots;
    if (selected)
        roots.assign(selected->nodes, selected->nodes + selected->nodes_count);
    else
        for (std::size_t i = 0; i < data->nodes_count; ++i)
            if (!data->nodes[i].parent)
                roots.push_back(&data->nodes[i]);
    // The traversal below imports every primitive of each node it reaches and rejects a node reached twice, so
    // this count, which visits each node once, is the number of primitives a successful import has.
    std::size_t primitive_count = 0;
    {
        std::vector<bool> counted(data->nodes_count);
        std::vector<const cgltf_node *> pending(roots.rbegin(), roots.rend());
        while (!pending.empty()) {
            const auto *node = pending.back();
            pending.pop_back();
            const auto node_index = static_cast<std::size_t>(node - data->nodes);
            if (node_index >= counted.size() || counted[node_index])
                continue;
            counted[node_index] = true;
            if (node->mesh)
                primitive_count += node->mesh->primitives_count;
            pending.insert(pending.end(), node->children, node->children + node->children_count);
        }
    }
    steps.add(images.size() + primitive_count);
    asset->textures.reserve(data->textures_count);
    for (std::size_t i = 0; i < data->textures_count; ++i) {
        const auto &texture = data->textures[i];
        auto &image = images.at(texture.image);
        if (!image.decoded) {
            steps.check();
            image.decoded = decode(image.encoded);
            steps.complete();
        }
        asset->textures.push_back({image.decoded, sampler(texture.sampler)});
    }
    // A source texture used as both colour and data needs distinct GPU encodings; the copy shares its image.
    std::map<std::pair<std::size_t, TextureEncoding>, int> texture_views;
    std::vector<bool> texture_used(data->textures_count);
    const auto texture_index = [&](const cgltf_texture_view &view, TextureEncoding encoding) {
        if (!view.texture)
            return -1;
        require(!view.has_transform, "Texture transforms are unsupported");
        const auto index = static_cast<std::size_t>(view.texture - data->textures);
        const auto key = std::pair{index, encoding};
        if (const auto found = texture_views.find(key); found != texture_views.end())
            return found->second;
        int result = static_cast<int>(index);
        if (texture_used.at(index)) {
            result = static_cast<int>(asset->textures.size());
            auto copy = asset->textures.at(index);
            asset->textures.push_back(std::move(copy));
        }
        texture_used[index] = true;
        asset->textures.at(result).encoding = encoding;
        texture_views.emplace(key, result);
        return result;
    };
    for (std::size_t i = 0; i < data->materials_count; ++i) {
        const auto &material = data->materials[i];
        Material value;
        value.name = name(material.name);
        value.metallic = 1;
        value.alpha_mode = material.alpha_mode == cgltf_alpha_mode_mask    ? AlphaMode::mask
                           : material.alpha_mode == cgltf_alpha_mode_blend ? AlphaMode::blend
                                                                           : AlphaMode::opaque;
        value.alpha_cutoff = material.alpha_cutoff;
        value.unlit = material.unlit;
        value.double_sided = material.double_sided;
        value.emissive = {material.emissive_factor[0], material.emissive_factor[1], material.emissive_factor[2]};
        value.normal_texture = texture_index(material.normal_texture, TextureEncoding::linear);
        value.normal_scale = material.normal_texture.scale;
        value.emissive_texture = texture_index(material.emissive_texture, TextureEncoding::srgb);
        value.occlusion_texture = texture_index(material.occlusion_texture, TextureEncoding::linear);
        value.occlusion_strength = material.occlusion_texture.scale;
        if (material.has_pbr_metallic_roughness) {
            const auto &pbr = material.pbr_metallic_roughness;
            const auto &texture = pbr.base_color_texture;
            require(!texture.has_transform, "Base-color texture transforms are unsupported");
            value.factor = {pbr.base_color_factor[0], pbr.base_color_factor[1], pbr.base_color_factor[2]};
            value.metallic = pbr.metallic_factor;
            value.roughness = pbr.roughness_factor;
            value.alpha = pbr.base_color_factor[3];
            value.texture = texture_index(texture, TextureEncoding::srgb);
            value.metallic_roughness_texture = texture_index(pbr.metallic_roughness_texture, TextureEncoding::linear);
        }
        validate_material(value, asset->textures);
        asset->materials.push_back(std::move(value));
    }
    std::vector<bool> visited(data->nodes_count);
    std::size_t total_vertices = 0;
    const auto visit = [&](const auto &self, const cgltf_node *node) -> void {
        const auto node_index = static_cast<std::size_t>(node - data->nodes);
        require(node_index < visited.size() && !visited[node_index], "Node visited twice in selected scene");
        visited[node_index] = true;
        require(!node->has_mesh_gpu_instancing, "EXT_mesh_gpu_instancing is unsupported");
        if (node->mesh) {
            ++asset->mesh_nodes;
            for (std::size_t p = 0; p < node->mesh->primitives_count; ++p) {
                steps.check();
                const auto &primitive = node->mesh->primitives[p];
                require(primitive.type == cgltf_primitive_type_triangles,
                        "Only triangle-list primitives are supported");
                require(!primitive.targets_count && !primitive.has_draco_mesh_compression,
                        "Morph targets/Draco are unsupported");
                const auto *positions = attribute(primitive, cgltf_attribute_type_position);
                const auto *normals = attribute(primitive, cgltf_attribute_type_normal);
                const auto *tangents = attribute(primitive, cgltf_attribute_type_tangent);
                const auto *colors = attribute(primitive, cgltf_attribute_type_color);
                const auto *joints = attribute(primitive, cgltf_attribute_type_joints);
                const auto *weights = attribute(primitive, cgltf_attribute_type_weights);
                require(positions && positions->type == cgltf_type_vec3, "Primitive needs VEC3 POSITION");
                for (std::size_t a = 0; a < primitive.attributes_count; ++a) {
                    const auto &item = primitive.attributes[a];
                    require(item.index == 0 ||
                                (item.type != cgltf_attribute_type_joints && item.type != cgltf_attribute_type_weights),
                            "More than four joint influences are unsupported");
                }
                if (node->skin)
                    require(joints && weights, "Skinned primitive requires JOINTS_0 and WEIGHTS_0");
                const cgltf_accessor *uv = nullptr;
                if (primitive.material) {
                    const auto &m = *primitive.material;
                    int uv_set = -1;
                    for (const auto *view : {&m.pbr_metallic_roughness.base_color_texture,
                                             &m.pbr_metallic_roughness.metallic_roughness_texture, &m.normal_texture,
                                             &m.emissive_texture, &m.occlusion_texture}) {
                        if (!view->texture)
                            continue;
                        require(uv_set < 0 || uv_set == view->texcoord, "Material maps must share one UV set");
                        uv_set = view->texcoord;
                    }
                    if (uv_set >= 0) {
                        uv = attribute(primitive, cgltf_attribute_type_texcoord, uv_set);
                        require(uv, "Material requires its referenced UV set");
                    }
                }
                SourcePrimitive value;
                value.node = node_index;
                value.mesh_name = name(node->mesh->name);
                value.skin = node->skin ? static_cast<int>(node->skin - data->skins) : -1;
                // The implicit glTF material is also metallic=1, roughness=1 and single-sided.
                if (!primitive.material && asset->materials.size() == data->materials_count) {
                    Material fallback;
                    fallback.name = "glTF default";
                    fallback.metallic = 1;
                    fallback.double_sided = false;
                    asset->materials.push_back(std::move(fallback));
                }
                value.material = primitive.material ? static_cast<int>(primitive.material - data->materials)
                                                    : static_cast<int>(data->materials_count);
                if (primitive.indices)
                    require(primitive.indices->type == cgltf_type_scalar &&
                                (primitive.indices->component_type == cgltf_component_type_r_8u ||
                                 primitive.indices->component_type == cgltf_component_type_r_16u ||
                                 primitive.indices->component_type == cgltf_component_type_r_32u),
                            "Invalid index accessor type");
                const auto count = primitive.indices ? primitive.indices->count : positions->count;
                require(count && count % 3 == 0 && count <= maximum_expanded_vertices - total_vertices,
                        "Invalid/excessive triangle count");
                total_vertices += count;
                value.vertices.reserve(count);
                for (std::size_t index = 0; index < count; ++index) {
                    const auto v = primitive.indices ? cgltf_accessor_read_index(primitive.indices, index) : index;
                    const auto position = read(positions, v, cgltf_type_vec3);
                    SourceVertex vertex;
                    vertex.position = {position[0], position[1], position[2]};
                    if (normals) {
                        const auto n = read(normals, v, cgltf_type_vec3);
                        vertex.normal = {n[0], n[1], n[2]};
                    }
                    // glTF 2.0: without supplied normals, flat normals are generated and tangents are ignored.
                    if (tangents && normals) {
                        const auto t = read(tangents, v, cgltf_type_vec4);
                        vertex.tangent = {t[0], t[1], t[2], t[3]};
                        require(vertex.tangent[3] == 1 || vertex.tangent[3] == -1, "Invalid tangent handedness");
                    }
                    if (colors) {
                        require(colors->type == cgltf_type_vec3 || colors->type == cgltf_type_vec4,
                                "Invalid COLOR_0 accessor");
                        const auto c = read(colors, v, colors->type);
                        vertex.color = {c[0], c[1], c[2]};
                        if (colors->type == cgltf_type_vec4)
                            vertex.alpha = c[3];
                    }
                    if (uv) {
                        const auto coords = read(uv, v, cgltf_type_vec2);
                        vertex.uv = {coords[0], coords[1]};
                    }
                    if (node->skin) {
                        require(joints->type == cgltf_type_vec4 && v < joints->count &&
                                    (joints->component_type == cgltf_component_type_r_8u ||
                                     joints->component_type == cgltf_component_type_r_16u),
                                "Invalid joint accessor");
                        require(cgltf_accessor_read_uint(joints, v, vertex.joints.data(), 4),
                                "Cannot read joint indices");
                        const auto w = read(weights, v, cgltf_type_vec4);
                        float sum = 0;
                        for (unsigned k = 0; k < 4; ++k) {
                            require(w[k] >= 0 && vertex.joints[k] < node->skin->joints_count,
                                    "Invalid skin weight/joint index");
                            sum += w[k];
                        }
                        require(sum > minimum_skin_weight_sum, "Skinned vertex has no weight");
                        for (unsigned k = 0; k < 4; ++k)
                            vertex.weights[k] = w[k] / sum;
                    }
                    value.vertices.push_back(vertex);
                }
                if (!normals)
                    for (std::size_t i = 0; i < count; i += 3) {
                        const auto n = normalized(cross(value.vertices[i + 1].position - value.vertices[i].position,
                                                        value.vertices[i + 2].position - value.vertices[i].position));
                        for (unsigned k = 0; k < 3; ++k)
                            value.vertices[i + k].normal = n;
                    }
                asset->primitives.push_back(std::move(value));
                steps.complete();
            }
        }
        for (std::size_t i = 0; i < node->children_count; ++i)
            self(self, node->children[i]);
    };
    for (const auto *root : roots)
        visit(visit, root);
    if (!motion_only)
        require(!asset->primitives.empty(), "Selected scene contains no renderable triangles");
    for (std::size_t a = 0; a < data->animations_count; ++a) {
        const auto &animation = data->animations[a];
        Animation clip;
        clip.name = name(animation.name);
        std::vector<std::pair<std::size_t, ChannelPath>> targeted;
        for (std::size_t c = 0; c < animation.channels_count; ++c) {
            const auto &channel = animation.channels[c];
            AnimationChannel value;
            // glTF 2.0: a channel without a target node is ignored.
            if (!channel.target_node)
                continue;
            require(!channel.target_node->has_matrix, "Animation cannot target a matrix node");
            value.node = static_cast<std::size_t>(channel.target_node - data->nodes);
            switch (channel.target_path) {
            case cgltf_animation_path_type_translation:
                value.path = ChannelPath::translation;
                break;
            case cgltf_animation_path_type_rotation:
                value.path = ChannelPath::rotation;
                break;
            case cgltf_animation_path_type_scale:
                value.path = ChannelPath::scale;
                break;
            default:
                throw std::runtime_error("Unsupported animation target (morph weights are not implemented)");
            }
            require(std::find(targeted.begin(), targeted.end(), std::pair{value.node, value.path}) == targeted.end(),
                    "Duplicate animation target channel");
            targeted.emplace_back(value.node, value.path);
            const auto *s = channel.sampler;
            require(s->interpolation == cgltf_interpolation_type_linear ||
                        s->interpolation == cgltf_interpolation_type_step,
                    "Unsupported animation interpolation: only LINEAR and STEP are implemented (CUBICSPLINE rejected)");
            value.interpolation =
                s->interpolation == cgltf_interpolation_type_step ? Interpolation::step : Interpolation::linear;
            require(s->input->count == s->output->count && s->input->count > 0, "Mismatched animation samples");
            value.times.reserve(s->input->count);
            value.values.reserve(s->input->count);
            for (std::size_t k = 0; k < s->input->count; ++k) {
                const auto time = double(read(s->input, k, cgltf_type_scalar)[0]);
                require(time >= 0 && (value.times.empty() || time > value.times.back()),
                        "Animation times must increase strictly");
                value.times.push_back(time);
                const auto sample =
                    read(s->output, k, value.path == ChannelPath::rotation ? cgltf_type_vec4 : cgltf_type_vec3);
                std::array<float, 4> output{sample[0], sample[1], sample[2], sample[3]};
                if (value.path == ChannelPath::rotation)
                    output = unit_quaternion(output);
                value.values.push_back(output);
            }
            clip.duration = std::max(clip.duration, value.times.back());
            clip.channels.push_back(std::move(value));
        }
        asset->animations.push_back(std::move(clip));
    }
    asset->notices.emplace_back(
        "Metallic/roughness materials with normal, occlusion and emissive maps; opaque, alpha-mask, "
        "alpha-blend and KHR_materials_unlit supported. One shared UV set per material.");
    if (data->extensions_used_count)
        asset->notices.emplace_back("Optional extensions other than KHR_materials_unlit are ignored; unsupported "
                                    "required extensions are rejected.");
    (void)sample_pose(*asset); // Validate engine-owned hierarchy before releasing cgltf data.
    if (motion_only)
        require(asset->primitives.empty() && asset->skins.empty() && asset->materials.empty() &&
                    asset->textures.empty() && !asset->animations.empty(),
                "Motion resources require animation without geometry, skins or materials");
    steps.check();
    return asset;
}

std::shared_ptr<const Asset> load_asset(std::span<const std::byte> bytes, const StagingOptions &options) {
    return read_asset(bytes, false, detail::StagingSteps(options));
}
std::shared_ptr<const Asset> load_motion_asset(std::span<const std::byte> bytes, const StagingOptions &options) {
    return read_asset(bytes, true, detail::StagingSteps(options));
}
std::shared_ptr<const Asset> load_asset(const std::filesystem::path &path, const StagingOptions &options) {
    const detail::StagingSteps steps(options);
    return read_asset(read_glb(path, steps), false, steps);
}
std::shared_ptr<const Asset> load_motion_asset(const std::filesystem::path &path, const StagingOptions &options) {
    const detail::StagingSteps steps(options);
    return read_asset(read_glb(path, steps), true, steps);
}

void pose_mesh_snapshot(const Asset &asset, const Pose &pose, MeshSnapshot &snapshot, std::size_t offset,
                        const Mat4 &attachment) {
    require_pose_for(asset, pose);
    std::vector<std::vector<Mat4>> palettes;
    for (const auto &skin : asset.skins) {
        std::vector<Mat4> palette;
        for (std::size_t j = 0; j < skin.joints.size(); ++j)
            palette.push_back(attachment * pose.world.at(skin.joints[j]) * skin.inverse_bind.at(j));
        palettes.push_back(std::move(palette));
    }
    for (const auto &primitive : asset.primitives) {
        const auto factor = primitive.material >= 0 ? asset.materials.at(primitive.material).factor : Vec3{1, 1, 1};
        const auto node_world = attachment * pose.world.at(primitive.node);
        for (auto &draw : snapshot.primitives)
            if (draw.first_vertex == offset)
                draw.node_world = node_world;
        bool reversed = false;
        for (std::size_t corner = 0; corner < primitive.vertices.size(); ++corner) {
            const auto &source = primitive.vertices[corner];
            Mat4 transform = node_world;
            if (primitive.skin >= 0) {
                transform = {};
                const auto &palette = palettes.at(primitive.skin);
                for (unsigned k = 0; k < 4; ++k)
                    if (source.weights[k] != 0) {
                        const auto &joint = palette.at(source.joints[k]);
                        for (unsigned m = 0; m < 16; ++m)
                            transform[m] += joint[m] * source.weights[k];
                    }
            }
            // The first corner's matrix decides the triangle's winding, as it does on the GPU.
            if (corner % 3 == 0)
                reversed = detail::reverses_winding(transform);
            auto &vertex = snapshot.vertices.at(offset++);
            vertex.position = point(transform, source.position);
            vertex.normal = normal(transform, source.normal);
            vertex.color = {source.color.x * factor.x, source.color.y * factor.y, source.color.z * factor.z};
            vertex.uv = source.uv;
            vertex.tangent = tangent(transform, source.tangent);
            vertex.alpha = source.alpha;
            for (float v : {vertex.position.x, vertex.position.y, vertex.position.z, vertex.color.x, vertex.color.y,
                            vertex.color.z})
                require(std::isfinite(v), "Non-finite posed vertex/material");
            // A reversed triangle swaps its last two corners, keeping its source winding against its normals.
            if (reversed && corner % 3 == 2)
                std::swap(snapshot.vertices.at(offset - 2), snapshot.vertices.at(offset - 1));
        }
    }
}
MeshSnapshot make_mesh_snapshot(const Asset &asset, const Pose &pose) {
    require_pose_for(asset, pose);
    MeshSnapshot snapshot;
    snapshot.mesh_nodes = asset.mesh_nodes;
    snapshot.skins = asset.skins.size();
    snapshot.materials = asset.materials;
    snapshot.textures = asset.textures;
    snapshot.notices = asset.notices;
    for (const auto &clip : asset.animations)
        snapshot.clips.push_back(clip.name);
    const auto rest = sample_pose(asset);
    for (const auto &skin : asset.skins)
        snapshot.joints += skin.joints.size();
    snapshot.bind_deviation = detail::bind_deviation(rest, asset.skins);
    snapshot.default_is_bind_pose = snapshot.bind_deviation < mesh_limits::bind_pose_tolerance;
    for (const auto &p : asset.primitives) {
        snapshot.primitives.push_back({asset.nodes.at(p.node).name, p.mesh_name,
                                       p.material >= 0 ? asset.materials.at(p.material).name : "default",
                                       pose.world.at(p.node), static_cast<std::uint32_t>(snapshot.vertices.size()),
                                       static_cast<std::uint32_t>(p.vertices.size()), p.material, true});
        snapshot.vertices.resize(snapshot.vertices.size() + p.vertices.size());
        if (p.skin >= 0)
            snapshot.skinned_vertices += p.vertices.size();
    }
    pose_mesh_snapshot(asset, pose, snapshot);
    for (const auto &v : snapshot.vertices)
        encapsulate(snapshot.bounds, v.position);
    return snapshot;
}
MeshSnapshot load_mesh_snapshot(const std::filesystem::path &path) {
    const auto asset = load_asset(path);
    return make_mesh_snapshot(*asset, sample_pose(*asset));
}
void print_mesh_report(const MeshSnapshot &snapshot, std::ostream &out) {
    out << "ASSET mesh_nodes=" << snapshot.mesh_nodes << " primitives=" << snapshot.primitives.size()
        << " materials=" << snapshot.materials.size() << " triangles=" << snapshot.vertices.size() / 3
        << " skins=" << snapshot.skins << " joints=" << snapshot.joints
        << " skinned_vertices=" << snapshot.skinned_vertices << " bind_deviation=" << snapshot.bind_deviation
        << " pose=" << (snapshot.default_is_bind_pose ? "bind" : "default") << '\n';
    const auto &bounds = snapshot.bounds;
    if (bounds.valid)
        out << "Bounds: [" << bounds.minimum.x << ',' << bounds.minimum.y << ',' << bounds.minimum.z << "] to ["
            << bounds.maximum.x << ',' << bounds.maximum.y << ',' << bounds.maximum.z << "]\n";
    else
        out << "Bounds: none\n";
    for (const auto &p : snapshot.primitives)
        out << "  " << p.node_name << " / " << p.material_name << ": " << p.vertex_count / 3 << " triangles\n";
    for (const auto &clip : snapshot.clips)
        out << "  Available clip: " << clip << '\n';
    for (const auto &notice : snapshot.notices)
        out << "NOTICE: " << notice << '\n';
}
} // namespace anima
