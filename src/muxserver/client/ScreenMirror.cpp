// SPDX-License-Identifier: Apache-2.0
#include <muxserver/client/ScreenMirror.h>

#include <vtbackend/CellFlags.h>
#include <vtbackend/TextScale.h>
#include <vtbackend/TextSizing.h>

#include <crispy/base64.h>

#include <algorithm>
#include <format>
#include <optional>
#include <ranges>
#include <set>
#include <unordered_map>

#include <muxserver/MirroredModes.h>
#include <muxserver/client/TtyRenderer.h>

namespace muxserver::client
{

namespace
{
    /// std::ranges::contains is not yet in Apple's libc++; find() is.
    [[nodiscard]] bool containsValue(std::vector<uint32_t> const& values, uint32_t needle)
    {
        return std::ranges::find(values, needle) != values.end();
    }

    constexpr uint32_t ContinuationMask = static_cast<uint32_t>(vtbackend::CellFlag::WideCharContinuation)
                                          | static_cast<uint32_t>(vtbackend::CellFlag::MulticellContinuation);

    [[nodiscard]] std::string cup(int64_t line, int64_t column)
    {
        return std::format("\033[{};{}H", line, column);
    }

    /// Emits `OSC <code> ; rgb:RR/GG/BB` for a 0xRRGGBB color (code 10 = default
    /// foreground, 11 = default background).
    [[nodiscard]] std::string oscColor(int code, uint32_t rgb)
    {
        return std::format("\033]{};rgb:{:02x}/{:02x}/{:02x}\033\\",
                           code,
                           (rgb >> 16) & 0xFFu,
                           (rgb >> 8) & 0xFFu,
                           rgb & 0xFFu);
    }

