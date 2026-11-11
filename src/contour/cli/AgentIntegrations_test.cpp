// SPDX-License-Identifier: Apache-2.0
//
// `contour agent report`'s mapping of Claude Code and Codex hook payloads onto pane status, one case per row.

#include <contour/cli/AgentIntegrations.hpp>

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <string>
#include <string_view>
#include <variant>

using namespace std::string_view_literals;
using contour::cli::HookOutcome;
using contour::cli::StatusClear;
using contour::cli::StatusReport;

namespace
{

HookOutcome map(std::string_view integrationId, std::string_view json)
{
    auto const* integration = contour::cli::findIntegration(integrationId);
    REQUIRE(integration != nullptr);
    auto document = glz::generic {};
    REQUIRE_FALSE(bool(glz::read_json(document, json)));
    return integration->map(document);
}

HookOutcome claude(std::string_view json)
{
    return map("claude-code", json);
}

void checkReport(HookOutcome const& outcome,
                 std::string_view state,
                 std::string_view kind,
                 std::string_view message)
{
    auto const* report = std::get_if<StatusReport>(&outcome);
    REQUIRE(report != nullptr);
    CHECK(report->state == state);
    CHECK(report->kind == kind);
    CHECK(report->message == message);
}

} // namespace

TEST_CASE("agent table: rows are found by id", "[contour][cli][agent]")
{
    CHECK(contour::cli::findIntegration("claude-code") != nullptr);
    CHECK(contour::cli::findIntegration("codex") != nullptr);
    CHECK(contour::cli::findIntegration("nope") == nullptr);
    for (auto const& row: contour::cli::agentIntegrations())
        CHECK(&contour::cli::integrationOf(row.id) == &row);
    CHECK_FALSE(contour::cli::integrationOf(contour::cli::AgentId::ClaudeCode).hookEvents.empty());
    CHECK_FALSE(contour::cli::integrationOf(contour::cli::AgentId::Codex).hookEvents.empty());
}

TEST_CASE("claude: prompt and tool results are working", "[contour][cli][agent]")
{
    checkReport(claude(R"({"hook_event_name":"UserPromptSubmit","prompt":"hi"})"), "working", "", "");
    checkReport(claude(R"({"hook_event_name":"PostToolUse","tool_name":"Bash"})"), "working", "", "");
}

TEST_CASE("claude: PreToolUse phrases the tool", "[contour][cli][agent]")
{
    checkReport(
        claude(
            R"({"hook_event_name":"PreToolUse","tool_name":"Edit","tool_input":{"file_path":"src/db/schema.rs"}})"),
        "working",
        "",
        "Editing src/db/schema.rs");
    checkReport(
        claude(R"({"hook_event_name":"PreToolUse","tool_name":"Bash","tool_input":{"command":"make test"}})"),
        "working",
        "",
        "Running: make test");
    checkReport(
        claude(R"({"hook_event_name":"PreToolUse","tool_name":"Read","tool_input":{"file_path":"a.txt"}})"),
        "working",
        "",
        "Reading a.txt");
    checkReport(claude(R"({"hook_event_name":"PreToolUse","tool_name":"WebFetch","tool_input":{"url":"x"}})"),
                "working",
                "",
                "WebFetch");
}

TEST_CASE("claude: a permission request blocks", "[contour][cli][agent]")
{
    checkReport(
        claude(
            R"({"hook_event_name":"PermissionRequest","tool_name":"Bash","tool_input":{"command":"psql $DEV_URL -f x.sql"}})"),
        "blocked",
        "permission",
        "Bash: psql $DEV_URL -f x.sql");
}

TEST_CASE("claude: notifications", "[contour][cli][agent]")
{
    checkReport(
        claude(
            R"({"hook_event_name":"Notification","notification_type":"permission_prompt","message":"Claude needs your permission to use Bash"})"),
        "blocked",
        "permission",
        "Claude needs your permission to use Bash");
    checkReport(
        claude(
            R"({"hook_event_name":"Notification","notification_type":"elicitation_dialog","message":"Which database?"})"),
        "blocked",
        "question",
        "Which database?");
    CHECK(std::holds_alternative<std::monostate>(claude(
        R"({"hook_event_name":"Notification","notification_type":"idle_prompt","message":"Claude is waiting for your input"})")));
    for (auto const type: { "agent_needs_input"sv, "elicitation_url_dialog"sv })
        checkReport(
            claude(std::format(
                R"({{"hook_event_name":"Notification","notification_type":"{}","message":"Pick one"}})",
                type)),
            "blocked",
            "question",
            "Pick one");
    // Not worth showing: other types, and no type at all.
    for (auto const type: { "auth_success"sv, "agent_completed"sv, "elicitation_complete"sv, ""sv })
        CHECK(std::holds_alternative<std::monostate>(claude(std::format(
            R"({{"hook_event_name":"Notification","notification_type":"{}","message":"Claude needs your permission"}})",
            type))));
    CHECK(std::holds_alternative<std::monostate>(claude(
        R"({"hook_event_name":"Notification","message":"Claude needs your permission to use Bash"})")));
}

