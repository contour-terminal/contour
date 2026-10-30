// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/input/vi/HintTypes.hpp>
#include <vtbackend/links/LinkMatcher.hpp>
#include <vtbackend/links/LinkPatterns.hpp>
#include <vtbackend/links/ScanText.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vtbackend
{

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

/// What a hint stands for, by its pattern's rule. Given the match and the grid row it starts on, it returns
/// the target the hint carries (a relative path resolved to an absolute one), or nullopt to drop it (a path
/// naming no file). The overlay still shows the text as printed.
using HintResolver = std::function<std::optional<LinkTarget>(LinkCandidate const&, LineOffset)>;

/// The patterns one hint activation scans with.
struct HintPatternSet
{
    /// The table, compiled once per profile (Terminal's Settings::linkPatterns).
    std::shared_ptr<LinkPatterns const> table = LinkPatterns::builtins();
    /// Which of its patterns take part; empty: all of them.
    std::vector<std::string> names {};
    /// Unset: every match is kept as printed.
    HintResolver resolve {};
};

/// The terminal region one hint activation scans.
struct HintScanArea
{
    /// The rows to scan, in ascending, consecutive grid-line order.
    std::vector<ScanRow> rows;
    /// Whether the row after the last one continues it.
    LineContinuation continuesBelow = LineContinuation::No;
    /// The rows whose matches may be labelled. A match starting outside it is discarded, because its label
    /// could not be drawn. Rows outside it still contribute their text, so a match running past the range
    /// still yields its complete text.
    HintRowRange labelableRows;
};

/// What one hint-mode activation should scan and do.
struct HintModeRequest
{
    /// The patterns to scan for.
    HintPatternSet patterns {};

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
    std::string label; ///< The label shown on the overlay (e.g. "a", "bc").
    /// What Copy, Paste and Select act on: the match as printed; a path, the absolute form it resolved to.
    std::string matchedText;
    /// What the hint stands for, which Open opens: a custom match's template expansion, a path resolved.
    LinkTarget target;
    /// Whether Open may open @ref target: not where the printed text chose a scheme outside the built-in URL
    /// schemes (LinkCandidate::opening).
    LinkOpening opening = LinkOpening::Refused;
    CellLocation start; ///< Start position in the grid.
    CellLocation end;   ///< End position in the grid (inclusive).
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
        /// @param match  The match: its text, what it stands for (a path resolved at its location), and its
        ///               grid-absolute range (line 0 = top of the page, negative = history; the end
        ///               inclusive, on a later row than the start where the match spans a wrapped line).
        /// @param action The action to perform on the match.
        virtual void onHintSelected(HintMatch const& match, HintAction action) = 0;

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
    /// @param patterns The patterns to scan with; taken by value and moved into the handler,
    ///                 so a caller with a throwaway set should std::move it in.
    /// @param action   The action to perform on selection.
    void activate(HintScanArea const& area, HintPatternSet patterns, HintAction action);

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
    HintPatternSet _patterns; ///< Stored on activate for refresh on scroll.
    std::vector<HintMatch> _allMatches;
    std::vector<HintMatch> _filteredMatches;
    std::string _filter;
};

} // namespace vtbackend
