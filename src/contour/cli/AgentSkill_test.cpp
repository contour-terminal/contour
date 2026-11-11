// SPDX-License-Identifier: Apache-2.0
//
// `contour agent setup`: the generated skill and where it is written, over an in-memory file system.

#include <contour/cli/AgentSkill.hpp>
#include <contour/cli/ControlVerbs.hpp>
#include <contour/cli/ExitStatus.hpp>

#include <core/Utf8Path.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>
#include <core/testing/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <vtcontrol/Describe.hpp>
#include <vtcontrol/Json.hpp>
#include <vthost/testing/ForwardingFileSystem.hpp>

using namespace std::string_literals;
using namespace std::string_view_literals;
using contour::cli::AgentOutcome;
using contour::cli::AgentSelection;
using contour::cli::ForeignSkill;
using contour::cli::HookAction;
using contour::cli::SetupMode;
using contour::cli::SkillState;

namespace
{

constexpr auto ClaudeSkill = "/h/.claude/skills/contour/SKILL.md"sv;
constexpr auto AgentsSkill = "/h/.agents/skills/contour/SKILL.md"sv;

/// Sets the agents up under /h with the given text.
std::vector<AgentOutcome> setup(core::platform::testing::InMemoryFileSystem& fs,
                                core::testing::FakeEnvironment const& env,
                                std::string_view text,
                                std::vector<std::string> const& agents = {},
                                SetupMode mode = SetupMode::Write,
                                ForeignSkill foreign = ForeignSkill::Keep)
{
    auto outcomes = contour::cli::setupAgentSkills(fs, env, "/h", agents, mode, foreign, text, {});
    REQUIRE(outcomes.has_value());
    return *std::move(outcomes);
}

/// @return The outcome for @p agent among @p outcomes; fails the test when there is none.
AgentOutcome const& outcomeOf(std::vector<AgentOutcome> const& outcomes, std::string_view agent)
{
    auto const found = std::ranges::find(outcomes, agent, &AgentOutcome::agent);
    REQUIRE(found != outcomes.end());
    return *found;
}

/// A file system where Claude Code is installed: its configuration directory exists.
core::platform::testing::InMemoryFileSystem withClaude()
{
    auto fs = core::platform::testing::InMemoryFileSystem {};
    fs.addDirectory("/h/.claude");
    return fs;
}

} // namespace

TEST_CASE("the skill names every verb and method", "[contour][cli][agent]")
{
    auto const skill = contour::cli::renderAgentSkill("1.2.3");

    CHECK(skill.starts_with("---\nname: contour\n"));
    CHECK(skill.contains(contour::cli::AgentSkillMarker));
    CHECK(skill.contains("1.2.3"));
    for (auto const& verb: contour::cli::controlVerbs())
    {
        auto spelled = std::string { verb.path };
        std::ranges::replace(spelled, '.', ' ');
        INFO(spelled);
        CHECK(skill.contains("`contour " + spelled));
    }
    auto const api = vtcontrol::listApi();
    for (auto const& method: api.methods)
        CHECK(skill.contains("`" + method.name + "`"));
    for (auto const& event: api.events)
        CHECK(skill.contains("`" + event.name + "`"));
    CHECK(skill.contains("$CONTOUR_PANE"));
    // The example an agent copies carries the `--` that keeps its text from being read as an option.
    CHECK(skill.contains("contour send --pane \"$pane\" --enter --wait -- 'make test'"));
    CHECK(skill.contains("Put `--` before the text"));
    CHECK(skill.contains("claude mcp add contour -- contour mcp"));
}

