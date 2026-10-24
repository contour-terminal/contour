// SPDX-License-Identifier: Apache-2.0
#include <vthost/HistorySeed.hpp>

#include <vtbackend/shell/CommandBlock.hpp>

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
        for (auto const [index, source]: history.lines | std::views::enumerate)
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

void seedRestoredHistory(vtbackend::Terminal& terminal, proto::Delta const& history, std::string_view marker)
{
    {
        auto const guard = std::lock_guard { terminal };
        auto const links = adoptHyperlinks(terminal, history);
        landRows(terminal.primaryScreen(), history, links);
        adoptRecords(terminal, history);
    }
    // Home first: the page was blank, and the marker belongs on its top row.
    terminal.writeToScreen(std::format("\033[H{}", marker));
}

std::string restoredMarker(std::chrono::local_seconds takenAt)
{
    return std::format("\033[2m── restored · snapshot of {:%Y-%m-%d %H:%M} ──\033[m\r\n", takenAt);
}

} // namespace vthost
