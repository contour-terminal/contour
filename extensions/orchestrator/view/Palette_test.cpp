// SPDX-License-Identifier: Apache-2.0
#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/view/Actions.hpp>
#include <orchestrator/view/Keys.hpp>
#include <orchestrator/view/Palette.hpp>
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

[[nodiscard]] KeyEvent special(KeyCode code)
{
    return KeyEvent { .key = code, .modifiers = Modifier::None, .codepoint = 0 };
}

/// @return The key event the palette names @p key by: `⏎`, `del`, or a character.
[[nodiscard]] KeyEvent eventOf(std::string_view key)
{
    if (key == "⏎")
        return special(KeyCode::Enter);
    if (key == "del")
        return special(KeyCode::Delete);
    return character(static_cast<char32_t>(key.front()));
}

/// @return The row the Fleet builds for the run RunId.
[[nodiscard]] Row runRowOf(Engine const& engine)
{
    auto const rows = buildRows(engine, StartMonoMs);
    return *std::ranges::find(rows, RunId, &Row::run);
}

/// An engine with one running agent whose tab is open, and its row as the Fleet builds it.
struct LiveRun
{
    Engine engine = bootEngine(World { .prs = { "github/prs-empty.json" },
                                       .stored = { storedRun(Running {}, RunId, ChangeRequestRef { 1 }) } });
    Row row = runRowOf(engine);

    [[nodiscard]] KeyContext context() const
    {
        return KeyContext { .selectable = 2, .selected = &row, .engine = &engine };
    }
};

[[nodiscard]] std::vector<std::string_view> labelsOf(std::span<PaletteEntry const> entries)
{
    return entries | std::views::transform(&PaletteEntry::label) | std::ranges::to<std::vector>();
}

} // namespace

TEST_CASE("the palette lists the row's actions, then the global commands, then the rest dimmed",
          "[orchestrator][view][palette]")
{
    auto const live = LiveRun {};
    auto const entries = paletteEntries(&live.row, &live.engine);
    // The row's actions first, its primary one on ⏎.
    REQUIRE(entries.size() >= 4);
    CHECK(entries[0] == PaletteEntry { .command = KeyCommand::Open, .key = "⏎", .label = "message" });
    CHECK(entries[1]
          == PaletteEntry { .command = KeyCommand::TranscriptTab, .key = "t", .label = "transcript" });
    // Every command once; a quick reply only where it answers, which a working run does not.
    for (auto const& command: commandRows())
        CHECK(std::ranges::count(entries, command.command, &PaletteEntry::command)
              == (replyDigitOf(command.command) ? 0 : 1));
    // Applying ones before dimmed ones.
    auto const firstDimmed =
        std::ranges::find_if(entries, [](PaletteEntry const& e) { return !e.reason.empty(); });
    CHECK(std::ranges::all_of(
        firstDimmed, entries.end(), [](PaletteEntry const& e) { return !e.reason.empty(); }));
    auto const labels = labelsOf(entries);
    auto const at = [&](std::string_view label) {
        return std::ranges::find(labels, label) - labels.begin();
    };
    CHECK(at("end") < at("new agent"));
    CHECK(at("new agent") < at("refresh the lists"));
    // Commands without a key are the palette's only; one that does not apply says why.
    auto const refresh = std::ranges::find(entries, KeyCommand::Refresh, &PaletteEntry::command);
    CHECK(refresh->key.empty());
    auto const dismiss = std::ranges::find(entries, KeyCommand::Dismiss, &PaletteEntry::command);
    CHECK(dismiss->reason == "not while it is working");
    auto const install = std::ranges::find(entries, KeyCommand::InstallPlugins, &PaletteEntry::command);
    CHECK(install->reason == "every plugin the tasks need is installed");
}

TEST_CASE("the palette filters fuzzily, those that apply first", "[orchestrator][view][palette]")
{
    auto const live = LiveRun {};
    auto const entries = paletteEntries(&live.row, &live.engine);
    CHECK(paletteMatches(entries, "").size() == entries.size());
    auto const refresh = paletteMatches(entries, "refr");
    REQUIRE_FALSE(refresh.empty());
    CHECK(entries[refresh.front().entry].label == "refresh the lists");
    CHECK(refresh.front().positions == std::vector<std::size_t> { 0, 1, 2, 3 });
    // An abbreviation matches too: "l24" is "show the last 24 hours".
    auto const timeline = paletteMatches(entries, "l24");
    REQUIRE_FALSE(timeline.empty());
    CHECK(entries[timeline.front().entry].command == KeyCommand::Timeline);
    // A dimmed match comes after every one that applies.
    auto const run = paletteMatches(entries, "r");
    auto const dimmed = [&](PaletteMatch const& match) {
        return !entries[match.entry].reason.empty();
    };
    CHECK(std::ranges::is_partitioned(run, std::not_fn(dimmed)));
    CHECK(paletteMatches(entries, "zzzz").empty());
}

