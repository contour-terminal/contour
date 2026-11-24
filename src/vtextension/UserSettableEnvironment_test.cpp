// SPDX-License-Identifier: Apache-2.0
#include <vtpty/PathStyle.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include <vtextension/UserSettableEnvironment.hpp>

using vtextension::CheckedEnvironmentSets;
using vtextension::EnvironmentSet;
using vtextension::UserValueFault;

TEST_CASE("UserSettableEnvironment: values are normalised or refused by their row's shape",
          "[vtextension][settable][user]")
{
    using vtpty::PathStyle;
    struct Row
    {
        std::string_view name, value, home;
        PathStyle style;
        std::expected<std::string, UserValueFault> expected;
    };
    auto const rows = std::array {
        Row { "CLAUDE_CONFIG_DIR", "~/.claude-work", "/home/u", PathStyle::Posix, "/home/u/.claude-work" },
        Row { "CLAUDE_CONFIG_DIR",
              "C:/Users/me/.claude-work",
              "C:\\Users\\me",
              PathStyle::Windows,
              "C:\\Users\\me\\.claude-work" },
        Row { "COPILOT_HOME",
              "~\\.copilot-work",
              "C:\\Users\\me",
              PathStyle::Windows,
              "C:\\Users\\me\\.copilot-work" },
        Row { "CLAUDE_CONFIG_DIR",
              ".claude-work",
              "/home/u",
              PathStyle::Posix,
              std::unexpected(UserValueFault::NotAbsolutePath) },
        Row { "CLAUDE_CONFIG_DIR",
              "~/.claude-work",
              "",
              PathStyle::Posix,
              std::unexpected(UserValueFault::NotAbsolutePath) },
        Row { "ANTHROPIC_BASE_URL",
              "https://llm.example.com/v1",
              "",
              PathStyle::Posix,
              "https://llm.example.com/v1" },
        Row { "ANTHROPIC_BASE_URL", "http://localhost:8080", "", PathStyle::Posix, "http://localhost:8080" },
        Row { "ANTHROPIC_BASE_URL",
              "http://127.0.0.1:4000/x",
              "",
              PathStyle::Posix,
              "http://127.0.0.1:4000/x" },
        Row { "ANTHROPIC_BASE_URL", "http://[::1]:4000", "", PathStyle::Posix, "http://[::1]:4000" },
        Row { "ANTHROPIC_BASE_URL",
              "http://llm.example.com",
              "",
              PathStyle::Posix,
              std::unexpected(UserValueFault::NotAllowedUrl) },
        Row { "ANTHROPIC_BASE_URL",
              "https://me:pw@llm.example.com",
              "",
              PathStyle::Posix,
              std::unexpected(UserValueFault::NotAllowedUrl) },
        Row { "ANTHROPIC_BASE_URL",
              "https://",
              "",
              PathStyle::Posix,
              std::unexpected(UserValueFault::NotAllowedUrl) },
        Row { "HTTPS_PROXY",
              "http://proxy.example.com:3128",
              "",
              PathStyle::Posix,
              "http://proxy.example.com:3128" },
        Row { "HTTP_PROXY", "socks5://127.0.0.1:1080", "", PathStyle::Posix, "socks5://127.0.0.1:1080" },
        Row { "HTTPS_PROXY",
              "proxy.example.com:3128",
              "",
              PathStyle::Posix,
              std::unexpected(UserValueFault::NotUrl) },
        Row { "HTTPS_PROXY",
              "http://u:p@proxy.example.com",
              "",
              PathStyle::Posix,
              std::unexpected(UserValueFault::NotUrl) },
        Row { "HTTPS_PROXY",
              "http://proxy example",
              "",
              PathStyle::Posix,
              std::unexpected(UserValueFault::NotUrl) },
        Row { "NO_PROXY",
              "localhost, .example.com,10.0.0.0/8",
              "",
              PathStyle::Posix,
              "localhost,.example.com,10.0.0.0/8" },
        Row { "NO_PROXY", "*", "", PathStyle::Posix, "*" },
        Row { "NO_PROXY", "a,,b", "", PathStyle::Posix, std::unexpected(UserValueFault::NotHostList) },
        Row { "NO_PROXY", "a;b", "", PathStyle::Posix, std::unexpected(UserValueFault::NotHostList) },
        Row { "PATH", "/usr/bin", "", PathStyle::Posix, std::unexpected(UserValueFault::NotSettable) },
        Row { "claude_config_dir", "/x", "", PathStyle::Posix, std::unexpected(UserValueFault::NotSettable) },
    };
    for (auto const& row: rows)
    {
        CAPTURE(row.name, row.value);
        CHECK(vtextension::normalisedUserValue(row.name, row.value, row.home, row.style) == row.expected);
    }
    CHECK(vtextension::isNormalisedUserValue("NO_PROXY", "a,b", vtpty::PathStyle::Posix));
    CHECK_FALSE(vtextension::isNormalisedUserValue("NO_PROXY", "a, b", vtpty::PathStyle::Posix));
    CHECK_FALSE(vtextension::isNormalisedUserValue("CLAUDE_CONFIG_DIR", "~/x", vtpty::PathStyle::Posix));
}

