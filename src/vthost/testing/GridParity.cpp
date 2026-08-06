// SPDX-License-Identifier: Apache-2.0
#include <vthost/testing/GridParity.hpp>

#include <vtbackend/Image.hpp>
#include <vtbackend/LineFlags.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <ranges>
#include <string_view>
#include <utility>

#include <vthost/GridWire.hpp>

namespace vthost::testing
{

namespace
{
    /// @return The URI @p id names on @p terminal, or empty for "no hyperlink".
    [[nodiscard]] std::string uriOf(vtbackend::Terminal const& terminal, uint16_t id)
    {
        if (id == 0)
            return {};
        auto const info = terminal.hyperlinks().hyperlinkById(vtbackend::HyperlinkId { id });
        return info ? info->uri : std::string { "<unresolved>" };
    }

    /// Names the line flags set in @p raw, so a report says `Wrapped` rather than `0x2`.
    ///
    /// Through vtbackend's own formatter, which is generated from the same table as the
    /// enumerators — so a flag added there cannot go unnamed here.
    [[nodiscard]] std::string describeLineFlags(uint16_t raw)
    {
        if (raw == 0)
            return "none";
        return std::format("{}", vtbackend::LineFlags::from_value(raw));
    }

    /// Whether @p raw carries the flag that gives `WireLine::promptEndOffset` its meaning.
    [[nodiscard]] bool marksPromptEnd(uint16_t raw)
    {
        return vtbackend::LineFlags::from_value(raw).contains(vtbackend::LineFlag::PromptEnd);
    }

    /// Whether @p raw carries the flag that gives `WireLine::commandEndOffset` its meaning.
    [[nodiscard]] bool marksCommandEnd(uint16_t raw)
    {
        return vtbackend::LineFlags::from_value(raw).contains(vtbackend::LineFlag::CommandEnd);
    }

    /// Names the SGR flags set in @p raw.
    [[nodiscard]] std::string describeCellFlags(uint32_t raw)
    {
        auto out = std::string {};
        for (auto const flag: vtbackend::CellFlagList)
            if ((raw & static_cast<uint32_t>(flag)) != 0)
                out += (out.empty() ? "" : "|") + std::format("{}", flag);
        return out.empty() ? "none" : out;
    }

    /// Describes the image fragment covering @p column of @p soa, or "none".
    ///
    /// Images are the one thing in the grid that `toWireLine` does NOT carry — they travel in a side
    /// table, so a comparison built only on wire rows is blind to them, and an image probe would pass
    /// with the mirror showing nothing at all.
    ///
    /// A description rather than a field-by-field comparison, because an image's IDENTITY is not
    /// comparable across terminals (pool ids are per-terminal counters) while its shape is: the source
    /// pixels' size and format, the cell box it was rasterized into, the placement policies, and which
    /// tile of it this cell shows. `cellSize` is deliberately absent — the pixel size of a cell is a
    /// property of the DISPLAY, so two faithful mirrors on different screens legitimately differ.
    [[nodiscard]] std::string describeImage(vtbackend::LineSoA const& soa, std::size_t column)
    {
        if (!soa.imageFragments.has_value())
            return "none";
        auto const it = soa.imageFragments->find(static_cast<uint16_t>(column));
        if (it == soa.imageFragments->end() || it->second == nullptr)
            return "none";
        auto const& fragment = *it->second;
        auto const& rasterized = fragment.rasterizedImage();
        auto const& image = rasterized.image();
        return std::format("{}x{} fmt={} span={}x{} layer={} align={} resize={} tile={},{}",
                           unbox<int>(image.width()),
                           unbox<int>(image.height()),
                           std::to_underlying(image.format()),
                           unbox<int>(rasterized.cellSpan().lines),
                           unbox<int>(rasterized.cellSpan().columns),
                           std::to_underlying(rasterized.layer()),
                           std::to_underlying(rasterized.alignmentPolicy()),
                           std::to_underlying(rasterized.resizePolicy()),
                           unbox<int>(fragment.offset().line),
                           unbox<int>(fragment.offset().column));
    }

    /// A cell's grapheme cluster as text, for a legible report.
    [[nodiscard]] std::string describeText(proto::WireCell const& cell)
    {
        if (cell.codepoint == 0)
            return "<none>";
        auto out = std::format("U+{:04X}", static_cast<uint32_t>(cell.codepoint));
        for (auto const extra: cell.clusterExtras)
            out += std::format("+U+{:04X}", static_cast<uint32_t>(extra));
        return out;
    }

    /// One comparable property of a wire row, and how to render it for the report.
    ///
    /// A TABLE rather than a chain of `if (a != b) note(...)`, for the reason the rest of this
    /// codebase prefers tables: a field added to `proto::WireLine` becomes one row here, and the
    /// comparator cannot quietly omit it the way a hand-written chain does. Comparing the RENDERED
    /// text is exact — every renderer below is injective — and it is the text the report needs
    /// anyway, so no value is formatted twice.
    struct LineField
    {
        std::string_view name;
        std::string (*render)(proto::WireLine const&);
    };