TEST_CASE("the skill and the docs list every exit status in the one table's words", "[contour][cli][agent]")
{
    using contour::cli::ExitStatus;

    // Every value meaningOf() knows (its switch is exhaustive, so a new enumerator lands there) is listed.
    for (auto const value: std::views::iota(0, 256))
    {
        auto const status = static_cast<ExitStatus>(value);
        INFO(value);
        CHECK(contour::cli::meaningOf(status).empty()
              != std::ranges::contains(contour::cli::ExitStatuses, status));
    }
    CHECK(std::ranges::is_sorted(contour::cli::ExitStatuses));

    auto const skill = contour::cli::renderAgentSkill("1.2.3");
    auto docFile = std::ifstream(CONTOUR_CONTROL_API_DOC, std::ios::binary);
    REQUIRE(docFile.good());
    auto doc = std::string { std::istreambuf_iterator<char>(docFile), {} };
    std::erase(doc, '\r'); // a Windows checkout ends the doc's lines with CRLF
    for (auto const status: contour::cli::ExitStatuses)
    {
        auto const code = contour::cli::exitCode(status);
        auto const meaning = contour::cli::meaningOf(status);
        INFO(code);
        CHECK(skill.contains(std::format("- {}: {}\n", code, meaning)));
        CHECK(doc.contains(std::format("| {} | {} |\n", code, meaning)));
    }
}

TEST_CASE("setup creates, then leaves unchanged, then updates", "[contour][cli][agent]")
{
    auto fs = withClaude();
    auto const env = core::testing::FakeEnvironment {};

    auto const created = setup(fs, env, "v1");
    REQUIRE(created.size() == 3);
    CHECK(outcomeOf(created, "claude-code").skill->state == SkillState::Created);
    CHECK(outcomeOf(created, "claude-code").skill->path == ClaudeSkill);
    CHECK(outcomeOf(created, "agents").skill->path == AgentsSkill);
    CHECK(fs.readFile(ClaudeSkill).value() == "v1");

    // Without the marker the text is foreign, so give the stored text one.
    auto const text = std::string { contour::cli::AgentSkillMarker } + "\nbody";
    auto const stored = setup(fs, env, text, {}, SetupMode::Write, ForeignSkill::Replace);
    CHECK(outcomeOf(stored, "claude-code").skill->state == SkillState::Updated);
    CHECK(outcomeOf(setup(fs, env, text), "claude-code").skill->state == SkillState::Unchanged);

    auto const newer = text + " 2";
    CHECK(outcomeOf(setup(fs, env, newer), "claude-code").skill->state == SkillState::Updated);
    CHECK(fs.readFile(ClaudeSkill).value() == newer);
}

TEST_CASE("a foreign skill is kept unless forced", "[contour][cli][agent]")
{
    auto fs = core::platform::testing::InMemoryFileSystem { { .path = std::string { ClaudeSkill },
                                                              .content = "mine" } };
    auto const env = core::testing::FakeEnvironment {};
    auto const text = std::string { contour::cli::AgentSkillMarker } + "\nbody";

    CHECK(outcomeOf(setup(fs, env, text), "claude-code").skill->state == SkillState::Foreign);
    CHECK(fs.readFile(ClaudeSkill).value() == "mine");

    CHECK(outcomeOf(setup(fs, env, text, {}, SetupMode::Write, ForeignSkill::Replace), "claude-code")
              .skill->state
          == SkillState::Updated);
    CHECK(fs.readFile(ClaudeSkill).value() == text);
}

TEST_CASE("CLAUDE_CONFIG_DIR moves the Claude skill", "[contour][cli][agent]")
{
    auto fs = core::platform::testing::InMemoryFileSystem {};
    auto const env = core::testing::FakeEnvironment { { { "CLAUDE_CONFIG_DIR", "/c" } } };

    auto const outcomes = setup(fs, env, "text", { "claude-code" });

    REQUIRE(outcomes.size() == 1);
    CHECK(outcomes[0].selection == AgentSelection::Named);
    CHECK(outcomes[0].skill->path == "/c/skills/contour/SKILL.md");
    CHECK(fs.exists("/c/skills/contour/SKILL.md"));
    CHECK_FALSE(fs.exists(ClaudeSkill));
}

