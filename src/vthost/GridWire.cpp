// SPDX-License-Identifier: Apache-2.0
#include <vthost/GridWire.hpp>

#include <vtbackend/core/CellFlags.hpp>
#include <vtbackend/core/LineFlags.hpp>
#include <vtbackend/grid/SoAClusterWriter.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <ranges>
#include <utility>
#include <vector>

namespace vthost
{

// The wire carries vtbackend's own flag bits verbatim (@see proto::WireLine::flags,
// proto::WireCell::flags), which is only sound while those bits are FROZEN. The tables below are a
// deliberately INDEPENDENT copy of the bit assignments: they exist to disagree. Renumbering a flag
// in VTBACKEND_LINE_FLAGS or VTBACKEND_CELL_FLAGS then fails to compile here, which is where the
// question "does this need a proto::CodecVersion bump?" has to be answered — instead of a peer on
// the old numbering silently decoding a curly underline as a dotted one.
//
// Asserting against the generating macro itself would be tautological (it defines the enumerator AS
// `1U << Bit`), so these numbers are written out on purpose. Adding a flag needs one row here too;
// forgetting it is caught by the count assertion at the end of each block.
namespace
{
    constexpr auto PinnedLineFlagBits = std::array {
        std::pair { vtbackend::LineFlag::Wrappable, 0 },
        std::pair { vtbackend::LineFlag::Wrapped, 1 },
        std::pair { vtbackend::LineFlag::Marked, 2 },
        std::pair { vtbackend::LineFlag::OutputStart, 3 },
        std::pair { vtbackend::LineFlag::DoubleWidth, 4 },
        std::pair { vtbackend::LineFlag::DoubleHeightTop, 5 },
        std::pair { vtbackend::LineFlag::DoubleHeightBottom, 6 },
        std::pair { vtbackend::LineFlag::CommandEnd, 7 },
        std::pair { vtbackend::LineFlag::PromptEnd, 8 },
    };

    constexpr auto PinnedCellFlagBits = std::array {
        std::pair { vtbackend::CellFlag::Bold, 0 },
        std::pair { vtbackend::CellFlag::Faint, 1 },
        std::pair { vtbackend::CellFlag::Italic, 2 },
        std::pair { vtbackend::CellFlag::Underline, 3 },
        std::pair { vtbackend::CellFlag::Blinking, 4 },
        std::pair { vtbackend::CellFlag::Inverse, 5 },
        std::pair { vtbackend::CellFlag::Hidden, 6 },
        std::pair { vtbackend::CellFlag::CrossedOut, 7 },
        std::pair { vtbackend::CellFlag::DoublyUnderlined, 8 },
        std::pair { vtbackend::CellFlag::CurlyUnderlined, 9 },
        std::pair { vtbackend::CellFlag::DottedUnderline, 10 },
        std::pair { vtbackend::CellFlag::DashedUnderline, 11 },
        std::pair { vtbackend::CellFlag::Framed, 12 },
        std::pair { vtbackend::CellFlag::Encircled, 13 },
        std::pair { vtbackend::CellFlag::Overline, 14 },
        std::pair { vtbackend::CellFlag::RapidBlinking, 15 },
        std::pair { vtbackend::CellFlag::CharacterProtected, 16 },
        std::pair { vtbackend::CellFlag::WideCharContinuation, 17 },
        std::pair { vtbackend::CellFlag::CharacterProtectedISO, 18 },
        std::pair { vtbackend::CellFlag::MulticellContinuation, 19 },
    };

