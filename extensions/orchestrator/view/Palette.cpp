// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/view/Palette.hpp>

#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>
#include <core/tui/completer/FuzzyMatch.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <orchestrator/core/Timeline.hpp>
#include <orchestrator/core/Utf8.hpp>
#include <orchestrator/view/Actions.hpp>
#include <orchestrator/view/Theme.hpp>

using namespace std::string_view_literals;

namespace orchestrator::view
{

namespace
{
    using core::tui::completer::FuzzyMatch;

    /// The score a fuzzy match starts from, before its bonuses.
    constexpr auto BaseScore = 100;
    /// What the palette says of an entry without a key.
    constexpr auto PaletteOnly = "palette only"sv;
    /// The rows the palette takes besides its list: the query and a blank above it, a blank and its keys
    /// below.
    constexpr auto PaletteFurnitureRows = 4;
    /// The cells the box and its padding take on either side.
    constexpr auto SideColumns = 2;
} // namespace

std::vector<PaletteEntry> paletteEntries(Row const* row, Engine const* engine)
{
    auto entries = std::vector<PaletteEntry> {};
    auto const listed = [&](KeyCommand command) {
        return std::ranges::contains(entries, command, &PaletteEntry::command);
    };
    // The row's actions, its primary one on `⏎`.
    auto const state = row != nullptr ? row->rowState : RowState::None;
    for (auto const [rank, action]: std::views::enumerate(actionsFor(state)))
        if (whyNot(row, action.command, engine).empty())
            entries.push_back(
                PaletteEntry { .command = action.command,
                               .key = rank == 0 ? keyNameOf(KeyCommand::Enter) : keyNameOf(action.command),
                               .label = labelOf(action) });
    // The global commands that apply, then everything else, with the reason it does not.
    for (auto const& command: commandRows())
        if (command.scope == CommandScope::Global && !listed(command.command)
            && whyNot(row, command.command, engine).empty())
            entries.push_back(PaletteEntry {
                .command = command.command, .key = keyNameOf(command.command), .label = command.label });
    for (auto const& command: commandRows())
        if (!listed(command.command))
            entries.push_back(PaletteEntry { .command = command.command,
                                             .key = keyNameOf(command.command),
                                             .label = command.label,
                                             .reason = whyNot(row, command.command, engine) });
    return entries;
}

std::vector<PaletteMatch> paletteMatches(std::span<PaletteEntry const> entries, std::string_view query)
{
    auto matches = std::vector<PaletteMatch> {};
    if (query.empty())
    {
        for (auto const index: std::views::iota(std::size_t { 0 }, entries.size()))
            matches.push_back(PaletteMatch { .entry = index });
        return matches;
    }
    auto scores = std::vector<int> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, entries.size()))
    {
        auto result = FuzzyMatch::matchSmartCase(entries[index].label, query);
        if (!result.matches)
            continue;
        scores.push_back(FuzzyMatch::calculateScore(BaseScore, entries[index].label, query, result));
        matches.push_back(PaletteMatch { .entry = index, .positions = std::move(result.positions) });
    }
    // Those that apply first; within each part, the best match first, ties in the entries' order.
    auto order = std::views::iota(std::size_t { 0 }, matches.size()) | std::ranges::to<std::vector>();
    std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) {
        auto const dimmed = [&](std::size_t at) {
            return !entries[matches[at].entry].reason.empty();
        };
        return std::pair { dimmed(a), -scores[a] } < std::pair { dimmed(b), -scores[b] };
    });
    return order | std::views::transform([&](std::size_t at) { return std::move(matches[at]); })
           | std::ranges::to<std::vector>();
}

std::optional<UiAction> applyPaletteKey(core::tui::KeyEvent const& key,
                                        Interaction& interaction,
                                        KeyContext const& context)
{
    using core::tui::KeyCode;
    using core::tui::Modifier;
    auto& palette = *interaction.palette;
    // esc closes it and keeps the query; so does a key with Alt, which is how an esc reads when the next key
    // follows it in the same read.
    if (key.key == KeyCode::Escape || core::tui::hasModifier(key.modifiers, Modifier::Alt))
    {
        interaction.paletteQuery = std::move(palette.query);
        interaction.palette.reset();
        return std::nullopt;
    }
    if (core::tui::hasModifier(key.modifiers, Modifier::Ctrl))
        return std::nullopt;
    switch (key.key)
    {
        case KeyCode::Up:
        case KeyCode::Down: {
            auto const count =
                paletteMatches(paletteEntries(context.selected, context.engine), palette.query).size();
            if (count != 0)
                palette.highlighted = key.key == KeyCode::Up ? (palette.highlighted + count - 1) % count
                                                             : (palette.highlighted + 1) % count;
            return std::nullopt;
        }
        case KeyCode::Backspace:
            dropLastCodepoint(palette.query);
            palette.highlighted = 0;
            return std::nullopt;
        case KeyCode::Enter: {
            auto const entries = paletteEntries(context.selected, context.engine);
            auto const matches = paletteMatches(entries, palette.query);
            if (palette.highlighted >= matches.size())
                return std::nullopt;
            auto const command = entries[matches[palette.highlighted].entry].command;
            interaction.palette.reset();
            // As its key would: a command that does not apply says why on the bar.
            return dispatch(command, interaction, context);
        }
        default: break;
    }
    if (auto const printable = printableOf(key))
    {
        appendUtf8(palette.query, *printable);
        palette.highlighted = 0;
    }
    return std::nullopt;
}