TEST_CASE("dry run writes nothing; --agent limits the targets", "[contour][cli][agent]")
{
    auto fs = withClaude();
    auto const env = core::testing::FakeEnvironment {};

    auto const dry = setup(fs, env, "text", {}, SetupMode::DryRun);
    CHECK(dry.size() == 3);
    CHECK(outcomeOf(dry, "claude-code").skill->state == SkillState::Created);
    CHECK_FALSE(fs.exists(ClaudeSkill));
    CHECK_FALSE(fs.exists(AgentsSkill));

    auto const only = setup(fs, env, "text", { "agents" });
    REQUIRE(only.size() == 1);
    CHECK(only[0].agent == "agents");
    CHECK(fs.exists(AgentsSkill));
    CHECK_FALSE(fs.exists(ClaudeSkill));

    auto const unknown = contour::cli::setupAgentSkills(fs,
                                                        env,
                                                        "/h",
                                                        std::vector<std::string> { "emacs" },
                                                        SetupMode::Write,
                                                        ForeignSkill::Keep,
                                                        "text",
                                                        {});
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().contains("emacs"));
    CHECK(unknown.error().contains("claude-code"));
    CHECK(unknown.error().contains("codex"));
}

TEST_CASE("agentSetup.namesAreTheReportsIntegrationIds", "[contour][cli][agent]")
{
    // `agent setup --agent X` and `agent report --agent X` name an agent alike.
    for (auto const& target: contour::cli::AgentTargets)
        if (target.hookAgent)
            CHECK(target.name == contour::cli::integrationOf(*target.hookAgent).integrationId);
}

namespace
{

constexpr auto ClaudeSettings = "/h/.claude/settings.json"sv;
constexpr auto CodexHooks = "/h/.codex/hooks.json"sv;

/// Sets the agents up with their hooks.
std::vector<AgentOutcome> setupWithHooks(core::platform::FileSystem& fs,
                                         core::testing::FakeEnvironment const& env,
                                         contour::cli::HookAction action,
                                         SetupMode mode = SetupMode::Write,
                                         std::vector<std::string> const& agents = {})
{
    auto outcomes = contour::cli::setupAgentSkills(fs,
                                                   env,
                                                   "/h",
                                                   agents,
                                                   mode,
                                                   ForeignSkill::Keep,
                                                   "text",
                                                   { .action = action, .contourExe = "/bin/contour" });
    REQUIRE(outcomes.has_value());
    return *std::move(outcomes);
}

/// A file system where both agents are installed.
core::platform::testing::InMemoryFileSystem withBothAgents()
{
    auto fs = withClaude();
    fs.addDirectory("/h/.codex");
    return fs;
}

} // namespace

TEST_CASE("agentSetup.installsHooksAndSkill", "[contour][cli][agent]")
{
    using contour::cli::HookMergeResult;
    auto fs = withBothAgents();
    auto const env = core::testing::FakeEnvironment {};

    auto const outcomes = setupWithHooks(fs, env, HookAction::Install);
    REQUIRE(outcomes.size() == 3);
    CHECK(fs.exists(ClaudeSkill));
    CHECK(fs.exists(AgentsSkill));
    auto const& claude = outcomeOf(outcomes, "claude-code");
    auto const& codex = outcomeOf(outcomes, "codex");
    REQUIRE(claude.hooks.has_value());
    CHECK(claude.hooks->path == ClaudeSettings);
    CHECK(claude.hooks->result == HookMergeResult::Changed);
    CHECK(fs.readFile(ClaudeSettings).value().contains("agent report --agent claude-code"));
    REQUIRE(codex.hooks.has_value());
    CHECK(codex.hooks->path == CodexHooks);
    CHECK_FALSE(codex.skill.has_value()); // Codex reads its skill from ~/.agents
    CHECK(fs.readFile(CodexHooks).value().contains("agent report --agent codex"));
    CHECK_FALSE(outcomeOf(outcomes, "agents").hooks.has_value());

    // Again: nothing changes.
    auto const again = setupWithHooks(fs, env, HookAction::Install);
    CHECK(outcomeOf(again, "claude-code").hooks->result == HookMergeResult::Unchanged);
    CHECK(outcomeOf(again, "codex").hooks->result == HookMergeResult::Unchanged);

    auto const text = contour::cli::formatSkillOutcomes(
        outcomes, SetupMode::Write, HookAction::Install, contour::cli::OutcomeFormat::Text);
    // The paths come from the outcomes: Windows joins them with its own separator.
    CHECK(text.contains(std::format("hooks  changed  {}", core::pathToUtf8(claude.hooks->path))));
    auto const json = contour::cli::formatSkillOutcomes(
        outcomes, SetupMode::Write, HookAction::Install, contour::cli::OutcomeFormat::Json);
    CHECK(json.contains(std::format(R"("hooks":{{"path":{},"state":"changed","detail":""}})",
                                    vtcontrol::quoteJsonString(core::pathToUtf8(codex.hooks->path)))));
}

