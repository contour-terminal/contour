// SPDX-License-Identifier: Apache-2.0
//
// The text edits `contour agent setup` makes to Claude Code's settings.json and Codex's hooks.json.

#include <contour/cli/AgentHookConfig.hpp>
#include <contour/cli/AgentIntegrations.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

using namespace std::string_literals;
using namespace std::string_view_literals;
using contour::cli::HookMergeResult;

namespace
{

constexpr auto Exe = "/opt/contour/bin/contour"sv;

contour::cli::AgentIntegration const& claudeRow()
{
    return contour::cli::integrationOf(contour::cli::AgentId::ClaudeCode);
}

contour::cli::AgentIntegration const& codexRow()
{
    return contour::cli::integrationOf(contour::cli::AgentId::Codex);
}

glz::generic parse(std::string const& json)
{
    auto document = glz::generic {};
    REQUIRE_FALSE(bool(glz::read_json(document, json)));
    return document;
}

glz::generic const& at(glz::generic const& node, std::string_view key)
{
    auto const* object = node.get_if<glz::generic::object_t>();
    REQUIRE(object != nullptr);
    auto const found = object->find(key);
    REQUIRE(found != object->end());
    return found->second;
}

glz::generic const& at(glz::generic const& node, std::size_t index)
{
    auto const* array = node.get_if<glz::generic::array_t>();
    REQUIRE(array != nullptr);
    REQUIRE(index < array->size());
    return (*array)[index];
}

std::size_t count(glz::generic const& node)
{
    if (auto const* array = node.get_if<glz::generic::array_t>())
        return array->size();
    if (auto const* object = node.get_if<glz::generic::object_t>())
        return object->size();
    return 0;
}

std::string commandOf(glz::generic const& hook)
{
    return *at(hook, "command").get_if<std::string>();
}

} // namespace

TEST_CASE("claudeHooks.mergeIntoEmpty", "[contour][cli][agent]")
{
    auto const merged = contour::cli::mergeHooks(claudeRow(), "", Exe);
    REQUIRE(merged.result == HookMergeResult::Changed);
    auto const document = parse(merged.text);
    auto const command = commandOf(at(at(at(at(at(document, "hooks"), "Stop"), 0), "hooks"), 0));
    CHECK(command.ends_with("agent report --agent claude-code"));
    CHECK(command.starts_with(Exe));
    // One group per event the table lists.
    auto const& events = contour::cli::integrationOf(contour::cli::AgentId::ClaudeCode).hookEvents;
    CHECK(count(at(document, "hooks")) == events.size());
    for (auto const event: events)
        CHECK(count(at(at(document, "hooks"), event)) == 1);
    CHECK(merged.text.contains("\n  \"hooks\""));
}

TEST_CASE("claudeHooks.keepsForeignHooks", "[contour][cli][agent]")
{
    auto const input =
        R"({"model":"opus","hooks":{"PreToolUse":[{"matcher":"Bash","hooks":[{"type":"command","command":"lint.sh"}]}]}})"s;
    auto const merged = contour::cli::mergeHooks(claudeRow(), input, Exe);
    REQUIRE(merged.result == HookMergeResult::Changed);
    auto const document = parse(merged.text);
    CHECK(*at(document, "model").get_if<std::string>() == "opus");
    auto const& groups = at(at(document, "hooks"), "PreToolUse");
    REQUIRE(count(groups) == 2);
    CHECK(commandOf(at(at(at(groups, 0), "hooks"), 0)) == "lint.sh");
    CHECK(commandOf(at(at(at(groups, 1), "hooks"), 0)).ends_with("agent report --agent claude-code"));
}

TEST_CASE("claudeHooks.idempotent", "[contour][cli][agent]")
{
    auto const first = contour::cli::mergeHooks(claudeRow(), "", Exe);
    auto const second = contour::cli::mergeHooks(claudeRow(), first.text, Exe);
    CHECK(second.result == HookMergeResult::Unchanged);
    CHECK(second.text == first.text);
}

TEST_CASE("claudeHooks.movedBinaryIsUpdatedInPlace", "[contour][cli][agent]")
{
    auto const first = contour::cli::mergeHooks(claudeRow(), "", "/old/contour");
    auto const second = contour::cli::mergeHooks(claudeRow(), first.text, Exe);
    REQUIRE(second.result == HookMergeResult::Changed);
    auto const document = parse(second.text);
    auto const& groups = at(at(document, "hooks"), "Stop");
    REQUIRE(count(groups) == 1);
    CHECK(commandOf(at(at(at(groups, 0), "hooks"), 0)).starts_with(Exe));
}

