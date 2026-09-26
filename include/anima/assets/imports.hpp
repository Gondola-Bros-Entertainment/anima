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

/// @file
/// Typed resource identities with explicit, content-based reimport.
///
/// Part of the `anima::assets` target; adds no dependencies. Importers turn project files into
/// immutable resources, and applications decide when to refresh and adopt new versions. Import is
/// synchronous on the calling thread: there is no file watcher or background worker, and edits
/// are found by the next AssetImports::refresh. This is not a file system transaction. Use a
/// registry and its references from one thread.

namespace anima {
/// Read access to project files during one import transaction.
///
/// Every importer in a transaction sees the same bytes for a path. Read input files only through
/// read() so they are tracked and verified; other side effects cannot be rolled back.
class ImportSource {
  public:
    /// Bytes of the file at project-relative @p path, recorded as a dependency.
    ///
    /// The span is borrowed until the importer returns; resources must copy what they keep.
    /// Throws `std::invalid_argument` for an empty or absolute path or one that resolves outside
    /// the project root, including through symbolic links; `std::length_error` for a file above
    /// the registry's byte limit; and `std::runtime_error` when the file cannot be read or changes
    /// while being read.
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

/// Identity of one imported resource that survives reimport.
///
/// Copies share the identity. A pointer returned by get() keeps its accepted immutable version
/// alive after later refreshes or AssetImports::erase. After erasure the reference keeps its
/// final version and revision.
template <class T> class AssetRef {
  public:
    /// An empty reference.
    AssetRef() = default;
    /// Whether the reference has an identity.
    explicit operator bool() const { return bool(state_); }
    /// The currently accepted resource. Throws `std::out_of_range` for an empty reference.
    std::shared_ptr<const T> get() const {
        if (!state_)
            throw std::out_of_range("Empty asset reference");
        return state_->value;
    }
    /// Revision of the accepted resource: 1 after the first import, plus one for each refresh that
    /// publishes it, even when the new resource equals the old. Throws `std::out_of_range` for an
    /// empty reference.
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

/// Registry of keyed importers with explicit, all-or-nothing refresh.
///
/// The registry keeps a copy of every accepted input's bytes for comparison. Importers cannot
/// mutate their registry or refresh it recursively.
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
    /// Registry for the project directory @p root, reading files of at most @p maximum_file_bytes
    /// (positive; 64 MiB by default). Throws `std::invalid_argument` for a zero limit or a root
    /// that is not a directory, and `std::filesystem::filesystem_error` when @p root does not
    /// exist.
    explicit AssetImports(std::filesystem::path root, std::size_t maximum_file_bytes = 64 * 1024 * 1024);
    /// Registers @p importer under @p key, imports immediately and returns the reference at
    /// revision 1.
    ///
    /// @p importer is called as `std::shared_ptr<const T>(ImportSource &)` now and on every
    /// refresh that affects it. Throws `std::invalid_argument` for an empty key, a key longer than
    /// 4096 bytes, a key already registered or a null resource; `std::runtime_error` when an input
    /// changes before publication; and `std::logic_error` when called from an importer or when an
    /// input another importer tracks has changed since it was accepted (refresh() first). Failures
    /// from ImportSource::read and the importer propagate. Nothing is registered on failure.
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
    /// The reference registered under @p key. Throws `std::out_of_range` for an unknown key or a
    /// resource type other than exactly `T`.
    template <class T> AssetRef<T> get(std::string_view key) const {
        const auto found = entries_.find(key);
        if (found == entries_.end() || found->second->type() != typeid(T))
            throw std::out_of_range("Unknown asset key or resource type");
        return AssetRef<T>(static_cast<Entry<T> &>(*found->second).state);
    }
    /// Reimports every resource whose inputs changed and publishes them together, returning their
    /// keys in lexical order.
    ///
    /// Rereads every tracked input and compares its bytes with the accepted copy; timestamps and
    /// sizes are not used. Affected importers run against one shared snapshot of the inputs, which
    /// is verified again before publication. Inputs an importer newly reads become tracked, and
    /// inputs no importer reads any more are dropped. Old resources stay alive while held. If a
    /// read, importer or verification fails, it throws and keeps every previous resource, revision
    /// and dependency. Scene objects and GPU resources are not changed. Throws `std::logic_error`
    /// when called from an importer, and `std::overflow_error` when a revision would exceed
    /// `UINT64_MAX`.
    std::vector<std::string> refresh();
    /// Removes the importer under @p key and the inputs only it tracked; returns false for an
    /// unknown key. Held references keep their final version, and a later add() under the same key
    /// creates a new identity. Throws `std::logic_error` when called from an importer.
    bool erase(std::string_view key);
    /// Normalized project-relative paths that the accepted import of @p key read, in first-read
    /// order. Throws `std::out_of_range` for an unknown key.
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