TEST_CASE("agentSetup.anAgentNotInstalledIsSkippedUnlessNamed", "[contour][cli][agent]")
{
    using contour::cli::HookMergeResult;
    auto fs = core::platform::testing::InMemoryFileSystem {};
    auto const env = core::testing::FakeEnvironment {};

    // Neither agent's directory exists: no settings file is made for an agent nobody runs.
    auto const outcomes = setupWithHooks(fs, env, HookAction::Install);
    CHECK(outcomeOf(outcomes, "claude-code").selection == AgentSelection::Skipped);
    CHECK(outcomeOf(outcomes, "codex").selection == AgentSelection::Skipped);
    CHECK(outcomeOf(outcomes, "agents").selection == AgentSelection::Detected);
    CHECK_FALSE(fs.exists("/h/.claude"));
    CHECK_FALSE(fs.exists("/h/.codex"));
    CHECK(fs.exists(AgentsSkill));
    auto const text = contour::cli::formatSkillOutcomes(
        outcomes, SetupMode::Write, HookAction::Install, contour::cli::OutcomeFormat::Text);
    auto const codexDir = core::pathToUtf8(outcomeOf(outcomes, "codex").configDir);
    CHECK(text.contains(
        std::format("skipped  {} not found; name it with --agent codex to set it up", codexDir)));
    // An uninstall does not tell how to set up what it skipped.
    auto const uninstalled = contour::cli::formatSkillOutcomes(setupWithHooks(fs, env, HookAction::Uninstall),
                                                               SetupMode::Write,
                                                               HookAction::Uninstall,
                                                               contour::cli::OutcomeFormat::Text);
    CHECK(uninstalled.contains(std::format("skipped  {} not found; nothing to remove\n", codexDir)));
    CHECK_FALSE(uninstalled.contains("to set it up"));

    // Naming one sets it up all the same.
    auto const named = setupWithHooks(fs, env, HookAction::Install, SetupMode::Write, { "codex" });
    REQUIRE(named.size() == 1);
    CHECK(named[0].selection == AgentSelection::Named);
    CHECK(named[0].hooks->result == HookMergeResult::Changed);
    CHECK(fs.exists(CodexHooks));
    CHECK_FALSE(fs.exists("/h/.claude"));
}

TEST_CASE("agentSetup.dryRunWritesNothing", "[contour][cli][agent]")
{
    auto fs = withBothAgents();
    auto const env = core::testing::FakeEnvironment {};

    auto const outcomes = setupWithHooks(fs, env, HookAction::Install, SetupMode::DryRun);
    CHECK(outcomeOf(outcomes, "claude-code").hooks->result == contour::cli::HookMergeResult::Changed);
    CHECK_FALSE(fs.exists(ClaudeSettings));
    CHECK_FALSE(fs.exists(CodexHooks));
    CHECK_FALSE(fs.exists(ClaudeSkill));
}

TEST_CASE("agentSetup.uninstallRemovesHooksOnly", "[contour][cli][agent]")
{
    auto fs = withBothAgents();
    auto const env = core::testing::FakeEnvironment {};
    (void) setupWithHooks(fs, env, HookAction::Install);

    // An uninstall needs no binary: the hooks are recognized by what follows it.
    auto const outcomes =
        contour::cli::setupAgentSkills(fs,
                                       env,
                                       "/h",
                                       {},
                                       SetupMode::Write,
                                       ForeignSkill::Keep,
                                       "text",
                                       { .action = HookAction::Uninstall, .contourExe = {} });
    REQUIRE(outcomes.has_value());
    CHECK(outcomeOf(*outcomes, "claude-code").skill->state == SkillState::Untouched);
    CHECK(outcomeOf(*outcomes, "claude-code").hooks->result == contour::cli::HookMergeResult::Changed);
    CHECK_FALSE(fs.readFile(ClaudeSettings).value().contains("agent report"));
    CHECK_FALSE(fs.readFile(CodexHooks).value().contains("agent report"));
    CHECK(fs.readFile(ClaudeSkill).value() == "text");
}

