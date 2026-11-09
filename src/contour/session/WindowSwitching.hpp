// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
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

/// Where a switch goes: a daemon (same, local label or saved host) and optionally one of its workspaces.
struct SwitchTarget
{
    vthost::proto::SwitchDaemonKind kind {}; ///< Same, Label or Host.
    std::string daemon;                      ///< The label or host name; empty for Same.
    WorkspaceTarget workspace {};            ///< Which of its workspaces; AnyWorkspace to decide.
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
    /// @param done Runs once on the GUI thread with the outcome or the failure reason.
    virtual void request(vtworkspace::WorkspaceId window,
                         SwitchTarget target,
                         SwitchMode mode,
                         SwitchCompletion done) = 0;
};

} // namespace contour::session
