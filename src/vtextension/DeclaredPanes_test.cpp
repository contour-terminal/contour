// SPDX-License-Identifier: Apache-2.0
//
// The agent-pane policy over a fake filesystem and a recording host, with the orchestrator's manifest from
// the conformance corpus: what reaches the host, what is refused before it does, and the environment a pane
// gets.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/DeclaredPanes.hpp>
#include <vtextension/PaneService.hpp>
#include <vtextension/ProcessService.hpp>
#include <vtextension/testing/FakeDirectoryOpener.hpp>
#include <vtextension/testing/OrchestratorManifest.hpp>

using namespace std::string_view_literals;
using namespace vtextension;
using vtextension::testing::FakeDirectoryOpener;
using vtextension::testing::orchestratorManifest;

namespace
{

constexpr auto SessionId = "0e1f2a3b-4c5d-4e6f-8a9b-0c1d2e3f4a5b"sv;

/// An IPaneHost that records every spawn it is asked to open, and answers a fixed id.
class RecordingPaneHost final: public IPaneHost
{
  public:
    [[nodiscard]] std::expected<std::string, PaneError> open(PaneSpawn spawn) override
    {
        spawns.push_back(std::move(spawn));
        return std::string { "pane-1" };
    }

    [[nodiscard]] std::expected<void, PaneError> close(std::string_view pane) override
    {
        closes.emplace_back(pane);
        return {};
    }

    [[nodiscard]] std::expected<std::vector<PaneInfo>, PaneError> list() override
    {
        return std::vector<PaneInfo> {};
    }

    [[nodiscard]] std::expected<void, PaneError> reveal(std::string_view /*pane*/, Origin /*origin*/) override
    {
        return {};
    }

    std::vector<PaneSpawn> spawns;   ///< Every open(), in order.
    std::vector<std::string> closes; ///< Every close(), in order.
};

/// One DeclaredPanes over fakes, with `/w` as `worktrees` and `/r` as `repos`, and the worktree `/w/run-1`.
struct Fixture
{
    explicit Fixture(std::vector<std::string> inherited = { "PATH=/usr/bin" },
                     std::vector<ProgramPath> programs = {})
    {
        for (auto const* path: { "/w", "/w/run-1", "/r", "/r/repo" })
            directories.add(path);
        auto created = DeclaredPanes::create(
            DeclaredPanesConfig { .manifest = manifest,
                                  .roots = { { .label = "worktrees", .paths = { "/w" } },
                                             { .label = "repos", .paths = { "/r" } } },
                                  .inheritedEnvironment = std::move(inherited),
                                  .programs = std::move(programs) },
            directories,
            host);
        REQUIRE(created.has_value());
        panes = std::move(*created);
    }

    /// Opens the `agent` template with valid arguments unless the case overrides them.
    [[nodiscard]] std::expected<std::string, PaneError> open(ConfigMap const& args,
                                                             std::string_view cwd = "/w/run-1",
                                                             std::string_view title = "run-1",
                                                             std::string_view tag = "run-1",
                                                             std::string_view paneTemplate = "agent") const
    {
        return panes->open(paneTemplate, args, cwd, title, tag, std::nullopt);
    }

    Manifest manifest = orchestratorManifest();
    FakeDirectoryOpener directories;
    RecordingPaneHost host;
    std::unique_ptr<DeclaredPanes> panes;
};

/// @return A title holding U+202E RIGHT-TO-LEFT OVERRIDE, spelled byte by byte.
[[nodiscard]] std::string rightToLeftOverride()
{
    auto title = std::string { "run" };
    for (auto const byte: { 0xE2, 0x80, 0xAE })
        title.push_back(static_cast<char>(byte));
    return title + "1";
}

[[nodiscard]] ConfigMap agentArguments()
{
    return { { "model", "opus" },
             { "session", std::string { SessionId } },
             { "board", "Keep STATUS.md." },
             { "prompt", "/x {url}" } };
}

using Environment = std::vector<std::pair<std::string, std::string>>;

/// @return The value of @p name in @p environment, if set.
[[nodiscard]] std::optional<std::string> valueOf(Environment const& environment, std::string_view name)
{
    auto const found = std::ranges::find(environment, name, &Environment::value_type::first);
    return found != environment.end() ? std::optional { found->second } : std::nullopt;
}

} // namespace

TEST_CASE("pane-open spawns the template's argv", "[vtextension][panes]")
{
    auto f = Fixture {};
    auto const opened = f.panes->open("agent", agentArguments(), "/w/run-1", "run-1", "run-1", "tab-7");

    REQUIRE(opened == "pane-1");
    REQUIRE(f.host.spawns.size() == 1);
    auto const& spawn = f.host.spawns.front();
    // One word per value: the prompt's space and braces stay inside its word.
    CHECK(spawn.argv
          == std::vector<std::string> { "claude",
                                        "--model",
                                        "opus",
                                        "--session-id",
                                        std::string { SessionId },
                                        "--append-system-prompt",
                                        "Keep STATUS.md.",
                                        "/x {url}" });
    REQUIRE(spawn.directory != nullptr);
    CHECK(spawn.directory->resolvedPath() == "/w/run-1");
    CHECK(spawn.cwd == "/w/run-1");
    CHECK(spawn.title == "run-1");
    CHECK(spawn.tag == "run-1");
    CHECK(spawn.nearTab == "tab-7");
}

