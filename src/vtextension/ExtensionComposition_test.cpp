// SPDX-License-Identifier: Apache-2.0
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <vtextension/ExtensionComposition.hpp>

using core::platform::testing::FileEntry;
using core::platform::testing::InMemoryFileSystem;
using vtextension::CommandPermission;
using vtextension::composeExtensions;
using vtextension::DeclaredExtension;
using vtextension::DeclaredExtensions;
using vtextension::extensionHostId;
using vtextension::ExtensionModuleLookup;
using vtextension::Placement;

namespace
{
[[nodiscard]] std::filesystem::path bundledDirectory()
{
    return { "/usr/share/contour/extensions" };
}

[[nodiscard]] std::filesystem::path userModule()
{
    return { "/home/user/ext/clock.wasm" };
}

/// @return @p text's bytes, as a module's.
[[nodiscard]] std::vector<std::byte> bytesOf(std::string_view text)
{
    auto bytes = std::vector<std::byte> {};
    for (auto const c: text)
        bytes.push_back(static_cast<std::byte>(c));
    return bytes;
}

[[nodiscard]] InMemoryFileSystem installed()
{
    return InMemoryFileSystem {
        FileEntry { .path = bundledDirectory() / "git.wasm", .content = "GIT" },
        FileEntry { .path = userModule(), .content = "CLOCK" },
    };
}

[[nodiscard]] DeclaredExtension bundled(std::string name)
{
    return DeclaredExtension {
        .name = std::move(name), .path = {}, .config = {}, .runCommand = CommandPermission::Ask
    };
}
} // namespace

TEST_CASE("ExtensionComposition: the host id is the config key and the module", "[vtextension][extensions]")
{
    // Spelled whole: `/` would join with the host's preferred separator, a backslash on Windows.
    CHECK(extensionHostId("git", std::filesystem::path { "/usr/share/contour/extensions/git.wasm" })
          == "git@/usr/share/contour/extensions/git.wasm");
}

TEST_CASE("ExtensionComposition: a profile's extensions, as a pane runs them", "[vtextension][extensions]")
{
    auto const fileSystem = installed();
    auto const directories = std::array { std::filesystem::path { "/nowhere" }, bundledDirectory() };
    auto const lookup = ExtensionModuleLookup { .fileSystem = fileSystem, .bundledDirectories = directories };

    auto declared = DeclaredExtensions {};
    declared.extensions.push_back(bundled("git"));
    declared.extensions.push_back(DeclaredExtension { .name = "clock",
                                                      .path = userModule(),
                                                      .config = { { "format", "%H:%M" } },
                                                      .runCommand = CommandPermission::Deny });

    SECTION("a bundled one is found by name, a configured one by its path")
    {
        auto const composed = composeExtensions(declared, lookup, {}, {});
        CHECK(composed.diagnostics.empty());
        REQUIRE(composed.host.extensions.size() == 2);
        auto const& git = composed.host.extensions[0];
        CHECK(git.name == "git");
        CHECK(git.hostId == "git@/usr/share/contour/extensions/git.wasm");
        CHECK(git.module == bytesOf("GIT"));
        CHECK(git.runCommand == CommandPermission::Ask);
        CHECK(git.placement == Placement::Segment);
        auto const& clock = composed.host.extensions[1];
        CHECK(clock.hostId == "clock@/home/user/ext/clock.wasm");
        CHECK(clock.module == bytesOf("CLOCK"));
        CHECK(clock.config.size() == 1);
        CHECK(clock.runCommand == CommandPermission::Deny);
    }

    SECTION("status_line.extension places that one as the take-over")
    {
        declared.takeOver = "clock";
        auto const composed = composeExtensions(declared, lookup, {}, {});
        CHECK(composed.diagnostics.empty());
        CHECK(composed.host.extensions[0].placement == Placement::Segment);
        CHECK(composed.host.extensions[1].placement == Placement::TakeOver);
    }

    SECTION("a take-over naming no declared extension is diagnosed, and nothing takes over")
    {
        declared.takeOver = "plasma";
        auto const composed = composeExtensions(declared, lookup, {}, {});
        REQUIRE(composed.diagnostics.size() == 1);
        CHECK(composed.diagnostics[0].contains("plasma"));
        for (auto const& spec: composed.host.extensions)
            CHECK(spec.placement == Placement::Segment);
    }

    SECTION("a module that is not found is passed on without bytes, and diagnosed")
    {
        declared.extensions.push_back(bundled("weather"));
        declared.extensions.push_back(DeclaredExtension { .name = "gone",
                                                          .path = "/home/user/ext/gone.wasm",
                                                          .config = {},
                                                          .runCommand = CommandPermission::Ask });
        auto const composed = composeExtensions(declared, lookup, {}, {});
        REQUIRE(composed.host.extensions.size() == 4);
        CHECK(composed.host.extensions[2].name == "weather");
        CHECK(composed.host.extensions[2].module.empty());
        CHECK(composed.host.extensions[3].module.empty());
        REQUIRE(composed.diagnostics.size() == 2);
        CHECK(composed.diagnostics[0].contains("weather"));
        CHECK(composed.diagnostics[1].contains("gone.wasm"));
    }
}
