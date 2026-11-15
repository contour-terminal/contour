// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <vector>

/// What `contour api call workspace.list` prints, as far as Panes.cpp reads it; nothing else includes this
/// header. Glaze reflects only types with linkage, so these cannot sit in an anonymous namespace. Every
/// member is defaulted, so an absent key keeps its default.
namespace orchestrator::proto::detail
{

/// One program-status record of a pane.
struct JsonProgramStatus
{
    std::string id;    ///< The record's id: a program's, or the integration's that reported it.
    std::string state; ///< `idle`, `working`, `done`, `blocked` or `error`.
};

/// One pane.
struct JsonPane
{
    std::string id;                               ///< `pane:<uuid>`.
    std::vector<JsonProgramStatus> programStatus; ///< Its program-status records.
};

/// One tab.
struct JsonTab
{
    std::vector<JsonPane> panes; ///< Its panes.
};

/// One workspace.
struct JsonWorkspace
{
    std::vector<JsonTab> tabs; ///< Its tabs.
};

/// The reply.
struct JsonWorkspaceList
{
    std::vector<JsonWorkspace> workspaces; ///< Every workspace.
};

} // namespace orchestrator::proto::detail
