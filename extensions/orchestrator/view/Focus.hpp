// SPDX-License-Identifier: Apache-2.0
//
// The Focus: the selected run, its tabs (Board, Transcript, Details), the latest of what it said, and the box
// at its bottom (the message box, the done box, or how it ended).
#pragma once

#include <core/Clock.hpp>
#include <core/Defines.hpp>
#include <core/tui/Canvas.hpp>
#include <core/tui/Component.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <orchestrator/core/Engine.hpp>
#include <orchestrator/view/Fleet.hpp>
#include <orchestrator/view/Keys.hpp>
#include <orchestrator/view/Sections.hpp>
#include <orchestrator/view/StatusBoard.hpp>
#include <orchestrator/view/Transcript.hpp>

namespace orchestrator::view
{

/// The fewest rows a Focus tab keeps: the furniture gives way, in its order, before the tab does.
inline constexpr int MinFocusContentRows = 8;

/// The rows `latest` takes: its rule and three entries.
inline constexpr int LatestRows = 4;

/// How many entries `latest` shows.
inline constexpr std::size_t LatestEntries = 3;

/// The furniture of the dashboard that gives way in a small tab, in the order it does: a bit each.
enum class Furniture : uint8_t
{
    None = 0,
    HeaderDetail = 0b0001, ///< The Focus header's second line: the title, the branch and the profile.
    Latest = 0b0010,       ///< `latest`, while the Board tab is open.
    TopBar = 0b0100,       ///< The top bar; its gauge and pill move to the right end of the action bar.
    TabStrip = 0b1000,     ///< The tab strip; the active tab moves into the header as `· board`.
};

/// @return Every item of @p a and @p b.
[[nodiscard]] constexpr Furniture operator|(Furniture a, Furniture b) noexcept
{
    return static_cast<Furniture>(static_cast<unsigned>(std::to_underlying(a))
                                  | static_cast<unsigned>(std::to_underlying(b)));
}

/// @return Whether @p set holds @p item.
[[nodiscard]] constexpr bool holds(Furniture set, Furniture item) noexcept
{
    return (static_cast<unsigned>(std::to_underlying(set)) & static_cast<unsigned>(std::to_underlying(item)))
           != 0;
}

/// What decides which furniture gives way.
struct FocusRows
{
    int height = 0;                 ///< The Focus's rows, its border included, with the top bar shown.
    int topBarRows = 0;             ///< The rows the Focus gains when the top bar gives way; 0: none.
    int bottomRows = 0;             ///< The rows its bottom box takes.
    FocusTab tab = FocusTab::Board; ///< The tab shown.
};

/// The furniture that gives way so that the Focus's tab keeps MinFocusContentRows: in the order the spec sets
/// (the header's second line; `latest` while the Board tab is open; the top bar; the tab strip), each only
/// while the tab still has fewer.
/// @param rows The Focus's rows and what takes them.
/// @return What gives way.
[[nodiscard]] Furniture furnitureGivingWay(FocusRows const& rows) noexcept;

/// What the Fleet holds, which decides what the Focus shows when no run is selected.
enum class FleetContent : uint8_t
{
    Runs = 0,     ///< Runs, or worktrees.
    Empty,        ///< Nothing: the Focus shows the primer.
    Unconfigured, ///< The configuration is missing or refused.
};

/// What the box at the bottom of the Focus is.
enum class BottomBox : uint8_t
{
    None = 0, ///< Nothing: a run that is preparing or ending, a worktree, no run.
    Message,  ///< The message box: `⏎ to write to …`, or a cursor while it has the keys.
    Asking,   ///< An asking run's: its request and the reply chips, in the message box's place.
    Done,     ///< A done run's: `✓ reported done`, and its actions.
    Ended,    ///< A finished run's: how it ended, and its actions.
};

/// What the Focus draws this frame: the dashboard sets it before each draw.
struct FocusFrame
{
    Row const* row = nullptr;                  ///< The selected row; nullptr: none.
    FocusTab tab = FocusTab::Board;            ///< The tab shown (activeTabOf()).
    FleetContent content = FleetContent::Runs; ///< What the Fleet holds.
    FocusSubject subject = {};                 ///< What the Board and Transcript tabs draw.
    Place place = Place::Fleet;                ///< Which pane has the keys.
    BottomBox box = BottomBox::None;           ///< The box at its bottom.
    Furniture givenWay = Furniture::None;      ///< The furniture the dashboard gave way: the top bar.
    std::array<News, 3> tabNews = {};          ///< By FocusTab: whether the tab changed since it was shown.
};

/// What the Details tab learnt drawing itself, for the keys.
struct DetailsPanelState
{
    std::size_t lines = 0; ///< How many lines the details take.
    std::size_t rows = 0;  ///< How many of them the tab showed at once.
};

/// What the Focus's tabs learnt drawing themselves.
struct FocusPanels
{
    BoardPanelState board = {};           ///< The Board tab's.
    TranscriptPanelState transcript = {}; ///< The Transcript tab's.
    DetailsPanelState details = {};       ///< The Details tab's.

