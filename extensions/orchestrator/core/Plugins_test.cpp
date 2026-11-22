// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <orchestrator/core/Fixtures.hpp>
#include <orchestrator/core/Plugins.hpp>

using namespace orchestrator;

TEST_CASE("Plugins: what claude plugin list --json says is enabled", "[orchestrator][plugins]")
{
    auto const listed = enabledPluginsOf(readFixture(FixtureRoot::Testdata, "claude/plugin-list.json"));
    REQUIRE(listed.has_value());
    CHECK(*listed
          == std::vector<std::string> { "clangd-lsp@claude-plugins-official",
                                        "contour-workflows@contour-terminal",
                                        "superpowers@superpowers-marketplace" });
    CHECK(missingPlugins(defaultPluginRequirements(), *listed).empty());

    // Installed but disabled is as good as missing: Claude Code does not load it.
    auto const disabled =
        enabledPluginsOf(readFixture(FixtureRoot::Testdata, "claude/plugin-list-disabled.json"));
    REQUIRE(disabled.has_value());
    CHECK(missingPlugins(defaultPluginRequirements(), *disabled) == defaultPluginRequirements());

    auto const without =
        enabledPluginsOf(readFixture(FixtureRoot::Testdata, "claude/plugin-list-without.json"));
    REQUIRE(without.has_value());
    CHECK(missingPlugins(defaultPluginRequirements(), *without) == defaultPluginRequirements());

    CHECK(enabledPluginsOf("[]") == std::vector<std::string> {});
    CHECK_FALSE(enabledPluginsOf("not json").has_value());
    CHECK_FALSE(enabledPluginsOf(R"({"id":"x"})").has_value());
}
