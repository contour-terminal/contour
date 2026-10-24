// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The in-memory model of a workspace snapshot: what the daemon writes so that a reboot, a crash or the
/// drift restart can bring the windows, tabs and panes back. @see SessionSnapshotCodec.hpp for the file.

#include <vtbackend/core/Color.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <vthost/proto/Pdu.hpp>
#include <vtworkspace/Primitives.hpp>

namespace vthost
{

/// An opaque 128-bit object id. The snapshot keeps raw bytes, so it names no id type of a later phase.
using ObjectUuid = std::array<std::uint8_t, 16>;

/// The identity of a window, tab or pane: its id and its label.
struct ObjectIdentityRecord
{
    ObjectUuid uuid {};
    std::optional<std::string> label {};

    bool operator==(ObjectIdentityRecord const&) const = default;
};

/// One pane (a leaf of a tab's split tree) as it was captured.
struct PaneSnapshot
{
    std::optional<ObjectIdentityRecord> identity {};
    std::optional<std::filesystem::path> workingDirectory {};
    std::optional<std::string> profile {};
    std::optional<std::string> foregroundProcess {};  ///< As the OS named it at capture.
    std::optional<std::string> runningCommandLine {}; ///< The newest Running block's command line.
    std::optional<proto::Delta> history {};           ///< Rows, hyperlinks, finished block records.

    bool operator==(PaneSnapshot const&) const = default;
};

/// A node of a tab's split tree.
struct PaneNode
{
    vtworkspace::SplitState split = vtworkspace::SplitState::None;
    double ratio = 0.5;                ///< The first child's share; splits only.
    std::vector<PaneNode> children {}; ///< Exactly two for a split, none for a leaf.
    PaneSnapshot pane {};              ///< Leaves only.

    bool operator==(PaneNode const&) const = default;
};

/// Whether a tab showed all its panes or one of them alone.
enum class TabZoom : std::uint8_t
{
    Tiled = 0,
    Zoomed = 1,
};

/// One tab as it was captured.
struct TabSnapshot
{
    std::optional<ObjectIdentityRecord> identity {};
    std::optional<std::string> title {};
    std::optional<vtbackend::RGBColor> color {};
    PaneNode root {};
    std::uint32_t activeLeaf = 0; ///< Index among the leaves, depth-first, first child first.
    TabZoom zoom = TabZoom::Tiled;

    bool operator==(TabSnapshot const&) const = default;
};

/// One window as it was captured.
struct WindowSnapshot
{
    std::optional<ObjectIdentityRecord> identity {};
    std::vector<TabSnapshot> tabs {};
    std::uint32_t activeTab = 0;

    bool operator==(WindowSnapshot const&) const = default;
};

/// Everything the daemon hosted at one instant.
struct WorkspaceSnapshot
{
    std::chrono::sys_seconds takenAt {};
    std::vector<WindowSnapshot> windows {};

    /// @return Whether the snapshot holds no window, i.e. there is nothing to restore.
    [[nodiscard]] bool empty() const noexcept { return windows.empty(); }

    bool operator==(WorkspaceSnapshot const&) const = default;
};

} // namespace vthost
