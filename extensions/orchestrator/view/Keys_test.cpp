// SPDX-License-Identifier: Apache-2.0
#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>

#include <orchestrator/view/Keys.hpp>
#include <orchestrator/view/Sections.hpp>
#include <orchestrator/view/ViewFixtures.hpp>

using namespace std::string_view_literals;
using namespace orchestrator;
using namespace orchestrator::view;
using namespace orchestrator::view::test;
using core::tui::KeyCode;
using core::tui::KeyEvent;
using core::tui::Modifier;

namespace
{

constexpr auto RunId = "pr-1-20261113-150847"sv;

[[nodiscard]] KeyEvent character(char32_t codepoint, Modifier modifiers = Modifier::None)
{
    return KeyEvent { .key = static_cast<KeyCode>(codepoint),
                      .modifiers = modifiers,
                      .codepoint = codepoint };
}

[[nodiscard]] KeyEvent special(KeyCode code, Modifier modifiers = Modifier::None)
{
    return KeyEvent { .key = code, .modifiers = modifiers, .codepoint = 0 };
}

/// An engine with one running agent, whose tab is open: its row has a message box.
struct LiveRun
{
    Engine engine = bootEngine(World { .prs = { "github/prs-empty.json" },
                                       .stored = { storedRun(Running {}, RunId, ChangeRequestRef { 1 }) } });
    Row row = Row { .kind = RowKind::Agent,
                    .run = std::string { RunId },
                    .name = "pr-1",
                    .offers = { KeyCommand::BoardTab, KeyCommand::TranscriptTab, KeyCommand::DetailsTab },
                    .attention = FleetGroup::Working,
                    .state = "working",
                    .rowState = RowState::Working };

    [[nodiscard]] KeyContext context(FocusTab tab = FocusTab::Board) const
    {
        return KeyContext { .selectable = 2, .selected = &row, .engine = &engine, .tab = tab };
    }
};

} // namespace

TEST_CASE("keyRows: every row's command is dispatched", "[orchestrator][view][keys]")
{
    CHECK(std::ranges::all_of(keyRows(), [](KeyRow const& row) { return isDispatched(row.command); }));
}

TEST_CASE("the tabs are b, t and d; the old keys are gone", "[orchestrator][view][keys]")
{
    CHECK(commandFor(character(U'b')) == KeyCommand::BoardTab);
    CHECK(commandFor(character(U't')) == KeyCommand::TranscriptTab);
    CHECK(commandFor(character(U'd')) == KeyCommand::DetailsTab);
    for (auto const key: { U'm', U'Z', U'[', U']' })
    {
        CAPTURE(static_cast<uint32_t>(key));
        CHECK(commandFor(character(key)) == std::nullopt);
    }
}

TEST_CASE("commandFor names keys by the table", "[orchestrator][view][keys]")
{
    struct Case
    {
        KeyEvent key = {};
        std::optional<KeyCommand> expected = std::nullopt;
    };
    auto const cases = std::array {
        Case { .key = special(KeyCode::Up), .expected = KeyCommand::Up },
        Case { .key = character(U'k'), .expected = KeyCommand::Up },
        Case { .key = character(U'j'), .expected = KeyCommand::Down },
        Case { .key = special(KeyCode::End), .expected = KeyCommand::End },
        Case { .key = special(KeyCode::Escape), .expected = KeyCommand::Back },
        Case { .key = special(KeyCode::Tab), .expected = KeyCommand::Tab },
        Case { .key = special(KeyCode::Delete), .expected = KeyCommand::Dismiss },
        Case { .key = character(U'o'), .expected = KeyCommand::OpenLink },
        Case { .key = character(U'r'), .expected = KeyCommand::Reveal },
        Case { .key = character(U'n'), .expected = KeyCommand::NewAgent },
        Case { .key = character(U'?'), .expected = KeyCommand::Palette },
        Case { .key = character(U'k', Modifier::Ctrl), .expected = KeyCommand::Palette },
        Case { .key = character(U'R', Modifier::Shift), .expected = std::nullopt },
        Case { .key = character(U'I', Modifier::Shift), .expected = std::nullopt },
        Case { .key = character(U'j', Modifier::Ctrl), .expected = std::nullopt },
        Case { .key = character(U'k', Modifier::Alt), .expected = std::nullopt },
        Case { .key = character(U'q'), .expected = std::nullopt },
    };
    for (auto const& [key, expected]: cases)
    {
        CAPTURE(static_cast<uint32_t>(key.key));
        CHECK(commandFor(key) == expected);
    }
}