TEST_CASE("claude: failures, elicitation", "[contour][cli][agent]")
{
    checkReport(claude(R"({"hook_event_name":"PostToolUseFailure","tool_name":"Bash"})"), "working", "", "");
    checkReport(claude(R"({"hook_event_name":"StopFailure","error":"rate_limit"})"), "error", "", "");
    checkReport(claude(R"({"hook_event_name":"Elicitation","message":"Which scope?"})"),
                "blocked",
                "question",
                "Which scope?");
}

TEST_CASE("claude: Stop is done, with the first line when there is one", "[contour][cli][agent]")
{
    checkReport(claude(R"({"hook_event_name":"Stop"})"), "done", "", "");
    checkReport(claude(R"({"hook_event_name":"Stop","last_assistant_message":"\nAll green\nmore"})"),
                "done",
                "",
                "All green");
}

TEST_CASE("claude: SessionEnd clears, the unknown is ignored", "[contour][cli][agent]")
{
    CHECK(std::holds_alternative<StatusClear>(claude(R"({"hook_event_name":"SessionEnd"})")));
    CHECK(std::holds_alternative<std::monostate>(claude(R"({"hook_event_name":"SomethingNew"})")));
    CHECK(std::holds_alternative<std::monostate>(claude(R"({})")));
    CHECK(std::holds_alternative<std::monostate>(claude(R"([1,2])")));
}

namespace
{
HookOutcome codex(std::string_view json)
{
    return map("codex", json);
}
} // namespace

TEST_CASE("codex: prompt and tool results are working", "[contour][cli][agent]")
{
    checkReport(codex(R"({"hook_event_name":"UserPromptSubmit","prompt":"hi"})"), "working", "", "");
    checkReport(codex(R"({"hook_event_name":"PostToolUse","tool_name":"Bash"})"), "working", "", "");
}

TEST_CASE("codex: PreToolUse phrases the tool like Claude", "[contour][cli][agent]")
{
    checkReport(
        codex(R"({"hook_event_name":"PreToolUse","tool_name":"Bash","tool_input":{"command":"make test"}})"),
        "working",
        "",
        "Running: make test");
    checkReport(
        codex(R"({"hook_event_name":"PreToolUse","tool_name":"apply_patch"})"), "working", "", "apply_patch");
}

TEST_CASE("codex: a permission request blocks", "[contour][cli][agent]")
{
    checkReport(
        codex(
            R"({"hook_event_name":"PermissionRequest","tool_name":"Bash","tool_input":{"command":"rm -rf build"}})"),
        "blocked",
        "permission",
        "Bash: rm -rf build");
}

TEST_CASE("codex: Stop is done with the first line, Interrupt is idle, SessionEnd clears",
          "[contour][cli][agent]")
{
    checkReport(codex(R"({"hook_event_name":"Stop","last_assistant_message":"All 214 tests pass\nmore"})"),
                "done",
                "",
                "All 214 tests pass");
    checkReport(codex(R"({"hook_event_name":"Stop"})"), "done", "", "");
    checkReport(codex(R"({"hook_event_name":"Interrupt"})"), "idle", "", "");
    CHECK(std::holds_alternative<StatusClear>(codex(R"({"hook_event_name":"SessionEnd"})")));
}

TEST_CASE("codex: other events and the old notify payload are ignored", "[contour][cli][agent]")
{
    CHECK(std::holds_alternative<std::monostate>(codex(R"({"hook_event_name":"PreModelRequest"})")));
    CHECK(std::holds_alternative<std::monostate>(codex(R"({"hook_event_name":"SubagentStop"})")));
    CHECK(std::holds_alternative<std::monostate>(
        codex(R"({"type":"agent-turn-complete","last-assistant-message":"x"})")));
}
