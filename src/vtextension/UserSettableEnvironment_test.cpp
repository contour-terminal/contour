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
          "[vtextension][user]")
{
    for (auto const& row: vtextension::UserSettableVariables)
    {
        CAPTURE(row.name);
        CHECK(vtextension::userSettableRowOf(row.name) != nullptr);
    }
    CHECK(vtextension::userSettableRowOf("ANTHROPIC_API_KEY") == nullptr);
    for (auto const& row: vtextension::UserValueFaultTexts)
        CHECK_FALSE(vtextension::userValueFaultText(row.fault).empty());
    CHECK(vtextension::userValueFaultText(UserValueFault::NotUrl)
          == "is not an http, https or socks5 URL without credentials");
}

TEST_CASE("UserSettableEnvironment: a value over the length limit is refused", "[vtextension][user]")
{
    auto const longHosts = std::string(vtextension::MaxUserValueBytes + 1U, 'a');
    CHECK(vtextension::normalisedUserValue("NO_PROXY", longHosts, "", vtpty::PathStyle::Posix)
          == std::unexpected(UserValueFault::NotHostList));
    auto const longUrl = "https://" + std::string(vtextension::MaxUserValueBytes, 'a');
    CHECK(vtextension::normalisedUserValue("ANTHROPIC_BASE_URL", longUrl, "", vtpty::PathStyle::Posix)
          == std::unexpected(UserValueFault::NotAllowedUrl));
}

TEST_CASE(
    "UserSettableEnvironment: an absolute path is bounded, and a control byte is refused on every platform",
    "[vtextension][user]")
{
    using vtpty::PathStyle;
    auto const exact = "/" + std::string(vtextension::MaxUserValueBytes - 1U, 'a');
    auto const over = "/" + std::string(vtextension::MaxUserValueBytes, 'a');
    CHECK(vtextension::normalisedUserValue("CLAUDE_CONFIG_DIR", exact, "", PathStyle::Posix) == exact);
    CHECK(vtextension::normalisedUserValue("CLAUDE_CONFIG_DIR", over, "", PathStyle::Posix)
          == std::unexpected(UserValueFault::NotAbsolutePath));
    auto const windowsOver = "C:\\" + std::string(vtextension::MaxUserValueBytes - 2U, 'a');
    CHECK(vtextension::normalisedUserValue("COPILOT_HOME", windowsOver, "", PathStyle::Windows)
          == std::unexpected(UserValueFault::NotAbsolutePath));

    auto const controls =
        std::array<std::string, 5> { "/x\ny", "/x\ty", std::string("/x\0y", 4), "/x\x7f", "/x\r" };
    for (auto const& value: controls)
    {
        CAPTURE(value);
        CHECK(vtextension::normalisedUserValue("CLAUDE_CONFIG_DIR", value, "", PathStyle::Posix)
              == std::unexpected(UserValueFault::NotAbsolutePath));
    }
    CHECK(vtextension::normalisedUserValue("COPILOT_HOME", "C:\\x\r", "", PathStyle::Windows)
          == std::unexpected(UserValueFault::NotAbsolutePath));
}