TEST_CASE("a run command acts only on a row that offers it", "[orchestrator][view][keys]")
{
    auto const agent = Row { .kind = RowKind::Agent,
                             .run = std::string { RunId },
                             .offers = { KeyCommand::EndRun },
                             .state = "working",
                             .rowState = RowState::Working };
    auto interaction = Interaction {};
    auto const context = KeyContext { .selectable = 1, .selected = &agent };
    CHECK(dispatch(KeyCommand::EndRun, interaction, context) == std::nullopt); // asks first
    CHECK(dispatch(KeyCommand::EndRun, interaction, context)
          == std::optional<UiAction> { EndRunAction { .run = std::string { RunId } } });
    // Every refusal says why on the bar.
    CHECK(dispatch(KeyCommand::RetryRemoval, interaction, context) == std::nullopt);
    CHECK(interaction.notice == "x retry removal · not while it is working");
    CHECK(dispatch(KeyCommand::Dismiss, interaction, context) == std::nullopt);
    CHECK(interaction.notice == "del dismiss · not while it is working");
    CHECK(dispatch(KeyCommand::Reveal, interaction, context) == std::nullopt);
    CHECK(interaction.notice == "r agent tab · its agent has no tab open");
    CHECK(dispatch(KeyCommand::EndRun, interaction, KeyContext {}) == std::nullopt);
    CHECK(interaction.notice == "e end · no run selected");
}

TEST_CASE("typing a filter", "[orchestrator][view][keys]")
{
    auto interaction = Interaction { .filtering = Filtering::Typing };
    CHECK(typeIntoFilter(character(U's'), interaction) == FilterInput::Taken);
    CHECK(typeIntoFilter(character(U'é'), interaction) == FilterInput::Taken);
    CHECK(interaction.filter == "sé");
    CHECK(typeIntoFilter(special(KeyCode::Backspace), interaction) == FilterInput::Taken);
    CHECK(interaction.filter == "s");
    CHECK(typeIntoFilter(special(KeyCode::Up), interaction) == FilterInput::NotTaken);
    CHECK(typeIntoFilter(special(KeyCode::Enter), interaction) == FilterInput::Taken);
    CHECK(interaction == Interaction { .filtering = Filtering::Off, .filter = "s" });
    CHECK(typeIntoFilter(character(U'x'), interaction) == FilterInput::NotTaken);
}

TEST_CASE("Del while typing a filter edits nothing and dismisses nothing", "[orchestrator][view][keys]")
{
    auto const finished = Row { .kind = RowKind::Finished,
                                .run = std::string { RunId },
                                .offers = { KeyCommand::Dismiss },
                                .rowState = RowState::Finished };
    auto interaction = Interaction { .filtering = Filtering::Typing, .filter = "pr" };
    auto const context = KeyContext { .selectable = 1, .selected = &finished };
    CHECK(applyKey(special(KeyCode::Delete), interaction, context) == std::nullopt);
    CHECK(interaction == Interaction { .filtering = Filtering::Typing, .filter = "pr" });
    interaction.filtering = Filtering::Off;
    CHECK(applyKey(special(KeyCode::Delete), interaction, context)
          == std::optional<UiAction> { DismissAction { .run = std::string { RunId } } });
}

TEST_CASE("ending a run asks once", "[orchestrator][view][keys]")
{
    auto const agent = Row { .kind = RowKind::Agent,
                             .run = std::string { RunId },
                             .offers = { KeyCommand::EndRun },
                             .rowState = RowState::Working };
    auto const context = KeyContext { .selectable = 2, .selected = &agent };

    auto interaction = Interaction {};
    CHECK(applyKey(character(U'e'), interaction, context) == std::nullopt);
    CHECK(interaction.pendingEnd == RunId);
    CHECK(applyKey(character(U'e'), interaction, context)
          == std::optional<UiAction> { EndRunAction { .run = std::string { RunId } } });
    CHECK(interaction == Interaction {});

    // Any other key keeps the run, and does nothing else.
    for (auto const& key: { character(U'j'), special(KeyCode::Escape), character(U'R'), character(U'q') })
    {
        CAPTURE(static_cast<uint32_t>(key.key));
        auto asked = Interaction {};
        CHECK(applyKey(character(U'e'), asked, context) == std::nullopt);
        CHECK(applyKey(key, asked, context) == std::nullopt);
        CHECK(asked == Interaction {});
    }
}