TEST_CASE("claudeHooks.quotesAnUnsafePath", "[contour][cli][agent]")
{
    auto const merged = contour::cli::mergeHooks(claudeRow(), "", "/my apps/contour");
    auto const document = parse(merged.text);
    CHECK(commandOf(at(at(at(at(at(document, "hooks"), "Stop"), 0), "hooks"), 0))
          == "'/my apps/contour' agent report --agent claude-code");
}

TEST_CASE("claudeHooks.malformed", "[contour][cli][agent]")
{
    for (auto const input: { "{ hooks: "sv, "[1]"sv, R"({"hooks":[]})"sv, R"({"hooks":{"Stop":3}})"sv })
    {
        INFO(input);
        auto const merged = contour::cli::mergeHooks(claudeRow(), input, Exe);
        CHECK(merged.result == HookMergeResult::Malformed);
        CHECK(merged.text.empty());
    }
    CHECK(contour::cli::removeHooks(claudeRow(), "{ hooks: ").result == HookMergeResult::Malformed);
}

TEST_CASE("claudeHooks.remove", "[contour][cli][agent]")
{
    auto const foreign =
        R"({"hooks":{"PreToolUse":[{"matcher":"Bash","hooks":[{"type":"command","command":"lint.sh"}]}]}})"s;
    auto const installed = contour::cli::mergeHooks(claudeRow(), foreign, Exe);
    auto const removed = contour::cli::removeHooks(claudeRow(), installed.text);
    REQUIRE(removed.result == HookMergeResult::Changed);
    auto const document = parse(removed.text);
    auto const& hooks = at(document, "hooks");
    REQUIRE(count(hooks) == 1); // the event arrays that held only ours are gone
    auto const& groups = at(hooks, "PreToolUse");
    REQUIRE(count(groups) == 1);
    CHECK(commandOf(at(at(at(groups, 0), "hooks"), 0)) == "lint.sh");

    CHECK(contour::cli::removeHooks(claudeRow(), removed.text).result == HookMergeResult::Unchanged);
    CHECK(contour::cli::removeHooks(claudeRow(), "").result == HookMergeResult::Unchanged);

    // Nothing of the user's left: the hooks object goes too.
    auto const alone = contour::cli::removeHooks(
        claudeRow(), contour::cli::mergeHooks(claudeRow(), R"({"model":"x"})", Exe).text);
    auto const aloneDocument = parse(alone.text);
    CHECK(count(aloneDocument) == 1);
}

TEST_CASE("codexHooks.mergeIntoEmpty", "[contour][cli][agent]")
{
    auto const merged = contour::cli::mergeHooks(codexRow(), "", Exe);
    REQUIRE(merged.result == HookMergeResult::Changed);
    auto const document = parse(merged.text);
    auto const& hooks = at(document, "hooks");
    CHECK(count(hooks) == codexRow().hookEvents.size());
    for (auto const event: codexRow().hookEvents)
    {
        auto const& groups = at(hooks, event);
        REQUIRE(count(groups) == 1);
        CHECK(at(at(groups, 0), "matcher").get_if<std::string>()->empty());
        auto const& hook = at(at(at(groups, 0), "hooks"), 0);
        CHECK(*at(hook, "type").get_if<std::string>() == "command");
        CHECK(commandOf(hook) == "/opt/contour/bin/contour agent report --agent codex");
        CHECK(*at(hook, "timeout").get_if<double>() == 5.0);
    }
    CHECK(count(hooks) == 7);
}

TEST_CASE("codexHooks.keepsForeignAndIsIdempotent", "[contour][cli][agent]")
{
    auto const input =
        R"({"hooks":{"Stop":[{"matcher":"","hooks":[{"type":"command","command":"notify-send done"}]}]},"x":1})"s;
    auto const merged = contour::cli::mergeHooks(codexRow(), input, Exe);
    REQUIRE(merged.result == HookMergeResult::Changed);
    auto const document = parse(merged.text);
    CHECK(count(document) == 2);
    auto const& groups = at(at(document, "hooks"), "Stop");
    REQUIRE(count(groups) == 2);
    CHECK(commandOf(at(at(at(groups, 0), "hooks"), 0)) == "notify-send done");
    CHECK(commandOf(at(at(at(groups, 1), "hooks"), 0)).ends_with("--agent codex"));

    auto const again = contour::cli::mergeHooks(codexRow(), merged.text, Exe);
    CHECK(again.result == HookMergeResult::Unchanged);
    CHECK(again.text == merged.text);
}

TEST_CASE("codexHooks.theTwoAgentsDoNotTouchEachOthersEntries", "[contour][cli][agent]")
{
    auto const both =
        contour::cli::mergeHooks(codexRow(), contour::cli::mergeHooks(claudeRow(), "", Exe).text, Exe);
    REQUIRE(both.result == HookMergeResult::Changed);
    auto const removed = contour::cli::removeHooks(codexRow(), both.text);
    REQUIRE(removed.result == HookMergeResult::Changed);
    CHECK(removed.text.contains("--agent claude-code"));
    CHECK_FALSE(removed.text.contains("--agent codex"));
}

