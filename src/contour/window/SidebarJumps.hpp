// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The jumps a window's sidebar starts into another daemon's rows: the one under way, what ends it, and what
/// each outcome says. Qt-free, so that every outcome is a test case without a window.

#include <contour/session/WindowSwitching.hpp>
#include <contour/window/LocationHistory.hpp>
#include <contour/window/SidebarSections.hpp>
#include <contour/window/SidebarTree.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <vthost/proto/Pdu.hpp>
#include <vtworkspace/Primitives.hpp>

namespace contour::window
{

/// How far a jump to a row has come, as SidebarModel::noteJump() announces it.
enum class JumpEvent : uint8_t
{
    Connecting = 0, ///< The jump waits for its daemon's link.
    Landed,         ///< The jump arrived where it went.
    Cancelled,      ///< The user cancelled it.
    Failed,         ///< Its daemon could not be reached.
};

/// Whether the user asked for the jump under way to end.
enum class JumpCancel : uint8_t
{
    None = 0,  ///< Not asked.
    Requested, ///< Asked (SidebarJumps::cancel()): a failure that follows is the cancel's.
};

/// @param outcome How the switch a jump made ended.
/// @param cancel Whether the user asked it to end.
/// @return What the jump's end says: Landed once the window shows the target or another window that does was
///         raised; Cancelled for a failure after the user's cancel; Failed for any other failure; nothing
///         for a switch that only showed the switcher popup, which says what it does itself.
[[nodiscard]] constexpr std::optional<JumpEvent> jumpEventOf(
    std::expected<vthost::proto::SwitchOutcome, std::string> const& outcome, JumpCancel cancel) noexcept
{
    if (!outcome)
        return cancel == JumpCancel::Requested ? JumpEvent::Cancelled : JumpEvent::Failed;
    switch (*outcome)
    {
        case vthost::proto::SwitchOutcome::Switched:
        case vthost::proto::SwitchOutcome::FocusedExisting: return JumpEvent::Landed;
        default: return std::nullopt;
    }
}

/// Why a jump from a row of an ended daemon run is not made, for the user.
inline constexpr auto JumpStaleRunReason = std::string_view { "the daemon restarted" };

/// The jumps of one window's sidebar into other daemons' rows.
///
/// One jump per mode is under way at a time -- one in this window, one into a window of its own -- and
/// neither ends the other, as the switching service keeps them in slots of their own. A newer jump of a mode
/// replaces that mode's (the switching service ends the older for the user's newer request), and only the
/// newest one's outcome is said; a replaced one says nothing. Each mode's jump says how it ended, a failure
/// or a timeout of either among it. A row of a daemon run that has ended is refused before anything is asked,
/// and said so.
class SidebarJumps
{
  public:
    /// Says a jump's progress (SidebarModel::noteJump()).
    using Announce = std::function<void(JumpEvent, SidebarTarget const&, std::string const&)>;

    /// What runs once a jump ended well: told whether this window shows the target (Switched) or another
    /// window that does was raised (FocusedExisting).
    using JumpThen = std::function<void(vthost::proto::SwitchOutcome)>;
    /// Told when a jump landed in this window -- it switched, rather than raising another window that shows
    /// the target or opening one: where the window was, where it went, and why (LocationHistory::landed()).
    using Landed =
        std::function<void(std::optional<SidebarTarget> from, SidebarTarget const& to, HistoryCause)>;
    /// Told when a jump back (HistoryCause::Back) failed or was cancelled: the place it went to is still one
    /// to go back to (LocationHistory::restore()).
    using Unreached = std::function<void(SidebarTarget const& place)>;

    /// @param switching What switches the window. Borrowed; it must outlive this.
    /// @param announce Says each step of a jump.
    /// @param changed Told whenever the jump under way starts or ends: the rows it marks change.
    /// @param landed Told each jump that landed in this window.
    /// @param unreached Told each jump back that failed or was cancelled; empty for nothing.
    SidebarJumps(session::WindowSwitching& switching,
                 Announce announce,
                 std::function<void()> changed,
                 Landed landed,
                 Unreached unreached = {});