TEST_CASE("running a command from the palette equals pressing its key", "[orchestrator][view][palette]")
{
    auto const live = LiveRun {};
    auto const entries = paletteEntries(&live.row, &live.engine);
    for (auto const& [index, entry]: std::views::enumerate(entries))
    {
        if (entry.key.empty() || !entry.reason.empty())
            continue;
        CAPTURE(entry.label);
        auto pressed = Interaction {};
        auto const byKey = applyKey(eventOf(entry.key), pressed, live.context());
        auto chosen =
            Interaction { .palette = PaletteState { .highlighted = static_cast<std::size_t>(index) } };
        auto const byPalette = applyPaletteKey(special(KeyCode::Enter), chosen, live.context());
        CHECK(byPalette == byKey);
        CHECK(chosen == pressed);
    }
}

TEST_CASE("the palette's keys: type, move, run; esc closes it and keeps the query",
          "[orchestrator][view][palette]")
{
    auto const live = LiveRun {};
    auto interaction = Interaction {};
    CHECK(applyKey(character(U'k', Modifier::Ctrl), interaction, live.context()) == std::nullopt);
    REQUIRE(interaction.palette.has_value());
    // Every printable key types: `e` is no command here.
    for (auto const c: U"enx")
        if (c != U'\0')
            CHECK(applyKey(character(c), interaction, live.context()) == std::nullopt);
    CHECK(interaction.palette->query == "enx");
    CHECK(interaction.pendingEnd.empty());
    CHECK(applyKey(special(KeyCode::Backspace), interaction, live.context()) == std::nullopt);
    CHECK(interaction.palette->query == "en");
    CHECK(applyKey(special(KeyCode::Down), interaction, live.context()) == std::nullopt);
    CHECK(interaction.palette->highlighted == 1);

    CHECK(applyKey(special(KeyCode::Escape), interaction, live.context()) == std::nullopt);
    CHECK_FALSE(interaction.palette.has_value());
    CHECK(interaction.paletteQuery == "en");
    // `?` opens it again, on the same query.
    CHECK(applyKey(character(U'?'), interaction, live.context()) == std::nullopt);
    REQUIRE(interaction.palette.has_value());
    CHECK(interaction.palette->query == "en");
    CHECK(interaction.paletteQuery.empty());

    // A key with Alt is an esc read with the next key: it closes it too.
    CHECK(applyKey(character(U'j', Modifier::Alt), interaction, live.context()) == std::nullopt);
    CHECK_FALSE(interaction.palette.has_value());

    // ⏎ runs the highlighted command: `end` asks once, as `e` does.
    interaction = Interaction { .palette = PaletteState { .query = "end" } };
    CHECK(applyKey(special(KeyCode::Enter), interaction, live.context()) == std::nullopt);
    CHECK_FALSE(interaction.palette.has_value());
    CHECK(interaction.pendingEnd == RunId);
}

TEST_CASE("a paste into the palette is text: it runs nothing", "[orchestrator][view][palette]")
{
    auto const live = LiveRun {};
    auto interaction = Interaction { .palette = PaletteState { .highlighted = 2 } };
    pasteInto(interaction, live.context(), "end\r\n");
    REQUIRE(interaction.palette.has_value());
    CHECK(interaction.palette == PaletteState { .query = "end " });
    CHECK(interaction.pendingEnd.empty());
}

TEST_CASE("a dimmed command run from the palette says why on the bar", "[orchestrator][view][palette]")
{
    auto const live = LiveRun {};
    auto interaction = Interaction { .palette = PaletteState { .query = "dismiss" } };
    CHECK(applyKey(special(KeyCode::Enter), interaction, live.context()) == std::nullopt);
    CHECK_FALSE(interaction.palette.has_value());
    CHECK(interaction.notice == "del dismiss · not while it is working");
}

TEST_CASE("the timeline opens from the palette, and esc closes it", "[orchestrator][view][palette]")
{
    auto fixture = DashboardFixture { World {} };
    CHECK(fixture.dashboard.input("\x0blast 24\r").empty());
    CHECK(fixture.dashboard.interaction().overlay == Overlay::Timeline);
    auto const screen = fixture.screen();
    CHECK(screen.contains("Last 24 hours"));
    CHECK(screen.contains("nothing ran in the last 24 hours"));
    // No letter acts behind it.
    CHECK(fixture.dashboard.input("n").empty());
    CHECK_FALSE(fixture.dashboard.launcher().has_value());
    CHECK(fixture.dashboard.input("\x1b").empty());
    fixture.escapeTimesOut();
    CHECK(fixture.dashboard.interaction().overlay == Overlay::None);
    CHECK_FALSE(fixture.screen().contains("Last 24 hours"));
}
