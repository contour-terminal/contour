// SPDX-License-Identifier: Apache-2.0
#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>

#include <orchestrator/core/DashboardKeys.hpp>
#include <orchestrator/view/Keys.hpp>
#include <orchestrator/view/Sections.hpp>

using namespace std::string_view_literals;
using namespace orchestrator::view;
using core::tui::KeyCode;
using core::tui::KeyEvent;
using core::tui::Modifier;

namespace
{

[[nodiscard]] KeyEvent character(char32_t codepoint, Modifier modifiers = Modifier::None)
{
    return KeyEvent { .key = static_cast<KeyCode>(codepoint),
                      .modifiers = modifiers,
                      .codepoint = codepoint };
}

[[nodiscard]] KeyEvent special(KeyCode code)
{
    return KeyEvent { .key = code, .modifiers = Modifier::None, .codepoint = 0 };
}

} // namespace

TEST_CASE("keyRows: every row's command is dispatched", "[orchestrator][view]")
{
    CHECK(std::ranges::all_of(keyRows(), [](KeyRow const& row) { return isDispatched(row.command); }));
}

TEST_CASE("keyRows: no review or transcript key yet", "[orchestrator][view]")
{
    for (auto const key: { "f"sv, "r"sv, "t"sv })
    {
        CAPTURE(key);
        CHECK(std::ranges::find(keyRows(), key, &KeyRow::key) == keyRows().end());
    }
}

TEST_CASE("commandFor names keys by the table", "[orchestrator][view]")
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
        Case { .key = special(KeyCode::Delete), .expected = KeyCommand::Dismiss },
        Case { .key = character(U'R', Modifier::Shift), .expected = KeyCommand::Refresh },
        Case { .key = character(U'j', Modifier::Ctrl), .expected = std::nullopt },
        Case { .key = character(U'q'), .expected = std::nullopt },
    };
    for (auto const& [key, expected]: cases)
    {
        CAPTURE(static_cast<uint32_t>(key.key));
        CHECK(commandFor(key, Screen::Dashboard) == expected);
    }
}

TEST_CASE("a run command acts only on a row that offers it", "[orchestrator][view]")
{
    auto const agent =
        Row { .kind = RowKind::Agent, .run = "pr-1-20261113-150847", .offers = { KeyCommand::EndRun } };
    auto interaction = Interaction {};
    auto const context = KeyContext { .band = Band::Wide, .selectable = 1, .selected = &agent };
    CHECK(dispatch(KeyCommand::EndRun, interaction, context) == std::nullopt); // asks first
    CHECK(dispatch(KeyCommand::EndRun, interaction, context)
          == std::optional<UiAction> { EndRunAction { .run = "pr-1-20261113-150847" } });
    CHECK(dispatch(KeyCommand::RetryRemoval, interaction, context) == std::nullopt);
    CHECK(dispatch(KeyCommand::Dismiss, interaction, context) == std::nullopt);
    CHECK(dispatch(KeyCommand::EndRun, interaction, KeyContext {}) == std::nullopt);
}

TEST_CASE("typing a filter", "[orchestrator][view]")
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

TEST_CASE("Del while typing a filter edits nothing and dismisses nothing", "[orchestrator][view]")
{
    auto const finished =
        Row { .kind = RowKind::Finished, .run = "pr-1-20261113-150847", .offers = { KeyCommand::Dismiss } };
    auto interaction = Interaction { .filtering = Filtering::Typing, .filter = "pr" };
    auto const context = KeyContext { .band = Band::Wide, .selectable = 1, .selected = &finished };
    CHECK(applyKey(special(KeyCode::Delete), interaction, context) == std::nullopt);
    CHECK(interaction == Interaction { .filtering = Filtering::Typing, .filter = "pr" });
    interaction.filtering = Filtering::Off;
    CHECK(applyKey(special(KeyCode::Delete), interaction, context)
          == std::optional<UiAction> { DismissAction { .run = "pr-1-20261113-150847" } });
}

TEST_CASE("ending a run asks once", "[orchestrator][view]")
{
    auto const agent =
        Row { .kind = RowKind::Agent, .run = "pr-1-20261113-150847", .offers = { KeyCommand::EndRun } };
    auto const context = KeyContext { .band = Band::Wide, .selectable = 2, .selected = &agent };

    auto interaction = Interaction {};
    CHECK(applyKey(character(U'e'), interaction, context) == std::nullopt);
    CHECK(interaction.pendingEnd == "pr-1-20261113-150847");
    CHECK(applyKey(character(U'e'), interaction, context)
          == std::optional<UiAction> { EndRunAction { .run = "pr-1-20261113-150847" } });
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

TEST_CASE("Tab shows details in the Narrow band", "[orchestrator][view]")
{
    // Enter dispatches or reveals; the details are Tab's wherever the band cannot show them beside the list.
    auto const tab = std::ranges::find(keyRows(), "tab"sv, &KeyRow::key);
    REQUIRE(tab != keyRows().end());
    CHECK(isHintedIn(*tab, Band::Medium, Screen::Dashboard));
    CHECK(isHintedIn(*tab, Band::Narrow, Screen::Dashboard));
    CHECK(!isHintedIn(*tab, Band::Wide, Screen::Dashboard));

    auto const item = Row { .kind = RowKind::PullRequest };
    for (auto const band: { Band::Narrow, Band::Medium })
    {
        auto interaction = Interaction {};
        auto const context = KeyContext { .band = band, .selectable = 1, .selected = &item };
        CHECK(dispatch(KeyCommand::Tab, interaction, context) == std::nullopt);
        CHECK(interaction.pane == Pane::Details);
        CHECK(dispatch(KeyCommand::Tab, interaction, context) == std::nullopt);
        CHECK(interaction.pane == Pane::List);
    }
    auto interaction = Interaction {};
    CHECK(dispatch(KeyCommand::Tab, interaction, KeyContext { .band = Band::Wide }) == std::nullopt);
    CHECK(interaction.pane == Pane::List);
}

TEST_CASE("every key the view binds is a dashboard key", "[orchestrator][view]")
{
    // The configuration refuses a task key among dashboardKeys(): a key bound here and missing there would
    // let a task take it, and never fire.
    for (auto const& row: keyRows())
    {
        CAPTURE(row.key);
        CHECK(
            std::ranges::contains(orchestrator::dashboardKeys(), row.key, &orchestrator::DashboardKey::name));
    }
}