TEST_CASE("codexHooks.malformed", "[contour][cli][agent]")
{
    for (auto const input: { "{ hooks: "sv, "[1]"sv, R"({"hooks":[]})"sv, R"({"hooks":{"Stop":3}})"sv })
    {
        INFO(input);
        auto const merged = contour::cli::mergeHooks(codexRow(), input, Exe);
        CHECK(merged.result == HookMergeResult::Malformed);
        CHECK(merged.text.empty());
    }
    CHECK(contour::cli::removeHooks(codexRow(), "{ hooks: ").result == HookMergeResult::Malformed);
}

TEST_CASE("codexHooks.remove", "[contour][cli][agent]")
{
    auto const foreign =
        R"({"hooks":{"Stop":[{"matcher":"","hooks":[{"type":"command","command":"keep.sh"}]}]}})"s;
    auto const removed =
        contour::cli::removeHooks(codexRow(), contour::cli::mergeHooks(codexRow(), foreign, Exe).text);
    REQUIRE(removed.result == HookMergeResult::Changed);
    auto const document = parse(removed.text);
    REQUIRE(count(at(document, "hooks")) == 1);
    CHECK(commandOf(at(at(at(at(at(document, "hooks"), "Stop"), 0), "hooks"), 0)) == "keep.sh");
    CHECK(contour::cli::removeHooks(codexRow(), removed.text).result == HookMergeResult::Unchanged);
    CHECK(contour::cli::removeHooks(codexRow(), "").result == HookMergeResult::Unchanged);

    auto const alone =
        contour::cli::removeHooks(codexRow(), contour::cli::mergeHooks(codexRow(), "", Exe).text);
    REQUIRE(alone.result == HookMergeResult::Changed);
    CHECK(count(parse(alone.text)) == 0);
}

TEST_CASE("claudeHooks.removeLeavesTheUsersEmptyEntries", "[contour][cli][agent]")
{
    // An empty group and an empty event array the user wrote are theirs, not leftovers of ours.
    auto const before =
        R"({"hooks":{"Stop":[{"matcher":"","hooks":[]}],"PreCompact":[],"Notification":[{"matcher":"x","hooks":[]}]}})"s;
    auto const installed = contour::cli::mergeHooks(claudeRow(), before, Exe);
    auto const removed = contour::cli::removeHooks(claudeRow(), installed.text);
    REQUIRE(removed.result == HookMergeResult::Changed);
    auto const document = parse(removed.text);
    auto const& hooks = at(document, "hooks");
    CHECK(count(at(hooks, "PreCompact")) == 0);
    REQUIRE(count(at(hooks, "Stop")) == 1);
    CHECK(count(at(at(at(hooks, "Stop"), 0), "hooks")) == 0);
    REQUIRE(count(at(hooks, "Notification")) == 1);
    CHECK(count(at(at(at(hooks, "Notification"), 0), "hooks")) == 0);
    CHECK(count(hooks) == 3); // and no event array of ours is left
}

TEST_CASE("claudeHooks.aWrapperMentioningOurCommandIsTheUsers", "[contour][cli][agent]")
{
    // Only the very command ourHook() writes is ours: one binary word, then the marker.
    auto const wrapped =
        R"({"hooks":{"Stop":[{"matcher":"","hooks":[)"
        R"({"type":"command","command":"my-log && contour agent report --agent claude-code"},)"
        R"({"type":"command","command":"contour agent report --agent claude-code --verbose"},)"
        R"({"type":"command","command":"'/a b/contour' 'x' agent report --agent claude-code"}]}]}})"s;
    CHECK(contour::cli::removeHooks(claudeRow(), wrapped).result == HookMergeResult::Unchanged);
    auto const merged = contour::cli::mergeHooks(claudeRow(), wrapped, Exe);
    auto const document = parse(merged.text);
    CHECK(count(at(at(at(at(document, "hooks"), "Stop"), 0), "hooks")) == 3); // theirs, untouched
    CHECK(count(at(at(document, "hooks"), "Stop")) == 2);                     // ours in a group of its own

    // What it writes, plain or quoted, it recognizes again.
    for (auto const exe: { "/opt/contour/bin/contour"sv, "/opt/my apps/it's/contour"sv })
    {
        auto const installed = contour::cli::mergeHooks(claudeRow(), "", exe);
        CHECK(contour::cli::mergeHooks(claudeRow(), installed.text, exe).result
              == HookMergeResult::Unchanged);
        CHECK_FALSE(contour::cli::removeHooks(claudeRow(), installed.text).text.contains("agent report"));
    }
}
