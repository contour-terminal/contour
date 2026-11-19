// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Defines.hpp>
#include <core/tui/Canvas.hpp>
#include <core/tui/Component.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/Engine.hpp>
#include <orchestrator/core/Transcript.hpp>
#include <orchestrator/view/Keys.hpp>
#include <orchestrator/view/LazyLines.hpp>
#include <orchestrator/view/Sections.hpp>
#include <orchestrator/view/Theme.hpp>

namespace orchestrator::view
{

/// The most characters of a transcript line drawn, its indentation aside; a longer one is cut with a `…`.
inline constexpr std::size_t MaxTranscriptLineChars = 4096;

/// The most lines a transcript page is drawn as; the last a `…` when more were cut.
inline constexpr std::size_t MaxTranscriptLines = 4000;

/// One drawn line of a transcript, and how it is drawn.
struct TranscriptLine
{
    std::string text = {};           ///< Sanitised; fitted to the panel when drawn.
    Role role = Role::Text;          ///< How it is drawn.
    DetailFit fit = DetailFit::Wrap; ///< How it is fitted to the panel.
    [[nodiscard]] bool operator==(TranscriptLine const&) const = default;
};

/// @param isoTime An entry's ISO 8601 UTC timestamp, such as `2026-11-19T18:36:55.086Z`.
/// @param utcOffsetMinutes The local time's offset from UTC at that instant.
/// @return Its local time of day as `HH:MM`; `--:--` for a timestamp that does not begin with one.
[[nodiscard]] std::string localClockOf(std::string_view isoTime, int32_t utcOffsetMinutes);

/// Where an entry stands on its page.
enum class EntryPlace : uint8_t
{
    First = 0, ///< The page's first: nothing above it.
    Later,     ///< After another, from which a blank line parts it.
};

/// @param entry An entry.
/// @return Its time of day as the header shows it: `HH:MM` in the local time its instant had, when the page
///         carries the offset valid then; else `HH:MM UTC`; `--:--` for a time that cannot be read.
[[nodiscard]] std::string entryClockOf(TranscriptEntry const& entry);

/// One entry as drawn lines: a blank line when it is not the page's first, its header
/// `<you|claude|tool <name>|result|error|note> · <entryClockOf()>` in its kind's role, and its text below it,
/// indented, sanitised and word-wrapped (indentedLines()).
/// @param entry The entry, untrusted.
/// @param place Where it stands on its page.
/// @param columns The cells a line may take.
/// @return The lines, in order.
[[nodiscard]] std::vector<TranscriptLine> entryLines(TranscriptEntry const& entry,
                                                     EntryPlace place,
                                                     int columns);

/// @param page A page that was read.
/// @return The page's footer: `written by Claude Code <version>`, then `N entries not understood` when N > 0;
///         empty when it has neither.
[[nodiscard]] std::string transcriptFooterOf(TranscriptPage const& page);

/// @param run The run.
/// @param page The page asked for.
/// @return How to read the page by hand: `contour agent transcript --session <uuid> --cwd <path>`, and
///         `--page <n>` beyond the first, quoted for a POSIX shell, one option a line, each line but the last
///         continued with ` \`, none of them cut; std::nullopt when the session or the worktree would not be
///         drawn as it is (showsAsItIs()), since what is drawn could then name another.
[[nodiscard]] std::optional<std::vector<std::string>> transcriptCommandOf(Run const& run, uint32_t page);

/// @param engine The engine.
/// @param tab The tab that shows it.
/// @param run The run a tab shows the transcript of.
/// @param page The page the tab asked for.
/// @return What the transcript screen says instead of a page: that it is being read, that the run was
///         dismissed, that the configuration is refused, that the formats differ, or why the read failed and
///         how to run it by hand (or that the command cannot be shown safely); std::nullopt when the page
///         asked for was read.
[[nodiscard]] std::optional<std::vector<TranscriptLine>> transcriptScreenText(Engine const& engine,
                                                                              std::string_view tab,
                                                                              std::string_view run,
                                                                              uint32_t page);

/// What the transcript panel learnt drawing itself, for the keys.
struct TranscriptPanelState
{
    std::size_t lines = 0; ///< How many lines the page's entries take; UnknownLineCount while not all are
                           ///< wrapped.
    std::size_t rows = 0;  ///< How many of them the panel showed at once.
    std::size_t wrappedEntries = 0; ///< How many entries the panel wrapped since it was made: for tests.
};

/// A run's transcript, a page at a time, read-only, in a rounded box titled `Transcript · <run> · page 2 of
/// 5`: the page's entries from Interaction::scroll on, and its footer; or why there is nothing to show.
/// Everything in it is drawn sanitised.
class TranscriptPanel final: public core::tui::Component
{
  public:
    /// @param engine The engine whose transcript is drawn (Engine::transcript()).
    /// @param interaction Where the user is: the transcript's run, its page and how far it is scrolled.
    /// @param tab The tab it is drawn in, whose page the engine keeps.
    /// @param state What the panel learnt drawing itself.
    TranscriptPanel(Engine const& engine CORE_LIFETIMEBOUND,
                    Interaction const& interaction CORE_LIFETIMEBOUND,
                    std::string const& tab CORE_LIFETIMEBOUND,
                    TranscriptPanelState& state CORE_LIFETIMEBOUND);

    void render(core::tui::Canvas& canvas) override;

  private:
    /// @return The page's lines at @p columns, from the top through line @p count: wrapped only as far as
    ///         that, and wrapped again only when the page or the width changed.
    [[nodiscard]] std::span<TranscriptLine const> linesFor(TranscriptPage const& page,
                                                           int columns,
                                                           std::size_t count);

    Engine const& _engine;
    Interaction const& _interaction;
    std::string const& _tab;
    TranscriptPanelState& _state;
    std::optional<TranscriptPage> _wrappedPage; ///< The page @ref _wrapped is made from.
    int _wrappedColumns = -1;                   ///< The width @ref _wrapped is made for.
    LazyLines<TranscriptLine> _wrapped;         ///< The page's lines, wrapped as far as a draw asked.
};

} // namespace orchestrator::view