TEST_CASE("UserSettableEnvironment: a set keeps what may be set and reports the rest",
          "[vtextension][settable][user]")
{
    auto const sets = std::vector {
        EnvironmentSet { .name = "work",
                         .variables = { { "CLAUDE_CONFIG_DIR", "~/.claude-work" }, { "PATH", "/bin" } } },
        EnvironmentSet { .name = "empty", .variables = { { "HTTPS_PROXY", "nope" } } },
    };
    auto const checked = vtextension::checkEnvironmentSets(sets, "/home/u", vtpty::PathStyle::Posix);
    REQUIRE(checked.sets.size() == 2);
    CHECK(checked.sets[0]
          == EnvironmentSet { .name = "work",
                              .variables = { { "CLAUDE_CONFIG_DIR", "/home/u/.claude-work" } } });
    CHECK(checked.sets[1].name == "empty");
    CHECK(checked.sets[1].variables.empty());
    REQUIRE(checked.problems.size() == 2);
    CHECK(checked.problems[0].set == "work");
    CHECK(checked.problems[0].variable == "PATH");
    CHECK(checked.problems[0].fault == UserValueFault::NotSettable);
    CHECK(checked.problems[1].fault == UserValueFault::NotUrl);
}

TEST_CASE("UserSettableEnvironment: every user-settable row is named and has fault text",
          "[vtextension][settable][user]")
{
    for (auto const& row: vtextension::UserSettableVariables)
    {
        CAPTURE(row.name);
        CHECK(vtextension::userSettableRowOf(row.name) != nullptr);
    }
    CHECK(vtextension::userSettableRowOf("ANTHROPIC_API_KEY") == nullptr);
    for (auto const& row: vtextension::UserValueFaultTexts)
        CHECK_FALSE(vtextension::userValueFaultText(row.fault).empty());
}

TEST_CASE("UserSettableEnvironment: a value over the length limit is refused",
          "[vtextension][settable][user]")
{
    auto const longHosts = std::string(vtextension::MaxUserValueBytes + 1U, 'a');
    CHECK(vtextension::normalisedUserValue("NO_PROXY", longHosts, "", vtpty::PathStyle::Posix)
          == std::unexpected(UserValueFault::NotHostList));
    auto const longUrl = "https://" + std::string(vtextension::MaxUserValueBytes, 'a');
    CHECK(vtextension::normalisedUserValue("ANTHROPIC_BASE_URL", longUrl, "", vtpty::PathStyle::Posix)
          == std::unexpected(UserValueFault::NotAllowedUrl));
}
