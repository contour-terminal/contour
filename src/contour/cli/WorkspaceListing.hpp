// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A daemon's workspaces as its control API lists them (`workspace.list`), for a window switch to decide on.

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vtcontrol/Api.hpp>

namespace contour::cli
{

/// Reads what `workspace.list` answered.
/// @param replyJson The reply JSON.
/// @return The workspaces, in the daemon's order, or why the reply does not read as a listing.
[[nodiscard]] std::expected<std::vector<vtcontrol::WorkspaceInfo>, std::string> decodeWorkspaceList(
    std::string_view replyJson);

/// Resolves what `contour client --workspace` names to the daemon's native id of it, in the forms every
/// control verb takes (vtcontrol::resolveTarget): its id or an id suffix, its label, or its 1-based position.
/// @param spec What the user named.
/// @param workspaces The daemon's workspaces, as `workspace.list` answered.
/// @return The native id; why none, when the spec names no workspace, several, or one too old to have one.
[[nodiscard]] std::expected<uint64_t, std::string> resolveWorkspaceSpec(
    std::string_view spec, std::span<vtcontrol::WorkspaceInfo const> workspaces);

} // namespace contour::cli
