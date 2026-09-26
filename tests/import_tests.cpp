#include <anima/assets/imports.hpp>
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace anima;
namespace {
struct Project {
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("anima-import-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Project() { std::filesystem::create_directory(root); }
    Project(const Project &) = delete;
    Project &operator=(const Project &) = delete;
    ~Project() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    void write(const char *file, std::string_view value) const {
        std::ofstream(root / file, std::ios::binary) << value;
    }
};
std::string text(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}
// Registers "first", which returns the shared input.
AssetRef<std::string> add_first(AssetImports &assets) {
    return assets.add<std::string>(
        "first", [](ImportSource &source) { return std::make_shared<const std::string>(text(source.read("shared"))); });
}
// Registers "second", which appends "-second" to the shared input and fails unless the side input is valid.
AssetRef<std::string> add_second(AssetImports &assets) {
    return assets.add<std::string>("second", [](ImportSource &source) {
        const auto shared = text(source.read("shared"));
        if (text(source.read("side")) != "valid")
            throw std::runtime_error("Invalid authored data");
        return std::make_shared<const std::string>(shared + "-second");
    });
}
} // namespace

TEST_CASE("A refresh reimports changed content and publishes all or nothing") {
    const Project project;
    project.write("shared", "one");
    project.write("side", "valid");
    AssetImports assets(project.root);
    int imports = 0;
    auto first = assets.add<std::string>("first", [&](ImportSource &source) {
        ++imports;
        return std::make_shared<const std::string>(text(source.read("shared")));
    });
    const auto second = add_second(assets);
    CHECK(assets.refresh().empty());
    CHECK(imports == 1); // An unchanged input is not reimported.
    const auto before = first.get();
    // An edit of equal size under the old timestamp still changes the content.
    const auto timestamp = std::filesystem::last_write_time(project.root / "shared");
    project.write("shared", "two");
    std::filesystem::last_write_time(project.root / "shared", timestamp);
    project.write("side", "broken");
    CHECK_THROWS_WITH_AS(assets.refresh(), "Invalid authored data", std::runtime_error);
    // The failed importer published nothing, not even the other importer's new version.
    CHECK(first.revision() == 1);
    CHECK(second.revision() == 1);
    CHECK(*first.get() == "one");
    project.write("side", "valid");
    CHECK(assets.refresh() == std::vector<std::string>{"first", "second"});
    CHECK(first.revision() == 2);
    CHECK(*first.get() == "two");
    CHECK(*second.get() == "two-second");
    CHECK(*before == "one"); // A held version is immutable.
}

TEST_CASE("Invalid registrations and reads are rejected without changing accepted resources") {
    const Project project;
    project.write("shared", "one");
    project.write("side", "valid");
    AssetImports assets(project.root);
    const auto first = add_first(assets);
    add_second(assets);
    CHECK_THROWS_WITH_AS(assets.get<int>("first"), "Unknown asset key or resource type", std::out_of_range);
    const auto constant = [](ImportSource &) { return std::make_shared<const int>(1); };
    CHECK_THROWS_WITH_AS(assets.add<int>("first", constant), "Invalid or duplicate asset key", std::invalid_argument);
    const auto escape = [](ImportSource &source) {
        (void)source.read("../outside");
        return std::make_shared<const int>(1);
    };
    CHECK_THROWS_WITH_AS(assets.add<int>("escape", escape), "Import dependency escapes the project root",
                         std::invalid_argument);
    const auto reentry = [&](ImportSource &) {
        assets.erase("first");
        return std::make_shared<const int>(1);
    };
    CHECK_THROWS_WITH_AS(assets.add<int>("reentry", reentry), "Import callbacks cannot mutate their registry",
                         std::logic_error);
    const auto race = [&](ImportSource &source) {
        auto value = text(source.read("shared"));
        project.write("shared", "new");
        return std::make_shared<const std::string>(value);
    };
    CHECK_THROWS_WITH_AS(assets.add<std::string>("race", race), "Import input changed before publication: shared",
                         std::runtime_error);
    // The accepted identities are unchanged, and the next refresh takes the race's edit like any other.
    CHECK(first.revision() == 1);
    CHECK(assets.refresh().size() == 2);
}

TEST_CASE("A missing input fails the refresh and keeps the last accepted version") {
    const Project project;
    project.write("shared", "one");
    AssetImports assets(project.root);
    const auto first = add_first(assets);
    const auto missing = std::filesystem::canonical(project.root) / "shared";
    std::filesystem::remove(missing);
    CHECK_THROWS_WITH_AS(assets.refresh(), ("Cannot open import dependency: " + missing.string()).c_str(),
                         std::runtime_error);
    CHECK(*first.get() == "one");
}

TEST_CASE("A reimport tracks the inputs its importer reads") {
    const Project project;
    project.write("selector", "a");
    project.write("a", "A");
    project.write("b", "B");
    AssetImports assets(project.root);
    const auto indirect = assets.add<std::string>("indirect", [](ImportSource &source) {
        return std::make_shared<const std::string>(text(source.read(text(source.read("selector")))));
    });
    project.write("selector", "b");
    CHECK(assets.refresh() == std::vector<std::string>{"indirect"});
    CHECK(*indirect.get() == "B");
    std::filesystem::remove(project.root / "a");
    CHECK(assets.refresh().empty()); // The input it no longer reads is not required.
}

TEST_CASE("Erasing an importer keeps its held version, and adding its key again creates a new identity") {
    const Project project;
    project.write("shared", "one");
    project.write("side", "valid");
    AssetImports assets(project.root);
    const auto first = add_first(assets);
    const auto second = add_second(assets);
    CHECK(assets.erase("first"));
    CHECK(first.get());
    CHECK_FALSE(assets.erase("first"));
    project.write("shared", "end");
    assets.refresh();
    CHECK(*first.get() == "one"); // The erased identity is neither refreshed nor revived.
    CHECK(*second.get() == "end-second");
    const auto replacement = add_first(assets);
    CHECK(replacement.revision() == 1);
    CHECK(*first.get() == "one");
}
