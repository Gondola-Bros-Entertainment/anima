#include "../detail/rotation_matrix.hpp"
#include "mesh_limits.hpp"
#include "surface_validation.hpp"
#include "texel_hold.hpp"
#include "winding.hpp"
#include <algorithm>
#include <anima/assets/scene_validation.hpp>
#include <anima/scene.hpp>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <climits>
#include <cstdio>
#include <exception>
#include <functional>
#include <limits>
#include <meshoptimizer.h>
#include <set>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace anima {
namespace {
constexpr float affine_tolerance = 1e-5F;
constexpr std::size_t maximum_object_key_digits = std::numeric_limits<std::uint64_t>::digits10 + 1;
void require(bool value, const char *message) {
    if (!value)
        throw std::invalid_argument(message);
}
// Throws std::out_of_range with @p message unless @p index is below @p size.
void require_index(std::size_t index, std::size_t size, const char *message) {
    if (index >= size)
        throw std::out_of_range(message);
}
constexpr auto material_factor_slot = "Material factor slot is outside the mesh's materials";
constexpr auto custom_material_slot = "Custom material slot is outside the mesh's materials";
constexpr auto primitive_slot = "Primitive is outside the mesh's primitives";
bool finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
void affine(const Mat4 &m) {
    for (auto v : m)
        require(std::isfinite(v), "Non-finite instance transform");
    require(std::abs(m[3]) < affine_tolerance && std::abs(m[7]) < affine_tolerance &&
                std::abs(m[11]) < affine_tolerance && std::abs(m[15] - 1) < affine_tolerance,
            "Instance transform must be affine");
}
void expand(RenderBounds &bounds, Vec3 v) {
    require(finite(v), "Non-finite render bounds");
    if (!bounds.valid) {
        bounds = {v, v, true};
        return;
    }
    bounds.minimum = {std::min(bounds.minimum.x, v.x), std::min(bounds.minimum.y, v.y),
                      std::min(bounds.minimum.z, v.z)};
    bounds.maximum = {std::max(bounds.maximum.x, v.x), std::max(bounds.maximum.y, v.y),
                      std::max(bounds.maximum.z, v.z)};
}
using Key = std::array<std::uint32_t, 24>;
Key key(const SourceVertex &v) {
    const auto b = [](float f) { return std::bit_cast<std::uint32_t>(f); };
    return {b(v.position.x), b(v.position.y), b(v.position.z), b(v.normal.x),   b(v.normal.y),   b(v.normal.z),
            b(v.color.x),    b(v.color.y),    b(v.color.z),    b(v.uv[0]),      b(v.uv[1]),      v.joints[0],
            v.joints[1],     v.joints[2],     v.joints[3],     b(v.weights[0]), b(v.weights[1]), b(v.weights[2]),
            b(v.weights[3]), b(v.tangent[0]), b(v.tangent[1]), b(v.tangent[2]), b(v.tangent[3]), b(v.alpha)};
}
struct Hash {
    std::size_t operator()(const Key &k) const noexcept {
        // FNV-1a mixing over each word in the bit-exact vertex key.
        constexpr std::size_t offset_basis = 14695981039346656037ULL, prime = 1099511628211ULL;
        std::size_t h = offset_basis;
        for (auto v : k) {
            h ^= v;
            h *= prime;
        }
        return h;
    }
};
} // namespace