    template <typename Flag, std::size_t N>
    [[nodiscard]] constexpr bool bitsAreAsPinned(std::array<std::pair<Flag, int>, N> const& pinned)
    {
        for (auto const& [flag, bit]: pinned)
            if (static_cast<uint32_t>(flag) != (1U << static_cast<unsigned>(bit)))
                return false;
        return true;
    }
} // namespace

static_assert(bitsAreAsPinned(PinnedLineFlagBits),
              "A LineFlag's bit value changed. Those bits travel on the native wire verbatim, so an "
              "older peer would mis-decode them: bump proto::CodecVersion (or restore the bit) and "
              "update the pin above.");
static_assert(PinnedLineFlagBits.size() == vtbackend::LineFlagList.size(),
              "A LineFlag was added or removed. Add (or drop) its row in PinnedLineFlagBits, which is "
              "what freezes the wire encoding.");
static_assert(bitsAreAsPinned(PinnedCellFlagBits),
              "A CellFlag's bit value changed. Those bits travel on the native wire verbatim, so an "
              "older peer would mis-decode them: bump proto::CodecVersion (or restore the bit) and "
              "update the pin above.");
static_assert(PinnedCellFlagBits.size() == vtbackend::CellFlagList.size(),
              "A CellFlag was added or removed. Add (or drop) its row in PinnedCellFlagBits, which is "
              "what freezes the wire encoding.");

uint32_t rawColor(vtbackend::Color color) noexcept
{
    static_assert(sizeof(vtbackend::Color) == sizeof(uint32_t));
    return std::bit_cast<uint32_t>(color);
}

vtbackend::Color colorFromRaw(uint32_t raw) noexcept
{
    return std::bit_cast<vtbackend::Color>(raw);
}

proto::WireCell toWireCell(vtbackend::LineSoA const& soa, std::size_t column)
{
    auto cell = proto::WireCell {};
    cell.codepoint = soa.codepoints[column];
    if (soa.clusterSize[column] >= 2)
    {
        auto const extras = static_cast<std::size_t>(soa.clusterSize[column] - 1);
        auto const start = soa.clusterPoolIndex[column];
        for (std::size_t i = 0; i < extras && start + i < soa.clusterPool.size(); ++i)
            cell.clusterExtras.push_back(soa.clusterPool[start + i]);
    }
    cell.width = soa.widths[column];
    cell.scale = soa.scales[column];
    cell.textScaleExtras = soa.textScaleExtras[column];
    cell.hyperlink = unbox<uint16_t>(soa.hyperlinks[column]);
    cell.foreground = rawColor(soa.sgr[column].foregroundColor);
    cell.background = rawColor(soa.sgr[column].backgroundColor);
    cell.underlineColor = rawColor(soa.sgr[column].underlineColor);
    cell.flags = static_cast<uint32_t>(soa.sgr[column].flags.value());
    return cell;
}

proto::WireCell wireCellOf(vtbackend::GraphicsAttributes const& attrs)
{
    // WireCell's own defaults give the ordinary GEOMETRY: width 1, scale 1, no extras. Only the
    // rendition comes from the pen — and all four parts of it, because that is what an omitted
    // column has to be reconstructible from.
    auto cell = proto::WireCell {};
    cell.foreground = rawColor(attrs.foregroundColor);
    cell.background = rawColor(attrs.backgroundColor);
    cell.underlineColor = rawColor(attrs.underlineColor);
    cell.flags = static_cast<uint32_t>(attrs.flags.value());
    return cell;
}

proto::WireCell fillCellOf(vtbackend::LineSoA const& soa)
{
    return wireCellOf(soa.fillAttrs);
}

vtbackend::GraphicsAttributes fillAttrsOf(proto::WireLine const& line)
{
    auto attrs = vtbackend::GraphicsAttributes {};
    attrs.foregroundColor = colorFromRaw(line.fillForeground);
    attrs.backgroundColor = colorFromRaw(line.fillBackground);
    attrs.underlineColor = colorFromRaw(line.fillUnderlineColor);
    attrs.flags = vtbackend::CellFlags::fromValue(line.fillFlags);
    return attrs;
}

void applyWireCell(vtbackend::LineSoA& soa,
                   std::size_t column,
                   proto::WireCell const& cell,
                   vtbackend::HyperlinkId hyperlink)
{
    auto const attrs = vtbackend::GraphicsAttributes { .foregroundColor = colorFromRaw(cell.foreground),
                                                       .backgroundColor = colorFromRaw(cell.background),
                                                       .underlineColor = colorFromRaw(cell.underlineColor),
                                                       .flags = vtbackend::CellFlags::fromValue(cell.flags) };
    vtbackend::writeCellToSoA(soa, column, cell.codepoint, cell.width, attrs, hyperlink);
    for (auto const extra: cell.clusterExtras)
        vtbackend::appendCodepointToCluster(
            soa, column, extra, vtbackend::ClusterWidthPolicy::FirstCodepoint);
    // Restored AFTER the writes above, both of which have their own opinion: writeCellToSoA resets
    // scale to 1, and appending a cluster codepoint re-measures the cluster's width. Neither may
    // overrule the sender, which is the terminal that actually ran the emulation.
    soa.widths[column] = cell.width;
    soa.scales[column] = cell.scale;
    soa.textScaleExtras[column] = cell.textScaleExtras;
}

void applyWireLine(vtbackend::Line& line,
                   proto::WireLine const& wire,
                   std::unordered_map<uint16_t, vtbackend::HyperlinkId> const& hyperlinks,
                   ContextIdMap const& contexts)
{
    auto const flags = vtbackend::LineFlags::fromValue(wire.flags);
    // The clear does three jobs at once: it drops stale content, it gives the omitted trailing
    // columns their color, and it puts the row into the blank state when nothing travelled — which
    // is what makes a uniformly-filled row as cheap on this side as it was on the wire.
    line.reset(flags, fillAttrsOf(wire));
    // After the reset, never before: Line::reset() clears both offsets.
    line.setPromptEndOffset(vtbackend::ColumnOffset(wire.promptEndOffset));
    line.setCommandEndOffset(vtbackend::ColumnOffset(wire.commandEndOffset));
    // Translated, never copied: the id is the SENDER's, and this terminal mints its own. An id with
    // no mapping resolves to none, exactly as an unmapped hyperlink id does. The zero test first,
    // because a session that never sees OSC 3008 stamps every line with it and would otherwise pay a
    // hash lookup per line per delta to learn nothing.
    if (wire.contextId != 0)
        if (auto const it = contexts.find(wire.contextId); it != contexts.end())
            line.adoptContext(it->second.local);
    if (wire.cells.empty())
        return;

    auto const columns = std::min(wire.cells.size(), unbox<std::size_t>(line.size()));
    auto& soa = line.materializedStorage();
    for (auto const column: std::views::iota(std::size_t { 0 }, columns))
    {
        auto const& cell = wire.cells[column];
        auto localLink = vtbackend::HyperlinkId {};
        if (auto const it = hyperlinks.find(cell.hyperlink); it != hyperlinks.end())
            localLink = it->second;
        applyWireCell(soa, column, cell, localLink);
    }
}

bool carriesImage(vtbackend::LineSoA const& soa, std::size_t column)
{
    if (!soa.imageFragments.has_value())
        return false;
    auto const it = soa.imageFragments->find(static_cast<uint16_t>(column));
    // A null fragment is not an image: the image side table skips those entries, so counting
    // them here would keep columns it never mentions.
    return it != soa.imageFragments->end() && it->second != nullptr;
}

proto::WireLine toWireLine(vtbackend::Grid const& grid,
                           vtbackend::LineOffset offset,
                           vtbackend::Line const& line)
{
    auto wire = proto::WireLine {};
    wire.stableId = grid.stableLineIdOf(offset);
    wire.flags = static_cast<uint16_t>(line.flags().value());
    wire.contextId = unbox<uint16_t>(line.contextId());
    wire.promptEndOffset = unbox<int32_t>(line.promptEndOffset());
    wire.commandEndOffset = unbox<int32_t>(line.commandEndOffset());
    wire.columns = unbox<uint32_t>(line.size());

    auto const& soa = line.storage();
    wire.fillForeground = rawColor(soa.fillAttrs.foregroundColor);
    wire.fillBackground = rawColor(soa.fillAttrs.backgroundColor);
    wire.fillUnderlineColor = rawColor(soa.fillAttrs.underlineColor);
    wire.fillFlags = static_cast<uint32_t>(soa.fillAttrs.flags.value());
    if (line.isBlank())
        return wire; // uniformly filled: no cells on the wire

    // Only the trailing RUN may go: an interior fill-equal cell still has to hold its column,
    // because the wire has no way to say "skip forward".
    //
    // The trim point is found BEFORE anything is built, so the reservation below is exact.
    // Building the whole row and shrinking afterwards would leave every WireLine holding
    // capacity for its full width — and a snapshot accumulates every line of the history
    // before a byte is encoded, so that capacity would all be resident at once.
    auto const columns = unbox<std::size_t>(line.size());
    auto const fillCell = fillCellOf(soa);
    auto meaningful = columns; // one past the last column worth sending
    while (meaningful > 0 && !carriesImage(soa, meaningful - 1)
           && toWireCell(soa, meaningful - 1) == fillCell)
        --meaningful;

    wire.cells.reserve(meaningful);
    for (auto const column: std::views::iota(std::size_t { 0 }, meaningful))
        wire.cells.push_back(toWireCell(soa, column));
    return wire;
}

std::vector<proto::WireCell> expandToFullWidth(proto::WireLine const& line)
{
    auto cells = line.cells;
    // Through fillAttrsOf and wireCellOf, so this cannot drift from what a receiver actually paints
    // nor from what the trim compared against.
    auto const reconstructed = wireCellOf(fillAttrsOf(line));
    cells.resize(std::max<std::size_t>(cells.size(), line.columns), reconstructed);
    return cells;
}

} // namespace vthost