TEST_CASE("Enter and Tab give a live run's Focus the keys; Esc and Tab give them back",
          "[orchestrator][view][keys]")
{
    auto const live = LiveRun {};
    auto interaction = Interaction {};
    CHECK(applyKey(special(KeyCode::Enter), interaction, live.context()) == std::nullopt);
    CHECK(interaction.place == Place::Focus);
    CHECK(applyKey(special(KeyCode::Escape), interaction, live.context()) == std::nullopt);
    CHECK(interaction.place == Place::Fleet);
    CHECK(applyKey(special(KeyCode::Tab), interaction, live.context()) == std::nullopt);
    CHECK(interaction.place == Place::Focus);
    CHECK(applyKey(special(KeyCode::Tab), interaction, live.context()) == std::nullopt);
    CHECK(interaction.place == Place::Fleet);
}

TEST_CASE("in the message box no letter is a command: every printable key types",
          "[orchestrator][view][keys]")
{
    auto const live = LiveRun {};
    auto interaction = Interaction { .place = Place::Focus };
    auto typed = std::string {};
    for (auto const& row: keyRows())
    {
        if (row.key.size() != 1)
            continue;
        CAPTURE(row.key);
        auto const key = static_cast<char32_t>(row.key.front());
        CHECK(applyKey(character(key), interaction, live.context()) == std::nullopt);
        typed += row.key;
    }
    // Digits type too: nothing answers while the box has the keys.
    for (auto const digit: { U'1', U'2', U'3' })
    {
        CHECK(applyKey(character(digit), interaction, live.context()) == std::nullopt);
        typed += static_cast<char>(digit);
    }
    CHECK(interaction.place == Place::Focus);
    CHECK(interaction.pendingEnd.empty());
    CHECK(interaction.drafts.at(std::string { RunId }) == typed);
}

TEST_CASE("Esc keeps the draft and sends nothing; Enter sends it and the keys return to the Fleet",
          "[orchestrator][view][keys]")
{
    auto const live = LiveRun {};
    auto interaction = Interaction { .place = Place::Focus };
    for (auto const c: U"rebase")
        if (c != U'\0')
            CHECK(applyKey(character(c), interaction, live.context()) == std::nullopt);
    CHECK(applyKey(special(KeyCode::Enter, Modifier::Alt), interaction, live.context()) == std::nullopt);
    CHECK(applyKey(character(U'x'), interaction, live.context()) == std::nullopt);
    CHECK(interaction.drafts.at(std::string { RunId }) == "rebase\nx");

    CHECK(applyKey(special(KeyCode::Escape), interaction, live.context()) == std::nullopt);
    CHECK(interaction.place == Place::Fleet);
    CHECK(interaction.drafts.at(std::string { RunId }) == "rebase\nx");

    // Back in the box, the draft is there; Enter sends it.
    CHECK(applyKey(special(KeyCode::Enter), interaction, live.context()) == std::nullopt);
    REQUIRE(interaction.place == Place::Focus);
    CHECK(applyKey(special(KeyCode::Enter), interaction, live.context())
          == std::optional<UiAction> {
              SendMessageAction { .run = std::string { RunId }, .text = "rebase\nx" } });
    CHECK(interaction.place == Place::Fleet);
    CHECK_FALSE(interaction.drafts.contains(std::string { RunId }));
    REQUIRE(interaction.sent.has_value());
    CHECK(interaction.sent->text == "rebase\nx");
    CHECK(interaction.notice == "sent to pr-1");
}

TEST_CASE("an empty message box sends nothing", "[orchestrator][view][keys]")
{
    auto const live = LiveRun {};
    auto interaction = Interaction { .place = Place::Focus };
    CHECK(applyKey(special(KeyCode::Enter), interaction, live.context()) == std::nullopt);
    CHECK(interaction.place == Place::Focus);
}