TEST_CASE("agentSetup.hookFilesFollowTheirVariables", "[contour][cli][agent]")
{
    auto fs = core::platform::testing::InMemoryFileSystem {};
    fs.addDirectory("/c");
    fs.addDirectory("/x");
    auto const env =
        core::testing::FakeEnvironment { { { "CLAUDE_CONFIG_DIR", "/c" }, { "CODEX_HOME", "/x" } } };
    (void) setupWithHooks(fs, env, HookAction::Install);
    CHECK(fs.exists("/c/settings.json"));
    CHECK(fs.exists("/x/hooks.json"));
}

TEST_CASE("agentSetup.aMissingCodexHomeIsReportedNotCreated", "[contour][cli][agent]")
{
    using contour::cli::HookMergeResult;
    auto fs = withClaude();
    auto const env = core::testing::FakeEnvironment { { { "CODEX_HOME", "/nope" } } };

    // Unnamed, a Codex whose home is missing is not installed.
    CHECK(outcomeOf(setupWithHooks(fs, env, HookAction::Install), "codex").selection
          == AgentSelection::Skipped);

    auto const outcomes =
        setupWithHooks(fs, env, HookAction::Install, SetupMode::Write, { "claude-code", "codex" });
    auto const& codex = outcomeOf(outcomes, "codex");
    CHECK(codex.hooks->result == HookMergeResult::DirectoryMissing);
    CHECK(codex.hooks->detail.contains("CODEX_HOME"));
    CHECK(codex.hooks->detail.contains("/nope"));
    CHECK_FALSE(fs.exists("/nope"));
    CHECK(outcomeOf(outcomes, "claude-code").hooks->result
          == HookMergeResult::Unchanged); // Claude's is unaffected (and already installed above).
    auto const text = contour::cli::formatSkillOutcomes(
        outcomes, SetupMode::Write, HookAction::Install, contour::cli::OutcomeFormat::Text);
    CHECK(text.contains("directory missing"));
}

TEST_CASE("agentSetup.malformedHookFilesAreKept", "[contour][cli][agent]")
{
    using contour::cli::HookMergeResult;
    auto fs = core::platform::testing::InMemoryFileSystem {
        { .path = std::string { ClaudeSettings }, .content = "{ nope" },
        { .path = std::string { CodexHooks }, .content = "[1]" }
    };
    auto const env = core::testing::FakeEnvironment {};

    auto const outcomes = setupWithHooks(fs, env, HookAction::Install);
    CHECK(outcomeOf(outcomes, "claude-code").hooks->result == HookMergeResult::Malformed);
    CHECK(outcomeOf(outcomes, "codex").hooks->result == HookMergeResult::Malformed);
    CHECK(fs.readFile(ClaudeSettings).value() == "{ nope");
    CHECK(fs.readFile(CodexHooks).value() == "[1]");
}

namespace
{

/// A disk on which a symlink resolves, for weaklyCanonical, as a real one's does; and that records every
/// file written in place.
struct LinkedDisk final: vthost::testing::ForwardingFileSystem
{
    std::map<std::filesystem::path, std::filesystem::path> links;
    mutable std::vector<std::filesystem::path> truncated;

    [[nodiscard]] std::filesystem::path weaklyCanonical(std::filesystem::path const& path) const override
    {
        if (auto const link = links.find(path); link != links.end())
            return link->second;
        return files.weaklyCanonical(path);
    }

    [[nodiscard]] std::expected<void, std::string> writeFile(std::filesystem::path const& path,
                                                             std::string_view content) const override
    {
        truncated.push_back(path);
        return files.writeFile(path, content);
    }
};

} // namespace

