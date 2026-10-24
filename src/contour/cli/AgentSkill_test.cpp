// SPDX-License-Identifier: Apache-2.0
//
// `contour agent setup`: the generated skill and where it is written, over an in-memory file system.

#include <contour/cli/AgentSkill.hpp>
#include <contour/cli/ControlVerbs.hpp>
#include <contour/cli/ExitStatus.hpp>

#include <core/platform/testing/InMemoryFileSystem.hpp>
#include <core/testing/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <vtcontrol/Describe.hpp>

using namespace std::string_literals;
using namespace std::string_view_literals;
using contour::cli::ForeignSkill;
using contour::cli::SetupMode;
using contour::cli::SkillOutcome;
using contour::cli::SkillState;

namespace
{

constexpr auto ClaudeSkill = "/h/.claude/skills/contour/SKILL.md"sv;
constexpr auto AgentsSkill = "/h/.agents/skills/contour/SKILL.md"sv;

/// Sets the agents up under /h with the given text.
std::vector<SkillOutcome> setup(core::platform::testing::InMemoryFileSystem& fs,
                                core::testing::FakeEnvironment const& env,
                                std::string_view text,
                                std::vector<std::string> const& agents = {},
                                SetupMode mode = SetupMode::Write,
                                ForeignSkill foreign = ForeignSkill::Keep)
{
    auto outcomes = contour::cli::setupAgentSkills(fs, env, "/h", agents, mode, foreign, text);
    REQUIRE(outcomes.has_value());
    return *std::move(outcomes);
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
    auto const doc = std::string { std::istreambuf_iterator<char>(docFile), {} };
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
    auto fs = core::platform::testing::InMemoryFileSystem {};
    auto const env = core::testing::FakeEnvironment {};

    auto created = setup(fs, env, "v1");
    REQUIRE(created.size() == 2);
    CHECK(created[0].state == SkillState::Created);
    CHECK(created[0].path == ClaudeSkill);
    CHECK(created[1].path == AgentsSkill);
    CHECK(fs.readFile(ClaudeSkill).value() == "v1");

    // Without the marker the text is foreign, so give the stored text one.
    auto const text = std::string { contour::cli::AgentSkillMarker } + "\nbody";
    auto stored = setup(fs, env, text, {}, SetupMode::Write, ForeignSkill::Replace);
    CHECK(stored[0].state == SkillState::Updated);
    CHECK(setup(fs, env, text)[0].state == SkillState::Unchanged);

    auto const newer = text + " 2";
    CHECK(setup(fs, env, newer)[0].state == SkillState::Updated);
    CHECK(fs.readFile(ClaudeSkill).value() == newer);
}

TEST_CASE("a foreign skill is kept unless forced", "[contour][cli][agent]")
{
    auto fs = core::platform::testing::InMemoryFileSystem { { .path = std::string { ClaudeSkill },
                                                              .content = "mine" } };
    auto const env = core::testing::FakeEnvironment {};
    auto const text = std::string { contour::cli::AgentSkillMarker } + "\nbody";

    CHECK(setup(fs, env, text)[0].state == SkillState::Foreign);
    CHECK(fs.readFile(ClaudeSkill).value() == "mine");

    CHECK(setup(fs, env, text, {}, SetupMode::Write, ForeignSkill::Replace)[0].state == SkillState::Updated);
    CHECK(fs.readFile(ClaudeSkill).value() == text);
}

TEST_CASE("CLAUDE_CONFIG_DIR moves the Claude skill", "[contour][cli][agent]")
{
    auto fs = core::platform::testing::InMemoryFileSystem {};
    auto const env = core::testing::FakeEnvironment { { { "CLAUDE_CONFIG_DIR", "/c" } } };

    auto const outcomes = setup(fs, env, "text", { "claude" });

    REQUIRE(outcomes.size() == 1);
    CHECK(outcomes[0].path == "/c/skills/contour/SKILL.md");
    CHECK(fs.exists("/c/skills/contour/SKILL.md"));
    CHECK_FALSE(fs.exists(ClaudeSkill));
}

TEST_CASE("dry run writes nothing; --agent limits the targets", "[contour][cli][agent]")
{
    auto fs = core::platform::testing::InMemoryFileSystem {};
    auto const env = core::testing::FakeEnvironment {};

    auto const dry = setup(fs, env, "text", {}, SetupMode::DryRun);
    CHECK(dry.size() == 2);
    CHECK(dry[0].state == SkillState::Created);
    CHECK_FALSE(fs.exists(ClaudeSkill));
    CHECK_FALSE(fs.exists(AgentsSkill));

    auto const only = setup(fs, env, "text", { "agents" });
    REQUIRE(only.size() == 1);
    CHECK(only[0].agent == "agents");
    CHECK(fs.exists(AgentsSkill));
    CHECK_FALSE(fs.exists(ClaudeSkill));

    auto const unknown = contour::cli::setupAgentSkills(
        fs, env, "/h", std::vector<std::string> { "emacs" }, SetupMode::Write, ForeignSkill::Keep, "text");
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().contains("emacs"));
    CHECK(unknown.error().contains("claude"));
}

TEST_CASE("setup reports its outcomes as text and JSON", "[contour][cli][agent]")
{
    auto const outcomes = std::vector<SkillOutcome> {
        { .agent = "claude", .path = "/h/.claude/skills/contour/SKILL.md", .state = SkillState::Created },
    };

    CHECK(contour::cli::formatSkillOutcomes(outcomes, SetupMode::Write, contour::cli::OutcomeFormat::Text)
          == "Claude Code  skill  created  /h/.claude/skills/contour/SKILL.md\n");
    CHECK(contour::cli::formatSkillOutcomes(outcomes, SetupMode::DryRun, contour::cli::OutcomeFormat::Text)
          == "Claude Code  skill  created (dry run)  /h/.claude/skills/contour/SKILL.md\n");
    CHECK(contour::cli::formatSkillOutcomes(outcomes, SetupMode::Write, contour::cli::OutcomeFormat::Json)
          == R"([{"agent":"claude","path":"/h/.claude/skills/contour/SKILL.md","state":"created"}])"
             "\n");
    CHECK(contour::cli::parseAgentList("claude,,agents") == std::vector<std::string> { "claude", "agents" });
    CHECK(contour::cli::parseAgentList("").empty());
}
