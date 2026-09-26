#include "mesh_limits.hpp"
#include "presentation_data.hpp"
#include <algorithm>
#include <anima/assets/preview.hpp>
#include <fstream>
#include <set>
#include <stdexcept>

namespace anima {
namespace {
using Json = presentation_data::Json;
const Json *optional(const Json &object, const std::string &key) {
    const auto value = object.find(key);
    return value == object.end() ? nullptr : &*value;
}
std::string text(const Json &object, const std::string &key) { return object.at(key).get<std::string>(); }
std::string optional_text(const Json &object, const std::string &key) {
    const auto *value = optional(object, key);
    return value ? value->get<std::string>() : "";
}
bool optional_bool(const Json &object, const std::string &key) {
    const auto *value = optional(object, key);
    return value && value->get<bool>();
}
double number(const Json &value) {
    if (!value.is_number())
        throw std::runtime_error("Manifest JSON requires a number");
    return value.get<double>();
}
constexpr std::streamoff maximum_manifest_bytes = 1024 * 1024;
constexpr int maximum_manifest_depth = 32;
// A bind signature is 64 lowercase hexadecimal digits.
constexpr std::size_t bind_signature_digits = 64;
void filename(const std::string &value) {
    const std::filesystem::path path = value;
    if (value.empty() || value.find('\0') != std::string::npos || value.find(':') != std::string::npos ||
        value.find('\\') != std::string::npos || path.is_absolute() || path.has_parent_path() || value == "." ||
        value == "..")
        throw std::runtime_error("Manifest model must be a filename beside the manifest: " + value);
}
Manifest load_manifest(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        throw std::runtime_error("Cannot open manifest: " + path.string());
    const auto size = input.tellg();
    if (size <= 0 || size > maximum_manifest_bytes)
        throw std::runtime_error("Manifest exceeds 1 MiB or is empty");
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    input.read(bytes.data(), size);
    if (!input)
        throw std::runtime_error("Cannot read manifest");
    Json json;
    try {
        json = presentation_data::parse(bytes, maximum_manifest_depth);
    } catch (const std::exception &error) {
        throw std::runtime_error("Invalid manifest JSON: " + std::string(error.what()));
    }
    Manifest result;
    result.directory = std::filesystem::absolute(path).parent_path();
    if (number(json.at("schema_version")) != 1 || text(json, "units") != "meters")
        throw std::runtime_error("Manifest requires schema_version 1 and meter units");
    result.asset_id = text(json, "asset_id");
    result.model = text(json, "model");
    filename(result.model);
    if (const auto *contract = optional(json, "motion_contract")) {
        result.motion_contract = contract->get<std::string>();
        filename(result.motion_contract);
    }
    const auto &skeleton = json.at("skeleton");
    result.skeleton_id = text(skeleton, "id");
    result.bind_signature = text(skeleton, "bind_signature");
    const auto count = number(skeleton.at("joint_count"));
    if (count < 1 || count > mesh_limits::maximum_skin_joints || std::floor(count) != count)
        throw std::runtime_error("Invalid manifest joint count");
    result.joint_count = static_cast<std::size_t>(count);
    if (result.skeleton_id.empty() || result.bind_signature.size() != bind_signature_digits ||
        !std::all_of(result.bind_signature.begin(), result.bind_signature.end(),
                     [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        throw std::runtime_error("Manifest needs a skeleton ID and hexadecimal bind signature");
    std::set<std::string> clips, items;
    for (const auto &item : json.at("clips").get_ref<const Json::array_t &>()) {
        ClipMetadata clip;
        clip.name = text(item, "name");
        clip.loop = item.at("loop").get<bool>();
        if (const auto *speed = optional(item, "reference_speed")) {
            clip.reference_speed = number(*speed);
            if (!std::isfinite(*clip.reference_speed) || *clip.reference_speed <= 0)
                throw std::runtime_error("Clip reference speed must be finite and positive");
        }
        if (!clips.insert(clip.name).second)
            throw std::runtime_error("Duplicate manifest clip: " + clip.name);
        if (const auto *events = optional(item, "events"))
            for (const auto &event : events->get_ref<const Json::array_t &>()) {
                ClipEvent value{number(event.at("time_seconds")), text(event, "event")};
                if (value.time < 0 || value.name.empty())
                    throw std::runtime_error("Invalid preview event");
                clip.events.push_back(value);
            }
        std::stable_sort(clip.events.begin(), clip.events.end(),
                         [](const auto &a, const auto &b) { return a.time < b.time; });
        result.clips.push_back(std::move(clip));
    }
    for (const auto &item : json.at("equipment").get_ref<const Json::array_t &>()) {
        EquipmentMetadata value{text(item, "id"),
                                text(item, "slot"),
                                optional_text(item, "model"),
                                optional_text(item, "mesh"),
                                optional_text(item, "skeleton"),
                                optional_text(item, "socket"),
                                optional_bool(item, "included")};
        if (!items.insert(value.id).second)
            throw std::runtime_error("Duplicate equipment ID: " + value.id);
        if (!value.model.empty())
            filename(value.model);
        result.equipment.push_back(std::move(value));
    }
    return result;
}
} // namespace
Manifest read_manifest(const std::filesystem::path &path) {
    // Missing or mistyped fields surface as the JSON library's exceptions; report them as invalid content.
    return detail::json_step<std::runtime_error>([&] { return load_manifest(path); });
}
void validate_manifest(const Manifest &manifest, const Asset &asset) {
    if (asset.skins.size() != 1 || asset.skins[0].joints.size() != manifest.joint_count)
        throw std::runtime_error("Character skin does not match manifest joint count");
    if (manifest.clips.size() != asset.animations.size())
        throw std::runtime_error("Manifest clip list does not match GLB animations");
    for (const auto &metadata : manifest.clips) {
        if (metadata.reference_speed && (!std::isfinite(*metadata.reference_speed) || *metadata.reference_speed <= 0))
            throw std::runtime_error("Clip reference speed must be finite and positive");
        const auto named = [&](const Animation &clip) { return clip.name == metadata.name; };
        if (std::ranges::count_if(asset.animations, named) != 1)
            throw std::runtime_error("Manifest clip must name exactly one animation: " + metadata.name);
        const auto &animation = *std::ranges::find_if(asset.animations, named);
        for (const auto &event : metadata.events)
            if (event.time > animation.duration)
                throw std::runtime_error("Preview event outside clip: " + metadata.name);
    }
}
} // namespace anima