TEST_CASE("agentSetup.replacesFilesDurablyAndThroughTheirSymlinks", "[contour][cli][agent]")
{
    auto disk = LinkedDisk {};
    auto const target = std::filesystem::path { "/h/dotfiles/claude-settings.json" };
    disk.files.addDirectory("/h/.claude");
    disk.files.addDirectory("/h/.codex");
    disk.files.addFile(target, R"({"model":"x"})");
    disk.files.addSymlink(std::filesystem::path { ClaudeSettings }, target);
    disk.links.emplace(std::filesystem::path { ClaudeSettings }, target);
    auto const env = core::testing::FakeEnvironment {};

    (void) setupWithHooks(disk, env, HookAction::Install);

    // The link is kept and its target edited, the user's setting with ours.
    CHECK(disk.files.isSymlink(std::filesystem::path { ClaudeSettings }));
    auto const edited = disk.files.readFile(target).value();
    CHECK(edited.contains(R"("model")"));
    CHECK(edited.contains("agent report --agent claude-code"));
    // And nothing was truncated in place: every write went to a temporary, which then replaced the file.
    REQUIRE_FALSE(disk.truncated.empty());
    CHECK(std::ranges::all_of(disk.truncated, [](std::filesystem::path const& path) {
        return path.filename().string().starts_with('.') && path.filename().string().ends_with(".tmp");
    }));
}

TEST_CASE("agentSetup.hookExecutablePrefersTheAppImage", "[contour][cli][agent]")
{
    auto asked = 0;
    auto const running = [&]() -> std::expected<std::filesystem::path, std::string> {
        ++asked;
        return std::filesystem::path { "/tmp/.mount_ContouXXXX/usr/bin/contour" };
    };

    // Under an AppImage the binary's mount vanishes on exit: the hook runs the AppImage itself.
    auto const appImage =
        core::testing::FakeEnvironment { { { "APPIMAGE", "/home/u/Apps/Contour-x86_64.AppImage" } } };
    CHECK(contour::cli::hookExecutable(appImage, running)
          == std::filesystem::path { "/home/u/Apps/Contour-x86_64.AppImage" });
    CHECK(asked == 0);

    CHECK(contour::cli::hookExecutable(core::testing::FakeEnvironment {}, running)
          == std::filesystem::path { "/tmp/.mount_ContouXXXX/usr/bin/contour" });
    CHECK(contour::cli::hookExecutable(core::testing::FakeEnvironment { { { "APPIMAGE", "" } } }, running)
              .has_value());
    CHECK(asked == 2);

    auto const unknown = []() -> std::expected<std::filesystem::path, std::string> {
        return std::unexpected(std::string { "no /proc" });
    };
    CHECK(contour::cli::hookExecutable(core::testing::FakeEnvironment {}, unknown).error() == "no /proc");
}

TEST_CASE("setup reports its outcomes as text and JSON", "[contour][cli][agent]")
{
    auto const outcomes = std::vector<AgentOutcome> {
        { .agent = "claude-code",
          .selection = AgentSelection::Detected,
          .configDir = "/h/.claude",
          .skill = contour::cli::SkillFileOutcome { .path = "/h/.claude/skills/contour/SKILL.md",
                                                    .state = SkillState::Created },
          .hooks = std::nullopt },
    };

    CHECK(contour::cli::formatSkillOutcomes(
              outcomes, SetupMode::Write, HookAction::Install, contour::cli::OutcomeFormat::Text)
          == "Claude Code  skill  created  /h/.claude/skills/contour/SKILL.md\n");
    CHECK(contour::cli::formatSkillOutcomes(
              outcomes, SetupMode::DryRun, HookAction::Install, contour::cli::OutcomeFormat::Text)
          == "Claude Code  skill  created (dry run)  /h/.claude/skills/contour/SKILL.md\n");
    CHECK(
        contour::cli::formatSkillOutcomes(
            outcomes, SetupMode::Write, HookAction::Install, contour::cli::OutcomeFormat::Json)
        == R"([{"agent":"claude-code","selection":"detected","skill":{"path":"/h/.claude/skills/contour/SKILL.md","state":"created"}}])"
           "\n");
    CHECK(contour::cli::parseAgentList("claude-code,,agents")
          == std::vector<std::string> { "claude-code", "agents" });
    CHECK(contour::cli::parseAgentList("").empty());
}
