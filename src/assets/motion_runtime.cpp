#include "mesh_limits.hpp"
#include "presentation_data.hpp"
#include <anima/assets/motion_runtime.hpp>
namespace anima {
namespace {
// Nearest evaluation joint of a model node that has none at or above it.
constexpr int no_joint = -1;
// The one motion contract version MotionRuntime accepts.
constexpr int motion_contract_version = 4;
// The one evaluation definition version a motion contract may embed.
constexpr int motion_evaluation_version = 1;
} // namespace
struct MotionRuntime::Impl {
  public:
    Impl(std::shared_ptr<const anima::Asset> asset, const anima::Manifest &manifest, const nlohmann::json &contract)
        : asset_(std::move(asset)), rig_(*asset_, bind(*asset_, manifest, contract)),
          nearest_joint_(nearest_joints(*asset_, rig_)) {
        load_resource(manifest, contract);
        const auto &definition = contract.at("evaluation");
        for (const auto &[name, roots] : detail::json_object(definition, "masks").items()) {
            if (name.empty() || !roots.is_array() || roots.empty())
                throw std::invalid_argument("Invalid runtime mask");
            auto &weights = masks_[name];
            weights.resize(rig_.size());
            std::set<std::string> unique;
            for (const auto &root : roots) {
                const auto id = root.get<std::string>();
                if (!unique.insert(id).second)
                    throw std::invalid_argument("Duplicate mask root");
                const auto mask = rig_.subtree_mask(rig_.joint(id));
                for (std::size_t i = 0; i < weights.size(); ++i)
                    weights[i] = std::max(weights[i], mask[i]);
            }
        }
        for (const auto &[name, value] : detail::json_object(definition, "chains").items()) {
            detail::json_fields(value, {"joints", "minimum_angle", "maximum_angle"});
            const auto &joints = value.at("joints");
            if (name.empty() || joints.size() != 3)
                throw std::invalid_argument("Invalid contact chain");
            anima::TwoBoneContact c;
            c.start = rig_.joint(joints.at(0).get<std::string>());
            c.middle = rig_.joint(joints.at(1).get<std::string>());
            c.end = rig_.joint(joints.at(2).get<std::string>());
            c.minimum_angle = detail::json_float(value.at("minimum_angle"));
            c.maximum_angle = detail::json_float(value.at("maximum_angle"));
            const auto rest = anima::sample_pose(*asset_);
            const auto encoded = rig_.encode(rest);
            const auto world = rig_.world(encoded);
            c.target = anima::point(world[c.end], {});
            c.pole = anima::point(world[c.middle], {}) + anima::Vec3{0, 0, 1};
            c.weight = 0;
            (void)anima::solve_contact(rig_, encoded, c);
            chains_.emplace(name, c);
        }
    }
    const std::shared_ptr<const anima::Asset> &model() const noexcept { return asset_; }
    const anima::EvaluationRig &rig() const { return rig_; }
    bool has_mask(std::string_view name) const { return masks_.contains(name); }
    const std::vector<float> &mask_named(std::string_view name) const {
        const auto found = masks_.find(name);
        if (found == masks_.end())
            throw std::out_of_range("Unknown motion mask: " + std::string(name));
        return found->second;
    }
    const anima::TwoBoneContact &chain_named(std::string_view name) const {
        const auto found = chains_.find(name);
        if (found == chains_.end())
            throw std::out_of_range("Unknown motion chain: " + std::string(name));
        return found->second;
    }
    std::size_t contact_end_node(std::string_view chain) const { return rig_.asset_node(chain_named(chain).end); }
    bool contact_affects_node(std::string_view chain, std::size_t node) const {
        const auto start = chain_named(chain).start;
        if (node >= nearest_joint_.size())
            throw std::out_of_range("Unknown model node");
        const auto joint = nearest_joint_[node];
        return joint != no_joint && rig_.descendant(static_cast<std::size_t>(joint), start);
    }
    bool contacts_overlap(std::string_view first, std::string_view second) const {
        const auto &a = chain_named(first);
        const auto &b = chain_named(second);
        return rig_.descendant(a.start, b.start) || rig_.descendant(b.start, a.start);
    }
    const anima::Animation &clip(std::string_view name) const { return anima::find_animation(*resource_, name); }
    const anima::ClipMetadata &metadata(std::string_view name) const {
        const auto found = metadata_.find(name);
        if (found == metadata_.end())
            throw std::out_of_range("Unknown base motion/action: " + std::string(name));
        return found->second;
    }
    const auto &clips() const { return metadata_; }
    bool is_layer(std::string_view name) const { return layers_.contains(name); }
    const std::string &layer_mask(std::string_view name) const {
        const auto found = layers_.find(name);
        if (found == layers_.end())
            throw std::out_of_range("Unknown motion layer: " + std::string(name));
        return found->second;
    }
    anima::Pose sample(std::string_view name, double time) const {
        const auto sampled = anima::sample_pose(*resource_, &clip(name), time);
        auto local = rest_.local;
        for (const auto &[source, target] : binding_) {
            local[target] = sampled.local[source];
        }
        return anima::pose_from_local(*asset_, local);
    }
    anima::Pose compose(std::string_view motion, double time, std::string_view layer) const {
        (void)metadata(motion);
        auto base = sample(motion, time);
        if (layer.empty())
            return base;
        const auto phase = std::clamp(time / clip(motion).duration, 0., 1.);
        const auto layered = sample(layer, phase * clip(layer).duration);
        return rig_.render_pose(
            base, rig_.override_layer(rig_.encode(base), rig_.encode(layered), mask_named(layer_mask(layer))));
    }
    void validate_layers(std::span<const std::string_view> layers) const {
        std::vector<float> occupied(rig_.size());
        for (const auto layer : layers)
            if (!layer.empty()) {
                const auto &weights = mask_named(layer_mask(layer));
                for (std::size_t i = 0; i < weights.size(); ++i) {
                    if (occupied[i] > 0 && weights[i] > 0)
                        throw std::invalid_argument("Motion layers have overlapping joint ownership");
                    occupied[i] += weights[i];
                }
            }
    }
    anima::Pose compose_layers(std::string_view motion, double time, std::span<const std::string_view> layers) const {
        validate_layers(layers);
        if (layers.empty())
            return compose(motion, time, {});
        if (layers.size() == 1)
            return compose(motion, time, layers.front());
        auto base = compose(motion, time, {});
        auto evaluated = rig_.encode(base);
        const auto phase = std::clamp(time / clip(motion).duration, 0., 1.);
        for (const auto layer : layers)
            if (!layer.empty())
                evaluated = rig_.override_layer(evaluated, rig_.encode(sample(layer, phase * clip(layer).duration)),
                                                mask_named(layer_mask(layer)));
        return rig_.render_pose(base, evaluated);
    }
    anima::Pose blend(const anima::Pose &from, const anima::Pose &to, float weight) const {
        if (weight == 0)
            return from;
        if (weight == 1)
            return to;
        return rig_.render_pose(
            to, rig_.override_layer(rig_.encode(from), rig_.encode(to), std::vector<float>(rig_.size(), weight)));
    }
    MotionEvaluation evaluate(const anima::Pose &source, const MotionControls &controls) const {
        if (controls.empty())
            return {source, {}}; // Preserve the exact default path.
        auto evaluated = rig_.encode(source);
        for (const auto &layer : controls.layers) {
            const bool additive = layer.mode == anima::LayerMode::additive;
            if (additive && layer.reference_clip.empty())
                throw std::invalid_argument("Additive motion layer needs a reference clip");
            if (!additive && !layer.reference_clip.empty())
                throw std::invalid_argument("Override layer cannot have an additive reference");
            if (is_layer(layer.clip) && layer.mask != layer_mask(layer.clip))
                throw std::invalid_argument("Layer control exceeds the resource's declared ownership");
            auto weights = mask_named(layer.mask);
            for (auto &weight : weights)
                weight *= layer.weight;
            const auto contribution = rig_.encode(sample(layer.clip, layer.time));
            evaluated =
                additive ? rig_.additive_layer(evaluated, contribution,
                                               rig_.encode(sample(layer.reference_clip, layer.reference_time)), weights)
                         : rig_.override_layer(evaluated, contribution, weights);
        }
        for (const auto &offset : controls.offsets) {
            const auto joint = rig_.joint(offset.joint);
            evaluated.local[joint] = anima::operator*(
                evaluated.local[joint], anima::blend_affine(anima::identity(), offset.delta, offset.weight));
        }
        MotionEvaluation result;
        for (const auto &request : controls.contacts) {
            auto contact = chain_named(request.chain);
            contact.target = request.target;
            contact.pole = request.pole;
            contact.weight = request.weight;
            contact.end_rotation = request.end_rotation;
            auto solved = anima::solve_contact(rig_, evaluated, contact);
            evaluated = std::move(solved.pose);
            result.contacts.push_back({request.chain, solved.error, solved.reachable, request.weight});
        }
        result.pose = rig_.render_pose(source, evaluated);
        return result;
    }

