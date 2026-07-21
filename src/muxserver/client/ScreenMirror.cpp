// SPDX-License-Identifier: Apache-2.0
#include <muxserver/client/ScreenMirror.h>

#include <vtbackend/CellFlags.h>
#include <vtbackend/TextScale.h>
#include <vtbackend/TextSizing.h>

#include <libunicode/convert.h>

#include <algorithm>
#include <format>
#include <iterator>
#include <ranges>
#include <set>
#include <string_view>
#include <unordered_map>

#include <muxserver/MirroredModes.h>
#include <muxserver/client/TtyRenderer.h>

namespace muxserver::client
{

namespace
{
    constexpr uint32_t ContinuationMask = static_cast<uint32_t>(vtbackend::CellFlag::WideCharContinuation)
                                          | static_cast<uint32_t>(vtbackend::CellFlag::MulticellContinuation);

    [[nodiscard]] std::string cup(int64_t line, int64_t column)
    {
        return std::format("\033[{};{}H", line, column);
    }

    [[nodiscard]] bool isScaled(proto::WireCell const& cell)
    {
        return cell.scale > 1 || cell.textScaleExtras != 0;
    }

    void appendCluster(std::string& out, proto::WireCell const& cell)
    {
        unicode::convert_to<char>(std::u32string_view(&cell.codepoint, 1), std::back_inserter(out));
        for (auto const extra: cell.clusterExtras)
            unicode::convert_to<char>(std::u32string_view(&extra, 1), std::back_inserter(out));
    }

    /// Emits the `OSC 66` request recreating @p cell's scaled-text block.
    void appendSizedText(std::string& out, proto::WireCell const& cell)
    {
        auto const scale = vtbackend::unpackTextScale(cell.scale, cell.textScaleExtras);
        out += std::format("\033]66;s={}", std::max<uint8_t>(scale.scale, 1));
        // An explicit width pins the block to exactly the columns the server
        // claimed, immune to any re-measuring difference.
        if (scale.scale > 0 && cell.width % scale.scale == 0)
        {
            auto const width = static_cast<uint8_t>(cell.width / scale.scale);
            if (width >= 1 && width <= vtbackend::text_sizing::MaxWidth)
                out += std::format(":w={}", width);
        }
        if (scale.numerator != 0 || scale.denominator != 0)
            out += std::format(":n={}:d={}", scale.numerator, scale.denominator);
        if (scale.verticalAlignment != 0)
            out += std::format(":v={}", scale.verticalAlignment);
        if (scale.horizontalAlignment != 0)
            out += std::format(":h={}", scale.horizontalAlignment);
        out += ';';
        appendCluster(out, cell);
        out += "\033\\";
    }

    /// Emits `OSC 8` opening (or closing, for id 0) a hyperlink.
    void appendHyperlink(std::string& out, uint16_t id, std::unordered_map<uint16_t, std::string> const& uris)
    {
        if (id == 0)
        {
            out += "\033]8;;\033\\";
            return;
        }
        auto const it = uris.find(id);
        out += std::format("\033]8;id={};{}\033\\", id, it != uris.end() ? it->second : std::string {});
    }

