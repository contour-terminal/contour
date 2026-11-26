// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/window/DaemonSection.hpp>
#include <contour/window/PaneKey.hpp>

#include <core/platform/Clock.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>

#include <vtcontrol/ObjectRef.hpp>
#include <vthost/proto/Pdu.hpp>
#include <vtworkspace/Primitives.hpp>

namespace contour::session
{

/// Whether a switch replaces this window's workspace or opens a new window for it.
enum class SwitchMode : uint8_t
{
    Switch = 0, ///< Replace what this window shows.
    Open,       ///< Show the target in a new window.
};

/// The workspace spec that asks for a new workspace on the target daemon, as a program spells it on the wire
/// (proto::SwitchTargetWire). Not a uuid, position, id suffix or a label anyone gives.
inline constexpr auto NewWorkspaceSpec = std::string_view { "+new" };

/// No workspace named: the switch decides -- the daemon's only one, the user's pick among several, a new one
/// for none.
struct AnyWorkspace
{
    bool operator==(AnyWorkspace const&) const = default;
};

/// A new workspace on the target daemon: the switcher popup's "+ new workspace on X".
struct NewWorkspace
{
    bool operator==(NewWorkspace const&) const = default;
};

/// A workspace by its 1-based position among the target daemon's: what the switching actions name.
struct WorkspaceAt
{
    uint32_t position = 0; ///< Its 1-based position.
    bool operator==(WorkspaceAt const&) const = default;
};

/// A workspace as a person or a program typed it -- a uuid, an id suffix, a label or a position -- resolved
/// against the target daemon as the control verbs resolve one (vtcontrol::resolveTarget).
struct WorkspaceNamed
{
    std::string spec; ///< What was typed; never empty.
    bool operator==(WorkspaceNamed const&) const = default;
};

/// Which workspace of the target daemon a switch goes to. Structured where the GUI knows the workspace -- by
/// its control id (vtcontrol::ObjectRef) or position -- so that nothing it names is read again as text, where
/// a workspace labelled "2" would win over the second one.
using WorkspaceTarget =
    std::variant<AnyWorkspace, NewWorkspace, WorkspaceAt, vtcontrol::ObjectRef, WorkspaceNamed>;

/// @param spec A workspace spec as a program spells it: empty for none, NewWorkspaceSpec for a new one.
/// @return The workspace it names.
[[nodiscard]] inline WorkspaceTarget workspaceTargetOf(std::string_view spec)
{
    if (spec.empty())
        return AnyWorkspace {};
    if (spec == NewWorkspaceSpec)
        return NewWorkspace {};
    return WorkspaceNamed { .spec = std::string { spec } };
}

/// What a switch's window activates once it shows the workspace: the row's tab, and its pane.
struct LandingFocus
{
    std::optional<uint64_t> tab;         ///< The daemon's tab id; none for the workspace's active tab.
    std::optional<window::PaneKey> pane; ///< The pane; none for the tab's active pane.
    bool operator==(LandingFocus const&) const = default;
};

/// Where a switch goes: a daemon (same, local label or saved host) and optionally one of its workspaces.
struct SwitchTarget
{
    vthost::proto::SwitchDaemonKind kind {}; ///< Same, Label or Host.
    std::string daemon;                      ///< The label or host name; empty for Same.
    WorkspaceTarget workspace {};            ///< Which of its workspaces; AnyWorkspace to decide.
    LandingFocus focus {};                   ///< What the window activates once it shows the workspace.
    /// The run of the daemon the workspace was named in; 0 for unknown (a program's switch). A switch naming
    /// a run the daemon is no longer on fails: a restarted daemon may reuse the id for another workspace.
    window::DaemonInstance instance {};
    bool operator==(SwitchTarget const&) const = default;
};

/// @param wire Where a program asked a window to go (proto::SwitchRequest).
/// @return The same, as a switch names it.
[[nodiscard]] inline SwitchTarget switchTargetOf(vthost::proto::SwitchTargetWire wire)
{
    return SwitchTarget { .kind = wire.kind,
                          .daemon = std::move(wire.daemon),
                          .workspace = workspaceTargetOf(wire.workspace) };
}

/// When a switch must have landed by, if it must: a program that asked for it waits for the answer only so
/// long (proto::SwitchRequest), and a switch landing after that would move a window nobody waits for any
/// more.
using SwitchLandBy = std::optional<core::platform::SteadyTimePoint>;

/// When a switch to another daemon tells its completion that it switched.
enum class SwitchTold : uint8_t
{
    /// Once the window is rebound to the daemon it switched to: a completion that acts in the window finds it
    /// there, and a window that cannot be rebound fails the switch instead.
    OnceLanded = 0,
    /// Before the window leaves its daemon: the answer goes out on the connection it was asked on, which the
    /// switch ends.
    BeforeLeaving,
};

/// Who waits for a switch, as the request states it: until when, and when it is told.
struct SwitchWaiter
{
    SwitchLandBy landBy;                      ///< When it must have landed by; none for the user's.
    SwitchTold told = SwitchTold::OnceLanded; ///< When its completion is told it switched.
};

/// The user, driving a switch from the GUI: no time to land by, told once the window landed.
inline constexpr auto UserSwitch = SwitchWaiter {};

/// Called once when a switch request finished, or failed with the reason.
using SwitchCompletion = std::function<void(std::expected<vthost::proto::SwitchOutcome, std::string>)>;

/// Performs window switches between daemons and workspaces.
class WindowSwitching
{
  public:
    WindowSwitching() = default;
    WindowSwitching(WindowSwitching const&) = delete;
    WindowSwitching(WindowSwitching&&) = delete;
    WindowSwitching& operator=(WindowSwitching const&) = delete;
    WindowSwitching& operator=(WindowSwitching&&) = delete;
    virtual ~WindowSwitching() = default;

