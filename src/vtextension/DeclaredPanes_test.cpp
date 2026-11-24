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
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/DaemonWorld.hpp>
#include <vtextension/DeclaredPanes.hpp>
#include <vtextension/PaneService.hpp>
#include <vtextension/ProcessService.hpp>
#include <vtextension/UserSettableEnvironment.hpp>
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

    [[nodiscard]] std::expected<void, PaneError> write(std::string_view pane, std::string_view bytes) override
    {
        writes.emplace_back(pane, bytes);
        return {};
    }

    std::vector<std::pair<std::string, std::string>> writes; ///< Every write(), in order.
    std::vector<PaneSpawn> spawns;                           ///< Every open(), in order.
    std::vector<std::string> closes;                         ///< Every close(), in order.
};

/// One DeclaredPanes over fakes, with `/w` as `worktrees` and `/r` as `repos`, and the worktree `/w/run-1`.
struct Fixture
{
    explicit Fixture(std::vector<std::string> inherited = { "PATH=/usr/bin" },
                     std::vector<ProgramPath> programs = {},
                     Manifest declared = orchestratorManifest(),
                     std::vector<EnvironmentSet> sets = {}):
        manifest { std::move(declared) }
    {
        for (auto const* path: { "/w", "/w/run-1", "/r", "/r/repo" })
            directories.add(path);
        auto created = DeclaredPanes::create(
            DeclaredPanesConfig { .manifest = manifest,
                                  .roots = { { .label = "worktrees", .paths = { "/w" } },
                                             { .label = "repos", .paths = { "/r" } } },
                                  .inheritedEnvironment = std::move(inherited),
                                  .programs = std::move(programs),
                                  .environmentSets = std::move(sets) },
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

    Manifest manifest;
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
             { "settings", R"({"permissions":{"defaultMode":"acceptEdits","allow":[],"deny":[]}})" },
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
    // One word per value: the prompt's space and braces stay inside its word, and so do the settings' quotes.
    CHECK(
        spawn.argv
        == std::vector<std::string> { "claude",
                                      "--model",
                                      "opus",
                                      "--session-id",
                                      std::string { SessionId },
                                      "--settings",
                                      R"({"permissions":{"defaultMode":"acceptEdits","allow":[],"deny":[]}})",
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
    REQUIRE(argv.size() == 10);
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

#ifdef _WIN32
TEST_CASE("paneWithheldNames folds names on Windows", "[vtextension][panes]")
{
    auto const inherited = std::vector<std::string> { "Git_Dir=x" };
    CHECK(vtextension::paneWithheldNames(inherited) == std::vector<std::string> { "Git_Dir" });
}
#endif

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

TEST_CASE("pane-send types checked text as one bracketed paste and Enter", "[vtextension][panes]")
{
    auto f = Fixture {};
    REQUIRE(f.manifest.paneInput == PaneInput::Typed);
    REQUIRE(f.panes->send("pane-1", "Run ctest again,\n\tthen push.").has_value());
    REQUIRE(f.host.writes.size() == 1);
    CHECK(f.host.writes.front().first == "pane-1");
    CHECK(f.host.writes.front().second == "\x1b[200~Run ctest again,\n\tthen push.\x1b[201~\r");
}

TEST_CASE("pane-send refuses text that could end its paste, and any text without pane-input",
          "[vtextension][panes]")
{
    auto f = Fixture {};
    for (auto const& text: { std::string {},                // nothing to send
                             std::string { "a\x1b[201~b" }, // an escape ends the paste
                             std::string { "a\rb" },        // a carriage return is Enter
                             std::string { "a\xc2\x85"
                                           "b" },     // U+0085, a C1 control
                             std::string { "a\xff" }, // not UTF-8
                             std::string(MaxPaneSendBytes + 1, 'x') })
    {
        CAPTURE(text.size());
        CHECK(f.panes->send("pane-1", text) == std::unexpected(PaneError::TextRefused));
    }
    CHECK(f.host.writes.empty());
    CHECK(f.panes->send("pane-1", std::string(MaxPaneSendBytes, 'x')).has_value());

    // A manifest without pane-input types nothing.
    auto manifest = orchestratorManifest();
    manifest.paneInput = PaneInput::Refused;
    auto directories = FakeDirectoryOpener {};
    for (auto const* path: { "/w", "/r" })
        directories.add(path);
    auto host = RecordingPaneHost {};
    auto panes =
        DeclaredPanes::create(DeclaredPanesConfig { .manifest = manifest,
                                                    .roots = { { .label = "worktrees", .paths = { "/w" } },
                                                               { .label = "repos", .paths = { "/r" } } },
                                                    .inheritedEnvironment = {} },
                              directories,
                              host);
    REQUIRE(panes.has_value());
    CHECK((*panes)->send("pane-1", "hello") == std::unexpected(PaneError::InputNotDeclared));
    CHECK(host.writes.empty());
}

TEST_CASE("pane-key presses one key alone, and nothing without pane-input", "[vtextension][panes]")
{
    auto f = Fixture {};
    REQUIRE(f.panes->press("pane-1", PaneKeyName::Digit1).has_value());
    REQUIRE(f.panes->press("pane-1", PaneKeyName::Digit9).has_value());
    REQUIRE(f.panes->press("pane-1", PaneKeyName::Escape).has_value());
    CHECK(f.host.writes
          == std::vector<std::pair<std::string, std::string>> {
              { "pane-1", "1" }, { "pane-1", "9" }, { "pane-1", "\x1b" } });

    auto manifest = orchestratorManifest();
    manifest.paneInput = PaneInput::Refused;
    auto directories = FakeDirectoryOpener {};
    for (auto const* path: { "/w", "/r" })
        directories.add(path);
    auto host = RecordingPaneHost {};
    auto panes =
        DeclaredPanes::create(DeclaredPanesConfig { .manifest = manifest,
                                                    .roots = { { .label = "worktrees", .paths = { "/w" } },
                                                               { .label = "repos", .paths = { "/r" } } },
                                                    .inheritedEnvironment = {} },
                              directories,
                              host);
    REQUIRE(panes.has_value());
    CHECK((*panes)->press("pane-1", PaneKeyName::Digit1) == std::unexpected(PaneError::InputNotDeclared));
    CHECK(host.writes.empty());
}

TEST_CASE("every pane key has its own bytes", "[vtextension][panes]")
{
    auto seen = std::vector<std::string_view> {};
    for (auto const index: std::views::iota(0U, abi::EnumCaseCount<PaneKeyName>))
    {
        auto const bytes = bytesOf(static_cast<PaneKeyName>(index));
        CHECK(bytes.size() == 1);
        CHECK_FALSE(std::ranges::contains(seen, bytes));
        seen.push_back(bytes);
    }
}

namespace
{

/// A pane `agent` that takes CLAUDE_CONFIG_DIR and NO_PROXY from the set its `env-set` argument names, and a
/// pane `plain` that takes no set.
constexpr auto SetsManifest = "name = sets\n"
                              "version = 2\n"
                              "root worktrees = config:worktree_root\n"
                              "root repos = config:repo_root\n"
                              "arg env-set = name\n"
                              "arg model = name\n"
                              "pane agent @worktrees = claude --model {model}\n"
                              "pane plain @worktrees = claude --model {model}\n"
                              "pane-env-set agent {env-set} = CLAUDE_CONFIG_DIR NO_PROXY\n"sv;

/// The user's sets: `work`, which holds an unlisted COPILOT_HOME and no NO_PROXY, and `broken`, whose value
/// fails its shape (as a set that bypassed checkEnvironmentSets() would).
[[nodiscard]] std::vector<EnvironmentSet> userSets()
{
    return { EnvironmentSet { .name = "work",
                              .variables = { { .name = "CLAUDE_CONFIG_DIR", .value = "/home/u/.claude-work" },
                                             { .name = "COPILOT_HOME", .value = "/home/u/.copilot" } } },
             EnvironmentSet { .name = "broken",
                              .variables = { { .name = "CLAUDE_CONFIG_DIR", .value = "relative" } } } };
}

/// A Fixture over SetsManifest and userSets(), with NO_PROXY inherited from the daemon.
[[nodiscard]] std::unique_ptr<Fixture> setsFixture()
{
    auto manifest = parseManifestText(SetsManifest);
    REQUIRE(manifest.has_value());
    return std::make_unique<Fixture>(
        std::vector<std::string> { "PATH=/usr/bin", "NO_PROXY=corp.example.com" },
        std::vector<ProgramPath> {},
        std::move(*manifest),
        userSets());
}

} // namespace

TEST_CASE("pane-open applies the listed variables of the set the call names", "[vtextension][panes][sets]")
{
    auto const f = setsFixture();
    REQUIRE(f->open({ { "model", "opus" }, { "env-set", "work" } }).has_value());

    REQUIRE(f->host.spawns.size() == 1);
    auto const& spawn = f->host.spawns.front();
    // The set argument is no placeholder: it chooses the set, and the argv does not see it.
    CHECK(spawn.argv == std::vector<std::string> { "claude", "--model", "opus" });
    CHECK(valueOf(spawn.environment, "CLAUDE_CONFIG_DIR") == "/home/u/.claude-work");
    // Held by the set, but not listed by the binding.
    CHECK_FALSE(valueOf(spawn.environment, "COPILOT_HOME").has_value());
    // Listed, but not held by the set: inherited from the daemon as any other variable, neither set nor
    // withheld.
    CHECK_FALSE(valueOf(spawn.environment, "NO_PROXY").has_value());
    CHECK_FALSE(std::ranges::contains(spawn.removedEnvironment, "NO_PROXY"));
}

TEST_CASE("pane-open without a set argument applies no set", "[vtextension][panes][sets]")
{
    auto const f = setsFixture();
    REQUIRE(f->open({ { "model", "opus" } }).has_value());

    REQUIRE(f->host.spawns.size() == 1);
    auto const& spawn = f->host.spawns.front();
    CHECK(spawn.argv == std::vector<std::string> { "claude", "--model", "opus" });
    CHECK_FALSE(valueOf(spawn.environment, "CLAUDE_CONFIG_DIR").has_value());
    CHECK_FALSE(valueOf(spawn.environment, "COPILOT_HOME").has_value());
}

TEST_CASE("pane-open refuses a set it cannot apply", "[vtextension][panes][sets]")
{
    struct Row
    {
        std::string_view what;
        std::string_view paneTemplate;
        ConfigMap args;
    };
    auto const row =
        GENERATE(Row { .what = "no such set",
                       .paneTemplate = "agent",
                       .args = { { "model", "opus" }, { "env-set", "gone" } } },
                 Row { .what = "not a name",
                       .paneTemplate = "agent",
                       .args = { { "model", "opus" }, { "env-set", "bad name" } } },
                 Row { .what = "a value failing its shape",
                       .paneTemplate = "agent",
                       .args = { { "model", "opus" }, { "env-set", "broken" } } },
                 Row { .what = "a template without a binding",
                       .paneTemplate = "plain",
                       .args = { { "model", "opus" }, { "env-set", "work" } } },
                 Row { .what = "the set argument twice",
                       .paneTemplate = "agent",
                       .args = { { "model", "opus" }, { "env-set", "work" }, { "env-set", "work" } } });
    INFO(row.what);
    auto const f = setsFixture();

    CHECK(f->open(row.args, "/w/run-1", "run-1", "run-1", row.paneTemplate)
          == std::unexpected(PaneError::ArgumentRejected));
    CHECK(f->host.spawns.empty());
}
