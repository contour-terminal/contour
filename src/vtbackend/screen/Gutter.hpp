// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/core/CommandBlockOutcome.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/render/RenderBuffer.hpp>
#include <vtbackend/screen/GutterSettings.hpp>
#include <vtbackend/screen/LocalTime.hpp>
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/shell/Folding.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vtbackend
{

// {{{ timestamps

/// Spells @p when in @p format.
/// @param format A std::format chrono specification without the braces.
/// @param when The local civil time to spell.
/// @return The text, or empty when @p format cannot spell it.
[[nodiscard]] std::string formatGutterTimestamp(std::string_view format, std::chrono::local_seconds when);

/// Spells the timestamps of a run of rows, top to bottom, leaving out each one that reads the same as
/// the row above -- so a command's output does not repeat the second it was printed in on every line.
///
/// Compares the SPELLED text, not only the instants: with "%H:%M" two rows seconds apart read the same,
/// and showing the second would say nothing new. An instant equal to the row above's is not even spelled.
class TimestampRun
{
  public:
    /// @param format A usable format, as resolveTimestampFormat() picks it (GutterLayout::timestampFormat).
    /// @param localTime The time zone the rows' times are read in.
    TimestampRun(std::string_view format, LocalTimeConverter localTime);

    /// The text the next row down shows.
    /// @param when The row's birth time; nullopt for a row never stamped.
    /// @return The time, or empty when the row has none or reads like the one above.
    [[nodiscard]] std::string next(std::optional<std::chrono::system_clock::time_point> when);

  private:
    /// @p when in the run's format; empty when the formatter refuses it.
    [[nodiscard]] std::string spell(std::chrono::local_seconds when) const;

    std::string _field;            ///< The format as a std::format replacement field, built once.
    LocalTimeConverter _localTime; ///< The time zone the rows' times are read in.
    std::optional<std::chrono::system_clock::time_point> _aboveInstant; ///< The row above's time.
    std::string _above; ///< What the row above spelled, shown or not; empty before the first row.
};

// }}}

// {{{ line numbers

/// What a row's line number reads under @p mode.
///
/// @param mode Which numbers the gutter shows. Off never reaches here: there is no segment to fill.
/// @param absoluteNumber The row's own number -- Grid::evictedRowCount() plus its distance from the
///                       addressable top, plus one.
/// @param cursorDistance The signed VISIBLE-row distance from the cursor row (Terminal::visibleDistance),
///                       so a collapsed fold counts as one row -- exactly what `j` / `k` count.
/// @return The number to show.
[[nodiscard]] uint64_t lineNumberFor(LineNumberMode mode,
                                     uint64_t absoluteNumber,
                                     int64_t cursorDistance) noexcept;

/// A line number as its segment shows it, held inline: it is spelled for every drawn row of every frame,
/// and is never wider than MaxLineNumberWidth.
class LineNumberText
{
  public:
    LineNumberText() = default;

    /// @param text The text, cut at MaxLineNumberWidth codepoints.
    explicit LineNumberText(std::u32string_view text) noexcept
    {
        for (auto const codepoint: text.substr(0, _codepoints.size()))
            append(codepoint);
    }

    /// The text, one codepoint per column.
    [[nodiscard]] std::u32string_view view() const noexcept { return { _codepoints.data(), _size }; }

    /// Whether there is no text at all.
    [[nodiscard]] bool empty() const noexcept { return _size == 0; }

    /// Appends @p codepoint, which must still fit.
    /// @param codepoint One column of text.
    void append(char32_t codepoint) noexcept { _codepoints.at(_size++) = codepoint; }

    /// @param text The text to compare with.
    /// @return Whether this reads exactly @p text.
    [[nodiscard]] bool operator==(std::u32string_view text) const noexcept { return view() == text; }

  private:
    std::array<char32_t, static_cast<std::size_t>(MaxLineNumberWidth)> _codepoints {};
    std::size_t _size = 0;
};

/// @p number, right-aligned in exactly @p width columns.
///
/// A number wider than that keeps its LAST width - 1 digits behind a `…`: the tail tells two nearby rows
/// apart, while the head is the same for a screenful of them.
/// @param number The number to spell.
/// @param width The columns to fill, at most MaxLineNumberWidth; none at all when it is not positive.
/// @return The text, one codepoint per column.
[[nodiscard]] LineNumberText lineNumberText(uint64_t number, int width) noexcept;

// }}}

// {{{ block column

/// What one row of the block column shows. Every glyph is drawn by vtrasterizer's box-drawing renderer,
/// so all of them are one size and weight in every font.
enum class BlockColumnGlyph : uint8_t
{
    None = 0,      ///< Nothing.
    FoldExpanded,  ///< The head of an expanded fold.
    FoldCollapsed, ///< The head of a collapsed fold.
    FoldBody,      ///< A row inside an expanded fold.
    FoldBodyEnd,   ///< The last row of an expanded fold.
    UserMark,      ///< A diamond, U+10F003 -- a row the user marked with vi `mm`.
    Running,       ///< A small triangle, U+10F002 -- the head of a command that has not finished.
    Finished,      ///< A disc, U+10F004 -- the head of a finished command with no fold drawn beside it.
    Failed,        ///< A cross, U+10F005 -- the head of a FAILED command with no fold drawn beside it.
    Count,         ///< Not a glyph; pins the codepoint table.
};

/// How heavy a row's fold strokes are drawn: its head's outline and stem, its body, its end.
///
/// One step heavier for a command that failed -- failure told by shape as well as by colour, which a reader
/// who cannot tell red from grey needs -- and one more for the fold under the pointer, where
/// GutterSettings::hoverStroke asks for it. A weight rather than more glyphs: what a row shows and how heavy
/// it is drawn are two facts, decided apart.
enum class FoldWeight : uint8_t
{
    Light = 0, ///< The text's own stroke.
    Heavy,     ///< Twice it.
    Heavier,   ///< Three times it.
    Count,     ///< Not a weight; pins the codepoint table.
};

/// Whether a row heads a command block, and in which state.
enum class BlockHeadState : uint8_t
{
    None = 0, ///< Not a head -- or the head of a prompt still being typed at, which says nothing yet.
    Running,  ///< The head of a command that is running.
    Finished, ///< The head of a command that has finished.
};

/// Whether a row carries the user's own mark (LineFlag::UserMark).
enum class UserMarkPresence : uint8_t
{
    Absent = 0, ///< The row carries no user mark.
    Present,    ///< vi `mm` marked the row.
};

/// Whether the pointer is over the fold a gutter row belongs to.
enum class GutterHover : uint8_t
{
    Rest = 0, ///< The pointer is elsewhere.
    Hovered,  ///< The pointer is over the fold the row belongs to.
};

/// The facts about one row that decide its block-column glyph, gathered by Terminal::fillGutter.
struct BlockColumnFacts
{
    FoldMarker fold = FoldMarker::None;               ///< Which part of a fold's column the row carries.
    BlockHeadState head = BlockHeadState::None;       ///< Whether it heads a running or a finished block.
    UserMarkPresence mark = UserMarkPresence::Absent; ///< Whether the user marked it.

    /// How the row's block ended; nullopt for a row of no block, or with exit status off.
    std::optional<CommandBlockOutcome> outcome {};

    bool operator==(BlockColumnFacts const&) const = default;
};

/// What the block column shows beside a row with @p facts, by precedence: a fold head, a user mark on any
/// other row, a fold body, a running command's head, a finished command's head.
///
/// The precedence alone gives a finished block with no foldable output its mark: a finished head that has
/// a fold drawn shows the fold control, which outranks the mark. With fold markers off no fold is drawn,
/// so every finished head shows its mark.
///
/// A failed command is told apart by shape as well as by colour: a cross rather than a disc on its head, and
/// its fold drawn heavier (foldWeightOf()) -- both only with exit status on, which is what colours them too.
/// @param facts What is true of the row.
/// @param settings Which of the column's jobs are switched on.
/// @return The glyph, or BlockColumnGlyph::None.
[[nodiscard]] BlockColumnGlyph blockColumnGlyph(BlockColumnFacts const& facts,
                                                GutterSettings const& settings) noexcept;

/// How heavy the fold strokes beside a row with @p facts are drawn.
/// @param facts What is true of the row.
/// @param settings The gutter's settings: exit status, and what hovering does.
/// @param hover Whether the pointer is over the row's fold.
/// @return Light, one step heavier where the command failed, and one more where the fold is hovered and
///         GutterSettings::hoverStroke asks for it.
[[nodiscard]] FoldWeight foldWeightOf(BlockColumnFacts const& facts,
                                      GutterSettings const& settings,
                                      GutterHover hover) noexcept;

/// Whether a press on the block column beside a row can toggle a fold there.
enum class FoldToggle : uint8_t
{
    Unavailable = 0, ///< No fold reaches the row, or folding or its controls are off.
    Available,       ///< A fold reaches the row, and a press may toggle it.
};

/// What a left press on the block column beside a row does.
enum class BlockColumnPress : uint8_t
{
    Swallow = 0,   ///< Nothing to act on there: the press is the gutter's all the same, and does nothing.
    ToggleFold,    ///< Collapse or expand the fold that reaches the row, wherever in its run.
    OpenBlockMenu, ///< Open the menu of the block the row heads: its status mark, with nothing to fold.
};

/// What a left press beside a row showing @p glyph does.
///
/// A fold wins wherever it reaches -- a user mark inside a fold's run toggles the fold as the rest of the run
/// does -- and a status mark with no fold to toggle opens its block's menu: the click a ● or a ✗ invites
/// then has something to do.
/// @param glyph The glyph the row shows (blockColumnGlyph()).
/// @param fold Whether a fold reaches the row and may be toggled.
/// @return What the press does.
[[nodiscard]] BlockColumnPress blockColumnPressFor(BlockColumnGlyph glyph, FoldToggle fold) noexcept;

/// The character @p glyph is drawn as at @p weight.
///
/// All are Contour's private-use codepoints, the fold's four in each of FoldWeight's three weights, and the
/// status marks in one, rather than the Unicode shapes they resemble (U+25C6, U+25B8, U+25CF): vtrasterizer
/// draws what it claims wherever it appears, and those three stay the font's in program output.
/// @param glyph The glyph.
/// @param weight The weight of its strokes (foldWeightOf()); a status mark has one.
/// @return Its codepoint; U+0000 for BlockColumnGlyph::None.
/// @see docs/vt-extensions/private-use-glyphs.md
[[nodiscard]] char32_t blockColumnCodepoint(BlockColumnGlyph glyph, FoldWeight weight) noexcept;

/// Whether the row with stable id @p stableId heads @p record's block, and in which state.
/// @param record The block the row belongs to (Terminal::commandBlockAt), or nullptr for none.
/// @param stableId The row's stable id.
/// @param stableIdGeneration The grid's Grid::stableIdGeneration(), which the record's head id must match.
/// @return The row's head state; None for a row that is not the recorded head.
[[nodiscard]] BlockHeadState blockHeadStateOf(CommandBlockRecord const* record,
                                              int64_t stableId,
                                              uint64_t stableIdGeneration) noexcept;

/// The colour a block is highlighted in as a whole -- the line a jump to it lands on: the block's own, as
/// its hovered glyphs take it.
/// @param palette The colour scheme.
/// @param outcome How the block ended; nullopt with exit status off.
/// @return The highlight's colour.
[[nodiscard]] RGBColor blockHighlightColor(ColorPalette const& palette,
                                           std::optional<CommandBlockOutcome> outcome) noexcept;

/// The colours a block-column glyph is drawn in.
///
/// A success the scheme does not colour -- and every glyph when exit status is off -- is drawn exactly as
/// the fold column always was: faded at rest, full strength hovered. A coloured outcome takes its colour
/// at rest and is lifted halfway toward the text colour hovered, so hovering still says "this is the run
/// a click acts on" without washing the status out. A user mark is full strength always.
/// @param palette The colour scheme.
/// @param glyph The glyph being drawn.
/// @param outcome How the row's block ended; nullopt for no block, or with exit status off.
/// @param hover Whether the pointer is over the row's fold.
/// @return The glyph's foreground and the page's own background.
[[nodiscard]] RGBColorPair blockColumnColors(ColorPalette const& palette,
                                             BlockColumnGlyph glyph,
                                             std::optional<CommandBlockOutcome> outcome,
                                             GutterHover hover) noexcept;

// }}}

// {{{ painting

/// The colours a gutter cell, or a collapsed fold's label, is drawn in.
/// @param colors The foreground and background.
/// @return @p colors, with any decoration in the foreground's colour.
[[nodiscard]] RenderAttributes gutterCellAttributes(RGBColorPair colors) noexcept;

/// The attributes the block column's glyph cell is drawn with: @p colors, and across the whole column.
///
/// One cell wide, the glyph fills its cell. Two wide, it is drawn as one glyph twice the width -- a fold
/// control and a status mark about twice the size -- rather than as one cell-sized glyph beside a blank.
/// @param block The block column's segment.
/// @param colors The glyph's colours, as blockColumnColors() picks them.
/// @return The attributes; flagged LineFlag::DoubleWidth for a column two cells wide.
[[nodiscard]] RenderAttributes blockColumnCellAttributes(GutterSegment const& block,
                                                         RGBColorPair colors) noexcept;

/// Everything one gutter row shows, gathered by Terminal::fillGutter and painted by appendGutterCells().
///
/// Every member has a default, so a row names only what it shows.
struct GutterRow
{
    LineOffset screenLine {};     ///< The screen row it is drawn beside.
    std::string timestamp {};     ///< The time to show, UTF-8; empty for none.
    LineNumberText lineNumber {}; ///< The number, already aligned to its segment; empty for none.
    BlockColumnGlyph glyph = BlockColumnGlyph::None; ///< The block column's glyph.
    RGBColorPair glyphColors {};           ///< The glyph's colours, as blockColumnColors() picks them.
    FoldWeight weight = FoldWeight::Light; ///< How heavy the glyph's strokes are (foldWeightOf()).
};

/// Appends the cells @p row is drawn as -- one per non-blank character, each at its own negative column,
/// the gutter's left edge being column -layout.totalColumns.
/// @param layout The gutter's segments.
/// @param row What the row shows; text wider than its segment is cut at the segment's edge.
/// @param textColors The timestamps' and line numbers' colours, from ColorPalette::gutterTextColors().
/// @param output Receives the cells.
void appendGutterCells(GutterLayout const& layout,
                       GutterRow const& row,
                       RGBColorPair textColors,
                       std::vector<RenderGutterCell>& output);

// }}}

// {{{ collapsed-fold label

/// The label a collapsed fold's head row carries: `⋯ 1,234 lines`.
/// @param hiddenLines How many rows the fold hides.
/// @return The label, one column per codepoint.
[[nodiscard]] std::u32string collapsedFoldLabel(uint64_t hiddenLines);

/// The cells a collapsed fold's label covers on its row: one blank column after the row's text, clipped at
/// the page's edge -- where layoutFoldLabel() draws it, and so where a click on it lands.
/// @param screenLine The screen row the label is drawn on.
/// @param textColumns How many columns of that row hold text (Line::trimmedColumns()).
/// @param pageColumns The page's width.
/// @param labelColumns How many columns the label takes (collapsedFoldLabel()'s, one per codepoint).
/// @return The cells, or nullopt when not one of them fits on the page.
[[nodiscard]] std::optional<ColumnRange> foldLabelExtent(LineOffset screenLine,
                                                         ColumnCount textColumns,
                                                         ColumnCount pageColumns,
                                                         std::size_t labelColumns) noexcept;

/// Lays @p label out one blank column after a row's text, clipped by width at the page edge, appending
/// its cells to @p output -- nothing when not one glyph of it fits.
/// @param output The frame's annotation cells (RenderBuffer::annotations).
/// @param screenLine The screen row of the fold's head.
/// @param textColumns How many columns of that row hold text (Line::trimmedColumns()).
/// @param pageColumns The page's width.
/// @param label The label, as collapsedFoldLabel() spells it.
/// @param attributes Its colours.
void layoutFoldLabel(std::vector<RenderCell>& output,
                     LineOffset screenLine,
                     ColumnCount textColumns,
                     ColumnCount pageColumns,
                     std::u32string_view label,
                     RenderAttributes const& attributes);

// }}}

} // namespace vtbackend