TEST_CASE("pane-open runs the executable the configuration names for the template's program",
          "[vtextension][panes]")
{
    // A daemon started by systemd has a PATH without ~/.local/bin, where Claude Code's installer puts claude.
    auto f = Fixture { { "PATH=/usr/bin" },
                       { ProgramPath { .name = "claude", .path = "/home/u/.local/bin/claude" },
                         ProgramPath { .name = "gh", .path = "/opt/gh" } } };
    REQUIRE(f.open(agentArguments()).has_value());

    REQUIRE(f.host.spawns.size() == 1);
    auto const& argv = f.host.spawns.front().argv;
    REQUIRE(argv.size() == 8);
    CHECK(argv.front() == "/home/u/.local/bin/claude");
    // Only the program is replaced: an argument spelled like a configured program stays as it is.
    CHECK(argv[1] == "--model");
}

TEST_CASE("pane-open refuses", "[vtextension][panes]")
{
    struct Row
    {
        std::string_view what;
        std::string_view paneTemplate = "agent";
        ConfigMap args = agentArguments();
        std::string_view cwd = "/w/run-1";
        std::string title = "run-1";
        std::string tag = "run-1";
        PaneError error;
    };
    auto withArgument = [](std::string_view name, std::string value) {
        auto args = agentArguments();
        std::ranges::find(args, name, &ConfigMap::value_type::first)->second = std::move(value);
        return args;
    };
    auto withoutSession = [] {
        auto args = agentArguments();
        std::erase_if(args, [](auto const& entry) { return entry.first == "session"; });
        return args;
    };
    auto const row = GENERATE_COPY(
        Row { .what = "an undeclared template", .paneTemplate = "shell", .error = PaneError::NotDeclared },
        Row { .what = "an option as the model",
              .args = withArgument("model", "-x"),
              .error = PaneError::ArgumentRejected },
        Row { .what = "no session", .args = withoutSession(), .error = PaneError::ArgumentRejected },
        Row { .what = "a repository as the cwd", .cwd = "/r/repo", .error = PaneError::CwdOutsideRoot },
        Row { .what = "a worktree that is gone", .cwd = "/w/gone", .error = PaneError::CwdMissing },
        Row { .what = "a title of 65 code points",
              .title = std::string(65, 'x'),
              .error = PaneError::BadTitle },
        Row { .what = "a title that reorders its text",
              .title = rightToLeftOverride(),
              .error = PaneError::BadTitle },
        Row { .what = "a tag with a space", .tag = "a b", .error = PaneError::BadTag },
        Row { .what = "a tag of 129 bytes", .tag = std::string(129, 'a'), .error = PaneError::BadTag });
    INFO(row.what);
    auto f = Fixture {};

    CHECK(f.open(row.args, row.cwd, row.title, row.tag, row.paneTemplate) == std::unexpected(row.error));
    CHECK(f.host.spawns.empty());
}

TEST_CASE("a worktree path that is no directory is refused as a declared process is", "[vtextension][panes]")
{
    for (auto const error: { vtpty::DirectoryError::NotADirectory, vtpty::DirectoryError::AccessDenied })
    {
        auto f = Fixture {};
        f.directories.failures.emplace("/w/run-1", error);
        CHECK(f.open(agentArguments()) == std::unexpected(PaneError::CwdNotADirectory));
        CHECK(f.host.spawns.empty());
    }
    // proc-start refuses the same directory as CwdNotADirectory: the guest hears one word from both.
    CHECK(hostErrorOf(PaneError::CwdNotADirectory) == HostError::NotADirectory);
    CHECK(hostErrorOf(PaneError::CwdNotADirectory) == hostErrorOf(ProcessError::CwdNotADirectory));
}

TEST_CASE("pane-open accepts a title of 64 code points and a tag of 128 bytes", "[vtextension][panes]")
{
    auto f = Fixture {};
    auto title = std::string {};
    for ([[maybe_unused]] auto const _: std::views::iota(0, 64))
        title += "\xC3\xA9"; // é: two bytes, one code point
    CHECK(f.open(agentArguments(), "/w/run-1", title, std::string(128, 'a')).has_value());
}

