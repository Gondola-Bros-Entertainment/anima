#include <anima/assets/imports.hpp>
#include <chrono>
#include <fstream>
#include <iostream>

namespace {
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class F> void rejects(F f) {
    try {
        f();
    } catch (const std::exception &) {
        return;
    }
    throw std::runtime_error("Invalid import was accepted");
}
struct Project {
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("anima-import-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Project() { std::filesystem::create_directory(root); }
    ~Project() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    void write(const char *file, std::string_view value) { std::ofstream(root / file, std::ios::binary) << value; }
};
std::string text(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}
} // namespace
int main() {
    try {
        using namespace anima;
        Project project;
        project.write("shared", "one");
        project.write("side", "valid");
        AssetImports assets(project.root);
        int imports = 0;
        auto first = assets.add<std::string>("first", [&](ImportSource &source) {
            ++imports;
            return std::make_shared<const std::string>(text(source.read("shared")));
        });
        auto second = assets.add<std::string>("second", [](ImportSource &source) {
            const auto main = text(source.read("shared"));
            if (text(source.read("side")) != "valid")
                throw std::runtime_error("Invalid authored data");
            return std::make_shared<const std::string>(main + "-second");
        });
        check(assets.refresh().empty() && imports == 1, "Unchanged source was reimported");
        const auto before = first.get();
        const auto timestamp = std::filesystem::last_write_time(project.root / "shared");
        project.write("shared", "two");
        std::filesystem::last_write_time(project.root / "shared", timestamp);
        project.write("side", "broken");
        rejects([&] { assets.refresh(); });
        check(first.revision() == 1 && second.revision() == 1 && *first.get() == "one",
              "Failed dependent importer published a partial transaction");
        project.write("side", "valid");
        check(assets.refresh() == std::vector<std::string>{"first", "second"} && first.revision() == 2 &&
                  *first.get() == "two" && *second.get() == "two-second" && *before == "one",
              "Shared dependency refresh lost immutable snapshots or missed equal-size/timestamp edits");
        rejects([&] { (void)assets.get<int>("first"); });
        rejects([&] { assets.add<int>("first", [](ImportSource &) { return std::make_shared<const int>(1); }); });
        rejects([&] {
            assets.add<int>("escape", [](ImportSource &source) {
                (void)source.read("../outside");
                return std::make_shared<const int>(1);
            });
        });
        rejects([&] {
            assets.add<int>("reentry", [&](ImportSource &) {
                assets.erase("first");
                return std::make_shared<const int>(1);
            });
        });
        rejects([&] {
            assets.add<std::string>("race", [&](ImportSource &source) {
                auto value = text(source.read("shared"));
                project.write("shared", "new");
                return std::make_shared<const std::string>(value);
            });
        });
        check(first.revision() == 2 && assets.refresh().size() == 2, "Rejected import changed accepted identities");
        std::filesystem::remove(project.root / "shared");
        rejects([&] { assets.refresh(); });
        check(*first.get() == "new", "Missing file discarded the last accepted version");
        project.write("shared", "new");
        project.write("selector", "a");
        project.write("a", "A");
        project.write("b", "B");
        auto indirect = assets.add<std::string>("indirect", [](ImportSource &source) {
            return std::make_shared<const std::string>(text(source.read(text(source.read("selector")))));
        });
        project.write("selector", "b");
        check(assets.refresh() == std::vector<std::string>{"indirect"} && *indirect.get() == "B",
              "Reimport did not discover its new dependencies");
        std::filesystem::remove(project.root / "a");
        check(assets.refresh().empty(), "Obsolete dependency remained required");
        check(assets.erase("first") && first.get() && !assets.erase("first"), "Eviction broke held snapshots");
        project.write("shared", "end");
        assets.refresh();
        check(*first.get() == "new" && *second.get() == "end-second", "Evicted identity was refreshed or revived");
        const auto replacement = assets.add<std::string>("first", [](ImportSource &source) {
            return std::make_shared<const std::string>(text(source.read("shared")));
        });
        check(replacement.revision() == 1 && *first.get() == "new", "Re-added key revived an old resource identity");
        std::cout << "PASS content/dependency reimport, transactional failures, immutable versions and eviction\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
