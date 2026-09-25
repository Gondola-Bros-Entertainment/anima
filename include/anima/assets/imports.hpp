#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeindex>
#include <utility>
#include <vector>

namespace anima {
// One immutable view of input files for an import transaction. All paths are
// relative to the project root. Returned bytes are borrowed during the importer.
class ImportSource {
  public:
    std::span<const std::byte> read(const std::filesystem::path &path);

  private:
    friend class AssetImports;
    using Files = std::map<std::filesystem::path, std::vector<std::byte>>;
    ImportSource(std::filesystem::path root, Files &files, std::size_t file_limit)
        : root_(std::move(root)), files_(files), file_limit_(file_limit) {}
    std::filesystem::path resolve(const std::filesystem::path &path) const;
    std::filesystem::path root_;
    Files &files_;
    std::size_t file_limit_;
    std::vector<std::filesystem::path> dependencies_;
};

namespace detail {
template <class T> struct ImportedAsset {
    std::shared_ptr<const T> value;
    std::uint64_t revision = 1;
};
} // namespace detail

// Resource identity survives reimport. A snapshot returned by get() keeps its
// accepted immutable version alive even after replacement or registry eviction.
template <class T> class AssetRef {
  public:
    AssetRef() = default;
    explicit operator bool() const { return bool(state_); }
    std::shared_ptr<const T> get() const {
        if (!state_)
            throw std::out_of_range("Empty asset reference");
        return state_->value;
    }
    std::uint64_t revision() const {
        if (!state_)
            throw std::out_of_range("Empty asset reference");
        return state_->revision;
    }

  private:
    friend class AssetImports;
    explicit AssetRef(std::shared_ptr<detail::ImportedAsset<T>> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::ImportedAsset<T>> state_;
};

// Explicit content-based refresh, single-threaded between scene updates. A refresh
// stages every affected importer and publishes all revisions only after every
// importer and dependency verification succeeds. No timestamps or silent fallback.
class AssetImports {
    struct Mutation {
        explicit Mutation(bool &active) : active_(active) {
            if (active_)
                throw std::logic_error("Import callbacks cannot mutate their registry");
            active_ = true;
        }
        ~Mutation() { active_ = false; }
        bool &active_;
    };

  public:
    explicit AssetImports(std::filesystem::path root, std::size_t maximum_file_bytes = 64 * 1024 * 1024);
    template <class T, class Importer> AssetRef<T> add(std::string key, Importer importer) {
        Mutation mutation(importing_);
        if (key.empty() || key.size() > 4096 || entries_.contains(key))
            throw std::invalid_argument("Invalid or duplicate asset key");
        auto entry = std::make_unique<Entry<T>>(std::move(importer));
        ImportSource::Files files;
        ImportSource source(root_, files, file_limit_);
        entry->prepare(source);
        verify(files);
        entry->dependencies = std::move(source.dependencies_);
        auto reference = AssetRef<T>(entry->state);
        // Complete allocations before publishing an identity or resource version.
        auto next_files = accepted_;
        for (const auto &[path, bytes] : files)
            if (accepted_.contains(path) && accepted_.at(path) != bytes)
                throw std::logic_error("Refresh changed shared inputs before adding an importer");
        for (const auto &[path, bytes] : files)
            next_files.insert_or_assign(path, bytes);
        auto *accepted = entry.get();
        entries_.emplace(std::move(key), std::move(entry));
        accepted->publish(false);
        accepted_.swap(next_files);
        accepted->discard();
        return reference;
    }
    template <class T> AssetRef<T> get(std::string_view key) const {
        const auto found = entries_.find(key);
        if (found == entries_.end() || found->second->type() != typeid(T))
            throw std::out_of_range("Unknown asset key or resource type");
        return AssetRef<T>(static_cast<Entry<T> &>(*found->second).state);
    }
    std::vector<std::string> refresh();
    bool erase(std::string_view key);
    std::vector<std::filesystem::path> dependencies(std::string_view key) const;

  private:
    struct EntryBase {
        virtual ~EntryBase() = default;
        virtual std::type_index type() const = 0;
        virtual void prepare(ImportSource &) = 0;
        virtual void publish(bool bump) noexcept = 0;
        virtual void discard() noexcept = 0;
        std::vector<std::filesystem::path> dependencies;
    };
    template <class T> struct Entry final : EntryBase {
        explicit Entry(std::function<std::shared_ptr<const T>(ImportSource &)> load) : importer(std::move(load)) {}
        std::type_index type() const override { return typeid(T); }
        void prepare(ImportSource &source) override {
            if (state->revision == UINT64_MAX)
                throw std::overflow_error("Asset revision exhausted");
            staged = importer(source);
            if (!staged)
                throw std::invalid_argument("Importer returned no resource");
        }
        void publish(bool bump) noexcept override {
            state->value.swap(staged);
            state->revision += bump;
        }
        void discard() noexcept override { staged.reset(); }
        std::function<std::shared_ptr<const T>(ImportSource &)> importer;
        std::shared_ptr<detail::ImportedAsset<T>> state = std::make_shared<detail::ImportedAsset<T>>();
        std::shared_ptr<const T> staged;
    };
    void verify(const ImportSource::Files &) const;
    void prune_inputs();
    std::filesystem::path root_;
    std::size_t file_limit_;
    std::map<std::string, std::unique_ptr<EntryBase>, std::less<>> entries_;
    ImportSource::Files accepted_;
    bool importing_{};
};
} // namespace anima
