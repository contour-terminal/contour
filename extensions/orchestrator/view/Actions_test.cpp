// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include <orchestrator/view/Actions.hpp>
#include <orchestrator/view/Fleet.hpp>
#include <orchestrator/view/Keys.hpp>
#include <orchestrator/view/Sections.hpp>
#include <orchestrator/view/ViewFixtures.hpp>

using namespace std::string_view_literals;
using namespace orchestrator;
using namespace orchestrator::view;
using namespace orchestrator::view::test;

namespace
{

constexpr auto RunId = "pr-1-20261113-150847"sv;

/// Every state a row can be in but RowState::None.
constexpr auto RowStates = std::array {
    RowState::Asking,  RowState::Waiting,  RowState::Errored,     RowState::Starting,
    RowState::Working, RowState::Done,     RowState::Ending,      RowState::Finished,
    RowState::Kept,    RowState::NewAgent, RowState::GroupHeader, RowState::Leftover,
};

/// The commands that have no key of their own: only `⏎` runs them, as a state's primary action.
constexpr auto EnterOnly = std::array { KeyCommand::Open, KeyCommand::RunAgain };

[[nodiscard]] Row workingRow()
{
    return Row { .kind = RowKind::Agent,
                 .run = std::string { RunId },
                 .name = "pr-1",
                 .offers = { KeyCommand::TranscriptTab,
                             KeyCommand::DetailsTab,
                             KeyCommand::Fold,
                             KeyCommand::BoardTab,
                             KeyCommand::EndRun },
                 .attention = FleetGroup::Working,
                 .state = "working",
                 .rowState = RowState::Working };
}

} // namespace

TEST_CASE("rowStateOf: what a run is, as far as its actions go", "[orchestrator][view][actions]")
{
    struct Case
    {
        RunState state;
        std::optional<AgentStatus> reported;
        RowState expected;
    };
    auto const cases = std::array {
        Case { .state = Preparing {}, .reported = std::nullopt, .expected = RowState::Starting },
        Case { .state = Running {}, .reported = std::nullopt, .expected = RowState::Starting },
        Case { .state = Running {}, .reported = AgentStatus::Working, .expected = RowState::Working },
        Case { .state = Running {}, .reported = AgentStatus::Idle, .expected = RowState::Waiting },
        Case { .state = Blocked {}, .reported = AgentStatus::Asking, .expected = RowState::Asking },
        Case { .state = Blocked {}, .reported = AgentStatus::Waiting, .expected = RowState::Waiting },
        Case { .state = Blocked {}, .reported = std::nullopt, .expected = RowState::Waiting },
        Case { .state = Blocked {}, .reported = AgentStatus::Error, .expected = RowState::Errored },
        Case { .state = Done {}, .reported = AgentStatus::Done, .expected = RowState::Done },
        Case { .state = Ending {}, .reported = AgentStatus::Done, .expected = RowState::Ending },
        Case { .state = Finished { .fate = Fate::Removed, .entries = {} },
               .reported = std::nullopt,
               .expected = RowState::Finished },
        Case { .state = Finished { .fate = Fate::Kept, .entries = {} },
               .reported = std::nullopt,
               .expected = RowState::Kept },
    };
    for (auto const& [index, row]: std::views::enumerate(cases))
    {
        CAPTURE(index);
        auto run = storedRun(row.state, RunId, ChangeRequestRef { 1 });
        run.lastStatus = row.reported;
        CHECK(rowStateOf(run) == row.expected);
    }
}

TEST_CASE("every state has one primary action, and lists a command once", "[orchestrator][view][actions]")
{
    CHECK(actionsFor(RowState::None).empty());
    for (auto const state: RowStates)
    {
        CAPTURE(std::to_underlying(state));
        auto const actions = actionsFor(state);
        REQUIRE_FALSE(actions.empty());
        for (auto const& [rank, action]: std::views::enumerate(actions))
        {
            CHECK(std::ranges::count(actions, action.command, &ActionRow::command) == 1);
            // A command only `⏎` runs can only be a primary action.
            if (std::ranges::contains(EnterOnly, action.command))
                CHECK(rank == 0);
        }
        CHECK(primaryAction(nullptr) == std::nullopt);
    }
    // Every action is in the table as one run per state.
    auto const states =
        actionRows() | std::views::transform(&ActionRow::when) | std::ranges::to<std::vector>();
    CHECK(std::ranges::is_sorted(
        states, {}, [](RowState state) { return std::ranges::find(RowStates, state) - RowStates.begin(); }));
}

TEST_CASE("every action is named, and has a key unless only ⏎ runs it", "[orchestrator][view][actions]")
{
    for (auto const& action: actionRows())
    {
        CAPTURE(std::to_underlying(action.when), std::to_underlying(action.command));
        CHECK_FALSE(labelOf(action).empty());
        CHECK(std::ranges::contains(commandRows(), action.command, &CommandRow::command));
        if (!std::ranges::contains(EnterOnly, action.command))
            CHECK(std::ranges::contains(keyRows(), action.command, &KeyRow::command));
    }
    for (auto const& command: commandRows())
        CHECK(isDispatched(command.command));
}