std::shared_ptr<const Mesh> Mesh::compile(const Asset &source, TexelRetention texel_retention) {
    return compile(source, texel_retention, {});
}
std::shared_ptr<const Mesh> Mesh::compile(const Asset &source, TexelRetention texel_retention, MeshLodOptions lods) {
    require(lods.levels <= 8, "Mesh LOD levels must be from 0 to 8");
    if (texel_retention != TexelRetention::keep && texel_retention != TexelRetention::until_upload)
        throw std::invalid_argument("Unknown texel retention");
    auto result = std::shared_ptr<Mesh>(new Mesh);
    result->rest_ = sample_pose(source);
    for (const auto &node : source.nodes)
        result->nodes_.emplace_back(node.name, node.parent);
    for (const auto &m : result->rest_.world)
        affine(m);
    auto materials = std::make_shared<MeshSnapshot>();
    materials->material_data = source.materials;
    materials->textures = source.textures;
    materials->mesh_nodes = source.mesh_nodes;
    materials->skins = source.skins.size();
    materials->materials = source.materials.size();
    materials->notices = source.notices;
    for (const auto &clip : source.animations)
        materials->clips.push_back(clip.name);
    detail::validate_scene(*materials, {}, detail::Texels::required);
    result->skins_ = source.skins;
    std::size_t palette_size = source.nodes.size();
    std::vector<std::uint32_t> offsets;
    for (const auto &skin : source.skins) {
        require(!skin.joints.empty() && skin.joints.size() <= mesh_limits::maximum_skin_joints &&
                    skin.joints.size() == skin.inverse_bind.size(),
                "Invalid render skin palette");
        require(palette_size <= UINT32_MAX - skin.joints.size(), "Render palette index overflow");
        offsets.push_back(static_cast<std::uint32_t>(palette_size));
        palette_size += skin.joints.size();
        materials->joints += skin.joints.size();
        std::set<std::size_t> seen;
        for (std::size_t j = 0; j < skin.joints.size(); ++j) {
            require(skin.joints[j] < source.nodes.size() && seen.insert(skin.joints[j]).second,
                    "Invalid render skin joint");
            affine(skin.inverse_bind[j]);
            const auto bind = result->rest_.world[skin.joints[j]] * skin.inverse_bind[j], unit = identity();
            affine(bind);
            for (unsigned k = 0; k < 16; ++k)
                materials->bind_deviation = std::max(materials->bind_deviation, std::abs(bind[k] - unit[k]));
        }
    }
    materials->default_is_bind_pose = materials->bind_deviation < mesh_limits::bind_pose_tolerance;
    require(palette_size <= UINT32_MAX, "Render palette index overflow");
    result->palette_size_ = palette_size;
    // Where each draw's vertices start, then where the last one's end.
    std::vector<std::size_t> vertex_ranges;
    for (const auto &primitive : source.primitives) {
        require(primitive.node < source.nodes.size(), "Invalid render primitive node");
        require(primitive.skin >= no_index && (primitive.skin < 0 || std::size_t(primitive.skin) < source.skins.size()),
                "Invalid render primitive skin");
        require(primitive.material >= no_index &&
                    (primitive.material < 0 || std::size_t(primitive.material) < source.materials.size()),
                "Invalid render primitive material");
        require(!primitive.vertices.empty() && primitive.vertices.size() % 3 == 0 &&
                    primitive.vertices.size() <= UINT32_MAX - result->indices_.size(),
                "Invalid render triangle count");
        require(primitive.vertices.size() <=
                    std::numeric_limits<std::size_t>::max() / sizeof(SourceVertex) - result->vertices_.size(),
                "Render vertex byte size overflow");
        const auto offset = primitive.skin < 0 ? static_cast<std::uint32_t>(primitive.node) : offsets[primitive.skin];
        const auto joint_count = primitive.skin < 0 ? 1 : source.skins[primitive.skin].joints.size();
        vertex_ranges.push_back(result->vertices_.size());
        result->draws_.push_back(
            {static_cast<std::uint32_t>(result->indices_.size()), static_cast<std::uint32_t>(primitive.vertices.size()),
             offset, static_cast<std::uint32_t>(joint_count), primitive.skin >= 0, primitive.material,
             static_cast<std::uint32_t>(primitive.node), source.nodes[primitive.node].name, primitive.mesh_name});
        if (primitive.skin >= 0)
            materials->skinned_vertices += primitive.vertices.size();
        std::vector<RenderBounds> bounds(joint_count);
        std::unordered_map<Key, std::uint32_t, Hash> unique;
        unique.reserve(primitive.vertices.size() / 2);
        for (const auto &v : primitive.vertices) {
            require(finite(v.position) && finite(v.normal) && finite(v.color) && std::isfinite(v.uv[0]) &&
                        std::isfinite(v.uv[1]) && std::isfinite(v.alpha) && v.alpha >= 0 && v.alpha <= 1 &&
                        std::all_of(v.tangent.begin(), v.tangent.end(), [](float x) { return std::isfinite(x); }) &&
                        (v.tangent[3] == 0 || v.tangent[3] == 1 || v.tangent[3] == -1),
                    "Invalid render vertex");
            if (primitive.skin >= 0) {
                float sum = 0;
                for (unsigned j = 0; j < 4; ++j) {
                    require(std::isfinite(v.weights[j]) && v.weights[j] >= 0 && v.joints[j] < joint_count,
                            "Invalid render skin influence");
                    sum += v.weights[j];
                    if (v.weights[j] > 0)
                        expand(bounds[v.joints[j]], v.position);
                }
                require(std::abs(sum - 1) < mesh_limits::skin_weight_tolerance,
                        "Render skin weights must be normalized");
            } else
                expand(bounds[0], v.position);
            auto [found, inserted] = unique.emplace(key(v), static_cast<std::uint32_t>(result->vertices_.size()));
            if (inserted)
                result->vertices_.push_back(v);
            result->indices_.push_back(found->second);
        }
        auto &parts = result->bounds_.emplace_back();
        for (std::size_t j = 0; j < bounds.size(); ++j)
            if (bounds[j].valid)
                parts.push_back({offset + static_cast<std::uint32_t>(j), bounds[j]});
    }
    vertex_ranges.push_back(result->vertices_.size());
    // Levels of detail, appended after every draw's own indices. Each primitive's vertices are contiguous, so each
    // draw simplifies against its own block of them, each level from the one before it, as meshoptimizer recommends
    // for a chain. The draw's border stays locked, as Godot locks it, since draws simplified apart, such as a mesh's
    // material subsets or compile_static() pieces, must still meet whichever levels they draw.
    //
    // Vertices weld only when every attribute matches, so a hard edge or a texture seam leaves several vertices at one
    // position. Strictly, the simplifier collapses none of them, which leaves faceted models nearly whole. Permissive
    // simplification may collapse across those discontinuities, charging the change in normals and colors to the
    // step's error, as meshoptimizer recommends for faceted meshes. Simplification writes only indices, so a collapse
    // gives the moved corners the target vertex's other attributes, which it does not weigh. Splits in those stay
    // protected: texture coordinates, which would smear the texture, tangent handedness, which would mirror the normal
    // map, vertex alpha, and in a skinned draw the joints and weights, which would tear the surface once posed.
    std::vector<unsigned> position_remap;
    std::vector<unsigned char> vertex_locks;
    for (std::size_t d = 0; lods.levels && d < result->draws_.size(); ++d) {
        auto &draw = result->draws_[d];
        const auto alpha_mode =
            draw.material >= 0 ? materials->material_data[std::size_t(draw.material)].alpha_mode : AlphaMode::opaque;
        if (alpha_mode == AlphaMode::mask)
            continue;
        const bool blended = alpha_mode == AlphaMode::blend;
        const auto first = vertex_ranges[d], count = vertex_ranges[d + 1] - first;
        std::vector<unsigned> current(result->indices_.begin() + draw.first_index,
                                      result->indices_.begin() + draw.first_index + draw.index_count);
        for (auto &index : current)
            index -= static_cast<unsigned>(first);
        const auto *vertex = &result->vertices_[first];
        const float *positions = &vertex->position.x, *attributes = &vertex->normal.x;
        // Normals, then vertex colors, which follow them in SourceVertex, at the weight of about 1 that meshoptimizer
        // suggests for normalized attributes.
        static_assert(offsetof(SourceVertex, color) == offsetof(SourceVertex, normal) + 3 * sizeof(float));
        constexpr std::array<float, 6> weights{1, 1, 1, 1, 1, 1};
        // A step may move the surface by up to the draw's extent, since the renderer draws a level only where its
        // error projects within the LOD threshold, and must drop at least 15% of the indices it starts from, as
        // meshoptimizer's cluster LOD reference requires. Clamping keeps attribute error at the scale of the positional
        // error that selection projects, as meshoptimizer recommends when the error chooses levels.
        constexpr float maximum_relative_error = 1, minimum_reduction = .85F;
        constexpr unsigned options =
            meshopt_SimplifyLockBorder | meshopt_SimplifyErrorClamped | meshopt_SimplifyPermissive;
        const auto scale = meshopt_simplifyScale(positions, count, sizeof(SourceVertex));
        position_remap.resize(count);
        meshopt_generatePositionRemap(position_remap.data(), positions, count, sizeof(SourceVertex));
        const auto split = [&](const SourceVertex &a, const SourceVertex &b) {
            return a.uv != b.uv || a.tangent[3] != b.tangent[3] || a.alpha != b.alpha ||
                   (draw.skinned && (a.joints != b.joints || a.weights != b.weights));
        };
        vertex_locks.assign(count, 0);
        for (std::size_t i = 0; i < count; ++i)
            if (const auto shared = position_remap[i]; shared != i && split(vertex[shared], vertex[i]))
                vertex_locks[i] = meshopt_SimplifyVertex_Protect;
        std::vector<unsigned> level;
        float error = 0;
        for (std::size_t k = 0; k < lods.levels; ++k) {
            float step = 0;
            level.resize(current.size());
            level.resize(meshopt_simplifyWithAttributes(
                level.data(), current.data(), current.size(), positions, count, sizeof(SourceVertex), attributes,
                sizeof(SourceVertex), weights.data(), weights.size(), vertex_locks.data(), current.size() / 6 * 3,
                maximum_relative_error, options, &step));
            if (level.empty() || float(level.size()) > float(current.size()) * minimum_reduction)
                break;
            require(level.size() <= UINT32_MAX - result->indices_.size(), "Invalid render triangle count");
            // A step's error is measured from the level it starts from, so the chain adds them, as meshoptimizer
            // recommends for a chain: a level's error then covers every step between it and the draw's own triangles.
            error += step * scale;
            // Simplification keeps the surviving triangles in source order. Reorder a level for the vertex cache,
            // except a blended draw's, whose triangles composite in that order. Only the draw's own material is known
            // here; a blended custom material assigned later composites the reordered levels.
            if (!blended)
                meshopt_optimizeVertexCache(level.data(), level.data(), level.size(), count);
            draw.levels.push_back(
                {static_cast<std::uint32_t>(result->indices_.size()), static_cast<std::uint32_t>(level.size()), error});
            for (const auto index : level)
                result->indices_.push_back(index + static_cast<std::uint32_t>(first));
            current.swap(level);
        }
    }
    result->texel_retention_ = texel_retention;
    if (texel_retention == TexelRetention::until_upload)
        result->texels_ = detail::hold_texels(materials->textures, "Mesh texture texels were released after upload");
    result->materials_ = std::move(materials);
    // The rest pose's palette places each bounds part, as Scene::append_pose does for an object at the origin.
    std::vector<Mat4> rest_palette(result->rest_.world.begin(), result->rest_.world.end());
    for (const auto &skin : result->skins_)
        for (std::size_t j = 0; j < skin.joints.size(); ++j)
            rest_palette.push_back(result->rest_.world[skin.joints[j]] * skin.inverse_bind[j]);
    for (const auto &parts : result->bounds_)
        for (const auto &part : parts)
            for (unsigned corner = 0; corner < 8; ++corner) {
                const auto &b = part.bound;
                expand(result->rest_bounds_,
                       point(rest_palette[part.palette],
                             {corner & 1 ? b.maximum.x : b.minimum.x, corner & 2 ? b.maximum.y : b.minimum.y,
                              corner & 4 ? b.maximum.z : b.minimum.z}));
            }
    return result;
}
std::vector<std::shared_ptr<const Image>> Mesh::texel_images() const {
    if (const auto *hold = texels_.get())
        return hold->images();
    std::vector<std::shared_ptr<const Image>> result;
    result.reserve(materials_->textures.size());
    for (const auto &texture : materials_->textures)
        result.push_back(texture.image);
    return result;
}
void Mesh::release_texels() const noexcept {
    if (auto *hold = texels_.get())
        hold->release();
}