    constexpr auto LineFields = std::array {
        LineField { "lineFlags", [](proto::WireLine const& l) { return describeLineFlags(l.flags); } },
        // The offsets read as "n/a" without their owning flag, because that is what they MEAN there:
        // every row erase clears them (@see Line::reset), so on an unmarked row they are
        // stale-but-unread rather than wrong, and comparing the raw value would report noise.
        LineField { "promptEndOffset",
                    [](proto::WireLine const& l) {
                        return marksPromptEnd(l.flags) ? std::format("{}", l.promptEndOffset)
                                                       : std::string { "n/a" };
                    } },
        LineField { "commandEndOffset",
                    [](proto::WireLine const& l) {
                        return marksCommandEnd(l.flags) ? std::format("{}", l.commandEndOffset)
                                                        : std::string { "n/a" };
                    } },
        // The fill is compared in its own right, not merely through the expansion of the cells: it
        // decides the colour of the columns a later resize newly exposes, so two rows whose every
        // present cell matches can still differ where it matters.
        LineField { "fill",
                    [](proto::WireLine const& l) {
                        return std::format("{:#x}/{:#x}/{:#x}/{}",
                                           l.fillForeground,
                                           l.fillBackground,
                                           l.fillUnderlineColor,
                                           describeCellFlags(l.fillFlags));
                    } },
    };

    /// One comparable property of a wire cell. @see LineField for why this is a table.
    ///
    /// `hyperlink` is deliberately absent: resolving one needs the terminal, which a renderer taking
    /// only the cell cannot reach.
    struct CellField
    {
        std::string_view name;
        std::string (*render)(proto::WireCell const&);
    };

    constexpr auto CellFields = std::array {
        CellField { "text", describeText },
        CellField { "width", [](proto::WireCell const& c) { return std::format("{}", c.width); } },
        CellField {
            "textScale",
            [](proto::WireCell const& c) { return std::format("{}/{:#x}", c.scale, c.textScaleExtras); } },
        CellField { "foreground",
                    [](proto::WireCell const& c) { return std::format("{:#x}", c.foreground); } },
        CellField { "background",
                    [](proto::WireCell const& c) { return std::format("{:#x}", c.background); } },
        CellField { "underlineColor",
                    [](proto::WireCell const& c) { return std::format("{:#x}", c.underlineColor); } },
        CellField { "cellFlags", [](proto::WireCell const& c) { return describeCellFlags(c.flags); } },
    };