    /// Renders @p row's cells at the current cursor position.
    ///
    /// @p faithful selects viewport semantics: orphan continuation cells are
    /// stepped over with CUF (their block's head re-claims them) and scaled
    /// heads re-emit their OSC 66 block. Stream mode (history scrolling
    /// through the page) degrades both to plain text and spaces, since
    /// cursor addressing is unavailable while scrolling.
    void renderCells(std::string& out,
                     proto::WireLine const& row,
                     std::unordered_map<uint16_t, std::string> const& uris,
                     bool faithful)
    {
        auto previousSgr = std::string {};
        auto currentLink = uint16_t { 0 };
        auto covered = 0;     // columns the previous emission already advanced over
        auto pendingSkip = 0; // orphan continuation columns awaiting a cursor step
        auto skipAfterWide = false;

        for (auto const& cell: row.cells)
        {
            if (covered > 0)
            {
                --covered;
                continue;
            }
            if (skipAfterWide)
            {
                skipAfterWide = false;
                if (cell.codepoint == 0)
                    continue; // the wide glyph already covered this column
            }
            if ((cell.flags & ContinuationMask) != 0 && cell.codepoint == 0)
            {
                if (faithful)
                    ++pendingSkip;
                else
                    out += ' ';
                continue;
            }

            if (pendingSkip > 0)
            {
                out += std::format("\033[{}C", pendingSkip);
                pendingSkip = 0;
            }
            if (cell.hyperlink != currentLink)
            {
                appendHyperlink(out, cell.hyperlink, uris);
                currentLink = cell.hyperlink;
            }
            auto sgr = sgrFor(cell);
            if (sgr != previousSgr)
            {
                out += sgr;
                previousSgr = std::move(sgr);
            }

            if (cell.codepoint == 0)
            {
                out += ' ';
                continue;
            }
            if (faithful && isScaled(cell))
            {
                appendSizedText(out, cell);
                covered = std::max(0, static_cast<int>(cell.width) - 1);
                continue;
            }
            appendCluster(out, cell);
            skipAfterWide = cell.width == 2;
        }

        if (currentLink != 0)
            appendHyperlink(out, 0, uris);
    }

    /// Paints one viewport row in place: address, fill-colored clear, cells.
    void paintRow(std::string& out, RemoteScreen const& screen, int64_t stableId, int64_t targetLine)
    {
        out += cup(targetLine, 1);
        auto const it = screen.rows.find(stableId);
        if (it == screen.rows.end())
        {
            out += "\033[0m\033[2K";
            return;
        }
        out += sgrForFill(it->second);
        out += "\033[2K";
        renderCells(out, it->second, screen.hyperlinks, /*faithful=*/true);
    }

