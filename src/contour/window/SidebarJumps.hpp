// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The jumps a window's sidebar starts into another daemon's rows: the one under way, what ends it, and what
/// each outcome says. Qt-free, so that every outcome is a test case without a window.

#include <contour/session/WindowSwitching.hpp>
#include <contour/window/SidebarSections.hpp>
#include <contour/window/SidebarTree.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

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
/// One jump is under way at a time: a newer one replaces it (the switching service ends the older for the
/// user's newer request), and only the newest one's outcome is said; a replaced one says nothing. A row of a
/// daemon run that has ended is refused before anything is asked, and said so.
class SidebarJumps
{
  public:
    /// Says a jump's progress (SidebarModel::noteJump()).
    using Announce = std::function<void(JumpEvent, SidebarTarget const&, std::string const&)>;

    /// @param switching What switches the window. Borrowed; it must outlive this.
    /// @param announce Says each step of a jump.
    /// @param changed Told whenever the jump under way starts or ends: the rows it marks change.
    SidebarJumps(session::WindowSwitching& switching, Announce announce, std::function<void()> changed);

    /// Goes to what another daemon's row names.
    /// @param window The window jumping.
    /// @param target The row's workspace, tab and pane.
    /// @param mode In this window, or a window of its own.
    /// @param controlId The control id of the row's workspace (jumpControlIdOf()): all zero leaves the choice
    ///        to the daemon; StaleRun refuses the jump.
    /// @param from Where the window is now, for a later jump back; none for nowhere.
    void jumpTo(vtworkspace::WorkspaceId window,
                SidebarTarget const& target,
                JumpMode mode,
                std::expected<std::array<uint8_t, 16>, JumpRefusal> const& controlId,
                std::optional<SidebarTarget> from);

    /// @return Where the jump under way goes; none while none is.
    [[nodiscard]] std::optional<SidebarTarget> pending() const;

    /// Ends the jump under way, leaving @p window as it was (session::WindowSwitching::cancel(), for the
    /// jump's mode); its end then says it was cancelled. Nothing while none is under way.
    /// @param window The window jumping.
    void cancel(vtworkspace::WorkspaceId window);

  private:
    /// The jump under way.
    struct PendingJump
    {
        uint64_t serial = 0;                                    ///< Which jump of the window this is.
        SidebarTarget target;                                   ///< Where it goes.
        std::optional<SidebarTarget> from;                      ///< Where the window was.
        session::SwitchMode mode = session::SwitchMode::Switch; ///< In this window, or a new one.
        std::function<void()> then;           ///< What runs once it landed; empty for nothing.
        JumpCancel cancel = JumpCancel::None; ///< Whether the user asked it to end.
    };

    /// Ends jump @p serial with @p outcome, if it is still the one under way.
    void finish(uint64_t serial, std::expected<vthost::proto::SwitchOutcome, std::string> const& outcome);

    session::WindowSwitching& _switching;
    Announce _announce;
    std::function<void()> _changed;
    std::optional<PendingJump> _pending;
    uint64_t _nextSerial = 1;
    /// Held by this alone: a completion that outlives it finds it gone.
    std::shared_ptr<SidebarJumps*> _alive;
};

} // namespace contour::window