    [[nodiscard]] bool isScaled(proto::WireCell const& cell)
    {
        return cell.scale > 1 || cell.textScaleExtras != 0;
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
        auto previousSgrKey = std::optional<decltype(sgrKeyOf(proto::WireCell {}))> {};
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
            if (auto const key = sgrKeyOf(cell); previousSgrKey != key)
            {
                out += sgrFor(cell);
                previousSgrKey = key;
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
        auto pending = std::vector<int64_t>(ids.begin(), ids.end()); // plain worklist, no tree churn
        while (!pending.empty())
        {
            auto const id = pending.back();
            pending.pop_back();
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
                    pending.push_back(candidate);
            }
        }
    }
} // namespace

std::string ScreenMirror::apply(RemoteScreen const& screen, proto::Delta const& delta)
{
    // A floor that jumped further than the viewport advanced means the server
    // discarded history WITHOUT scrolling it through the page (a `clear`/CSI 3 J,
    // which the incremental path — no line changes, no viewport move — would
    // otherwise leave as ghost scrollback). fullReplay re-emits ESC[3J and
    // rebuilds local scrollback from what the server still holds.
    auto const floorOutranScroll = screen.stableFloor - _floor > screen.viewportBase - _viewportBase;
    if (!_primed || delta.snapshot != 0 || screen.generation != _generation || screen.columns != _columns
        || screen.lines != _lines || screen.screenType != _screenType || screen.viewportBase < _viewportBase
        || floorOutranScroll)
        return fullReplay(screen);

    auto const oldBase = _viewportBase;
    auto const newBase = screen.viewportBase;
    auto const lines = static_cast<int64_t>(screen.lines);
    _viewportBase = newBase;
    _floor = screen.stableFloor;

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
    appendImages(out, screen);
    if (delta.titleChanged != 0)
        out += std::format("\033]0;{}\033\\", delta.title);
    if (delta.cursorShapeChanged != 0)
        out += std::format("\033[{} q", delta.cursorShape); // DECSCUSR
    if (delta.cwdChanged != 0)
        out += std::format("\033]7;{}\033\\", delta.cwd); // OSC 7 working directory
    if (delta.colorsChanged != 0)
    {
        out += oscColor(10, delta.defaultForeground);
        out += oscColor(11, delta.defaultBackground);
    }
    out += "\033[0m";
    out += cup(delta.cursorLine + 1, delta.cursorColumn + 1);
    if (containsValue(_setModes, VisibleCursorModeNumber))
        out += "\033[?25h";
    return out;
}

void ScreenMirror::syncModes(std::string& out, RemoteScreen const& screen)
{
    for (auto const mode: MirroredModes)
    {
        auto const number = vtbackend::toDECModeNum(mode);
        if (number == VisibleCursorModeNumber)
            continue; // handled by the paint epilogue: hidden while painting
        auto const want = containsValue(screen.setModes, number);
        if (_modesKnown && want == containsValue(_setModes, number))
            continue;
        out += std::format("\033[?{}{}", number, want ? 'h' : 'l');
    }
    // A copy of <= 15 ints per delta beats per-delta tree allocations.
    _setModes = screen.setModes;
    _modesKnown = true;
}

std::string ScreenMirror::fullReplay(RemoteScreen const& screen)
{
    _primed = true;
    _generation = screen.generation;
    _viewportBase = screen.viewportBase;
    _floor = screen.stableFloor;
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
    if (screen.cursorShape != 0)
        out += std::format("\033[{} q", screen.cursorShape); // DECSCUSR
    if (!screen.cwd.empty())
        out += std::format("\033]7;{}\033\\", screen.cwd); // OSC 7 working directory
    if (screen.defaultForeground != 0 || screen.defaultBackground != 0)
    {
        out += oscColor(10, screen.defaultForeground);
        out += oscColor(11, screen.defaultBackground);
    }
    syncModes(out, screen);
    appendImages(out, screen);
    out += "\033[0m";
    out += cup(screen.cursorLine + 1, screen.cursorColumn + 1);
    if (containsValue(_setModes, VisibleCursorModeNumber))
        out += "\033[?25h";
    return out;
}

void ScreenMirror::appendImages(std::string& out, RemoteScreen const& screen)
{
    if (screen.imageCells.empty())
        return;

    auto const base = _viewportBase;
    auto const lines = static_cast<int64_t>(_lines);

    /// One image's placement, accumulated from its covered cells in the viewport.
    struct Placement
    {
        int64_t anchorLine = 0;    ///< 1-based mirror row of the image's top-left cell.
        uint16_t anchorColumn = 0; ///< 1-based mirror column of that cell.
        uint32_t rows = 0;         ///< Cell rows the image spans.
        uint32_t cols = 0;         ///< Cell columns the image spans.
        uint8_t layer = 1;         ///< ImageLayer underlying (0 Below, 1 Replace, 2 Above).
        bool anchored = false;     ///< The (0,0) tile is inside the viewport.
    };
    auto placements = std::unordered_map<uint32_t, Placement> {};

    for (auto const& [stableId, columns]: screen.imageCells)
    {
        if (stableId < base || stableId >= base + lines)
            continue;
        for (auto const& [column, entry]: columns)
        {
            auto& placement = placements[entry.imageId];
            placement.layer = entry.layer;
            placement.rows = std::max(placement.rows, static_cast<uint32_t>(entry.offsetLine) + 1);
            placement.cols = std::max(placement.cols, static_cast<uint32_t>(entry.offsetColumn) + 1);
            if (entry.offsetLine == 0 && entry.offsetColumn == 0)
            {
                placement.anchorLine = stableId - base + 1;
                placement.anchorColumn = static_cast<uint16_t>(column + 1);
                placement.anchored = true;
            }
        }
    }

    for (auto const& [imageId, placement]: placements)
    {
        if (!placement.anchored)
            continue; // the image's top scrolled into history — a resync re-places it
        auto const* data = screen.imageData(imageId);
        if (data == nullptr)
            continue; // pixels not fetched yet; the image handler places it once they land

        if (_storedImages.insert(imageId).second)
        {
            auto const body = crispy::base64::encode(
                std::string_view { reinterpret_cast<char const*>(data->data.data()), data->data.size() });
            // GIP f = ImageFormat underlying + 1 (Auto=1, RGB=2, RGBA=3, PNG=4).
            out += std::format("\033P!go=u,n=muximg_{},f={},w={},h={};!{}\033\\",
                               imageId,
                               data->format + 1,
                               data->width,
                               data->height,
                               body);
        }
        out += cup(placement.anchorLine, placement.anchorColumn);
        // z=3 StretchToFill fills the reported cell box; L carries the layer; no `u`
        // header, so the placement must not move the cursor. (The source alignment/
        // resize policy is not on the native wire yet — a fidelity follow-up.)
        out += std::format("\033P!go=r,n=muximg_{},c={},r={},z=3,L={}\033\\",
                           imageId,
                           placement.cols,
                           placement.rows,
                           placement.layer);
    }
}

std::string ScreenMirror::applyImage(RemoteScreen const& screen, uint32_t imageId)
{
    auto out = std::string {};
    if (screen.imageData(imageId) == nullptr)
    {
        // Dropped server-side: release it so a reused id can never show stale pixels.
        if (_storedImages.erase(imageId) != 0)
            out += std::format("\033P!go=d,n=muximg_{}\033\\", imageId);
        return out;
    }
    // The pixels arrived after the delta that referenced them was already painted:
    // hide the cursor, (re)place the cached images, restore the cursor.
    out += "\033[?25l";
    appendImages(out, screen);
    out += cup(screen.cursorLine + 1, screen.cursorColumn + 1);
    if (containsValue(_setModes, VisibleCursorModeNumber))
        out += "\033[?25h";
    return out;
}

std::string ScreenMirror::applyEvent(proto::SessionEvent const& event)
{
    switch (static_cast<proto::SessionEventKind>(event.kind))
    {
        case proto::SessionEventKind::Bell: return "\a";
        case proto::SessionEventKind::Notify:
            // OSC 777 notify;title;body — the mirror terminal raises notify().
            return std::format("\033]777;notify;{};{}\033\\", event.a, event.b);
        case proto::SessionEventKind::ClipboardSet:
            // OSC 52 write, data re-encoded as base64; the mirror's copyToClipboard
            // fires under the client's own write permission.
            return std::format("\033]52;{};{}\033\\", event.a, crispy::base64::encode(event.b));
    }
    return {}; // unknown kind: ignore (forward compatibility)
}

} // namespace muxserver::client