    /// Expands @p ids so that every row holding a scaled block's continuation
    /// cells drags the rows above it (up to the block's maximum height) into
    /// the repaint: clearing a continuation row destroys the whole block, so
    /// its head row must repaint afterwards.
    void expandForBlocks(std::set<int64_t>& ids, RemoteScreen const& screen, int64_t lowest)
    {
        auto pending = std::set<int64_t> { ids };
        while (!pending.empty())
        {
            auto const id = *pending.begin();
            pending.erase(pending.begin());
            auto const it = screen.rows.find(id);
            if (it == screen.rows.end())
                continue;
            auto const holdsContinuation =
                std::ranges::any_of(it->second.cells, [](proto::WireCell const& cell) {
                    return (cell.flags & static_cast<uint32_t>(vtbackend::CellFlag::MulticellContinuation))
                           != 0;
                });
            if (!holdsContinuation)
                continue;
            for (auto above = int64_t { 1 }; above < int64_t { vtbackend::text_sizing::MaxScale }; ++above)
            {
                auto const candidate = id - above;
                if (candidate < lowest || !screen.rows.contains(candidate))
                    break;
                if (ids.insert(candidate).second)
                    pending.insert(candidate);
            }
        }
    }
} // namespace

std::string ScreenMirror::apply(RemoteScreen const& screen, proto::Delta const& delta)
{
    if (!_primed || delta.snapshot != 0 || screen.generation != _generation || screen.columns != _columns
        || screen.lines != _lines || screen.screenType != _screenType || screen.viewportBase < _viewportBase)
        return fullReplay(screen);

    auto const oldBase = _viewportBase;
    auto const newBase = screen.viewportBase;
    auto const lines = static_cast<int64_t>(screen.lines);
    _viewportBase = newBase;

    auto out = std::string { "\033[?25l" };

    // 1. Repaint changed rows still inside the OLD viewport at their old
    //    positions — including rows about to scroll into history, so local
    //    history keeps their final content.
    auto oldViewportIds = std::set<int64_t> {};
    for (auto const& line: delta.lines)
        if (line.stableId >= oldBase && line.stableId < oldBase + lines)
            oldViewportIds.insert(line.stableId);
    expandForBlocks(oldViewportIds, screen, oldBase);
    for (auto const id: std::views::reverse(oldViewportIds))
        paintRow(out, screen, id, id - oldBase + 1);

    // 2. Scroll the viewport advance in. Rows that pass straight through
    //    (entering and leaving within one update) render inline while they
    //    scroll; rows landing in the new viewport get a faithful paint below.
    if (newBase > oldBase)
    {
        out += "\033[0m";
        out += cup(lines, 1);
        for (auto id = oldBase + lines; id < newBase + lines; ++id)
        {
            out += "\n\r";
            if (id < newBase)
                if (auto const it = screen.rows.find(id); it != screen.rows.end())
                    renderCells(out, it->second, screen.hyperlinks, /*faithful=*/false);
        }

        auto newViewportIds = std::set<int64_t> {};
        for (auto const& line: delta.lines)
            if (line.stableId >= std::max(newBase, oldBase + lines) && line.stableId < newBase + lines)
                newViewportIds.insert(line.stableId);
        expandForBlocks(newViewportIds, screen, newBase);
        for (auto const id: std::views::reverse(newViewportIds))
            paintRow(out, screen, id, id - newBase + 1);
    }

    syncModes(out, screen);
    out += "\033[0m";
    out += cup(delta.cursorLine + 1, delta.cursorColumn + 1);
    if (_setModes.contains(VisibleCursorModeNumber))
        out += "\033[?25h";
    return out;
}

void ScreenMirror::syncModes(std::string& out, RemoteScreen const& screen)
{
    auto const target = std::set<uint32_t>(screen.setModes.begin(), screen.setModes.end());
    for (auto const mode: MirroredModes)
    {
        auto const number = vtbackend::toDECModeNum(mode);
        if (number == VisibleCursorModeNumber)
            continue; // handled by the paint epilogue: hidden while painting
        auto const want = target.contains(number);
        if (_modesKnown && want == _setModes.contains(number))
            continue;
        out += std::format("\033[?{}{}", number, want ? 'h' : 'l');
    }
    _setModes = target;
    _modesKnown = true;
}

std::string ScreenMirror::fullReplay(RemoteScreen const& screen)
{
    _primed = true;
    _generation = screen.generation;
    _viewportBase = screen.viewportBase;
    _columns = screen.columns;
    _lines = screen.lines;
    _screenType = screen.screenType;

    auto const lines = static_cast<int64_t>(screen.lines);
    auto out = std::string { "\033[?25l\033[?7l" };
    out += screen.screenType == 1 ? "\033[?1049h" : "\033[?1049l";
    out += "\033[0m\033[H\033[2J";
    if (screen.screenType == 0)
        out += "\033[3J"; // the replay below rebuilds local scrollback from scratch

    // Stream history (then the viewport) through the page top-down: once the
    // page is full, each further line scrolls a REAL row into scrollback, so
    // local history matches remote history with no filler.
    auto const firstId =
        screen.rows.empty() ? screen.viewportBase : std::min(screen.rows.begin()->first, screen.viewportBase);
    if (firstId < screen.viewportBase && screen.screenType == 0)
    {
        auto row = int64_t { 1 };
        for (auto id = firstId; id < screen.viewportBase + lines; ++id)
        {
            if (row <= lines)
                out += cup(row++, 1);
            else
                out += "\n\r";
            out += "\033[0m";
            if (auto const it = screen.rows.find(id); it != screen.rows.end())
                renderCells(out, it->second, screen.hyperlinks, /*faithful=*/false);
        }
    }

    // Faithful viewport paint, bottom-up (see the file comment on blocks).
    for (auto line = lines - 1; line >= 0; --line)
        paintRow(out, screen, screen.viewportBase + line, line + 1);

    out += std::format("\033]0;{}\033\\", screen.title);
    syncModes(out, screen);
    out += "\033[0m";
    out += cup(screen.cursorLine + 1, screen.cursorColumn + 1);
    if (_setModes.contains(VisibleCursorModeNumber))
        out += "\033[?25h";
    return out;
}

} // namespace muxserver::client
