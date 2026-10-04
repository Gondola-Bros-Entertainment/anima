#include "mesh_limits.hpp"
#include "presentation_data.hpp"
#include <algorithm>
#include <anima/assets/manifest.hpp>
#include <fstream>
#include <set>
#include <stdexcept>

namespace anima {
namespace {
using Json = presentation_data::Json;
std::string text(const Json &object, const std::string &key) { return object.at(key).get<std::string>(); }
double number(const Json &value) {
    if (!value.is_number())
        throw std::invalid_argument("Manifest JSON requires a number");
    return value.get<double>();
}
const Json::array_t &array(const Json &value, const char *message) {
    if (!value.is_array())
        throw std::invalid_argument(message);
    return value.get_ref<const Json::array_t &>();
}
// The one manifest version read_manifest accepts.
constexpr int manifest_version = 4;
constexpr std::streamoff maximum_manifest_bytes = 1024 * 1024;
constexpr int maximum_manifest_depth = 32;
// A bind signature is 64 lowercase hexadecimal digits.
constexpr std::size_t bind_signature_digits = 64;
// The file that @p value, the UTF-8 text of manifest field @p field, names beside the manifest.
std::filesystem::path filename(const char *field, const std::string &value) {
    const auto message = "Manifest " + std::string(field) + " must be a filename beside the manifest: " + value;
    auto result = presentation_data::relative_document_path(value, {}, message.c_str());
    if (result.has_parent_path() || value == ".")
        throw std::invalid_argument(message);
    return result;
}
Manifest load_manifest(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        throw std::invalid_argument("Cannot open manifest: " + detail::utf8_text(path));
    const auto size = input.tellg();
    if (size <= 0 || size > maximum_manifest_bytes)
        throw std::invalid_argument("Manifest exceeds 1 MiB or is empty");
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    input.read(bytes.data(), size);
    if (!input)
        throw std::runtime_error("Cannot read manifest");
    Json json;
    try {
        json = presentation_data::parse(bytes, maximum_manifest_depth);
    } catch (const std::exception &error) {
        throw std::invalid_argument("Invalid manifest JSON: " + std::string(error.what()));
    }
    detail::json_version(json, "version", manifest_version, "Unsupported model manifest version");
    detail::json_fields(json, {"version", "units", "asset_id", "model", "motion_contract", "skeleton", "clips"});
    if (text(json, "units") != "meters")
        throw std::invalid_argument("Manifest units must be meters");
    Manifest result;
    result.directory = std::filesystem::absolute(path).parent_path();
    result.asset_id = text(json, "asset_id");
    result.model = filename("model", text(json, "model"));
    if (const auto &contract = json.at("motion_contract"); !contract.is_null())
        result.motion_contract = filename("motion_contract", contract.get<std::string>());
    const auto &skeleton = json.at("skeleton");
    detail::json_fields(skeleton, {"id", "bind_signature", "joint_count"});
    result.skeleton_id = text(skeleton, "id");
    result.bind_signature = text(skeleton, "bind_signature");
    const auto count = number(skeleton.at("joint_count"));
    if (count < 1 || count > mesh_limits::maximum_skin_joints || std::floor(count) != count)
        throw std::invalid_argument("Invalid manifest joint count");
    result.joint_count = static_cast<std::size_t>(count);
    if (result.skeleton_id.empty() || result.bind_signature.size() != bind_signature_digits ||
        !std::all_of(result.bind_signature.begin(), result.bind_signature.end(),
                     [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        throw std::invalid_argument("Manifest needs a skeleton ID and hexadecimal bind signature");
    std::set<std::string> clips;
    for (const auto &item : array(json.at("clips"), "Manifest clips must be an array")) {
        detail::json_fields(item, {"name", "loop", "reference_speed", "events"});
        ClipMetadata clip;
        clip.name = text(item, "name");
        clip.loop = item.at("loop").get<bool>();
        if (const auto &speed = item.at("reference_speed"); !speed.is_null()) {
            clip.reference_speed = number(speed);
            if (!std::isfinite(*clip.reference_speed) || *clip.reference_speed <= 0)
                throw std::invalid_argument("Clip reference speed must be finite and positive");
        }
        if (!clips.insert(clip.name).second)
            throw std::invalid_argument("Duplicate manifest clip: " + clip.name);
        for (const auto &event : array(item.at("events"), "Manifest clip events must be an array")) {
            detail::json_fields(event, {"time", "name"});
            ClipEvent value{number(event.at("time")), text(event, "name")};
            if (value.time < 0 || value.name.empty())
                throw std::invalid_argument("Invalid manifest clip event");
            clip.events.push_back(value);
        }
        std::stable_sort(clip.events.begin(), clip.events.end(),
                         [](const auto &a, const auto &b) { return a.time < b.time; });
        result.clips.push_back(std::move(clip));
    }
    return result;
}
} // namespace
Manifest read_manifest(const std::filesystem::path &path) {
    // Missing or mistyped fields surface as the JSON library's exceptions; report them as invalid content.
    return detail::json_step([&] { return load_manifest(path); });
}
void validate_manifest(const Manifest &manifest, const Asset &asset) {
    if (asset.skins.size() != 1 || asset.skins[0].joints.size() != manifest.joint_count)
        throw std::invalid_argument("Model skin does not match manifest joint count");
    if (manifest.clips.size() != asset.animations.size())
        throw std::invalid_argument("Manifest clip list does not match GLB animations");
    for (const auto &metadata : manifest.clips) {
        if (metadata.reference_speed && (!std::isfinite(*metadata.reference_speed) || *metadata.reference_speed <= 0))
            throw std::invalid_argument("Clip reference speed must be finite and positive");
        const auto named = [&](const Animation &clip) { return clip.name == metadata.name; };
        if (std::ranges::count_if(asset.animations, named) != 1)
            throw std::invalid_argument("Manifest clip must name exactly one animation: " + metadata.name);
        const auto &animation = *std::ranges::find_if(asset.animations, named);
        for (const auto &event : metadata.events)
            if (event.time > animation.duration)
                throw std::invalid_argument("Manifest clip event outside its clip: " + metadata.name);
    }
}
} // namespace anima
