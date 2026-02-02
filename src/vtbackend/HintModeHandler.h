// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/primitives.h>

#include <functional>
#include <regex>
#include <string>
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

/// A named regex pattern used for hint scanning.
struct HintPattern
{
    std::string name;
    std::regex regex;
    /// Optional post-match validator. When set, only matches for which
    /// this returns true are kept. Used e.g. to check filesystem existence.
    std::function<bool(std::string const&)> validator;
};

/// A single match found during hint scanning, with its label and grid positions.
struct HintMatch
{
    std::string label;       ///< The label shown on the overlay (e.g. "a", "bc").
    std::string matchedText; ///< The actual matched text.
    CellLocation start;      ///< Start position in the grid.
    CellLocation end;        ///< End position in the grid (inclusive).
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
        virtual void onHintSelected(std::string const& matchedText, HintAction action) = 0;

        /// Called when hint mode is entered.
        virtual void onHintModeEntered() = 0;

        /// Called when hint mode is exited.
        virtual void onHintModeExited() = 0;

        /// Requests a screen redraw.
        virtual void requestRedraw() = 0;
    };

    explicit HintModeHandler(Executor& executor);

    /// Activates hint mode by scanning visible lines for matches.
    ///
    /// @param visibleLines   Text of each visible line, indexed by line offset.
    /// @param pageSize       The terminal page size.
    /// @param patterns       The regex patterns to scan for.
    /// @param action         The action to perform on selection.
    void activate(std::vector<std::string> const& visibleLines,
                  PageSize pageSize,
                  std::vector<HintPattern> const& patterns,
                  HintAction action);

    /// Re-scans visible lines using previously stored patterns and action.
    /// Called on viewport scroll to update hints without re-entering hint mode.
    void refresh(std::vector<std::string> const& visibleLines, PageSize pageSize);

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
    /// Clears existing matches, scans visible lines, sorts, deduplicates, and assigns labels.
    void rescanLines(std::vector<std::string> const& visibleLines, PageSize pageSize);

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

} // namespace vtbackend
