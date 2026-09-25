#include <algorithm>
#include <anima/assets/imports.hpp>
#include <fstream>
#include <set>

namespace anima {
namespace {
std::vector<std::byte> read_file(const std::filesystem::path &path, std::size_t limit) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error("Cannot open import dependency: " + path.string());
    const auto size = file.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > limit)
        throw std::length_error("Import dependency exceeds the byte limit");
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    file.seekg(0);
    if (!bytes.empty())
        file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file || file.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("Import dependency changed while reading");
    return bytes;
}
} // namespace
std::filesystem::path ImportSource::resolve(const std::filesystem::path &path) const {
    if (path.empty() || path.is_absolute())
        throw std::invalid_argument("Import dependencies must be project-relative paths");
    const auto resolved = std::filesystem::weakly_canonical(root_ / path);
    auto root = root_.begin(), candidate = resolved.begin();
    for (; root != root_.end(); ++root, ++candidate)
        if (candidate == resolved.end() || *root != *candidate)
            throw std::invalid_argument("Import dependency escapes the project root");
    return resolved;
}
std::span<const std::byte> ImportSource::read(const std::filesystem::path &path) {
    const auto key = path.lexically_normal();
    const auto resolved = resolve(key);
    if (std::find(dependencies_.begin(), dependencies_.end(), key) == dependencies_.end())
        dependencies_.push_back(key);
    auto found = files_.find(key);
    if (found == files_.end())
        found = files_.emplace(key, read_file(resolved, file_limit_)).first;
    return found->second;
}
AssetImports::AssetImports(std::filesystem::path root, std::size_t maximum_file_bytes)
    : root_(std::filesystem::canonical(root)), file_limit_(maximum_file_bytes) {
    if (!std::filesystem::is_directory(root_) || !file_limit_)
        throw std::invalid_argument("Invalid asset import root or byte limit");
}
void AssetImports::verify(const ImportSource::Files &files) const {
    ImportSource::Files unused;
    ImportSource source(root_, unused, file_limit_);
    for (const auto &[path, bytes] : files)
        if (read_file(source.resolve(path), file_limit_) != bytes)
            throw std::runtime_error("Import input changed before publication: " + path.string());
}
std::vector<std::string> AssetImports::refresh() {
    Mutation mutation(importing_);
    ImportSource::Files files;
    ImportSource resolver(root_, files, file_limit_);
    std::set<std::filesystem::path> changed;
    for (const auto &[key, entry] : entries_) {
        (void)key;
        for (const auto &path : entry->dependencies) {
            if (files.contains(path))
                continue;
            auto bytes = read_file(resolver.resolve(path), file_limit_);
            if (bytes != accepted_.at(path))
                changed.insert(path);
            files.emplace(path, std::move(bytes));
        }
    }
    std::vector<std::string> updated;
    std::map<EntryBase *, std::vector<std::filesystem::path>> dependencies;
    try {
        for (const auto &[key, entry] : entries_) {
            if (std::none_of(entry->dependencies.begin(), entry->dependencies.end(),
                             [&](const auto &path) { return changed.contains(path); }))
                continue;
            ImportSource source(root_, files, file_limit_);
            entry->prepare(source);
            dependencies.emplace(entry.get(), std::move(source.dependencies_));
            updated.push_back(key);
        }
        verify(files);
        // Retain old resources until every accepted value/dependency is published.
        for (auto &[entry, inputs] : dependencies) {
            entry->dependencies.swap(inputs);
            entry->publish(true);
        }
        accepted_.swap(files);
        prune_inputs();
        for (auto &[key, entry] : entries_) {
            (void)key;
            entry->discard();
        }
    } catch (...) {
        for (auto &[key, entry] : entries_) {
            (void)key;
            entry->discard();
        }
        throw;
    }
    return updated;
}
bool AssetImports::erase(std::string_view key) {
    Mutation mutation(importing_);
    const auto found = entries_.find(key);
    if (found == entries_.end())
        return false;
    entries_.erase(found);
    prune_inputs();
    return true;
}
void AssetImports::prune_inputs() {
    std::erase_if(accepted_, [&](const auto &input) {
        return std::none_of(entries_.begin(), entries_.end(), [&](const auto &entry) {
            const auto &paths = entry.second->dependencies;
            return std::find(paths.begin(), paths.end(), input.first) != paths.end();
        });
    });
}
std::vector<std::filesystem::path> AssetImports::dependencies(std::string_view key) const {
    const auto found = entries_.find(key);
    if (found == entries_.end())
        throw std::out_of_range("Unknown asset key");
    return found->second->dependencies;
}
} // namespace anima