TEST_CASE("⏎ does each state's primary action", "[orchestrator][view][actions]")
{
    struct Case
    {
        RowState state;
        KeyCommand primary;
        std::string_view label;
    };
    auto const cases = std::array {
        Case { .state = RowState::Asking, .primary = KeyCommand::Open, .label = "see request" },
        Case { .state = RowState::Waiting, .primary = KeyCommand::Open, .label = "reply" },
        Case { .state = RowState::Errored, .primary = KeyCommand::Open, .label = "reply" },
        Case { .state = RowState::Starting, .primary = KeyCommand::Open, .label = "message" },
        Case { .state = RowState::Working, .primary = KeyCommand::Open, .label = "message" },
        Case { .state = RowState::Done, .primary = KeyCommand::Open, .label = "follow up" },
        Case { .state = RowState::Finished, .primary = KeyCommand::RunAgain, .label = "run again" },
        Case { .state = RowState::Kept, .primary = KeyCommand::RetryRemoval, .label = "retry removal" },
        Case { .state = RowState::NewAgent, .primary = KeyCommand::NewAgent, .label = "new agent" },
        Case { .state = RowState::GroupHeader, .primary = KeyCommand::Fold, .label = "unfold" },
    };
    for (auto const& [state, primary, label]: cases)
    {
        CAPTURE(std::to_underlying(state));
        auto const row = Row { .rowState = state };
        CHECK(primaryAction(&row) == primary);
        CHECK(labelOf(actionsFor(state).front()) == label);
    }
}

TEST_CASE("whyNot names why an action does not apply", "[orchestrator][view][actions]")
{
    auto const working = workingRow();
    CHECK(whyNot(&working, KeyCommand::EndRun, nullptr).empty());
    // Not in the state's table.
    CHECK(whyNot(&working, KeyCommand::Dismiss, nullptr) == "not while it is working");
    // In it, but the row does not offer it.
    CHECK(whyNot(&working, KeyCommand::OpenLink, nullptr) == "it has no link");
    CHECK(whyNot(&working, KeyCommand::Reveal, nullptr) == "its agent has no tab open");
    // Not a run.
    CHECK(whyNot(nullptr, KeyCommand::EndRun, nullptr) == "no run selected");
    auto const heading = Row { .kind = RowKind::Heading, .rowState = RowState::GroupHeader };
    CHECK(whyNot(&heading, KeyCommand::EndRun, nullptr) == "not on a group heading");
    // A key no table lists always applies.
    CHECK(whyNot(nullptr, KeyCommand::Up, nullptr).empty());
    // A global command asks the engine.
    CHECK(whyNot(nullptr, KeyCommand::NewAgent, nullptr).starts_with("the orchestrator is not configured"));
    auto const engine = bootEngine(World {});
    CHECK(whyNot(nullptr, KeyCommand::NewAgent, &engine).empty());
    CHECK(whyNot(nullptr, KeyCommand::InstallPlugins, &engine) == "every plugin the tasks need is installed");

    CHECK(refusalNotice(&working, KeyCommand::Dismiss, nullptr) == "del dismiss · not while it is working");
    CHECK(refusalNotice(nullptr, KeyCommand::RunAgain, nullptr) == "run again · no run selected");
    CHECK(refusalNotice(&working, KeyCommand::EndRun, nullptr).empty());
}

TEST_CASE("the bar: the row's actions in rank order, then ^k all actions", "[orchestrator][view][actions]")
{
    auto const working = workingRow();
    CHECK(actionHints(&working)
          == std::vector<std::string> { "⏎ message", "t transcript", "b board", "e end" });
    CHECK(barText(&working, Place::Fleet, MessageKeys::Elsewhere, 200)
          == "⏎ message · t transcript · b board · e end · n new agent · ^k all actions");
    // Cut between hints, never inside one; the palette always shows.
    CHECK(barText(&working, Place::Fleet, MessageKeys::Elsewhere, 41)
          == "⏎ message · t transcript · ^k all actions");
    CHECK(barText(&working, Place::Fleet, MessageKeys::Elsewhere, 14) == "^k all actions");
    // In the Focus, esc steps back; in the message box, the box's keys.
    CHECK(barText(&working, Place::Focus, MessageKeys::Elsewhere, 200)
          == "⏎ message · t transcript · b board · e end · esc back · n new agent · ^k all actions");
    CHECK(barText(&working, Place::Focus, MessageKeys::InBox, 200)
          == "⏎ send · alt⏎ line · esc keep draft · ^k all actions · pgup pgdn scroll");
    // An action the row does not offer is not hinted: a finished run's board went with its worktree.
    auto finished = Row { .kind = RowKind::Finished,
                          .run = std::string { RunId },
                          .offers = { KeyCommand::TranscriptTab, KeyCommand::DetailsTab },
                          .state = "removed",
                          .rowState = RowState::Finished };
    CHECK(actionHints(&finished) == std::vector<std::string> { "⏎ run again", "t transcript" });
    finished.offers.push_back(KeyCommand::Dismiss);
    CHECK(actionHints(&finished)
          == std::vector<std::string> { "⏎ run again", "t transcript", "del dismiss" });
    // Nothing selected: what starts something.
    CHECK(barText(nullptr, Place::Fleet, MessageKeys::Elsewhere, 200) == "n new agent · ^k all actions");
    auto const newAgent = Row { .kind = RowKind::NewAgent, .rowState = RowState::NewAgent };
    CHECK(barText(&newAgent, Place::Fleet, MessageKeys::Elsewhere, 200) == "⏎ new agent · ^k all actions");
}