TEST_CASE("a run opens on its group's tab, and a chosen tab sticks until its state changes",
          "[orchestrator][view][keys]")
{
    auto row = Row { .kind = RowKind::Agent,
                     .run = std::string { RunId },
                     .offers = { KeyCommand::BoardTab, KeyCommand::TranscriptTab, KeyCommand::DetailsTab },
                     .attention = FleetGroup::Working,
                     .state = "working",
                     .rowState = RowState::Working };
    auto interaction = Interaction {};
    CHECK(activeTabOf(interaction, &row) == FocusTab::Board);
    row.attention = FleetGroup::NeedsYou;
    CHECK(activeTabOf(interaction, &row) == FocusTab::Transcript);
    row.attention = FleetGroup::Finished;
    CHECK(activeTabOf(interaction, &row) == FocusTab::Details);

    row.attention = FleetGroup::Working;
    auto const context = KeyContext { .selectable = 1, .selected = &row };
    CHECK(dispatch(KeyCommand::DetailsTab, interaction, context) == std::nullopt);
    CHECK(activeTabOf(interaction, &row) == FocusTab::Details);
    // It starts asking: the request shows, though the user left it on Details.
    row.attention = FleetGroup::NeedsYou;
    row.state = "asking";
    CHECK(activeTabOf(interaction, &row) == FocusTab::Transcript);

    // A tab the row does not offer gives way to Details: a removed worktree has no board.
    row.offers = { KeyCommand::TranscriptTab, KeyCommand::DetailsTab };
    row.attention = FleetGroup::Working;
    row.state = "working";
    interaction.chosenTab.reset();
    CHECK(activeTabOf(interaction, &row) == FocusTab::Details);
}

TEST_CASE("the transcript follows the agent until it is scrolled up, and end returns to it",
          "[orchestrator][view][keys]")
{
    auto const live = LiveRun {};
    auto context = live.context(FocusTab::Transcript);
    context.lines = 100;
    context.rows = 20;
    context.pages = 1;
    auto interaction = Interaction { .place = Place::Focus };
    REQUIRE(interaction.follow == Follow::Live);

    CHECK(applyKey(special(KeyCode::PageUp), interaction, context) == std::nullopt);
    CHECK(interaction.follow == Follow::Away);
    CHECK(interaction.scroll == 80 - 19);

    // Scrolled back down to the end, it follows again.
    CHECK(applyKey(special(KeyCode::PageDown), interaction, context) == std::nullopt);
    CHECK(interaction.follow == Follow::Live);

    // From the Fleet, too: pgup scrolls the Focus's tab.
    interaction.place = Place::Fleet;
    CHECK(applyKey(special(KeyCode::PageUp), interaction, context) == std::nullopt);
    CHECK(interaction.follow == Follow::Away);
    CHECK(applyKey(character(U'k'), interaction, context)
          == std::nullopt); // moves the selection, not the tab
    CHECK(interaction.follow == Follow::Away);

    interaction.place = Place::Focus;
    CHECK(applyKey(special(KeyCode::End), interaction, context) == std::nullopt);
    CHECK(interaction.follow == Follow::Live);
    CHECK(interaction.scroll == 0);
}

TEST_CASE("left turns the transcript to an earlier page, which stops following; its last follows again",
          "[orchestrator][view][keys]")
{
    auto const live = LiveRun {};
    auto context = live.context(FocusTab::Transcript);
    context.pages = 3;
    auto interaction = Interaction { .place = Place::Focus };
    CHECK(applyKey(special(KeyCode::Left), interaction, context) == std::nullopt);
    CHECK(interaction.page == 2);
    CHECK(interaction.follow == Follow::Away);
    CHECK(applyKey(special(KeyCode::Right), interaction, context) == std::nullopt);
    CHECK(interaction.page == 0);
    CHECK(interaction.follow == Follow::Live);
}

TEST_CASE("o asks to open the run's link", "[orchestrator][view][keys]")
{
    auto live = LiveRun {};
    live.row.offers.push_back(KeyCommand::OpenLink);
    auto interaction = Interaction {};
    CHECK(applyKey(character(U'o'), interaction, live.context())
          == UiAction {
              OpenUrlAction { .url = "https://github.com/contour-terminal/contour/pull/1", .label = "#1" } });
}