TEST_CASE("the pane environment appends to the inherited git config", "[vtextension][panes]")
{
    auto f = Fixture {
        { "PATH=/usr/bin", "GIT_CONFIG_COUNT=1", "GIT_CONFIG_KEY_0=user.x", "GIT_CONFIG_VALUE_0=1" },
    };
    REQUIRE(f.open(agentArguments()).has_value());
    auto const& environment = f.host.spawns.front().environment;

    CHECK(valueOf(environment, "GIT_CONFIG_COUNT") == "3");
    CHECK(valueOf(environment, "GIT_CONFIG_KEY_1") == "push.default");
    CHECK(valueOf(environment, "GIT_CONFIG_VALUE_1") == "upstream");
    CHECK(valueOf(environment, "GIT_CONFIG_KEY_2") == "push.autoSetupRemote");
    CHECK(valueOf(environment, "GIT_CONFIG_VALUE_2") == "true");
    // The user's own pair is not the template's to set: it is inherited, and kept.
    CHECK_FALSE(valueOf(environment, "GIT_CONFIG_KEY_0").has_value());
    auto const& removed = f.host.spawns.front().removedEnvironment;
    CHECK_FALSE(std::ranges::contains(removed, "GIT_CONFIG_KEY_0"));
    CHECK_FALSE(std::ranges::contains(removed, "GIT_CONFIG_COUNT"));
}

TEST_CASE("without an inherited count the template's pairs are used as declared", "[vtextension][panes]")
{
    auto f = Fixture {};
    REQUIRE(f.open(agentArguments()).has_value());

    CHECK(f.host.spawns.front().environment
          == Environment { { "GIT_CONFIG_COUNT", "2" },
                           { "GIT_CONFIG_KEY_0", "push.default" },
                           { "GIT_CONFIG_VALUE_0", "upstream" },
                           { "GIT_CONFIG_KEY_1", "push.autoSetupRemote" },
                           { "GIT_CONFIG_VALUE_1", "true" } });
}

TEST_CASE("the pane withholds git and gh repository selection", "[vtextension][panes]")
{
    auto f = Fixture {
        { "PATH=/usr/bin",
          "GIT_DIR=/elsewhere/.git",
          "GIT_WORK_TREE=/elsewhere",
          "GIT_CONFIG_PARAMETERS='core.hooksPath'='/x'",
          "GH_REPO=someone/else",
          "GH_HOST=example.com",
          "GIT_SSH_COMMAND=ssh -i key",
          "GIT_CONFIG_COUNT=1",
          "GIT_CONFIG_KEY_0=user.x",
          "GIT_CONFIG_VALUE_0=1" },
    };
    REQUIRE(f.open(agentArguments()).has_value());
    auto const& removed = f.host.spawns.front().removedEnvironment;

    for (auto const* name: { "GIT_DIR", "GIT_WORK_TREE", "GIT_CONFIG_PARAMETERS", "GH_REPO", "GH_HOST" })
    {
        INFO(name);
        CHECK(std::ranges::contains(removed, std::string_view { name }));
    }
    for (auto const* name:
         { "PATH", "GIT_SSH_COMMAND", "GIT_CONFIG_COUNT", "GIT_CONFIG_KEY_0", "GIT_CONFIG_VALUE_0" })
    {
        INFO(name);
        CHECK_FALSE(std::ranges::contains(removed, std::string_view { name }));
    }
}

TEST_CASE("close, list and reveal are the host's", "[vtextension][panes]")
{
    auto f = Fixture {};
    CHECK(f.panes->close("pane-9").has_value());
    CHECK(f.host.closes == std::vector<std::string> { "pane-9" });
    CHECK(f.panes->list().has_value());
    CHECK(f.panes->reveal("pane-9", 0).has_value());
}

TEST_CASE("a host that opens no panes answers not-supported-yet", "[vtextension][panes]")
{
    auto unserved = UnservedPanes {};
    CHECK(unserved.open("agent", {}, "/w", "t", "t", std::nullopt)
          == std::unexpected(PaneError::NotSupportedYet));
    CHECK(unserved.close("p") == std::unexpected(PaneError::NotSupportedYet));
    CHECK(unserved.list() == std::unexpected(PaneError::NotSupportedYet));
    CHECK(unserved.reveal("p", 0) == std::unexpected(PaneError::NotSupportedYet));
}

TEST_CASE("every pane error reaches the guest as its host-error", "[vtextension][panes]")
{
    struct Row
    {
        PaneError error;
        HostError hostError;
    };
    auto const row = GENERATE(Row { PaneError::NotDeclared, HostError::BadArgument },
                              Row { PaneError::ArgumentRejected, HostError::BadArgument },
                              Row { PaneError::CwdOutsideRoot, HostError::BadArgument },
                              Row { PaneError::CwdMissing, HostError::Gone },
                              Row { PaneError::CwdNotADirectory, HostError::NotADirectory },
                              Row { PaneError::ProgramMissing, HostError::ProgramMissing },
                              Row { PaneError::BadTitle, HostError::BadArgument },
                              Row { PaneError::BadTag, HostError::BadArgument },
                              Row { PaneError::NotOwned, HostError::NotOwned },
                              Row { PaneError::TooMany, HostError::TooMany },
                              Row { PaneError::HostRefused, HostError::Limit },
                              Row { PaneError::NotReconciled, HostError::Limit },
                              Row { PaneError::NotSupportedYet, HostError::NotSupportedYet });
    CHECK(hostErrorOf(row.error) == row.hostError);
}
