// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/ChildEnvironment.hpp>
#include <vtextension/Manifest.hpp>

using vtextension::EnvironmentEntry;

namespace
{

using Environment = std::vector<std::pair<std::string, std::string>>;

[[nodiscard]] bool has(Environment const& environment, std::string_view name)
{
    return std::ranges::contains(
        environment, name, [](auto const& entry) -> std::string_view { return entry.first; });
}

} // namespace

TEST_CASE("ChildEnvironment: inheritance rows", "[daemon][processes]")
{
    auto const inherited = std::vector<std::string> {
        "GIT_DIR=/elsewhere/.git",
        "GIT_CONFIG_PARAMETERS='core.pager'='evil'",
        "GH_REPO=someone/else",
        "GH_HOST=example.com",
        "GH_ENTERPRISE_TOKEN=t",
        "CONTOUR_PANE=1",
        "GIT_SSH_COMMAND=ssh -i key",
        "GIT_SSH=ssh",
        "GIT_ASKPASS=/usr/bin/askpass",
        "GH_TOKEN=token",
        "PATH=/usr/bin",
        "MYGIT_DIR=kept",
        "NO_EQUALS_SIGN",
    };
    auto const environment = vtextension::childEnvironment(inherited, {}, {});

    for (auto const name: std::array<std::string_view, 7> { "GIT_DIR",
                                                            "GIT_CONFIG_PARAMETERS",
                                                            "GH_REPO",
                                                            "GH_HOST",
                                                            "GH_ENTERPRISE_TOKEN",
                                                            "CONTOUR_PANE",
                                                            "NO_EQUALS_SIGN" })
    {
        INFO(name);
        CHECK_FALSE(has(environment, name));
    }
    for (auto const name: std::array<std::string_view, 6> {
             "GIT_SSH_COMMAND", "GIT_SSH", "GIT_ASKPASS", "GH_TOKEN", "PATH", "MYGIT_DIR" })
    {
        INFO(name);
        CHECK(has(environment, name));
    }
    CHECK(std::ranges::contains(environment,
                                std::pair<std::string, std::string> { "GIT_SSH_COMMAND", "ssh -i key" }));
}

TEST_CASE("ChildEnvironment: a later layer replaces a name in place", "[daemon][processes]")
{
    auto const inherited = std::vector<std::string> { "PATH=/usr/bin", "LC_ALL=de_DE.UTF-8", "HOME=/home/u" };
    auto const commandEnvironment = std::vector<EnvironmentEntry> {
        { .name = "GIT_CONFIG_COUNT", .value = "2" },
        { .name = "LC_ALL", .value = "en_US.UTF-8" },
    };
    auto const perCommand = std::vector<EnvironmentEntry> {
        { .name = "LC_ALL", .value = "C" },
        { .name = "GIT_CONFIG_COUNT", .value = "3" },
        { .name = "GIT_OPTIONAL_LOCKS", .value = "0" },
    };
    CHECK(vtextension::childEnvironment(inherited, commandEnvironment, perCommand)
          == Environment {
              { "PATH", "/usr/bin" },
              { "LC_ALL", "C" },
              { "HOME", "/home/u" },
              { "GIT_CONFIG_COUNT", "3" },
              { "GIT_OPTIONAL_LOCKS", "0" },
          });
}