// {{{ PaletteView

void PaletteView::place(core::tui::Size size, std::vector<PaletteEntry> entries)
{
    _size = size;
    _entries = std::move(entries);
}

int PaletteView::heightFor(std::span<PaletteEntry const> entries) const
{
    if (!_palette)
        return 0;
    auto const matches = static_cast<int>(paletteMatches(entries, _palette->query).size());
    auto const listRows = std::clamp(matches, 1, MaxPaletteRows) + (matches > MaxPaletteRows ? 1 : 0);
    return listRows + PaletteFurnitureRows + 2;
}

void PaletteView::render(core::tui::Canvas& canvas)
{
    if (!_palette)
        return;
    auto const& theme = canvas.theme();
    canvas.clear(styleOf(theme, Role::Text));
    drawFrame(canvas, "✦ All actions", Role::Focus);
    auto inside = insideFrame(canvas);
    auto const width = inside.width() - SideColumns;
    auto const end = width + 1;

    auto const x = 1 + inside.putString(0, 1, "› ", styleOf(theme, Role::Accent));
    if (_palette->query.empty())
        inside.putString(0, x, cut("▏type to filter the actions", end - x), styleOf(theme, Role::Dim));
    else
        inside.putString(0, x, cut(std::format("{}▏", _palette->query), end - x), styleOf(theme, Role::Text));

    auto const matches = paletteMatches(_entries, _palette->query);
    auto const listRows = std::max(0, inside.height() - PaletteFurnitureRows);
    auto const cutList = std::cmp_greater(matches.size(), listRows) && listRows > 1;
    auto const shownRows = static_cast<std::size_t>(cutList ? listRows - 1 : listRows);
    auto const highlighted = std::min(_palette->highlighted, matches.empty() ? 0 : matches.size() - 1);
    auto const first = matches.size() <= shownRows
                           ? std::size_t { 0 }
                           : std::min(highlighted > shownRows / 2 ? highlighted - (shownRows / 2) : 0,
                                      matches.size() - shownRows);
    auto y = 2;
    if (matches.empty())
        inside.putString(y, 2, cut("no action matches", width - 1), styleOf(theme, Role::Dim));
    for (auto const at: std::views::iota(first, std::min(matches.size(), first + shownRows)))
    {
        auto const& match = matches[at];
        auto const& entry = _entries[match.entry];
        auto const isSelected = at == highlighted;
        auto const isDimmed = !entry.reason.empty();
        if (isSelected)
            inside.putString(y, 0, "▍", styleOf(theme, Role::Accent));
        auto const key = entry.key.empty() ? PaletteOnly : entry.key;
        auto const keyColumns = columnsOf(key);
        inside.putString(y, end - keyColumns, key, styleOf(theme, Role::Dim));
        auto textRole = isSelected ? Role::Selection : Role::Text;
        if (isDimmed)
            textRole = Role::Dim;
        auto matched = styleOf(theme, isDimmed ? Role::Dim : Role::Accent);
        matched.underline = true;
        auto const room = end - keyColumns - 1 - 2;
        auto const label = cut(entry.label, room);
        auto const column = 2
                            + core::tui::putStringWithHighlights(
                                inside, y, 2, label, styleOf(theme, textRole), matched, match.positions);
        // cut() drops leading blanks: the reason starts a cell after the label.
        if (isDimmed && room + 2 - column > 4)
            inside.putString(y,
                             column + 1,
                             cut(std::format("· {}", entry.reason), room + 1 - column),
                             styleOf(theme, Role::Dim));
        ++y;
    }
    if (cutList)
        inside.putString(
            y++, 2, std::format("↓ {} more", matches.size() - first - shownRows), styleOf(theme, Role::Dim));
    auto const hints = std::array { "⏎ run"sv, "↑↓ choose"sv, "esc close"sv };
    inside.putString(inside.height() - 1, 1, joinHints(hints, width), styleOf(theme, Role::Dim));
}

// }}}
// {{{ TimelineView

