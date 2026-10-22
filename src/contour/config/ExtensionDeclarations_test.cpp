// SPDX-License-Identifier: Apache-2.0
#include <contour/config/ExtensionDeclarations.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <utility>

using contour::config::declaredExtensionsOf;
using contour::config::ExtensionConfig;
using contour::config::Permission;
using contour::config::TerminalProfile;
using contour::config::toCommandPermission;
using vtextension::CommandPermission;
using vtextension::DeclaredExtension;

TEST_CASE("ExtensionDeclarations: run_command as the runtime takes it", "[contour][config][extensions]")
{
    struct Row
    {
        Permission configured;
        CommandPermission expected;
    };
    constexpr auto Rows = std::array {
        Row { .configured = Permission::Deny, .expected = CommandPermission::Deny },
        Row { .configured = Permission::Allow, .expected = CommandPermission::Allow },
        Row { .configured = Permission::Ask, .expected = CommandPermission::Ask },
    };
    for (auto const& row: Rows)
        CHECK(toCommandPermission(row.configured) == row.expected);
}

TEST_CASE("ExtensionDeclarations: a profile's extensions, in no configuration type",
          "[contour][config][extensions]")
{
    auto profile = TerminalProfile {};
    profile.extensions.value() = {
        ExtensionConfig { .name = "git", .path = {}, .config = {}, .runCommand = Permission::Ask },
        ExtensionConfig { .name = "clock",
                          .path = "/home/user/ext/clock.wasm",
                          .config = { { "format", "%H:%M" }, { "zone", "UTC" } },
                          .runCommand = Permission::Deny },
    };
    profile.statusLine.value().extension = "clock";

    auto const declared = declaredExtensionsOf(profile);

    CHECK(declared.takeOver == "clock");
    REQUIRE(declared.extensions.size() == 2);
    CHECK(declared.extensions[0]
          == DeclaredExtension {
              .name = "git", .path = {}, .config = {}, .runCommand = CommandPermission::Ask });
    CHECK(declared.extensions[1]
          == DeclaredExtension { .name = "clock",
                                 .path = "/home/user/ext/clock.wasm",
                                 .config = { { "format", "%H:%M" }, { "zone", "UTC" } },
                                 .runCommand = CommandPermission::Deny });
}
