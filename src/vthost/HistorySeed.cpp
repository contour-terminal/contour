// SPDX-License-Identifier: Apache-2.0
#include <vthost/HistorySeed.hpp>

#include <vtbackend/shell/CommandBlock.hpp>

#include <core/Utils.hpp>

#include <algorithm>
#include <format>
#include <memory>
#include <mutex>
#include <ranges>
#include <unordered_map>
#include <utility>
#include <vector>

#include <vthost/CommandBlockWire.hpp>
#include <vthost/GridWire.hpp>

namespace vthost
{

namespace
{
    /// The glyph the divider's rules are made of.
    constexpr std::string_view DividerRule = "─";

    /// @return @p glyph written @p count times.
    [[nodiscard]] std::string repeated(std::string_view glyph, size_t count)
    {
        return std::ranges::to<std::string>(std::views::repeat(glyph, count) | std::views::join);
    }

    /// @return The number of cells @p text takes, for text whose every glyph is one cell wide: its code
    /// points.
    [[nodiscard]] size_t cellsOf(std::string_view text) noexcept
    {
        return static_cast<size_t>(std::ranges::count_if(text, [](char byte) {
            return (static_cast<unsigned char>(byte) & 0b1100'0000U) != 0b1000'0000U;
        }));
    }

    /// @return The first @p cells cells of @p text (one-cell glyphs, see cellsOf()).
    [[nodiscard]] std::string cutToCells(std::string_view text, size_t cells)
    {
        auto seen = size_t { 0 };
        auto end = text.size();
        for (auto const [index, byte]: core::views::enumerate(text))
        {
            if ((static_cast<unsigned char>(byte) & 0b1100'0000U) == 0b1000'0000U)
                continue;
            if (seen++ == cells)
            {
                end = index;
                break;
            }
        }
        return std::string { text.substr(0, end) };
    }

    /// The seam where a seeded history's still-Running command record is ended. A snapshot taken mid-command
    /// leaves a Running record under the divider; Step 3f Task 12 adds the Abandoned end state and
    /// Terminal::abandonRunningCommand(), and this is the one place that call goes once it has landed.
    void abandonSeededRunningCommand(vtbackend::Terminal const& /*terminal*/) noexcept
    {
    }

    /// Registers @p history's hyperlinks with @p terminal, as a mirror does.
    /// @return Wire id to the terminal's own id.
    [[nodiscard]] std::unordered_map<uint16_t, vtbackend::HyperlinkId> adoptHyperlinks(
        vtbackend::Terminal& terminal, proto::Delta const& history)
    {
        auto ids = std::unordered_map<uint16_t, vtbackend::HyperlinkId> {};
        auto& store = terminal.hyperlinks();
        for (auto const& entry: history.hyperlinks)
        {
            if (entry.id == 0)
                continue;
            auto const id = store.nextHyperlinkId++;
            store.cache.emplace(id,
                                std::make_shared<vtbackend::HyperlinkInfo>(
                                    vtbackend::HyperlinkInfo { .userId = {}, .uri = entry.uri }));
            ids.insert_or_assign(entry.id, id);
        }
        return ids;
    }

    /// Writes the rows of @p history into @p page so that they end up in its history, oldest first, and the
    /// page is blank again.
    void landRows(vtbackend::Screen& page,
                  proto::Delta const& history,
                  std::unordered_map<uint16_t, vtbackend::HyperlinkId> const& links)
    {
        auto const pageLines = unbox<int>(page.pageSize().lines);
        auto const contexts = ContextIdMap {};
        // The first rows fill the page from its top; once it is full, each further row scrolls the oldest up
        // into the history and takes the bottom row. Scrolling the first rows instead would push the page's
        // blank rows into the history ahead of them.
        for (auto const [index, source]: core::views::enumerate(history.lines))
        {
            auto wire = source;
            // Stamped by the session the row came from, which is not this terminal's: counted from another
            // epoch, it would name the wrong minute.
            wire.bornAt = 0;
            auto row = static_cast<int>(index);
            if (row >= pageLines)
            {
                page.scrollUp(vtbackend::LineCount(1));
                row = pageLines - 1;
            }
            applyWireLine(page.grid().lineAt(vtbackend::LineOffset(row)), wire, links, contexts);
        }
        // What the page still holds goes up into the history behind them.
        page.scrollUp(vtbackend::LineCount(std::min<int>(static_cast<int>(history.lines.size()), pageLines)));
    }

    /// Adopts the records of @p history and heads each on the rows now in the primary grid.
    void adoptRecords(vtbackend::Terminal& terminal, proto::Delta const& history)
    {
        auto adopted = std::vector<vtbackend::CommandBlockId> {};
        auto evicted = std::vector<vtbackend::CommandBlockId> {};
        for (auto const& wire: history.commandBlocks)
        {
            terminal.adoptCommandBlock(fromWireCommandBlock(wire, nullptr), vtbackend::AdoptMode::Snapshot);
            adopted.emplace_back(wire.id);
            if (hostHeadOf(wire) == HostHead::Evicted)
                evicted.emplace_back(wire.id);
        }
        terminal.retireCommandBlockHeads(evicted);
        terminal.placeCommandBlockHeads(adopted);
    }
} // namespace

void seedRestoredHistory(vtbackend::Terminal& terminal,
                         proto::Delta const& history,
                         RestoreDivider const& divider)
{
    {
        auto const guard = std::lock_guard { terminal };
        auto const links = adoptHyperlinks(terminal, history);
        landRows(terminal.primaryScreen(), history, links);
        adoptRecords(terminal, history);
        abandonSeededRunningCommand(terminal);
    }
    // Home first: the page was blank, and the divider belongs on its top row. The row is never wider than
    // the page, so it needs no autowrap switching and leaves the profile's DECAWM as it was.
    auto const columns = terminal.pageSize().columns;
    auto const row = cutToCells(restoredDivider(divider, columns), unbox<size_t>(columns));
    terminal.writeToScreen(std::format("\033[H\033[2m{}\033[m\r\n", row));
    auto const guard = std::lock_guard { terminal };
    terminal.primaryScreen().grid().lineAt(vtbackend::LineOffset(0)).setWrappable(false);
}

std::string restoredDividerText(RestoreDivider const& divider)
{
    auto const endedOn = std::chrono::floor<std::chrono::days>(divider.sessionEndedAt);
    return endedOn == divider.today
               ? std::format("Restored · session ended {:%H:%M}", divider.sessionEndedAt)
               : std::format("Restored · session ended {:%Y-%m-%d %H:%M}", divider.sessionEndedAt);
}

std::string restoredDivider(RestoreDivider const& divider, vtbackend::ColumnCount columns)
{
    auto text = restoredDividerText(divider);
    auto const width = cellsOf(text);
    auto const available = unbox<size_t>(columns);
    if (available < width + 2)
        return text;
    auto const leftover = available - width - 2;
    auto const left = leftover / 2;
    auto const right = leftover - left;
    return std::format("{} {} {}", repeated(DividerRule, left), text, repeated(DividerRule, right));
}

} // namespace vthost