namespace
{
    /// The bars a chart is drawn with, lowest first; an hour with nothing draws the first.
    constexpr auto Bars = std::array { "·"sv, "▁"sv, "▂"sv, "▃"sv, "▄"sv, "▅"sv, "▆"sv, "▇"sv, "█"sv };

    /// @return @p values as bars, each scaled to the largest; a value above nothing draws at least the
    /// lowest.
    [[nodiscard]] std::string barsOf(std::span<uint64_t const> values)
    {
        auto const top = std::ranges::max(values);
        auto bars = std::string {};
        for (auto const value: values)
        {
            auto const level = value == 0 || top == 0
                                   ? std::size_t { 0 }
                                   : std::max<std::size_t>(1, ((value * (Bars.size() - 1)) + top - 1) / top);
            bars += Bars[level];
        }
        return bars;
    }

    template <typename T>
    [[nodiscard]] std::array<uint64_t, TimelineBuckets> widened(std::array<T, TimelineBuckets> const& values)
    {
        auto wide = std::array<uint64_t, TimelineBuckets> {};
        std::ranges::copy(values, wide.begin());
        return wide;
    }

    /// One line of the timeline: what it charts, and what that adds up to.
    struct ChartLine
    {
        std::string_view label;
        std::array<uint64_t, TimelineBuckets> (*series)(Timeline const&);
        std::string (*summary)(Timeline const&);
    };

    /// The timeline's lines, in order: a new one is a row here.
    constexpr auto ChartLines = std::array {
        ChartLine { .label = "agents active"sv,
                    .series = [](Timeline const& timeline) { return widened(timeline.active); },
                    .summary =
                        [](Timeline const& timeline) {
                            return std::format(
                                "peak {} · now {}", std::ranges::max(timeline.active), timeline.activeNow);
                        } },
        ChartLine { .label = "tokens out"sv,
                    .series = [](Timeline const& timeline) { return timeline.tokens; },
                    .summary =
                        [](Timeline const& timeline) {
                            auto total = uint64_t { 0 };
                            for (auto const tokens: timeline.tokens)
                                total += tokens;
                            return std::format("{} in 24h", countText(total));
                        } },
        ChartLine { .label = "runs started"sv,
                    .series = [](Timeline const& timeline) { return widened(timeline.started); },
                    .summary =
                        [](Timeline const& timeline) {
                            auto started = uint64_t { 0 };
                            auto ended = uint64_t { 0 };
                            for (auto const index: std::views::iota(std::size_t { 0 }, TimelineBuckets))
                            {
                                started += timeline.started[index];
                                ended += timeline.ended[index];
                            }
                            return std::format("{} started · {} ended", started, ended);
                        } },
    };

    /// The cells a chart line's label takes.
    constexpr auto ChartLabelColumns = 15;
    /// The widest a summary is drawn.
    constexpr auto SummaryColumns = 24;
    /// The rows the timeline takes besides its charts: a blank and its keys, and its box.
    constexpr auto TimelineFurnitureRows = 4;
    /// What it says when nothing ran in the last day.
    constexpr auto NothingRan = "nothing ran in the last 24 hours"sv;
} // namespace

core::tui::Size TimelineView::preferredSize() const
{
    return { .width = 1 + ChartLabelColumns + static_cast<int>(TimelineBuckets) + 2 + SummaryColumns + 1 + 2,
             .height = static_cast<int>(ChartLines.size()) + TimelineFurnitureRows };
}

void TimelineView::render(core::tui::Canvas& canvas)
{
    auto const& theme = canvas.theme();
    canvas.clear(styleOf(theme, Role::Text));
    drawFrame(canvas, "Last 24 hours · an hour a bar", Role::Focus);
    auto inside = insideFrame(canvas);
    auto const width = inside.width() - SideColumns;
    auto const timeline = timelineOf(_engine.runs(), _engine.wallMs());
    if (timeline == Timeline {})
        inside.putString(0, 1, cut(NothingRan, width), styleOf(theme, Role::Dim));
    else
    {
        auto y = 0;
        for (auto const& line: ChartLines)
        {
            inside.putString(y, 1, cut(line.label, ChartLabelColumns - 1), styleOf(theme, Role::Dim));
            auto const bars = cut(barsOf(line.series(timeline)), std::max(0, width - ChartLabelColumns));
            auto const used = 1 + ChartLabelColumns
                              + inside.putString(y, 1 + ChartLabelColumns, bars, styleOf(theme, Role::Text));
            inside.putString(
                y, used + 2, cut(line.summary(timeline), width + 1 - used - 2), styleOf(theme, Role::Dim));
            ++y;
        }
    }
    inside.putString(inside.height() - 1, 1, cut("esc close", width), styleOf(theme, Role::Dim));
}

// }}}

} // namespace orchestrator::view