  private:
    // Nearest evaluation joint at or above each node of @p asset in its hierarchy, or no_joint. A node that is not an
    // evaluation joint follows its asset parent (see EvaluationRig::render_pose), so it moves with that joint. @p rig
    // has checked that the hierarchy is acyclic, with every parent in range.
    static std::vector<int> nearest_joints(const anima::Asset &asset, const anima::EvaluationRig &rig) {
        constexpr int unresolved = -2;
        std::vector<int> result(asset.nodes.size(), unresolved);
        for (std::size_t i = 0; i < rig.size(); ++i)
            result[rig.asset_node(i)] = static_cast<int>(i);
        // Each walk up the hierarchy stops at the first resolved node and resolves the nodes it passed, so the walks
        // visit every node once in all.
        std::vector<std::size_t> path;
        for (std::size_t node = 0; node < result.size(); ++node) {
            auto n = static_cast<int>(node);
            for (; n >= 0 && result[static_cast<std::size_t>(n)] == unresolved;
                 n = asset.nodes[static_cast<std::size_t>(n)].parent)
                path.push_back(static_cast<std::size_t>(n));
            const auto joint = n < 0 ? no_joint : result[static_cast<std::size_t>(n)];
            for (const auto passed : path)
                result[passed] = joint;
            path.clear();
        }
        return result;
    }
    static std::vector<anima::EvaluationJoint> bind(const anima::Asset &asset, const anima::Manifest &manifest,
                                                    const nlohmann::json &data) {
        // Every object is checked for unknown fields, as the other presentation readers do.
        detail::json_version(data, "version", motion_contract_version, "Unsupported motion contract version");
        detail::json_fields(data, {"version", "skeleton", "evaluation", "resource", "clips", "layers"});
        const auto &skeleton = data.at("skeleton");
        const auto &definition = data.at("evaluation");
        detail::json_fields(skeleton, {"id", "bind_signature", "joint_count"});
        detail::json_version(definition, "version", motion_evaluation_version, "Unsupported motion evaluation version");
        detail::json_fields(definition, {"version", "id", "parents", "masks", "chains"});
        if (definition.at("id").get<std::string>().empty() || skeleton.at("id") != manifest.skeleton_id ||
            skeleton.at("bind_signature") != manifest.bind_signature ||
            skeleton.at("joint_count") != manifest.joint_count)
            throw std::invalid_argument("Motion rig/skin contract mismatch");
        const auto &parents = definition.at("parents");
        if (!parents.is_object() || parents.size() != manifest.joint_count)
            throw std::invalid_argument("Evaluation joint count mismatch");
        std::map<std::string, int> names;
        std::vector<anima::EvaluationJoint> result;
        for (const auto &[name, parent] : parents.items()) {
            (void)parent;
            names[name] = static_cast<int>(result.size());
            result.push_back({name, anima::find_node(asset, name), -1});
        }
        for (auto &joint : result) {
            const auto &parent = parents.at(joint.name);
            if (!parent.is_null())
                joint.parent = names.at(parent.get<std::string>());
        }
        return result;
    }
    void load_resource(const anima::Manifest &manifest, const nlohmann::json &document) {
        const auto path = std::filesystem::path(document.at("resource").get<std::string>());
        if (path.empty() || path.is_absolute() || path.extension() != ".glb" ||
            std::any_of(path.begin(), path.end(), [](const auto &part) { return part == ".."; }))
            throw std::invalid_argument("Motion resource must be a relative GLB inside its contract directory");
        resource_ = anima::load_motion_asset((manifest.directory / manifest.motion_contract).parent_path() / path);
        rest_ = anima::sample_pose(*asset_);
        const auto source_rest = anima::sample_pose(*resource_);
        for (std::size_t i = 0; i < resource_->nodes.size(); ++i) {
            const auto target = anima::find_node(*asset_, resource_->nodes[i].name);
            const auto &from = resource_->nodes[i];
            const auto &to = asset_->nodes[target];
            const auto parent_name = [](const anima::Asset &a, const anima::AssetNode &n) {
                return n.parent < 0 ? std::string{} : a.nodes.at(static_cast<std::size_t>(n.parent)).name;
            };
            if (parent_name(*resource_, from) != parent_name(*asset_, to))
                throw std::invalid_argument("Motion resource hierarchy differs from body bind");
            for (std::size_t j = 0; j < 16; ++j)
                if (std::abs(source_rest.world[i][j] - rest_.world[target][j]) > mesh_limits::rest_pose_tolerance)
                    throw std::invalid_argument("Motion resource bind differs from body model");
            binding_.emplace_back(i, target);
        }
        for (std::size_t i = 0; i < rig_.size(); ++i)
            (void)anima::find_node(*resource_, asset_->nodes[rig_.asset_node(i)].name);
        for (const auto &value : detail::json_array(document, "clips")) {
            detail::json_fields(value, {"name", "loop", "reference_speed", "events"});
            anima::ClipMetadata info;
            info.name = value.at("name").get<std::string>();
            info.loop = value.at("loop").get<bool>();
            if (const auto &speed = value.at("reference_speed"); !speed.is_null()) {
                info.reference_speed = speed.get<double>();
                if (!std::isfinite(*info.reference_speed) || *info.reference_speed <= 0)
                    throw std::invalid_argument("Invalid motion reference speed");
            }
            double previous = -1;
            for (const auto &event : detail::json_array(value, "events")) {
                detail::json_fields(event, {"time", "name"});
                anima::ClipEvent cue{event.at("time").get<double>(), event.at("name").get<std::string>()};
                if (!std::isfinite(cue.time) || cue.time < previous || cue.time < 0 ||
                    cue.time > clip(info.name).duration || cue.name.empty())
                    throw std::invalid_argument("Invalid motion cue");
                previous = cue.time;
                info.events.push_back(std::move(cue));
            }
            if (clip(info.name).duration <= 0)
                throw std::invalid_argument("Motion requires a positive duration");
            if (info.name.empty() || !metadata_.emplace(info.name, std::move(info)).second)
                throw std::invalid_argument("Duplicate/empty motion identity");
        }
        // Layers are read before the constructor decodes the masks, so each reads its mask's roots from the document.
        const auto &masks = detail::json_object(document.at("evaluation"), "masks");
        for (const auto &[name, value] : detail::json_object(document, "layers").items()) {
            detail::json_fields(value, {"mask", "owned_joints", "context_joints"});
            const auto mask = value.at("mask").get<std::string>();
            const auto &roots = masks.at(mask);
            std::set<std::string> owned, context;
            for (const auto &root : roots) {
                const auto weights = rig_.subtree_mask(rig_.joint(root.get<std::string>()));
                for (std::size_t i = 0; i < weights.size(); ++i)
                    if (weights[i] > 0)
                        owned.insert(asset_->nodes[rig_.asset_node(i)].name);
            }
            const auto &parents = document.at("evaluation").at("parents");
            for (const auto &joint : owned)
                if (!parents.at(joint).is_null()) {
                    const auto parent = parents.at(joint).get<std::string>();
                    if (!owned.contains(parent))
                        context.insert(parent);
                }
            if (value.at("owned_joints").get<std::set<std::string>>() != owned ||
                value.at("context_joints").get<std::set<std::string>>() != context || metadata_.contains(name))
                throw std::invalid_argument("Invalid motion layer ownership/context");
            for (const auto &channel : clip(name).channels) {
                const auto &joint = resource_->nodes[channel.node].name;
                if (!owned.contains(joint) && !context.contains(joint))
                    throw std::invalid_argument("Motion layer animates joints it does not own");
            }
            layers_.emplace(name, mask);
        }
        if (metadata_.empty() || metadata_.size() + layers_.size() != resource_->animations.size())
            throw std::invalid_argument("Motion contract must cover every resource clip exactly once");
    }
    std::shared_ptr<const anima::Asset> asset_, resource_;
    anima::Pose rest_;
    std::vector<std::pair<std::size_t, std::size_t>> binding_;
    std::map<std::string, anima::ClipMetadata, std::less<>> metadata_;
    std::map<std::string, std::string, std::less<>> layers_;
    anima::EvaluationRig rig_;
    // Nearest evaluation joint at or above each asset node; see nearest_joints.
    std::vector<int> nearest_joint_;
    std::map<std::string, std::vector<float>, std::less<>> masks_;
    std::map<std::string, anima::TwoBoneContact, std::less<>> chains_;
};
MotionRuntime::MotionRuntime(std::shared_ptr<const Asset> asset, const Manifest &manifest, std::string_view contract) {
    if (!asset)
        throw std::invalid_argument("Motion runtime requires an asset");
    impl_ = presentation_data::decode_step(
        [&] { return std::make_shared<Impl>(std::move(asset), manifest, presentation_data::parse(contract)); });
}
std::shared_ptr<const MotionRuntime> MotionRuntime::load(std::shared_ptr<const Asset> asset, const Manifest &manifest) {
    if (manifest.motion_contract.empty())
        throw std::invalid_argument("Actor requires an independent motion contract");
    return std::make_shared<MotionRuntime>(
        std::move(asset), manifest, presentation_data::read(manifest.directory / manifest.motion_contract).dump());
}
const std::shared_ptr<const Asset> &MotionRuntime::model() const noexcept { return impl_->model(); }
const EvaluationRig &MotionRuntime::rig() const { return impl_->rig(); }
bool MotionRuntime::has_mask(std::string_view name) const { return impl_->has_mask(name); }
std::size_t MotionRuntime::contact_end_node(std::string_view chain) const { return impl_->contact_end_node(chain); }
bool MotionRuntime::contact_affects_node(std::string_view chain, std::size_t node) const {
    return impl_->contact_affects_node(chain, node);
}
bool MotionRuntime::contacts_overlap(std::string_view first, std::string_view second) const {
    return impl_->contacts_overlap(first, second);
}
const Animation &MotionRuntime::clip(std::string_view name) const { return impl_->clip(name); }
const ClipMetadata &MotionRuntime::metadata(std::string_view name) const { return impl_->metadata(name); }
const std::map<std::string, ClipMetadata, std::less<>> &MotionRuntime::clips() const { return impl_->clips(); }
bool MotionRuntime::is_layer(std::string_view name) const { return impl_->is_layer(name); }
const std::string &MotionRuntime::layer_mask(std::string_view name) const { return impl_->layer_mask(name); }
Pose MotionRuntime::sample(std::string_view name, double time) const { return impl_->sample(name, time); }
Pose MotionRuntime::compose(std::string_view motion, double time, std::string_view layer) const {
    return impl_->compose(motion, time, layer);
}
void MotionRuntime::validate_layers(std::span<const std::string_view> layers) const {
    return impl_->validate_layers(layers);
}
Pose MotionRuntime::compose_layers(std::string_view motion, double time,
                                   std::span<const std::string_view> layers) const {
    return impl_->compose_layers(motion, time, layers);
}
Pose MotionRuntime::blend(const Pose &from, const Pose &to, float weight) const {
    return impl_->blend(from, to, weight);
}
MotionEvaluation MotionRuntime::evaluate(const Pose &source, const MotionControls &controls) const {
    return impl_->evaluate(source, controls);
}
} // namespace anima
