// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/Commands.hpp>
#include <orchestrator/proto/Environment.hpp>

using namespace std::string_view_literals;
using namespace orchestrator;
using namespace orchestrator::proto;

TEST_CASE("Environment: what the driver's children never inherit", "[orchestrator][proto]")
{
    struct Row
    {
        std::string_view entry;
        bool withheld;
    };
    static constexpr auto Rows = std::array {
        Row { .entry = "CONTOUR_PANE=pane:1", .withheld = true },
        Row { .entry = "CONTOUR_DAEMON=/run/x", .withheld = true },
        Row { .entry = "GIT_DIR=/elsewhere/.git", .withheld = true },
        Row { .entry = "GIT_WORK_TREE=/elsewhere", .withheld = true },
        Row { .entry = "GIT_INDEX_FILE=/tmp/index", .withheld = true },
        Row { .entry = "GIT_CONFIG_PARAMETERS='core.pager=x'", .withheld = true },
        Row { .entry = "GIT_CONFIG_COUNT=1", .withheld = true },
        Row { .entry = "GIT_CONFIG_KEY_0=core.sshCommand", .withheld = true },
        Row { .entry = "GIT_EXEC_PATH=/opt/git", .withheld = true },
        Row { .entry = "GIT_CEILING_DIRECTORIES=/", .withheld = true },
        Row { .entry = "GIT_SSH_COMMAND=ssh -i ~/.ssh/work_key", .withheld = false },
        Row { .entry = "GIT_SSH=/usr/bin/plink", .withheld = false },
        Row { .entry = "GIT_ASKPASS=/usr/libexec/askpass", .withheld = false },
        Row { .entry = "GIT_SSH_COMMANDX=x", .withheld = true },
        Row { .entry = "GH_REPO=other/repo", .withheld = true },
        Row { .entry = "GH_HOST=ghe.example", .withheld = true },
        Row { .entry = "GH_ENTERPRISE_TOKEN=t", .withheld = true },
        Row { .entry = "GITHUB_ENTERPRISE_TOKEN=t", .withheld = true },
        Row { .entry = "GH_TOKEN=t", .withheld = false },
        Row { .entry = "GITHUB_TOKEN=t", .withheld = false },
        Row { .entry = "GH_CONFIG_DIR=/home/u/.config/gh", .withheld = false },
        Row { .entry = "GH_REPOSITORY=x", .withheld = false },
        Row { .entry = "PATH=/usr/bin", .withheld = false },
        Row { .entry = "HOME=/home/u", .withheld = false },
        Row { .entry = "LANG=C.UTF-8", .withheld = false },
        Row { .entry = "SSH_AUTH_SOCK=/run/ssh", .withheld = false },
        Row { .entry = "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/bus", .withheld = false },
        Row { .entry = "HTTPS_PROXY=http://proxy", .withheld = false },
        Row { .entry = "SSL_CERT_FILE=/etc/ssl/cert.pem", .withheld = false },
        Row { .entry = "MYGIT_DIR=/x", .withheld = false },
    };
    auto environment = std::vector<std::string> {};
    auto kept = std::vector<std::string> {};
    for (auto const& row: Rows)
    {
        CAPTURE(row.entry);
        CHECK(isWithheld(row.entry) == row.withheld);
        environment.emplace_back(row.entry);
        if (!row.withheld)
            kept.emplace_back(row.entry);
    }
    CHECK(withoutWithheld(environment) == kept);
}

TEST_CASE("Environment: lookup reads one variable", "[orchestrator][proto]")
{
    auto const environment = std::vector<std::string> { "HOME=/home/u", "HOMEDIR=/x", "EMPTY=", "A=b=c" };
    struct Row
    {
        std::string_view name;
        std::optional<std::string_view> value;
    };
    static constexpr auto Rows = std::array {
        Row { .name = "HOME", .value = "/home/u" },  Row { .name = "HOMEDIR", .value = "/x" },
        Row { .name = "HO", .value = std::nullopt }, Row { .name = "EMPTY", .value = std::nullopt },
        Row { .name = "A", .value = "b=c" },         Row { .name = "MISSING", .value = std::nullopt },
    };
    for (auto const& row: Rows)
    {
        CAPTURE(row.name);
        auto const value = lookup(environment, row.name);
        CHECK(value.has_value() == row.value.has_value());
        if (value && row.value)
            CHECK(*value == *row.value);
    }
}

TEST_CASE("Environment: layers override what came before, one entry per name", "[orchestrator][proto]")
{
    static constexpr auto First = std::array {
        EnvironmentVariable { .name = "GIT_CONFIG_COUNT", .value = "2" },
        EnvironmentVariable { .name = "LC_ALL", .value = "C" },
    };
    static constexpr auto Second = std::array {
        EnvironmentVariable { .name = "LC_ALL", .value = "POSIX" },
    };
    auto const layers = std::array { std::span<EnvironmentVariable const>(First),
                                     std::span<EnvironmentVariable const>(Second) };
    auto const result = withVariables({ "PATH=/usr/bin", "LC_ALL=de_DE", "LC_ALLX=keep" }, layers);
    CHECK(result
          == std::vector<std::string> {
              "PATH=/usr/bin", "LC_ALLX=keep", "GIT_CONFIG_COUNT=2", "LC_ALL=POSIX" });
}