    /// Records a divergence, unless @p gaps is already at its cap.
    void note(std::vector<ParityGap>& gaps,
              std::size_t maxGaps,
              int64_t line,
              int column,
              std::string field,
              std::string expected,
              std::string actual)
    {
        if (gaps.size() >= maxGaps)
            return;
        gaps.push_back(ParityGap { .line = line,
                                   .column = column,
                                   .field = std::move(field),
                                   .expected = std::move(expected),
                                   .actual = std::move(actual) });
    }
} // namespace

namespace
{
    /// @return The page @p page names on @p terminal.
    [[nodiscard]] vtbackend::Screen const& pageOf(vtbackend::Terminal const& terminal, ComparedPage page)
    {
        if (page == ComparedPage::HostWritableStatus)
            return terminal.hostWritableStatusLineDisplay();
        // The DISPLAYED page, which is the one the daemon serializes (@see
        // Terminal::displayedPageIndex, and NativeSession's pushDelta). Three near-misses this
        // avoids:
        //
        //  - `currentScreen()` is the page VT OUTPUT targets, which a decoupled display (DECPCCM
        //    off) or any NP/PP flip makes a different page from the one on screen;
        //  - `activeDisplay()` follows DECSASD, so an app writing its status line would redirect the
        //    comparison away from the page under test;
        //  - `isAlternateScreen() ? alternateScreen() : primaryScreen()` reads the xterm alt page,
        //    which is NOT where DEC pages 1..14 live — they are `pageAt(n)`, and reading the alt page
        //    instead compares an unrelated blank grid.
        //
        // Symmetric on both sides even though the mirror keeps a page-1..14 display in its ALT page
        // (the wire's screenType has only primary/alt-like to say it with): the mirror's displayed
        // index then IS that alt page, so both sides resolve to the grid actually holding the content.
        return terminal.displayedPage();
    }
} // namespace

std::vector<ParityGap> compareGrids(vtbackend::Terminal const& server,
                                    vtbackend::Terminal const& mirror,
                                    ComparedPage page,
                                    std::size_t maxGaps)
{
    auto gaps = std::vector<ParityGap> {};
    auto const& serverGrid = pageOf(server, page).grid();
    auto const& mirrorGrid = pageOf(mirror, page).grid();

    // Compare over the rows BOTH address. A shallower mirror history is a capacity choice,
    // not a fidelity gap, so it is reported once rather than as a divergence per missing row.
    auto const serverHistory = unbox<int64_t>(serverGrid.historyLineCount());
    auto const mirrorHistory = unbox<int64_t>(mirrorGrid.historyLineCount());
    if (serverHistory != mirrorHistory)
        note(gaps,
             maxGaps,
             0,
             -1,
             "historyLineCount",
             std::format("{}", serverHistory),
             std::format("{}", mirrorHistory));

    auto const lines = unbox<int64_t>(serverGrid.pageSize().lines);
    if (lines != unbox<int64_t>(mirrorGrid.pageSize().lines))
        note(gaps,
             maxGaps,
             0,
             -1,
             "pageSize.lines",
             std::format("{}", lines),
             std::format("{}", unbox<int64_t>(mirrorGrid.pageSize().lines)));

    auto const top = -std::min(serverHistory, mirrorHistory);
    for (auto const row: std::views::iota(top, lines))
    {
        // Stop once the cap is reached instead of walking the rest to hand `note` findings it will
        // drop. Result-preserving — the gaps are in row-then-column order, so the first `maxGaps`
        // are the same either way — and it is what makes the probes that poll with `maxGaps = 1`
        // affordable: those call this on a loop until two grids converge, and every call before
        // they do would otherwise re-render every cell of both grids through std::format.
        if (gaps.size() >= maxGaps)
            break;
        auto const offset = vtbackend::LineOffset::cast_from(row);
        auto const expected = toWireLine(serverGrid, offset, serverGrid.lineAt(offset));
        auto const actual = toWireLine(mirrorGrid, offset, mirrorGrid.lineAt(offset));

        if (expected.columns != actual.columns)
        {
            note(gaps,
                 maxGaps,
                 row,
                 -1,
                 "columns",
                 std::format("{}", expected.columns),
                 std::format("{}", actual.columns));
            continue; // a width difference makes per-column comparison meaningless
        }
        for (auto const& field: LineFields)
            if (auto const want = field.render(expected), got = field.render(actual); want != got)
                note(gaps, maxGaps, row, -1, std::string { field.name }, want, got);

        // Expanded first: how a row is STORED (blank vs materialized) decides how much of it
        // travels, and that is not a parity difference.
        auto const expectedCells = expandToFullWidth(expected);
        auto const actualCells = expandToFullWidth(actual);
        auto const& expectedSoa = serverGrid.lineAt(offset).storage();
        auto const& actualSoa = mirrorGrid.lineAt(offset).storage();
        for (auto const column: std::views::iota(std::size_t { 0 }, expectedCells.size()))
        {
            auto const col = static_cast<int>(column);
            // Images are read off the STORAGE, not the wire row: they are the one part of a grid row
            // that travels beside it rather than inside it, so a comparison built on wire rows alone
            // is blind to them.
            if (auto const want = describeImage(expectedSoa, column), got = describeImage(actualSoa, column);
                want != got)
                note(gaps, maxGaps, row, col, "image", want, got);

            for (auto const& field: CellFields)
                if (auto const want = field.render(expectedCells[column]),
                    got = field.render(actualCells[column]);
                    want != got)
                    note(gaps, maxGaps, row, col, std::string { field.name }, want, got);

            // Hyperlinks stand outside the table because resolving one needs the TERMINAL: the ids
            // are per-terminal counters, so equal ids would be meaningless and unequal ones a false
            // positive. Only the URI is comparable.
            if (auto const want = uriOf(server, expectedCells[column].hyperlink),
                got = uriOf(mirror, actualCells[column].hyperlink);
                want != got)
                note(gaps, maxGaps, row, col, "hyperlink", want, got);
        }
    }
    return gaps;
}

std::string describeGaps(std::vector<ParityGap> const& gaps)
{
    auto out = std::string {};
    for (auto const& gap: gaps)
    {
        if (gap.column < 0)
            out += std::format(
                "line {}: {}: expected {}, got {}\n", gap.line, gap.field, gap.expected, gap.actual);
        else
            out += std::format("line {} col {}: {}: expected {}, got {}\n",
                               gap.line,
                               gap.column,
                               gap.field,
                               gap.expected,
                               gap.actual);
    }
    return out;
}

std::string summarizeGaps(std::vector<ParityGap> const& gaps)
{
    auto counts = std::map<std::string, std::size_t> {};
    for (auto const& gap: gaps)
        ++counts[gap.field];

    auto ordered = std::vector<std::pair<std::string, std::size_t>> { counts.begin(), counts.end() };
    std::ranges::sort(ordered, [](auto const& a, auto const& b) { return a.second > b.second; });

    auto out = std::string {};
    for (auto const& [field, count]: ordered)
        out += std::format("  {:<16} {} occurrence(s)\n", field, count);
    return out;
}

} // namespace vthost::testing
