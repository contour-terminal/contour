// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the gutter can be configured to show, which of those configurations are usable, and the layout
/// they yield: everything a configuration, a window reserving the gutter's width or a hit-test needs.
///
/// Kept apart from Gutter.hpp, whose painter fills a RenderBuffer: Settings.hpp and contour's configuration
/// include this header, and would otherwise compile the render buffer and the grid behind it in every
/// translation unit that names a setting.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace vtbackend
{

// {{{ timestamp formats

/// The timestamp format a gutter falls back to: hours, minutes and seconds, in local time.
inline constexpr std::string_view DefaultTimestampFormat = "%H:%M:%S";

/// The widest a timestamp segment may be, in columns. A format that spells more is a mistake rather
/// than a preference: the gutter takes its width out of the page.
inline constexpr int MaxTimestampColumns = 32;

/// Why a timestamp format cannot be used.
enum class TimestampFormatError : uint8_t
{
    Empty = 0, ///< Nothing to format with.
    /// It does not begin with a `%` conversion. std::format rejects most leading literals itself, but reads
    /// a leading digit as a width and a leading `<`, `>` or `^` -- after a fill character or not -- as an
    /// alignment, and pads the timestamp with them.
    NoLeadingConversion,
    Malformed, ///< Not a std::format chrono specification, or it spells a control character.
    /// It spells a character that does not take exactly one column -- a double-width one such as 時, or
    /// a zero-width one such as a combining mark. The gutter gives every character of a timestamp a column
    /// of its own, so the first would overrun the width reserved for it and the second would be torn from
    /// the character it belongs on.
    WideOrZeroWidthCharacter,
    TooWide, ///< It spells more than MaxTimestampColumns columns.
};

/// What to tell a user about @p error, in a log line that already names the setting.
/// @param error Why the format was refused.
/// @return A clause that completes "the timestamp format cannot be used because ...".
[[nodiscard]] std::string_view timestampFormatErrorText(TimestampFormatError error) noexcept;

/// The number of columns @p format spells a timestamp in, measured by formatting a sample.
///
/// The sample is the widest-spelled instant there is -- a Wednesday in September, at 23:59:59 -- so a
/// format naming the weekday or the month is measured at its longest, and the gutter, whose width
/// never follows its content, is wide enough on every day of the year.
///
/// Formatting is also the validation: it is the only test of what the renderer will do with the string,
/// which a parser of our own would merely guess at. The one thing checked before it is that the format
/// begins with a `%` conversion, because std::format reads some other beginnings as padding options
/// rather than rejecting them.
///
/// Every character the sample spells must take exactly one column, or the format is refused: the gutter
/// gives each character of a timestamp a column of its own, so a double-width literal such as 時 would run
/// past the width measured here, and a zero-width one such as a combining mark would be drawn apart from
/// the character it belongs on.
///
/// @param format A std::format chrono specification without the braces, e.g. "%H:%M:%S".
/// @return The width in columns, or why the format cannot be used.
[[nodiscard]] std::expected<int, TimestampFormatError> measureTimestampFormat(std::string_view format);

/// The format a gutter actually spells its timestamps in, and the columns that takes.
struct ResolvedTimestampFormat
{
    std::string_view format; ///< The requested format when it is usable, DefaultTimestampFormat otherwise.
    int columns = 0;         ///< The columns @ref format spells a timestamp in.
};

/// Applies the gutter's one fallback rule: a format that cannot be used is replaced by the default.
///
/// The one place that rule lives, so the width a gutter reserves and the format it spells cannot disagree.
/// @param requested The configured format.
/// @return The format to spell, a view of @p requested or of DefaultTimestampFormat, and its width.
[[nodiscard]] ResolvedTimestampFormat resolveTimestampFormat(std::string_view requested);

// }}}

// {{{ settings and layout

/// Which line numbers the gutter shows, matching vim's `nu` / `rnu` combinations.
enum class LineNumberMode : uint8_t
{
    Off = 0,  ///< `nonu nornu`: none.
    Absolute, ///< `nu`: every row's own number, counted from the session's first row.
    Relative, ///< `rnu`: every row's distance from the cursor row; the cursor row reads 0.
    Hybrid,   ///< `nu rnu`: the cursor row's own number, every other row's distance.
};

/// The gutter's segments, left to right.
enum class GutterSegmentKind : uint8_t
{
    Timestamp = 0, ///< The time each row was first reached.
    LineNumber,    ///< The row's number, or its distance from the cursor.
    BlockColumn,   ///< Fold controls, exit status and user marks.
    Count,         ///< Not a segment; pins the tables keyed by kind.
};

/// The narrowest line-number segment `gutter.line_number_width` may ask for.
inline constexpr int MinLineNumberWidth = 3;

/// The widest line-number segment `gutter.line_number_width` may ask for.
inline constexpr int MaxLineNumberWidth = 10;

/// The narrowest block column `gutter.block_column_width` may ask for: one cell, its glyphs cell-sized.
inline constexpr int MinBlockColumnWidth = 1;

/// The widest block column `gutter.block_column_width` may ask for: two cells, its glyphs drawn across both.
/// No wider, because a glyph is drawn one or two cells wide, and in three a two-cell glyph could not be
/// centred; a fold control two cells wide is as tall as the row already allows.
inline constexpr int MaxBlockColumnWidth = 2;

/// How the block column is drawn: in cells, or as a pixel strip beside them.
///
/// A chrome decision rather than a configuration key: contour's chrome row picks it, and this header holds
/// it so that both the configuration and the terminal can name it.
enum class BlockColumnForm : uint8_t
{
    Glyphs = 0, ///< The private-use glyph column, in cells after the line numbers.
    Rail,       ///< A strip of fixed pixel width after the cells, holding the block rail.
};

/// What hovering a fold does to its column's strokes, beside bringing them to full strength.
enum class FoldHoverStroke : uint8_t
{
    Unchanged = 0, ///< The strokes keep their weight: only the colour says the fold is under the pointer.
    Bolder,        ///< The hovered fold's line, its head and its end are drawn one stroke heavier.
};

/// What the gutter shows: which segments it has and what each draws.
///
/// The bools mirror the YAML keys they come from one to one -- `folding.show_markers`,
/// `gutter.exit_status`, `gutter.user_marks`, `gutter.timestamps` -- the configuration carve-out in
/// AGENT.md, carried into vtbackend unchanged. gutterLayoutFor() reads them as rows of its table.
///
/// Every switch defaults off, so a default-constructed Settings reserves no gutter: an embedder that
/// configures nothing gets the bare grid. The user-facing defaults belong to contour's configuration.
struct GutterSettings
{
    /// The block column draws fold controls.
    bool foldMarkers = false;

    /// The block column colours a command block by its outcome, and draws ▸ / ●.
    bool exitStatus = false;

    /// The block column draws ◆ on rows marked with vi `mm`.
    bool userMarks = false;

    /// The columns the block column takes while any of its three jobs is on; its glyphs are drawn across
    /// all of them, so at two each is twice the size it is at one. Any value is safe: gutterLayoutFor()
    /// holds it to MinBlockColumnWidth..MaxBlockColumnWidth.
    int blockColumnWidth = MinBlockColumnWidth;

    /// Which line numbers, if any.
    LineNumberMode lineNumbers = LineNumberMode::Off;

    /// The columns the line numbers take. Any value is safe: gutterLayoutFor() holds it to
    /// MinLineNumberWidth..MaxLineNumberWidth, the one place that range is enforced on the way to the screen.
    int lineNumberWidth = 6;

    /// Show the time each row was first reached.
    bool timestamps = false;

    /// A std::format chrono specification, without the braces, spelled in local time.
    std::string timestampFormat = std::string { DefaultTimestampFormat };

    /// What hovering a fold does to its strokes (`gutter.bold_on_hover`, converted at the boundary).
    FoldHoverStroke hoverStroke = FoldHoverStroke::Unchanged;

    /// How the block column is drawn: in cells, or as the rail's pixel strip after them. From the chrome row,
    /// not the YAML: @c blockColumnWidth applies to the glyph cells only.
    BlockColumnForm blockColumnForm = BlockColumnForm::Glyphs;

    bool operator==(GutterSettings const&) const = default;
};

/// One segment's place in the gutter. Columns count from the gutter's left edge.
struct GutterSegment
{
    GutterSegmentKind kind {}; ///< What it shows.
    int firstColumn {};        ///< Its leftmost column; 0 is the gutter's left edge.
    int columns {};            ///< How many columns it takes.

    bool operator==(GutterSegment const&) const = default;
};

/// Whether the gutter has the block rail's pixel strip after its cells.
enum class BlockRailPresence : uint8_t
{
    Absent = 0, ///< No strip: the block column, if any, is cells.
    Present,    ///< The strip, whose width is the chrome's, not a number of cells.
};

/// The gutter's segments in drawing order, and the width they add up to.
///
/// One description of the strip for whatever reserves, hit-tests or paints it: read from one place, the
/// width a window reserves and the width a terminal draws cannot disagree.
struct GutterLayout
{
    int totalColumns = 0; ///< The sum of the segments' widths; 0 means there is no gutter.

    /// The format the timestamp segment spells, as resolveTimestampFormat() picked it; empty without that
    /// segment. Carried here so a painter never resolves, or validates, the configured format again.
    std::string timestampFormat;

    /// Whether the block rail's strip follows the cells: under BlockColumnForm::Rail, exactly when any of
    /// the block column's three jobs is on. Not counted in @ref totalColumns.
    BlockRailPresence rail = BlockRailPresence::Absent;

    /// The segments present, left to right.
    [[nodiscard]] std::span<GutterSegment const> segments() const noexcept
    {
        return std::span { _segments }.first(_segmentCount);
    }

    /// The segment of @p kind, when the layout has one.
    /// @param kind The segment asked for.
    /// @return Its place in the gutter, or nullopt when the settings leave it out.
    [[nodiscard]] std::optional<GutterSegment> segment(GutterSegmentKind kind) const noexcept;

    /// Places a segment of @p kind right of the ones already present, and widens the gutter by it.
    /// @param kind The segment; each kind at most once.
    /// @param columns Its width.
    void append(GutterSegmentKind kind, int columns) noexcept;

  private:
    /// Held inline, bounded by the number of kinds: the layout is built every frame, and a heap
    /// allocation per frame would buy nothing.
    std::array<GutterSegment, static_cast<std::size_t>(GutterSegmentKind::Count)> _segments {};
    std::size_t _segmentCount = 0; ///< How many of _segments are present.
};

/// The layout @p settings ask for.
///
/// Constant for a given configuration, never a function of content: a gutter that widened with its line
/// numbers would resize the page, and reflow it, in the middle of a command's output.
/// @param settings What the gutter shows.
/// @return Its segments, left to right, and their total width.
[[nodiscard]] GutterLayout gutterLayoutFor(GutterSettings const& settings);

/// The segment under gutter column @p column, for the hit-test.
/// @param layout The gutter's layout.
/// @param column A column counted from the gutter's left edge.
/// @return Its kind, or nullopt when @p column is outside the gutter.
[[nodiscard]] std::optional<GutterSegmentKind> gutterSegmentAt(GutterLayout const& layout,
                                                               int column) noexcept;

// }}}

} // namespace vtbackend