TEST_CASE("UserSettableEnvironment: URL shapes are judged at their edges", "[vtextension][user]")
{
    using vtpty::PathStyle;
    struct Row
    {
        std::string_view name, value;
        std::expected<std::string, UserValueFault> expected;
    };
    auto const rows = std::array {
        Row {
            "ANTHROPIC_BASE_URL", "HTTPS://llm.example.com", std::unexpected(UserValueFault::NotAllowedUrl) },
        Row { "ANTHROPIC_BASE_URL", "https://llm.example.com/", "https://llm.example.com/" },
        Row { "ANTHROPIC_BASE_URL", "https://[::1]:443", "https://[::1]:443" },
        Row { "ANTHROPIC_BASE_URL", "http://localhost", "http://localhost" },
        Row { "ANTHROPIC_BASE_URL", "http://localhost:8080/", "http://localhost:8080/" },
        Row { "ANTHROPIC_BASE_URL",
              "http://localhost.example.com",
              std::unexpected(UserValueFault::NotAllowedUrl) },
        Row { "ANTHROPIC_BASE_URL", "http://127.0.0.1:", std::unexpected(UserValueFault::NotAllowedUrl) },
        Row { "ANTHROPIC_BASE_URL", "http://[::1", std::unexpected(UserValueFault::NotAllowedUrl) },
        Row { "ANTHROPIC_BASE_URL", "https://[]", std::unexpected(UserValueFault::NotAllowedUrl) },
        Row { "ANTHROPIC_BASE_URL", "https://a\x01", std::unexpected(UserValueFault::NotAllowedUrl) },
        Row { "ANTHROPIC_BASE_URL", "https://llm example", std::unexpected(UserValueFault::NotAllowedUrl) },
        Row { "ANTHROPIC_BASE_URL", "ftp://llm.example.com", std::unexpected(UserValueFault::NotAllowedUrl) },
        Row { "HTTPS_PROXY", "socks5h://proxy.example.com:1080", "socks5h://proxy.example.com:1080" },
        Row { "HTTP_PROXY", "http://localhost", "http://localhost" },
        Row { "HTTPS_PROXY", "ftp://proxy.example.com", std::unexpected(UserValueFault::NotUrl) },
        Row { "HTTPS_PROXY", "HTTP://proxy.example.com", std::unexpected(UserValueFault::NotUrl) },
        Row { "HTTPS_PROXY", "https://[]", std::unexpected(UserValueFault::NotUrl) },
        Row { "HTTPS_PROXY", "http://a\x7f", std::unexpected(UserValueFault::NotUrl) },
    };
    for (auto const& row: rows)
    {
        CAPTURE(row.name, row.value);
        CHECK(vtextension::normalisedUserValue(row.name, row.value, "", PathStyle::Posix) == row.expected);
    }
}

TEST_CASE("UserSettableEnvironment: a URL is bounded at exactly MaxUserValueBytes", "[vtextension][user]")
{
    using vtpty::PathStyle;
    auto const limit = vtextension::MaxUserValueBytes;
    auto const exact = "https://" + std::string(limit - 8U, 'a');
    auto const over = "https://" + std::string(limit - 7U, 'a');
    CHECK(vtextension::normalisedUserValue("ANTHROPIC_BASE_URL", exact, "", PathStyle::Posix) == exact);
    CHECK(vtextension::normalisedUserValue("ANTHROPIC_BASE_URL", over, "", PathStyle::Posix)
          == std::unexpected(UserValueFault::NotAllowedUrl));

    auto const proxyExact = "http://" + std::string(limit - 7U, 'a');
    auto const proxyOver = "http://" + std::string(limit - 6U, 'a');
    CHECK(vtextension::normalisedUserValue("HTTPS_PROXY", proxyExact, "", PathStyle::Posix) == proxyExact);
    CHECK(vtextension::normalisedUserValue("HTTPS_PROXY", proxyOver, "", PathStyle::Posix)
          == std::unexpected(UserValueFault::NotUrl));
}

TEST_CASE("UserSettableEnvironment: a host list is trimmed, and refused when empty or malformed",
          "[vtextension][user]")
{
    using vtpty::PathStyle;
    struct Row
    {
        std::string_view value;
        std::expected<std::string, UserValueFault> expected;
    };
    auto const rows = std::array {
        Row { "", std::unexpected(UserValueFault::NotHostList) },
        Row { "a,", std::unexpected(UserValueFault::NotHostList) },
        Row { " ", std::unexpected(UserValueFault::NotHostList) },
        Row { " a , b ", "a,b" },
        Row { "*.example.com", std::unexpected(UserValueFault::NotHostList) },
        Row { "a b", std::unexpected(UserValueFault::NotHostList) },
        Row { "[::1],10.0.0.0/8", "[::1],10.0.0.0/8" },
    };
    for (auto const& row: rows)
    {
        CAPTURE(row.value);
        CHECK(vtextension::normalisedUserValue("NO_PROXY", row.value, "", PathStyle::Posix) == row.expected);
    }
    auto const exact = std::string(vtextension::MaxUserValueBytes, 'a');
    auto const over = std::string(vtextension::MaxUserValueBytes + 1U, 'a');
    CHECK(vtextension::normalisedUserValue("NO_PROXY", exact, "", PathStyle::Posix) == exact);
    CHECK(vtextension::normalisedUserValue("NO_PROXY", over, "", PathStyle::Posix)
          == std::unexpected(UserValueFault::NotHostList));
}
