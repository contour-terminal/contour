// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/primitives.h>

#include <functional>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace vtbackend
{

/// Defines the action to perform when a hint is selected.
enum class HintAction : uint8_t
{
    Copy,         ///< Copy matched text to clipboard.
    Open,         ///< Open matched text (e.g. URL in browser).
    Paste,        ///< Paste matched text into the terminal input.
    CopyAndPaste, ///< Copy to clipboard and paste into terminal.
    Select,       ///< Pre-select the match range in visual mode.
};

/// How much of the terminal a hint activation scans.
enum class HintScope : uint8_t
{
    Visible = 0,    ///< The visible page only.
    Scrollback = 1, ///< The visible page plus scrollback history, up to a configured limit.
};

/// Whether a physical row continues the logical line begun on the row above.
///
/// A logical line is what was actually written; the rows below its head exist only because the
/// window happened to be too narrow. Hint patterns are matched against the logical line, so a URL
/// broken across two rows is still one match.
enum class LineContinuation : uint8_t
{
    No = 0,  ///< This row starts a new logical line.
    Yes = 1, ///< This row is a wrapped continuation of the row above.
};

/// One physical terminal row handed to the hint scanner.
struct HintScanRow
{
    /// The row's full, untrimmed text: exactly one codepoint per grid cell, so a codepoint index
    /// into it is a column offset (wide-character continuation cells emit a space).
    std::string text;
    /// Grid line offset of this row. Zero is the top of the page; negative reaches into history.
    LineOffset line;
    LineContinuation continuation = LineContinuation::No;
};

/// An inclusive range of grid rows.
struct HintRowRange
{
    LineOffset first; ///< Topmost row (inclusive).
    LineOffset last;  ///< Bottommost row (inclusive).

    [[nodiscard]] constexpr bool contains(LineOffset line) const noexcept
    {
        return first <= line && line <= last;
    }
};

/// The terminal region one hint activation scans.
struct HintScanArea
{
    /// The rows to scan, in ascending, consecutive grid-line order.
    std::vector<HintScanRow> rows;

    /// The rows whose matches may be labelled. A match starting outside it is discarded — its label
    /// could not be drawn, so it would be unreachable. Rows outside it still contribute their text,
    /// so a match running past the range still yields its complete text.
    HintRowRange labelableRows;
};

/// A named regex pattern used for hint scanning.
struct HintPattern
{
    std::string name;
    std::regex regex;
    /// Optional post-match validator. When set, only matches for which
    /// this returns true are kept. Used e.g. to check filesystem existence.
    std::function<bool(std::string const&)> validator;
    /// Optional post-match transformer. When set, the matched text is rewritten
    /// before being stored in HintMatch. Used e.g. to resolve relative paths
    /// to absolute paths. The overlay still shows the original terminal text.
    std::function<std::string(std::string const&)> transformer;
};

/// What one hint-mode activation should scan and do.
struct HintModeRequest
{
    /// The regex patterns to scan for.
    std::vector<HintPattern> patterns;

    /// What to do with the match the user selects.
    HintAction action = HintAction::Copy;

    /// How much of the terminal to scan.
    HintScope scope = HintScope::Visible;

    /// How many scrollback rows to scan at most under HintScope::Scrollback; ignored otherwise.
    /// Zero — the default, matching the default scope — scans no history at all.
    LineCount scrollbackLimit { 0 };
};

/// A single match found during hint scanning, with its label and grid positions.
///
/// Positions are grid-absolute (zero is the top of the page, negative reaches into history), so
/// they survive scrolling. @ref start and @ref end may sit on different rows when the match spans
/// a wrapped logical line; the region they name is a run of text, not a rectangle.
struct HintMatch
{
    std::string label;       ///< The label shown on the overlay (e.g. "a", "bc").
    std::string matchedText; ///< The actual matched text.
    CellLocation start;      ///< Start position in the grid.
    CellLocation end;        ///< End position in the grid (inclusive).
};

/// A run of consecutive physical rows forming one logical line, plus what is needed to map a
/// codepoint index within @ref text back to the grid cell it came from.
struct HintLogicalLine
{
    std::string text;     ///< Concatenated row texts, in row order.
    LineOffset firstLine; ///< Grid offset of the first row.

    /// Where each row ends within @ref text, as an exclusive codepoint index, in row order. Stored
    /// cumulatively rather than as per-row lengths so @ref gridPositionOf can binary-search it: a
    /// linear walk is O(rows) per call, and one wrapped line can be a thousand rows long.
    std::vector<size_t> rowCodepointEnds;
};

/// Handles hint mode logic: scanning visible text for regex matches,
/// assigning alphabetic labels, and progressively filtering by typed input.
class HintModeHandler
{
  public:
    /// Interface for the handler to communicate with the terminal.
    class Executor
    {
      public:
        virtual ~Executor() = default;

        /// Called when a hint has been selected by the user.
        ///
        /// @param matchedText The text that was matched by the hint pattern.
        /// @param action      The action to perform on the match.
        /// @param start       The grid-absolute start position of the match (line 0 = top of the
        ///                    page, negative = history).
        /// @param end         The grid-absolute end position of the match (inclusive). It may lie
        ///                    on a later row than @p start when the match spans a wrapped line.
        virtual void onHintSelected(std::string const& matchedText,
                                    HintAction action,
                                    CellLocation start,
                                    CellLocation end) = 0;

        /// Called when hint mode is entered.
        virtual void onHintModeEntered() = 0;

        /// Called when hint mode is exited.
        virtual void onHintModeExited() = 0;

        /// Requests a screen redraw.
        virtual void requestRedraw() = 0;
    };

    explicit HintModeHandler(Executor& executor);

    /// Activates hint mode by scanning @p area for matches.
    ///
    /// Rows flagged as wrapped continuations are joined with the row above before matching, so one
    /// pattern may span several rows.
    ///
    /// @param area     The rows to scan and which of them may carry a label.
    /// @param patterns The regex patterns to scan for; taken by value and moved into the handler,
    ///                 so a caller with a throwaway vector should std::move it in.
    /// @param action   The action to perform on selection.
    void activate(HintScanArea const& area, std::vector<HintPattern> patterns, HintAction action);

    /// Re-scans @p area using the patterns and action stored by @ref activate.
    /// Called on viewport scroll to update visible-scope hints without re-entering hint mode.
    void refresh(HintScanArea const& area);

    /// Tracks grid content scrolling into history while a hint session is open, so a label keeps
    /// pointing at the same text instead of at whatever text scrolled into its old grid-absolute
    /// position. Every match moves up by @p lines; a match whose start has scrolled out of the
    /// @p historyLineCount-deep buffer is dropped, and the session deactivates if none remain.
    ///
    /// @param lines            Rows scrolled into history (positive), as reported by the buffer.
    /// @param historyLineCount The scrollback depth after the scroll.
    void applyScroll(LineOffset lines, LineCount historyLineCount);

    /// Deactivates hint mode.
    void deactivate();

    /// Returns true if hint mode is currently active.
    [[nodiscard]] bool isActive() const noexcept { return _active; }

    /// Processes a single character input for progressive label filtering.
    /// Returns true if the input was consumed.
    bool processInput(char32_t ch);

    /// Returns the currently filtered matches.
    [[nodiscard]] std::vector<HintMatch> const& matches() const noexcept { return _filteredMatches; }

    /// Returns the typed filter prefix.
    [[nodiscard]] std::string const& currentFilter() const noexcept { return _filter; }

    /// Returns the hint action for the current session.
    [[nodiscard]] HintAction action() const noexcept { return _action; }

    /// Returns built-in default hint patterns (URLs, file paths, git hashes).
    [[nodiscard]] static std::vector<HintPattern> builtinPatterns();

  private:
    /// Core scanning logic shared by activate() and refresh().
    /// Clears existing matches, scans the rows, sorts, deduplicates, and assigns labels.
    void rescanLines(HintScanArea const& area);

    /// Assigns labels to all matches.
    void assignLabels();

    /// Updates the filtered matches based on the current filter prefix.
    void updateFilteredMatches();

    Executor& _executor;
    bool _active = false;
    HintAction _action = HintAction::Copy;
    std::vector<HintPattern> _patterns; ///< Stored on activate for refresh on scroll.
    std::vector<HintMatch> _allMatches;
    std::vector<HintMatch> _filteredMatches;
    std::string _filter;
};

/// Extracts a local filesystem path from a file:// URL (as set by OSC 7).
/// Returns the URL unchanged if it does not start with "file://".
[[nodiscard]] auto extractPathFromFileUrl(std::string const& url) -> std::string;

/// The local filesystem path a working-directory URL points at, or nullopt when the URL names a host
/// other than @p localHost — a remote (e.g. SSH) working directory that does not exist on this machine.
///
/// OSC 7 reports the working directory as file://HOST/PATH. A host that is empty, "localhost", or that
/// shares its first DNS label with @p localHost — case-insensitively, so "host" matches "host.example.com"
/// — is this machine; the returned path has the file:// scheme, the host authority and the leading "//"
/// stripped (see @ref extractPathFromFileUrl). Any other host is remote and yields nullopt, as does a URL
/// that carries no path at all.
///
/// The local host name is a parameter rather than read here so the decision stays pure and unit-testable;
/// the caller injects it (QHostInfo::localHostName() in the GUI).
///
/// @param url       The working-directory URL: an OSC 7 file:// URL, or a bare local path.
/// @param localHost This machine's host name.
/// @return The local path to open, or nullopt when @p url is remote or path-less.
[[nodiscard]] auto localWorkingDirectory(std::string const& url, std::string_view localHost)
    -> std::optional<std::string>;

/// Converts a UTF-8 byte offset within @p text to the corresponding codepoint index.
///
/// In the column-aligned UTF-8 strings the hint scanner uses (see @ref Line::toUtf8ColumnAligned),
/// each grid cell emits exactly one codepoint (a wide character's continuation cell emits a space).
/// Therefore the codepoint index equals the column index for these strings.
[[nodiscard]] auto utf8ByteOffsetToCodepointIndex(std::string_view text, size_t byteOffset) noexcept
    -> size_t;

/// Converts a monotonically non-decreasing sequence of UTF-8 byte offsets within one string to
/// codepoint indices in a single forward pass over that string.
///
/// @ref utf8ByteOffsetToCodepointIndex counts from the start of the string on every call, which is
/// quadratic across the many matches of one long logical line — and a wrapped logical line can be a
/// thousand rows of text. Regex matches arrive in increasing, non-overlapping position order, so
/// this remembers where the previous conversion ended and counts only the delta.
class Utf8CodepointCursor
{
  public:
    explicit Utf8CodepointCursor(std::string_view text) noexcept: _text { text } {}

    /// @param byteOffset Must be at least the offset given to the previous call.
    /// @return The codepoint index @p byteOffset falls on.
    [[nodiscard]] auto codepointIndexAt(size_t byteOffset) noexcept -> size_t;

  private:
    std::string_view _text;
    size_t _byteOffset = 0;
    size_t _codepointIndex = 0;
};

/// Groups @p rows into logical lines, joining each run of wrapped continuations onto the row that
/// heads it. A leading continuation row (its head scrolled out of the scanned range) heads its own
/// logical line rather than being dropped.
///
/// @param rows Rows in ascending, consecutive grid-line order.
/// @return One entry per logical line, in the same order.
[[nodiscard]] auto buildLogicalLines(std::vector<HintScanRow> const& rows) -> std::vector<HintLogicalLine>;

/// Maps a codepoint index within @p logical back to the grid cell it came from.
///
/// An index at or past the end of the logical line clamps to its last cell, so a caller need not
/// special-case a zero-length tail.
[[nodiscard]] auto gridPositionOf(HintLogicalLine const& logical, size_t codepointIndex) noexcept
    -> CellLocation;

} // namespace vtbackend