    /// @param tab A tab.
    /// @return How many lines it had as last drawn, and how many of them it showed.
    [[nodiscard]] std::pair<std::size_t, std::size_t> extentOf(FocusTab tab) const noexcept;
};

/// @param engine The engine.
/// @param row The selected row; nullptr: none.
/// @param place Which pane has the keys.
/// @return The box at the bottom of the Focus for @p row.
[[nodiscard]] BottomBox bottomBoxOf(Engine const& engine, Row const* row, Place place) noexcept;

/// @param replies An asking run's replies.
/// @param columns The cells the chips take.
/// @return The chips, as many to a line as fit, a few cells apart; one empty line for none.
[[nodiscard]] std::vector<std::vector<QuickReply const*>> chipLinesOf(std::span<QuickReply const> replies,
                                                                      int columns);

/// @param box A bottom box.
/// @param row The selected row: an asking row's chips.
/// @param draft The draft it shows, for the message box.
/// @param columns The cells its text takes.
/// @param height The Focus's rows: a message box grows with its draft, up to a third of them.
/// @return The rows the box takes, its border included; 0 for none.
[[nodiscard]] int bottomRowsOf(
    BottomBox box, Row const* row, std::string_view draft, int columns, int height);

/// The Focus, in a rounded box whose border names the run (`pr-2375 · fix CI · sonnet`) and its state: the
/// header's second line, the tab strip, the tab, `latest`, and the bottom box; or the primer while the Fleet
/// is empty. Everything in it is drawn sanitised; its URLs are hyperlinks.
class Focus final: public core::tui::Component
{
  public:
    /// @param engine The engine whose runs, boards and transcripts it draws.
    /// @param interaction Where the user is: the drafts, the message sent.
    /// @param frame What it draws this frame.
    /// @param tab The id of the dashboard's tab, whose transcript page the engine keeps.
    /// @param clock What a board's age moves on with.
    /// @param panels What its tabs learnt drawing themselves.
    Focus(Engine const& engine CORE_LIFETIMEBOUND,
          Interaction const& interaction CORE_LIFETIMEBOUND,
          FocusFrame const& frame CORE_LIFETIMEBOUND,
          std::string const& tab CORE_LIFETIMEBOUND,
          core::IClock const& clock CORE_LIFETIMEBOUND,
          FocusPanels& panels CORE_LIFETIMEBOUND);

    void render(core::tui::Canvas& canvas) override;

  private:
    void renderRun(core::tui::Canvas& canvas, Run const* run);
    void renderDetails(core::tui::Canvas& canvas);
    void renderLatest(core::tui::Canvas& canvas, Run const& run);
    void renderBottom(core::tui::Canvas& canvas);
    void renderAsking(core::tui::Canvas& canvas);
    /// @return The selected run's draft; empty: none.
    [[nodiscard]] std::string_view draftOf() const noexcept;

    Engine const& _engine;
    Interaction const& _interaction;
    FocusFrame const& _frame;
    std::string const& _tab;
    FocusPanels& _panels;
    StatusBoardPanel _board;
    TranscriptPanel _transcript;
};

} // namespace orchestrator::view