std::string ObjectKey::string() const { return std::to_string(value); }
ObjectKey ObjectKey::parse(std::string_view text) {
    ObjectKey key;
    if (text.empty() || text.size() > maximum_object_key_digits || (text.size() > 1 && text.front() == '0'))
        throw std::invalid_argument("Invalid object key");
    const auto result = std::from_chars(text.data(), text.data() + text.size(), key.value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        throw std::invalid_argument("Invalid object key");
    return key;
}
Scene::Scene() : lifetime_(std::make_shared<detail::SceneLifetime>()) {
    static std::atomic<std::uint64_t> next{1};
    owner_ = next.fetch_add(1);
    if (!owner_)
        std::terminate();
    lifetime_->scene = this;
}
Scene::~Scene() {
    // The hook, constructor or driver on the stack resumes using this scene after its callback
    // returns, so destroying it here would let that caller write freed memory.
    if (updating_ || constructing_) {
        std::fputs("Scene destroyed while running component hooks, constructing a component or held by a "
                   "scene driver; destroy it after the call returns\n",
                   stderr);
        std::terminate();
    }
    invalidate();
    release();
}
void Scene::invalidate() noexcept {
    lifetime_->scene = nullptr;
    keys_.clear();
    active_.clear();
    removed_instances_ = 0;
    object_count_ = 0;
    // The slots still own every record, so dropping the lists destroys no component.
    component_types_.clear();
    scheduled_ = {};
    for (auto &entry : slots_)
        for (auto &[type, record] : entry.components) {
            (void)type;
            record->attached = false;
        }
}
void Scene::release() noexcept {
    // All identities are already invalid. Release native storage before callbacks.
    std::shared_ptr<detail::ComponentRecord> retired;
    for (auto &entry : slots_) {
        for (auto &[type, record] : entry.components) {
            (void)type;
            record->retired_next = std::move(retired);
            retired = std::move(record);
        }
        entry.components.clear();
    }
    slots_.clear();
    free_slots_.clear();
    while (retired) {
        auto next = std::move(retired->retired_next);
        retired->disable();
        retired = std::move(next);
    }
}
bool Scene::contains(Id id) const noexcept {
    return lifetime_->scene && id.owner == owner_ && id.slot < slots_.size() && slots_[id.slot].alive &&
           slots_[id.slot].generation == id.generation;
}
Scene::Slot &Scene::slot(Id id) {
    if (!contains(id))
        throw std::out_of_range("Stale or foreign GameObject handle");
    return slots_[id.slot];
}
const Scene::Slot &Scene::slot(Id id) const { return const_cast<Scene *>(this)->slot(id); }
Scene::Instance &Scene::get(Id id) {
    auto &value = slot(id).value;
    if (!value.asset)
        throw std::logic_error("GameObject has no MeshRenderer");
    return value;
}
const Scene::Instance &Scene::instance(Id id) const { return const_cast<Scene *>(this)->get(id); }
void Scene::pose(Instance &value, const Pose &pose, const Mat4 &world) {
    std::vector<Mat4> palette;
    std::vector<RenderBounds> bounds;
    palette.reserve(value.asset->palette_size_);
    bounds.reserve(value.asset->draws_.size());
    auto combined = append_pose(*value.asset, pose, world, palette, bounds);
    if (value.placements)
        combined = place(*value.placements, world, bounds);
    // All validation and allocations completed before publishing pose and bounds.
    value.palette.swap(palette);
    value.primitive_bounds.swap(bounds);
    value.bounds = combined;
    value.world = world;
}
RenderBounds Scene::place(const MeshPlacements &placements, const Mat4 &world, std::span<RenderBounds> bounds) {
    RenderBounds combined;
    const auto relative = placements.primitive_bounds();
    for (std::size_t i = 0; i < bounds.size(); ++i) {
        bounds[i] = {};
        const auto &b = relative[i];
        if (!b.valid)
            continue;
        for (unsigned corner = 0; corner < 8; ++corner)
            expand(bounds[i],
                   point(world, {corner & 1 ? b.maximum.x : b.minimum.x, corner & 2 ? b.maximum.y : b.minimum.y,
                                 corner & 4 ? b.maximum.z : b.minimum.z}));
        expand(combined, bounds[i].minimum);
        expand(combined, bounds[i].maximum);
    }
    return combined;
}
RenderBounds Scene::append_pose(const Mesh &asset, const Pose &pose, const Mat4 &world, std::vector<Mat4> &palette,
                                std::vector<RenderBounds> &bounds) {
    require(pose.world.size() == asset.rest_.world.size(), "Pose does not match render asset");
    affine(world);
    const auto first_matrix = palette.size(), first_bound = bounds.size();
    for (const auto &node : pose.world) {
        affine(node);
        palette.push_back(world * node);
        affine(palette.back());
    }
    for (const auto &skin : asset.skins_)
        for (std::size_t j = 0; j < skin.joints.size(); ++j) {
            palette.push_back(world * pose.world[skin.joints[j]] * skin.inverse_bind[j]);
            affine(palette.back());
        }
    bounds.resize(first_bound + asset.draws_.size());
    const auto posed = std::span(bounds).subspan(first_bound);
    for (std::size_t i = 0; i < posed.size(); ++i)
        for (const auto &part : asset.bounds_[i])
            for (unsigned corner = 0; corner < 8; ++corner) {
                const auto &b = part.bound;
                expand(posed[i], point(palette[first_matrix + part.palette],
                                       {corner & 1 ? b.maximum.x : b.minimum.x, corner & 2 ? b.maximum.y : b.minimum.y,
                                        corner & 4 ? b.maximum.z : b.minimum.z}));
            }
    for (auto &bound : posed)
        if (bound.valid) {
            // Pad for the accepted weight error and an equal rounding margin in
            // the convex influence union at the GPU boundary.
            const auto pad = [](float lo, float hi) {
                return std::max({1.F, std::abs(lo), std::abs(hi)}) * (2 * mesh_limits::skin_weight_tolerance);
            };
            const Vec3 margin{pad(bound.minimum.x, bound.maximum.x), pad(bound.minimum.y, bound.maximum.y),
                              pad(bound.minimum.z, bound.maximum.z)};
            const auto lo = bound.minimum - margin, hi = bound.maximum + margin;
            expand(bound, lo);
            expand(bound, hi);
        }
    RenderBounds combined;
    for (const auto &bound : posed)
        if (bound.valid) {
            expand(combined, bound.minimum);
            expand(combined, bound.maximum);
        }
    return combined;
}
GameObject Scene::create(std::string name, std::shared_ptr<const Mesh> mesh) {
    if (!next_key_)
        throw std::overflow_error("Scene object keys exhausted");
    const ObjectKey key{next_key_++}; // Failure consumes an identity, never recycles it.
    return create_with_key(key, std::move(name), std::move(mesh));
}
GameObject Scene::create_with_key(ObjectKey key, std::string name, std::shared_ptr<const Mesh> mesh) {
    if (!lifetime_->scene)
        throw std::logic_error("Cannot create objects during scene teardown");
    require(key.value && !keys_.contains(key.value), "Duplicate or null scene object key");
    std::size_t index = slots_.size();
    if (free_slots_.empty()) {
        slots_.emplace_back();
        try {
            // Grows only when slots_ reallocates, so reserving stays amortized constant time.
            free_slots_.reserve(slots_.capacity());
        } catch (...) {
            slots_.pop_back();
            throw;
        }
    } else {
        // The lowest free slot, which a scan from the first slot would find.
        std::pop_heap(free_slots_.begin(), free_slots_.end(), std::greater<>{});
        index = free_slots_.back();
        free_slots_.pop_back();
    }
    auto &entry = slots_[index];
    entry.name = std::move(name);
    entry.key = key;
    entry.alive = true;
    ++object_count_;
    const Id id{owner_, entry.generation, index};
    try {
        keys_.emplace(key.value, id);
        auto transform = std::make_shared<detail::ComponentRecord>(object(id), typeid(ObjectTransform), false);
        transform->value =
            std::make_unique<detail::ComponentBox<ObjectTransform>>(object(id), ObjectTransform(object(id)));
        entry.components.emplace(typeid(ObjectTransform), transform);
        index_component(transform);
        if (mesh)
            assign_mesh(id, std::move(mesh));
    } catch (...) {
        remove(id);
        throw;
    }
    return object(id);
}
GameObject Scene::object(Id id) {
    (void)slot(id);
    return GameObject(lifetime_, id);
}
GameObject Scene::find(ObjectKey key) const noexcept {
    const auto found = keys_.find(key.value);
    return found != keys_.end() && contains(found->second) ? GameObject(lifetime_, found->second) : GameObject{};
}
std::vector<GameObject> Scene::roots() {
    std::vector<GameObject> result;
    for (std::size_t i = 0; i < slots_.size(); ++i)
        if (slots_[i].alive && !slots_[i].parent)
            result.push_back(object({owner_, slots_[i].generation, i}));
    return result;
}
Scene::Id Scene::add(std::shared_ptr<const Mesh> asset) {
    require(bool(asset), "Null mesh");
    return create({}, std::move(asset)).id();
}
void Scene::assign_mesh(Id id, std::shared_ptr<const Mesh> mesh, const Pose *initial_pose) {
    auto &entry = slot(id);
    require(mesh || !initial_pose, "An initial pose requires a mesh");
    if (!mesh) {
        if (entry.value.asset)
            retire_instance(entry);
        entry.value = {};
        entry.pose.reset();
        detach_component(id, typeid(MeshRenderer));
        return;
    }
    Instance next;
    next.asset = std::move(mesh);
    std::optional<Pose> next_pose;
    if (initial_pose)
        next_pose = *initial_pose;
    for (const auto &material : next.asset->materials_->material_data)
        next.factors.push_back(material.factor);
    next.custom_materials.resize(next.factors.size());
    next.primitive_visible.resize(next.asset->draws_.size(), true);
    pose(next, next_pose ? *next_pose : next.asset->rest_, entry.world);
    // Allocate before publishing. Replacement keeps the object's transform, but
    // resets overrides and uses either the authored initial pose or mesh defaults.
    if (!entry.value.asset) {
        auto renderer = std::make_shared<detail::ComponentRecord>(object(id), typeid(MeshRenderer), false);
        renderer->value = std::make_unique<detail::ComponentBox<MeshRenderer>>(object(id), MeshRenderer(object(id)));
        entry.components.emplace(typeid(MeshRenderer), renderer);
        try {
            index_component(renderer);
            active_.push_back(id);
        } catch (...) {
            detach_component(id, typeid(MeshRenderer));
            throw;
        }
        entry.instance = active_.size() - 1;
    }
    entry.value = std::move(next);
    entry.value.active = entry.active_hierarchy;
    component(id, typeid(MeshRenderer))->enabled = true;
    entry.pose = std::move(next_pose);
}
void Scene::link_child(std::size_t parent, std::size_t child) noexcept {
    auto &owner = slots_[parent];
    auto &entry = slots_[child];
    entry.previous_sibling = owner.last_child;
    entry.next_sibling = no_slot;
    if (owner.last_child == no_slot)
        owner.first_child = child;
    else
        slots_[owner.last_child].next_sibling = child;
    owner.last_child = child;
}
void Scene::unlink_child(std::size_t child) noexcept {
    auto &entry = slots_[child];
    auto &owner = slots_[entry.parent->slot];
    if (entry.previous_sibling == no_slot)
        owner.first_child = entry.next_sibling;
    else
        slots_[entry.previous_sibling].next_sibling = entry.next_sibling;
    if (entry.next_sibling == no_slot)
        owner.last_child = entry.previous_sibling;
    else
        slots_[entry.next_sibling].previous_sibling = entry.previous_sibling;
    entry.previous_sibling = entry.next_sibling = no_slot;
}
void Scene::retire_instance(Slot &entry) noexcept {
    active_[entry.instance] = Id{};
    entry.instance = no_slot;
    // Compacting once half the entries are null keeps removal amortized constant time and the
    // list at most twice the renderer count, even when instances() is never called.
    if (++removed_instances_ > active_.size() / 2)
        compact_instances();
}
void Scene::compact_instances() const noexcept {
    // Keeps the order renderers were added, as erasing each one in place did.
    std::size_t kept = 0;
    for (const auto id : active_)
        if (id != Id{}) {
            slots_[id.slot].instance = kept;
            active_[kept++] = id;
        }
    active_.erase(active_.begin() + static_cast<std::ptrdiff_t>(kept), active_.end());
    removed_instances_ = 0;
}
std::span<const Scene::Id> Scene::instances() const {
    if (removed_instances_)
        compact_instances();
    return active_;
}
void Scene::release_slot(std::size_t index) noexcept {
    free_slots_.push_back(index); // Within the capacity reserved when the slot was created.
    std::push_heap(free_slots_.begin(), free_slots_.end(), std::greater<>{});
}
void Scene::remove(Id id) {
    auto &root = slot(id);
    if (root.parent)
        unlink_child(id.slot);
    root.parent.reset();
    std::shared_ptr<detail::ComponentRecord> retired;
    // Leaf-first traversal uses existing links, with no allocation or recursion. Each object is
    // its parent's last child when it is reached, so unlinking it takes constant time.
    auto current = id.slot;
    for (;;) {
        auto &entry = slots_[current];
        if (entry.last_child != no_slot) {
            current = entry.last_child;
            continue;
        }
        const auto parent = entry.parent;
        if (parent)
            unlink_child(current);
        if (entry.value.asset)
            retire_instance(entry);
        entry.value = {};
        entry.name.clear();
        keys_.erase(entry.key.value);
        entry.key = {};
        entry.world = entry.local = identity();
        entry.parent.reset();
        entry.pose.reset();
        // Retire values without allocating. Destructors run after the whole
        // subtree is removed, so they cannot observe partially removed objects.
        for (auto &[type, component] : entry.components) {
            (void)type;
            if (component->attached)
                unindex_component(*component);
            component->attached = false;
            component->retired_next = std::move(retired);
            retired = std::move(component);
        }
        entry.components.clear();
        entry.alive = false;
        entry.active_self = entry.active_hierarchy = true;
        --object_count_;
        if (entry.generation != UINT64_MAX)
            ++entry.generation;
        // A slot whose generation is exhausted is never reused, so no stale Id can match it.
        if (entry.generation != UINT64_MAX)
            release_slot(current);
        if (!parent)
            break;
        current = parent->slot;
    }
    const bool was_updating = std::exchange(updating_, true);
    while (retired) {
        auto next = std::move(retired->retired_next);
        retired->disable();
        retired.reset();
        retired = std::move(next);
    }
    updating_ = was_updating;
}
void Scene::set_pose(Id id, const Pose &value, const Mat4 &world) {
    auto &entry = slot(id);
    if (get(id).placements)
        throw std::logic_error("A renderer that draws placements keeps the rest pose");
    // A stored pose with room for this one takes the copy in place. Otherwise the copy is made
    // first, so a failed allocation publishes nothing.
    const bool fits = entry.pose && entry.pose->local.capacity() >= value.local.size() &&
                      entry.pose->world.capacity() >= value.world.size();
    std::optional<Pose> copy;
    if (!fits)
        copy.emplace(value);
    update_transform(id, to_local(id, world), world, &value);
    if (copy)
        entry.pose = std::move(copy);
    else {
        // Within capacity, copying these trivially copyable elements neither allocates nor throws.
        static_assert(std::is_trivially_copyable_v<Transform> && std::is_trivially_copyable_v<Mat4>);
        entry.pose->local = value.local;
        entry.pose->world = value.world;
    }
}
void Scene::set_transform(Id id, const Mat4 &world) { update_transform(id, to_local(id, world), world); }
Mat4 Scene::to_local(Id id, const Mat4 &world) const {
    const auto &entry = slot(id);
    if (world == entry.world)
        return entry.local;
    return entry.parent ? inverse(slot(*entry.parent).world) * world : world;
}
void Scene::set_local_transform(Id id, const Mat4 &local) {
    const auto &entry = slot(id);
    update_transform(id, local, entry.parent ? slot(*entry.parent).world * local : local);
}
void Scene::update_transform(Id id, const Mat4 &local, const Mat4 &world, const Pose *replacement) {
    auto &entry = slot(id);
    affine(local);
    affine(world);
    // Most animated objects are leaves or keep their placement between samples, so only a moved
    // object with children re-poses more than itself.
    const bool moved = world != entry.world, descendants = moved && entry.first_child != no_slot;
    if (descendants || (entry.value.asset && (replacement || moved))) {
        // Validate every affected palette and bound in the working lists before publishing any.
        posed_objects_.clear();
        posed_palettes_.clear();
        posed_bounds_.clear();
        posed_objects_.push_back({id, world, {}, {}, {}});
        for (std::size_t i = 0; i < posed_objects_.size(); ++i) {
            const auto current = posed_objects_[i].id;
            const auto placed = posed_objects_[i].world;
            const auto &source = slots_[current.slot];
            affine(placed);
            if (source.value.asset) {
                posed_objects_[i].palette = posed_palettes_.size();
                posed_objects_[i].bounds = posed_bounds_.size();
                posed_objects_[i].combined = append_pose(*source.value.asset,
                                                         current == id && replacement ? *replacement
                                                         : source.pose                ? *source.pose
                                                                                      : source.value.asset->rest_,
                                                         placed, posed_palettes_, posed_bounds_);
                if (source.value.placements)
                    posed_objects_[i].combined = place(
                        *source.value.placements, placed,
                        std::span(posed_bounds_).subspan(posed_objects_[i].bounds, source.value.asset->draws_.size()));
            }
            if (descendants)
                for (auto child = source.first_child; child != no_slot; child = slots_[child].next_sibling)
                    posed_objects_.push_back({id_at(child), placed * slots_[child].local, {}, {}, {}});
        }
        // A renderer keeps its mesh, so its lists keep their sizes and copying cannot allocate.
        for (const auto &posed : posed_objects_) {
            auto &target = slots_[posed.id.slot];
            target.world = posed.world;
            auto &value = target.value;
            if (!value.asset)
                continue;
            std::copy_n(posed_palettes_.begin() + static_cast<std::ptrdiff_t>(posed.palette), value.palette.size(),
                        value.palette.begin());
            std::copy_n(posed_bounds_.begin() + static_cast<std::ptrdiff_t>(posed.bounds),
                        value.primitive_bounds.size(), value.primitive_bounds.begin());
            value.bounds = posed.combined;
            value.world = posed.world;
        }
    }
    entry.local = local;
    entry.world = world;
}
void Scene::reparent(Id id, std::optional<Id> parent, ReparentMode mode) {
    auto &entry = slot(id);
    require(mode == ReparentMode::keep_world || mode == ReparentMode::keep_local, "Invalid reparent mode");
    constexpr auto cycle = "GameObject parenting would create a cycle";
    require(parent != id, cycle);
    if (entry.parent == parent)
        return;
    // Only a parent inside the moved subtree closes a cycle, so a leaf needs no search, and a
    // subtree is searched once instead of walking every ancestor of the new parent.
    const bool leaf = entry.first_child == no_slot;
    std::vector<Id> affected;
    if (!leaf) {
        affected = subtree(id);
        require(!parent || std::find(affected.begin(), affected.end(), *parent) == affected.end(), cycle);
    }
    const auto parent_world = parent ? slot(*parent).world : identity();
    const auto local =
        mode == ReparentMode::keep_world ? (parent ? inverse(parent_world) * entry.world : entry.world) : entry.local;
    const auto world = mode == ReparentMode::keep_world ? entry.world : parent_world * local;
    update_transform(id, local, world);
    // Relinking cannot fail, so the accepted transform and the new links are published together.
    if (entry.parent)
        unlink_child(id.slot);
    entry.parent = parent;
    if (parent)
        link_child(parent->slot, id.slot);
    refresh_activation(leaf ? std::span<const Id>(&id, 1) : std::span<const Id>(affected));
}
std::vector<Scene::Id> Scene::subtree(Id id) const {
    std::vector<Id> result{id};
    for (std::size_t i = 0; i < result.size(); ++i)
        for (auto child = slots_[result[i].slot].first_child; child != no_slot; child = slots_[child].next_sibling)
            result.push_back(id_at(child));
    return result;
}
void Scene::refresh_activation(std::span<const Id> objects) noexcept {
    for (const auto id : objects) {
        auto &entry = slots_[id.slot];
        entry.active_hierarchy = entry.active_self && (!entry.parent || slots_[entry.parent->slot].active_hierarchy);
        entry.value.active = entry.active_hierarchy;
    }
}
Scene &GameObject::scene() const {
    const auto lifetime = lifetime_.lock();
    if (!lifetime || !lifetime->scene || !lifetime->scene->contains(id_))
        throw std::out_of_range("Expired GameObject handle");
    return *lifetime->scene;
}
bool GameObject::valid() const noexcept {
    const auto lifetime = lifetime_.lock();
    return lifetime && lifetime->scene && lifetime->scene->contains(id_);
}
std::string GameObject::name() const { return scene().slot(id_).name; }
ObjectKey GameObject::key() const { return scene().slot(id_).key; }
void GameObject::set_name(std::string name) { scene().slot(id_).name = std::move(name); }
bool GameObject::active_self() const { return scene().slot(id_).active_self; }
bool GameObject::active_in_hierarchy() const { return scene().slot(id_).active_hierarchy; }
void GameObject::set_active(bool active) {
    auto &owner = scene();
    auto &entry = owner.slot(id_);
    if (entry.active_self == active)
        return;
    const auto affected = owner.subtree(id_);
    entry.active_self = active;
    owner.refresh_activation(affected);
}
ObjectTransform GameObject::transform() const {
    (void)scene();
    return ObjectTransform(*this);
}
Mat4 GameObject::world_matrix() const { return scene().slot(id_).world; }
Mat4 GameObject::local_matrix() const { return scene().slot(id_).local; }
Vec3 GameObject::local_position() const {
    const auto local = local_matrix();
    return translation_of(local);
}
void GameObject::set_local_position(Vec3 position) {
    auto local = local_matrix();
    set_translation(local, position);
    set_local_matrix(local);
}
void GameObject::set_local_transform(const Transform &transform) { set_local_matrix(matrix(transform)); }
void GameObject::set_local_matrix(const Mat4 &local) { scene().set_local_transform(id_, local); }
Quat GameObject::local_rotation() const { return detail::orientation(detail::upper(local_matrix())); }
void GameObject::set_local_rotation(const Quat &rotation) {
    const auto local = local_matrix();
    set_local_matrix(matrix({translation_of(local), rotation, detail::axis_scale(detail::upper(local))}));
}
Vec3 GameObject::local_scale() const { return detail::axis_scale(detail::upper(local_matrix())); }
void GameObject::set_local_scale(Vec3 scale) {
    const auto local = local_matrix();
    set_local_matrix(matrix({translation_of(local), detail::orientation(detail::upper(local)), scale}));
}
std::optional<Transform> GameObject::local_transform() const { return decompose(local_matrix()); }
GameObject GameObject::parent() const {
    auto &owner = scene();
    const auto parent = owner.slot(id_).parent;
    return parent ? owner.object(*parent) : GameObject{};
}
std::vector<GameObject> GameObject::children() const {
    auto &owner = scene();
    std::vector<GameObject> result;
    for (auto child = owner.slot(id_).first_child; child != Scene::no_slot; child = owner.slots_[child].next_sibling)
        result.push_back(GameObject(owner.lifetime_, owner.id_at(child)));
    return result;
}
void GameObject::set_parent(GameObject parent, ReparentMode mode) {
    auto &owner = scene();
    require(&parent.scene() == &owner, "GameObject parent belongs to another scene");
    owner.reparent(id_, parent.id(), mode);
}
void GameObject::clear_parent(ReparentMode mode) { scene().reparent(id_, {}, mode); }
Vec3 GameObject::position() const {
    const auto world = world_matrix();
    return translation_of(world);
}
void GameObject::set_position(Vec3 position) {
    auto world = world_matrix();
    set_translation(world, position);
    set_world_matrix(world);
}
void GameObject::set_world_transform(const Transform &transform) { set_world_matrix(matrix(transform)); }
void GameObject::set_world_matrix(const Mat4 &world) { scene().set_transform(id_, world); }
Quat GameObject::rotation() const { return detail::orientation(detail::upper(world_matrix())); }
void GameObject::set_rotation(const Quat &rotation) {
    const auto world = world_matrix();
    set_world_matrix(matrix({translation_of(world), rotation, detail::axis_scale(detail::upper(world))}));
}
Vec3 GameObject::forward() const { return normalized(-axis_z(world_matrix())); }
Vec3 GameObject::right() const { return normalized(axis_x(world_matrix())); }
Vec3 GameObject::up() const { return normalized(axis_y(world_matrix())); }
void GameObject::look_at(Vec3 target, Vec3 up) { set_rotation(look_rotation(target - position(), up)); }
bool GameObject::has_renderer() const { return bool(scene().slot(id_).value.asset); }
MeshRenderer GameObject::add_mesh(std::shared_ptr<const Mesh> mesh) {
    require(bool(mesh), "Null mesh");
    if (has_renderer())
        throw std::logic_error("GameObject already has a MeshRenderer");
    scene().assign_mesh(id_, std::move(mesh));
    return MeshRenderer(*this);
}
MeshRenderer GameObject::renderer() const {
    (void)scene().instance(id_);
    return MeshRenderer(*this);
}
void GameObject::remove_mesh() { scene().assign_mesh(id_, {}); }
void GameObject::destroy() { scene().remove(id_); }
std::shared_ptr<const Mesh> MeshRenderer::mesh() const { return object_.scene().instance(object_.id_).asset; }
void MeshRenderer::set_mesh(std::shared_ptr<const Mesh> mesh) {
    require(bool(mesh), "Null mesh");
    (void)object_.scene().instance(object_.id_);
    object_.scene().assign_mesh(object_.id_, std::move(mesh));
}
const Pose &MeshRenderer::pose() const {
    const auto &scene = object_.scene();
    const auto &value = scene.instance(object_.id_);
    const auto &entry = scene.slot(object_.id_);
    return entry.pose ? *entry.pose : value.asset->rest_pose();
}
void MeshRenderer::set_pose(const Pose &pose) { object_.scene().set_pose(object_.id_, pose, object_.world_matrix()); }
void MeshRenderer::set_pose(const Pose &pose, const Mat4 &world) { object_.scene().set_pose(object_.id_, pose, world); }
bool MeshRenderer::visible() const { return object_.scene().instance(object_.id_).visible; }
void MeshRenderer::set_visible(bool visible) { object_.scene().set_visible(object_.id_, visible); }
Vec3 MeshRenderer::material_factor(std::size_t material) const {
    const auto &factors = object_.scene().instance(object_.id_).factors;
    require_index(material, factors.size(), material_factor_slot);
    return factors[material];
}
void MeshRenderer::set_material_factor(std::size_t material, Vec3 factor) {
    object_.scene().set_material_factor(object_.id_, material, factor);
}
void MeshRenderer::clear_material_factor(std::size_t material) {
    object_.scene().clear_material_factor(object_.id_, material);
}
std::shared_ptr<const CustomMaterial> MeshRenderer::custom_material(std::size_t material) const {
    const auto &custom = object_.scene().instance(object_.id_).custom_materials;
    require_index(material, custom.size(), custom_material_slot);
    return custom[material];
}
void MeshRenderer::set_custom_material(std::size_t material, std::shared_ptr<const CustomMaterial> custom) {
    object_.scene().set_custom_material(object_.id_, material, std::move(custom));
}
bool MeshRenderer::primitive_visible(std::size_t primitive) const {
    const auto &visible = object_.scene().instance(object_.id_).primitive_visible;
    require_index(primitive, visible.size(), primitive_slot);
    return visible[primitive];
}
void MeshRenderer::set_primitive_visible(std::size_t primitive, bool visible) {
    object_.scene().set_primitive_visible(object_.id_, primitive, visible);
}
bool MeshRenderer::casts_shadows() const { return object_.scene().instance(object_.id_).casts_shadows; }
void MeshRenderer::set_casts_shadows(bool casts) { object_.scene().set_casts_shadows(object_.id_, casts); }
VisibilityRange MeshRenderer::visibility_range() const {
    return object_.scene().instance(object_.id_).visibility_range;
}
void MeshRenderer::set_visibility_range(const VisibilityRange &range) {
    object_.scene().set_visibility_range(object_.id_, range);
}
std::shared_ptr<const MeshPlacements> MeshRenderer::placements() const {
    return object_.scene().instance(object_.id_).placements;
}
void MeshRenderer::set_placements(std::shared_ptr<const MeshPlacements> placements) {
    object_.scene().set_placements(object_.id_, std::move(placements));
}
void MeshRenderer::set_placement_transforms(std::span<const Mat4> transforms) {
    set_placements(MeshPlacements::create(mesh(), transforms));
}
RenderBounds MeshRenderer::bounds() const { return object_.scene().instance(object_.id_).bounds; }
void Scene::set_material_factor(Id id, std::size_t material, Vec3 factor) {
    auto &value = get(id);
    require(finite(factor) && factor.x >= 0 && factor.x <= 1 && factor.y >= 0 && factor.y <= 1 && factor.z >= 0 &&
                factor.z <= 1,
            "Invalid render material factor");
    require_index(material, value.factors.size(), material_factor_slot);
    value.factors[material] = factor;
}
void Scene::clear_material_factor(Id id, std::size_t material) {
    auto &value = get(id);
    require_index(material, value.factors.size(), material_factor_slot);
    value.factors[material] = value.asset->materials_->material_data[material].factor;
}
void Scene::set_custom_material(Id id, std::size_t material, std::shared_ptr<const CustomMaterial> custom) {
    auto &value = get(id);
    auto &slots = value.custom_materials;
    require_index(material, slots.size(), custom_material_slot);
    require(!value.placements || !custom || custom->reads_placements(),
            "A custom material that draws placements must read them");
    slots[material] = std::move(custom);
}
void Scene::set_visible(Id id, bool visible) {
    get(id).visible = visible;
    component(id, typeid(MeshRenderer))->enabled = visible;
}
void Scene::set_primitive_visible(Id id, std::size_t primitive, bool visible) {
    auto &shown = get(id).primitive_visible;
    require_index(primitive, shown.size(), primitive_slot);
    shown[primitive] = visible;
}
void Scene::set_casts_shadows(Id id, bool casts) { get(id).casts_shadows = casts; }
void validate_visibility_range(const VisibilityRange &range) {
    const auto finite_nonnegative = [](float v) { return std::isfinite(v) && v >= 0; };
    require(finite_nonnegative(range.begin) && finite_nonnegative(range.begin_margin) &&
                finite_nonnegative(range.end_margin) && !std::isnan(range.end) && range.end > range.begin &&
                double(range.begin_margin) + range.end_margin <= double(range.end) - range.begin,
            "A visibility range requires 0 <= begin < end and margins that fit between them");
    // No distance reaches an infinite end, so a margin before it would never dissolve anything.
    require(std::isfinite(range.end) || range.end_margin == 0, "An endless visibility range has no end margin");
}
void Scene::set_visibility_range(Id id, const VisibilityRange &range) {
    auto &value = get(id);
    validate_visibility_range(range);
    value.visibility_range = range;
}
void Scene::set_placements(Id id, std::shared_ptr<const MeshPlacements> placements) {
    auto &entry = slot(id);
    auto &value = get(id);
    if (placements) {
        require(placements->mesh() == value.asset, "Placements copy another mesh");
        require(std::all_of(value.custom_materials.begin(), value.custom_materials.end(),
                            [](const auto &custom) { return !custom || custom->reads_placements(); }),
                "A custom material that draws placements must read them");
        if (entry.pose)
            throw std::logic_error("A renderer with a pose cannot draw placements");
    }
    // Without placements a renderer draws the one copy its palette places, so its bounds come from its pose.
    std::vector<Mat4> palette;
    std::vector<RenderBounds> bounds;
    auto combined =
        append_pose(*value.asset, entry.pose ? *entry.pose : value.asset->rest_, entry.world, palette, bounds);
    if (placements)
        combined = place(*placements, entry.world, bounds);
    value.primitive_bounds.swap(bounds);
    value.bounds = combined;
    value.placements = std::move(placements);
}
RenderBounds Scene::bounds() const {
    RenderBounds result;
    for (auto id : instances()) {
        const auto &value = instance(id);
        if (!value.visible || !value.active)
            continue;
        for (std::size_t i = 0; i < value.primitive_bounds.size(); ++i)
            if (value.primitive_visible[i] && value.primitive_bounds[i].valid) {
                expand(result, value.primitive_bounds[i].minimum);
                expand(result, value.primitive_bounds[i].maximum);
            }
    }
    return result;
}
MeshSnapshot Scene::snapshot(SceneGeometryBudget budget) const {
    std::size_t corners = 0;
    const auto objects = instances();
    for (auto id : objects) {
        const auto &value = instance(id);
        const auto copies = value.placements ? value.placements->transforms().size() : 1;
        // Each draw's own indices; levels of detail follow them in Mesh::indices() and are not snapshotted.
        std::size_t size = 0;
        for (const auto &draw : value.asset->draws_)
            size += draw.index_count;
        require(size <= (std::numeric_limits<std::size_t>::max() - corners) / copies, "Snapshot vertex overflow");
        corners += size * copies;
    }
    (void)validate_scene_geometry(corners, budget);
    MeshSnapshot result;
    result.vertices.reserve(corners);
    RenderBounds bounds;
    for (auto id : objects) {
        const auto &value = instance(id);
        const auto &asset = *value.asset;
        const auto &description = *asset.materials();
        const auto material_offset = result.material_data.size(), texture_offset = result.textures.size();
        require(description.material_data.size() <= INT_MAX && description.textures.size() <= INT_MAX &&
                    material_offset <= INT_MAX - description.material_data.size() &&
                    texture_offset <= INT_MAX - description.textures.size(),
                "Snapshot material index overflow");
        result.textures.insert(result.textures.end(), description.textures.begin(), description.textures.end());
        for (std::size_t i = 0; i < description.material_data.size(); ++i) {
            auto material = description.material_data[i];
            material.factor = value.factors[i];
            for (auto *index : {&material.texture, &material.normal_texture, &material.metallic_roughness_texture,
                                &material.emissive_texture, &material.occlusion_texture})
                if (*index >= 0)
                    *index += static_cast<int>(texture_offset);
            result.material_data.push_back(std::move(material));
        }
        result.mesh_nodes += description.mesh_nodes;
        result.skins += description.skins;
        result.joints += description.joints;
        result.bind_deviation = std::max(result.bind_deviation, description.bind_deviation);
        result.default_is_bind_pose &= description.default_is_bind_pose;
        result.clips.insert(result.clips.end(), description.clips.begin(), description.clips.end());
        result.notices.insert(result.notices.end(), description.notices.begin(), description.notices.end());
        // Each placement draws a copy whose node matrices are the object's world matrix times the placement times
        // the rest pose's; without placements, the palette places the one copy.
        const std::span<const Mat4> placements =
            value.placements ? value.placements->transforms() : std::span<const Mat4>();
        std::vector<Mat4> copy_palette;
        for (std::size_t copy = 0; copy < std::max<std::size_t>(placements.size(), 1); ++copy) {
            if (!placements.empty()) {
                copy_palette.clear();
                for (const auto &node : asset.rest_.world)
                    copy_palette.push_back(value.world * placements[copy] * node);
            }
            const auto &palette = placements.empty() ? value.palette : copy_palette;
            for (std::size_t i = 0; i < asset.draws().size(); ++i) {
                const auto &draw = asset.draws()[i];
                const bool visible = value.visible && value.active && value.primitive_visible[i];
                const auto factor = draw.material < 0 ? Vec3{1, 1, 1} : value.factors[draw.material];
                result.primitives.push_back(
                    {draw.node_name, draw.mesh_name,
                     draw.material < 0 ? "default" : description.material_data[draw.material].name, palette[draw.node],
                     static_cast<std::uint32_t>(result.vertices.size()), draw.index_count,
                     draw.material < 0 ? no_index : static_cast<int>(material_offset) + draw.material, visible});
                if (draw.skinned)
                    result.skinned_vertices += draw.index_count;
                bool reversed = false;
                for (std::size_t j = draw.first_index; j < std::size_t(draw.first_index) + draw.index_count; ++j) {
                    const auto corner = j - draw.first_index;
                    const auto &source = asset.vertices()[asset.indices()[j]];
                    auto transform = palette[draw.palette_offset];
                    if (draw.skinned) {
                        transform = {};
                        for (unsigned influence = 0; influence < 4; ++influence)
                            if (source.weights[influence] != 0)
                                for (unsigned k = 0; k < 16; ++k)
                                    transform[k] += palette[draw.palette_offset + source.joints[influence]][k] *
                                                    source.weights[influence];
                    }
                    // The first corner's matrix decides the triangle's winding, as it does on the GPU.
                    if (corner % 3 == 0)
                        reversed = detail::reverses_winding(transform);
                    const auto position = point(transform, source.position);
                    result.vertices.push_back(
                        {position,
                         normal(transform, source.normal),
                         {source.color.x * factor.x, source.color.y * factor.y, source.color.z * factor.z},
                         source.uv,
                         tangent(transform, source.tangent),
                         source.alpha});
                    if (visible)
                        expand(bounds, position);
                    // A reversed triangle swaps its last two corners, keeping its source winding against its normals.
                    if (reversed && corner % 3 == 2)
                        std::swap(result.vertices[result.vertices.size() - 2], result.vertices.back());
                }
            }
        }
    }
    result.materials = result.material_data.size();
    if (bounds.valid) {
        result.minimum = bounds.minimum;
        result.maximum = bounds.maximum;
    }
    return result;
}
} // namespace anima