    /// Goes to what another daemon's row names.
    /// @param window The window jumping.
    /// @param target The row's workspace, tab and pane.
    /// @param mode In this window, or a window of its own.
    /// @param controlId The control id of the row's workspace (jumpControlIdOf()): all zero leaves the choice
    ///        to the daemon; StaleRun refuses the jump.
    /// @param from Where the window is now, for a later jump back; none for nowhere.
    /// @param cause Why it jumps, as a landing here is recorded (Landed).
    /// @param then What runs once it landed, in this window or by raising another that shows the target; not
    ///        for a jump that failed, was cancelled or was replaced. Empty for nothing.
    void jumpTo(vtworkspace::WorkspaceId window,
                SidebarTarget const& target,
                JumpMode mode,
                std::expected<std::array<uint8_t, 16>, JumpRefusal> const& controlId,
                std::optional<SidebarTarget> from,
                HistoryCause cause,
                JumpThen then = {});

    /// Makes a new workspace on another daemon, then shows it in @p window: the jump under way, pending on
    /// the daemon's row, as any jump is (session::WindowSwitching::createWorkspace()).
    /// @param window The window jumping.
    /// @param daemonTarget The daemon's row: its daemon and run; its workspace is not read.
    /// @param from Where the window is now, for a later jump back; none for nowhere.
    void createAndJump(vtworkspace::WorkspaceId window,
                       SidebarTarget const& daemonTarget,
                       std::optional<SidebarTarget> from);

    /// @return Where the jumps under way go, by mode.
    [[nodiscard]] PendingJumps pending() const;

    /// Ends the jump of @p mode under way, leaving @p window as it was (session::WindowSwitching::cancel());
    /// its end then says it was cancelled. The other mode's jump goes on. Nothing while none is under way.
    /// @param window The window jumping.
    /// @param mode Which jump: the one in this window (Escape, CancelSwitch, the location chip) or the
    ///        Open in New Window (its own row's Cancel).
    void cancel(vtworkspace::WorkspaceId window, JumpMode mode);

    /// Lets the jump in this window under way go without a word: a switch of the window this sidebar did not
    /// ask for (SwitchToWorkspace, the switcher, the location chip) replaces it, and the switching service
    /// ends it for that newer request (spec §5.5, the newer wins). Its end says nothing and its then does not
    /// run. Nothing while none is under way.
    void release();

    /// Ends the jump in this window under way without a word: a navigation in this window replaced it (spec
    /// §5.5, the newer wins). The switching service is told to end it (session::WindowSwitching::cancel()),
    /// nothing is said and its then does not run. An Open in New Window goes on, as it moves no window of
    /// this one. Nothing while none is under way.
    /// @param window The window jumping.
    void supersede(vtworkspace::WorkspaceId window);

  private:
    /// A jump under way.
    struct PendingJump
    {
        uint64_t serial = 0;                     ///< Which jump of the window this is.
        SidebarTarget target;                    ///< Where it goes.
        std::optional<SidebarTarget> from;       ///< Where the window was.
        HistoryCause cause = HistoryCause::Jump; ///< Why it jumps, as its landing is recorded.
        JumpThen then;                           ///< What runs once it landed; empty for nothing.
        JumpCancel cancel = JumpCancel::None;    ///< Whether the user asked it to end.
    };

    /// Makes @p target's jump the one under way of @p mode, said to be connecting.
    /// @return Its serial, and the completion its switch is told.
    [[nodiscard]] session::SwitchCompletion start(SidebarTarget const& target,
                                                  std::optional<SidebarTarget> from,
                                                  session::SwitchMode mode,
                                                  HistoryCause cause,
                                                  JumpThen then);

    /// Ends jump @p serial with @p outcome, if it is still the one under way of its mode.
    void finish(uint64_t serial, std::expected<vthost::proto::SwitchOutcome, std::string> const& outcome);

    /// @return The slot the jump of @p mode waits in.
    [[nodiscard]] std::optional<PendingJump>& slotOf(session::SwitchMode mode) noexcept
    {
        return _pending.at(static_cast<std::size_t>(std::to_underlying(mode)));
    }

    /// @return The slot the jump of @p mode waits in.
    [[nodiscard]] std::optional<PendingJump> const& slotOf(session::SwitchMode mode) const noexcept
    {
        return _pending.at(static_cast<std::size_t>(std::to_underlying(mode)));
    }

    session::WindowSwitching& _switching;
    Announce _announce;
    std::function<void()> _changed;
    Landed _landed;
    Unreached _unreached;
    /// The jump under way of each mode, by session::SwitchMode: in this window, and an Open.
    std::array<std::optional<PendingJump>, 2> _pending;
    uint64_t _nextSerial = 1;
    /// Held by this alone: a completion that outlives it finds it gone.
    std::shared_ptr<SidebarJumps*> _alive;
};

} // namespace contour::window
