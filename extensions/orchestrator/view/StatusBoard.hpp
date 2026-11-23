// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Clock.hpp>
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

#include <orchestrator/core/Board.hpp>
#include <orchestrator/core/Engine.hpp>
#include <orchestrator/view/BoardMarkdown.hpp>
#include <orchestrator/view/Keys.hpp>
#include <orchestrator/view/LazyLines.hpp>

namespace orchestrator::view
{

/// The most characters of a board's line drawn, its indentation aside; a longer line is cut with a `…`.
inline constexpr std::size_t MaxBoardLineChars = 400;

/// What a board's age reads, and when it next reads otherwise.
struct BoardAge
{
    std::string text = {};    ///< Such as `just now`, `4 min ago`, `3 h ago` or `2 days ago`.
    uint64_t changesInMs = 0; ///< How much older it gets before @ref text changes.
};

/// @param ageMs How long ago the board was written.
/// @return Its age as the board's first line says it: `just now` below a minute, minutes below an hour,
///         hours below 48 hours, days beyond.
[[nodiscard]] BoardAge boardAgeOf(uint64_t ageMs);

/// @param board A board; nullptr: one no read was asked for yet, which is Unread.
/// @return What the board says instead of its contents: why it is not shown, or that it is being read;
///         std::nullopt for a board that was read.
[[nodiscard]] std::optional<std::string_view> boardStateText(Board const* board) noexcept;

/// @param engine The engine.
/// @param run The run a tab shows the board of.
/// @return What the board screen says instead of the board's contents: that the run was dismissed, that the
///         configuration is refused, or boardStateText() of its board; std::nullopt for a board that was
///         read.
[[nodiscard]] std::optional<std::string_view> boardScreenText(Engine const& engine,
                                                              std::string_view run) noexcept;

/// Where a board's published page may live: the only links boardPageOf() accepts, so that a board, which is
/// untrusted text, cannot dress any other link up as its page.
inline constexpr std::string_view BoardPageOrigin = "https://claude.ai/";

/// What the board's age line says for its page, linked to it.
inline constexpr std::string_view PageLabel = "open page";

/// @param bytes A board's contents.
/// @return The link of the page its agent published it to: the first line that reads `Page: <link>`, the
///         label in any case and optionally bold (`**Page:** <link>`), whose link is a whole printable-ASCII
///         URL under BoardPageOrigin; std::nullopt for a board that names none.
[[nodiscard]] std::optional<std::string> boardPageOf(std::string_view bytes);

/// What the board panel learnt drawing itself, for the keys and the next repaint.
struct BoardPanelState
{
    std::size_t lines = 0; ///< How many lines the board's contents take; UnknownLineCount while not all are
                           ///< wrapped.
    std::size_t rows = 0;  ///< How many of them the panel showed at once.
    std::optional<uint64_t>
        ageChangeMs = {}; ///< When the age it shows changes, in monotonic ms; none: never.
};

/// A run's status board, read-only, as the Focus's Board tab draws it: its age, then its contents from
/// FocusSubject::scroll on; or why there is nothing to show. Everything in it is drawn sanitised.
class StatusBoardPanel final: public core::tui::Component
{
  public:
    /// @param engine The engine whose board is drawn (Engine::board()).
    /// @param subject The board's run and how far it is scrolled.
    /// @param clock Now, in the engine's monotonic milliseconds: what the board's age moves on with.
    /// @param state What the panel learnt drawing itself.
    StatusBoardPanel(Engine const& engine CORE_LIFETIMEBOUND,
                     FocusSubject const& subject CORE_LIFETIMEBOUND,
                     core::IClock const& clock CORE_LIFETIMEBOUND,
                     BoardPanelState& state CORE_LIFETIMEBOUND);

    void render(core::tui::Canvas& canvas) override;

  private:
    /// Draws the board's age on the first row, and after it the link to its page while it names one.
    /// @param canvas The panel.
    /// @param width The cells the line may take.
    /// @param age What the age says.
    void drawAgeLine(core::tui::Canvas& canvas, int width, std::string_view age) const;

    /// @return The board's contents at @p columns, from the top through line @p count: wrapped only as far as
    ///         that, and wrapped again only when the contents or the width changed.
    [[nodiscard]] std::span<BoardLine const> linesFor(std::string_view bytes, int columns, std::size_t count);

    Engine const& _engine;
    FocusSubject const& _subject;
    core::IClock const& _clock;
    BoardPanelState& _state;
    std::string _wrappedBytes;        ///< The bytes @ref _logical was made from.
    std::optional<std::string> _page; ///< boardPageOf() the bytes @ref _logical was made from.
    int _wrappedColumns = -1;         ///< The width @ref _wrapped is made for.
    std::vector<BoardLine> _logical;  ///< The contents' lines, sanitised (displayLines()) and rendered as
                                      ///< Markdown (renderBoardMarkdown()), not wrapped.
    LazyLines<BoardLine> _wrapped;    ///< The contents' lines, wrapped as far as a draw asked.
};

} // namespace orchestrator::view