    /// Switches or opens @p target for @p window; never blocks.
    /// @param window The window asking.
    /// @param target Where to go.
    /// @param mode Replace this window's workspace or open a new window.
    /// @param waiter Who waits for it: until when -- one that cannot start before is refused, and one that
    ///        has not landed by then gives up -- and when @p done is told it switched.
    /// @param done Runs once on the GUI thread with the outcome or the failure reason.
    virtual void request(vtworkspace::WorkspaceId window,
                         SwitchTarget target,
                         SwitchMode mode,
                         SwitchWaiter waiter,
                         SwitchCompletion done) = 0;

    /// Creates a workspace labelled @p label on @p daemon, then switches @p window to it or opens it; never
    /// blocks. Fails by default: a window with no switching service makes no workspace elsewhere.
    /// @param window The window asking.
    /// @param daemon The daemon; its workspace spec is not read.
    /// @param label The new workspace's label; empty for none.
    /// @param mode Switch the window, or open a new one.
    /// @param done Told how the switch went, or why no workspace was made.
    virtual void createWorkspace(vtworkspace::WorkspaceId window,
                                 SwitchTarget const& daemon,
                                 std::string label,
                                 SwitchMode mode,
                                 SwitchCompletion done)
    {
        std::ignore = window;
        std::ignore = daemon;
        std::ignore = label;
        std::ignore = mode;
        done(std::unexpected(std::string { "no workspace can be made there from this window" }));
    }

    /// Told by the manager that @p window closed: a switch of it still under way ends, as cancelled. Nothing
    /// by default.
    /// @param window The window's workspace, as request() named it.
    virtual void windowClosed(vtworkspace::WorkspaceId window) { std::ignore = window; }

    /// Ends @p window's switch of @p mode under way as the user cancelled it: the window stays as it was, and
    /// the switch's completion is told it was cancelled. A switch of the other mode goes on. Nothing by
    /// default, and nothing while none is under way.
    /// @param window The window's workspace, as request() named it.
    /// @param mode Which of its switches: the one in place, or an Open.
    virtual void cancel(vtworkspace::WorkspaceId window, SwitchMode mode)
    {
        std::ignore = window;
        std::ignore = mode;
    }
};

} // namespace contour::session
