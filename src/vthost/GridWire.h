// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Grid → wire conversion: how a `vtbackend` row becomes a `proto::WireLine`.
///
/// This lives beside neither end on purpose. `src/vthost/proto` is deliberately
/// vtbackend-free (it is the byte format, nothing more), while the rule below is a
/// statement about BOTH ends of the protocol, so it cannot live in the daemon's
/// connection code either — the client has to agree with it, and a test has to be able
/// to check that it does.
///
/// **The omit/reconstruct contract.** A row does not send the trailing columns that
/// already match its own fill, and a row that is uniformly its fill sends no cells at
/// all. An absent column means: a cell wearing the row's WHOLE fill rendition — all four
/// of `fillForeground`, `fillBackground`, `fillUnderlineColor`, `fillFlags`, and no text.
/// The fill can therefore describe any pen the screen was erased with, which is what keeps
/// the omission working for the case it exists for: a full-screen clear under `\e[7m\e[2J`
/// over a deep scrollback sends nothing per row instead of a cell per column.
///
/// Every receiver must therefore paint the fill across a row before writing cells over
/// it; rendering `cells` alone truncates every filled region at the last column that
/// happened to be sent. @see proto::WireLine::cells, and the mirror's `renderRow`.

#include <vtbackend/Grid.h>
#include <vtbackend/Line.h>
#include <vtbackend/LineSoA.h>

#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include <vthost/proto/Pdu.h>

namespace vthost
{

/// Reinterprets a color as the raw bits the wire carries.
///
/// @param color The color to encode.
/// @return Its raw representation. NOTE a DEFAULT color does not encode as zero — the
///         type tag occupies the high bits — so `proto::WireCell`'s zero-initialised
///         color fields do not describe a default cell. @see fillCellOf.
[[nodiscard]] uint32_t rawColor(vtbackend::Color color) noexcept;

/// The inverse of @ref rawColor.
/// @param raw The bits off the wire.
/// @return The color they denote.
[[nodiscard]] vtbackend::Color colorFromRaw(uint32_t raw) noexcept;

/// Converts one column into its wire form.
/// @param soa The line's storage; must be materialized (a blank line has no columns).
/// @param column The column to convert.
/// @return That column's cell.
[[nodiscard]] proto::WireCell toWireCell(vtbackend::LineSoA const& soa, std::size_t column);

/// A textless cell wearing exactly @p attrs — what an omitted column decodes back to.
///
/// The one place a `GraphicsAttributes` becomes a `WireCell`, read by the trim (through
/// @ref fillCellOf), the expansion and the grid population, so a rendition field added to
/// `WireCell` cannot be honoured by one of them and forgotten by the others. Note the fields it
/// does NOT take from `proto::WireCell`'s own defaults: a reset `vtbackend::Color` does not
/// encode as zero.
/// @param attrs The pen to describe.
/// @return The cell that pen paints.
[[nodiscard]] proto::WireCell wireCellOf(vtbackend::GraphicsAttributes const& attrs);

/// The cell an omitted trailing column of a row with @p soa's fill decodes back to.
/// @param soa The line's storage, for its fill attributes.
/// @return The cell a receiver reconstructs for a column that never travelled.
[[nodiscard]] proto::WireCell fillCellOf(vtbackend::LineSoA const& soa);

/// Whether @p column carries an image fragment.
///
/// Such a column is never omitted. Its placement travels in a side table keyed by column,
/// so dropping the cell would not lose the image — but it would let the row's fill paint
/// over the columns the image occupies, and images are rare enough
/// (@see vtbackend::LineSoA::imageFragments: null on 99.99% of lines) that the saved bytes
/// are not worth reasoning about the interaction.
/// @param soa The line's storage.
/// @param column The column to test.
/// @return True when an image fragment covers it.
[[nodiscard]] bool carriesImage(vtbackend::LineSoA const& soa, std::size_t column);

/// Converts one grid row into its wire form (caller holds the terminal lock).
/// @param grid The grid the row belongs to, for its stable line id.
/// @param offset The row's offset within @p grid (negative addresses history).
/// @param line The row itself.
/// @return The wire row, with its trailing fill run omitted per the file comment.
[[nodiscard]] proto::WireLine toWireLine(vtbackend::Grid const& grid,
                                         vtbackend::LineOffset offset,
                                         vtbackend::Line const& line);

/// The pen a receiver clears a row with — the row's whole fill rendition, colours and flags alike.
///
/// The single definition of what an omitted column means, read by both directions — the trim
/// (through @ref fillCellOf), the expansion, and the grid population. @see the file comment.
/// @param line The wire row whose fill to interpret.
/// @return The attributes an absent column wears.
[[nodiscard]] vtbackend::GraphicsAttributes fillAttrsOf(proto::WireLine const& line);

/// Writes one wire cell into @p column of @p soa — the inverse of @ref toWireCell.
///
/// Goes through the engine's own `writeCellToSoA` rather than assigning the arrays, because that
/// is what maintains the state a direct write would silently corrupt: the `trivial` fast-path flag
/// the render path reads as O(1), the grapheme-cluster extra pool, and the image fragment a text
/// write replaces. The server's own measurements (width, scale) are then restored over it — the
/// authority on how many columns a cluster occupies is the terminal that measured it, never a
/// re-measurement here, which is how a late variation selector would disagree.
/// @param soa The line's storage; must already be materialized.
/// @param column The column to write.
/// @param cell The cell off the wire.
/// @param hyperlink The LOCAL hyperlink id for this cell (wire ids are the sender's; @see
///        applyWireLine).
void applyWireCell(vtbackend::LineSoA& soa,
                   std::size_t column,
                   proto::WireCell const& cell,
                   vtbackend::HyperlinkId hyperlink);

/// Brings @p line to exactly what @p wire describes — the inverse of @ref toWireLine.
///
/// Clears the row to its fill first, which is both how stale content goes and how the omitted
/// trailing columns get their color; then writes the cells that travelled. @p line keeps its OWN
/// width: a wire row wider than the receiver's grid is truncated rather than overrunning it, which
/// is the transient state between a pane resizing and the daemon re-projecting.
/// @param line The grid row to overwrite.
/// @param wire What it should hold.
/// @param hyperlinks Wire hyperlink id → this terminal's own id. Ids are per-terminal counters, so
///        a cell's id is meaningless until translated; an unmapped id becomes "no hyperlink".
void applyWireLine(vtbackend::Line& line,
                   proto::WireLine const& wire,
                   std::unordered_map<uint16_t, vtbackend::HyperlinkId> const& hyperlinks);

/// Restores every column of @p line, undoing the trailing-fill omission.
///
/// The inverse of the contract above, and the reason it is stated in one place: a receiver
/// reconstructs these columns by painting the fill, so anything comparing two rows for
/// equivalence must reconstruct them too. Without it a blank row and a materialized row of
/// identical appearance compare as different, because how a row is STORED decides how much
/// of it travels.
/// @param line The wire row to expand.
/// @return Exactly `line.columns` cells.
[[nodiscard]] std::vector<proto::WireCell> expandToFullWidth(proto::WireLine const& line);

} // namespace vthost
