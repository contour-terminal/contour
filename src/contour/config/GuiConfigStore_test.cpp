// SPDX-License-Identifier: Apache-2.0
#include <contour/config/GuiConfigStore.hpp>

#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>
#include <string>

#include <vthost/testing/FaultyFileSystem.hpp>

using contour::config::colorSchemeFile;
using contour::config::FileGuiConfigStore;
using contour::config::guiSettingsFile;
using contour::config::profileFile;
using contour::config::relativePathOf;
using contour::config::SideFileKind;
using vthost::testing::FaultOp;
using vthost::testing::FaultyFileSystem;

namespace
{
/// Where the stores of these tests keep contour.yml's siblings.
[[nodiscard]] std::filesystem::path configDir()
{
    return std::filesystem::path { "/config" };
}

/// The entries of @p directory in @p fileSystem; none when it cannot be listed.
[[nodiscard]] std::size_t entriesIn(core::platform::FileSystem const& fileSystem,
                                    std::filesystem::path const& directory)
{
    auto const listed = fileSystem.listDirectory(directory);
    return listed ? listed->size() : 0;
}
} // namespace

TEST_CASE("GuiConfigStore: each kind of side file has its path", "[guiconfigstore]")
{
    CHECK(relativePathOf(guiSettingsFile()) == std::filesystem::path { "settings.yml" });
    CHECK(relativePathOf(profileFile("main")) == std::filesystem::path { "profiles" } / "main.yml");
    CHECK(relativePathOf(colorSchemeFile("Contour Dark"))
          == std::filesystem::path { "colorschemes" } / "Contour Dark.yml");
}

TEST_CASE("FileGuiConfigStore: a write creates profiles/ and reads back", "[guiconfigstore]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    auto store = FileGuiConfigStore { fileSystem, configDir() };

    REQUIRE(store.write(profileFile("main"), "a: 1\n").has_value());

    auto const read = store.read(profileFile("main"));
    REQUIRE(read.has_value());
    CHECK(*read == std::optional<std::string> { "a: 1\n" });
    CHECK(fileSystem.isDirectory(configDir() / "profiles"));

    auto const absent = store.read(profileFile("absent"));
    REQUIRE(absent.has_value());
    CHECK(*absent == std::optional<std::string> {});
}

TEST_CASE("FileGuiConfigStore: a refused replace keeps the old file and leaves no temporary",
          "[guiconfigstore]")
{
    auto const target = configDir() / "profiles" / "main.yml";
    auto const fileSystem = FaultyFileSystem { [&](FaultOp op, std::filesystem::path const& path) {
        return op == FaultOp::Rename && path == target;
    } };
    REQUIRE(fileSystem.files.writeFile(target, "old").has_value());
    auto store = FileGuiConfigStore { fileSystem, configDir() };

    auto const written = store.write(profileFile("main"), "new");

    REQUIRE_FALSE(written.has_value());
    CHECK(written.error().contains("Permission denied"));
    CHECK(fileSystem.readFile(target) == std::expected<std::string, std::string> { "old" });
    CHECK(entriesIn(fileSystem, configDir() / "profiles") == 1);
}

TEST_CASE("FileGuiConfigStore: a failed write reports it and leaves no temporary", "[guiconfigstore]")
{
    auto const target = configDir() / "profiles" / "main.yml";
    auto armed = false;
    auto const fileSystem = FaultyFileSystem { [&](FaultOp op, std::filesystem::path const& /*path*/) {
        return armed && op == FaultOp::WriteFile;
    } };
    REQUIRE(fileSystem.files.writeFile(target, "old").has_value());
    auto store = FileGuiConfigStore { fileSystem, configDir() };

    armed = true;
    auto const written = store.write(profileFile("main"), "new");

    REQUIRE_FALSE(written.has_value());
    CHECK(written.error().contains("No space left on device"));
    CHECK(fileSystem.readFile(target) == std::expected<std::string, std::string> { "old" });
    CHECK(entriesIn(fileSystem, configDir() / "profiles") == 1);
}

TEST_CASE("FileGuiConfigStore: a stale temporary from an earlier crash does not block a write",
          "[guiconfigstore]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    REQUIRE(fileSystem.writeFile(configDir() / "profiles" / ".main.yml.tmp", "half").has_value());
    auto store = FileGuiConfigStore { fileSystem, configDir() };

    REQUIRE(store.write(profileFile("main"), "whole").has_value());

    CHECK(store.read(profileFile("main"))
          == std::expected<std::optional<std::string>, std::string> { "whole" });
    CHECK(entriesIn(fileSystem, configDir() / "profiles") == 1);
}

TEST_CASE("FileGuiConfigStore: remove", "[guiconfigstore]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    auto store = FileGuiConfigStore { fileSystem, configDir() };

    SECTION("an absent file is success")
    {
        CHECK(store.remove(colorSchemeFile("absent")).has_value());
    }

    SECTION("a present file is gone")
    {
        REQUIRE(store.write(colorSchemeFile("midnight"), "x").has_value());
        REQUIRE(store.remove(colorSchemeFile("midnight")).has_value());
        auto const read = store.read(colorSchemeFile("midnight"));
        REQUIRE(read.has_value());
        CHECK_FALSE(read->has_value());
    }
}

TEST_CASE("FileGuiConfigStore: list names the side files of one kind", "[guiconfigstore]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    auto store = FileGuiConfigStore { fileSystem, configDir() };

    auto const none = store.list(SideFileKind::ColorScheme);
    REQUIRE(none.has_value());
    CHECK(none->empty());

    REQUIRE(store.write(colorSchemeFile("Contour Dark"), "x").has_value());
    REQUIRE(store.write(colorSchemeFile("midnight"), "y").has_value());
    REQUIRE(store.write(profileFile("main"), "z").has_value());
    REQUIRE(fileSystem.writeFile(configDir() / "colorschemes" / "notes.txt", "not a scheme").has_value());

    auto const schemes = store.list(SideFileKind::ColorScheme);
    REQUIRE(schemes.has_value());
    CHECK(*schemes == std::vector { colorSchemeFile("Contour Dark"), colorSchemeFile("midnight") });

    auto const settings = store.list(SideFileKind::GuiSettings);
    REQUIRE(settings.has_value());
    CHECK(settings->empty());
    REQUIRE(store.write(guiSettingsFile(), "default_profile: main\n").has_value());
    CHECK(store.list(SideFileKind::GuiSettings)
          == std::expected<std::vector<contour::config::SideFile>, std::string> {
              std::vector { guiSettingsFile() } });
}
