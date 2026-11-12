// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/window/WorkspaceDirectory.hpp>
#include <contour/window/WorkspaceDirectorySource.hpp>

#include <cstdint>
#include <map>
#include <span>
#include <string>

#include <vthost/WorkspaceGit.hpp>
#include <vtworkspace/Primitives.hpp>
#include <vtworkspace/SessionModel.hpp>

namespace contour::session
{

/// The labels the user gave the workspaces of this process. A daemon's labels live in its object registry;
/// an in-process workspace has none, so they are kept here.
class WorkspaceLabels
{
  public:
    /// Gives @p workspace the label @p label; an empty label removes it.
    /// @param workspace The workspace.
    /// @param label Its label.
    void set(vtworkspace::WorkspaceId workspace, std::string label);

    /// @param workspace The workspace.
    /// @return Its label; empty when it has none.
    [[nodiscard]] std::string labelOf(vtworkspace::WorkspaceId workspace) const;

    /// Drops the label of @p workspace, which is gone.
    /// @param workspace The workspace.
    void forget(vtworkspace::WorkspaceId workspace);

  private:
    std::map<uint64_t, std::string> _labels;
};

/// What InProcessDirectory asks of the window it lists the workspaces of. Implemented by the window's
/// controller.
class InProcessWindow
{
  public:
    InProcessWindow() = default;
    InProcessWindow(InProcessWindow const&) = delete;
    InProcessWindow& operator=(InProcessWindow const&) = delete;
    InProcessWindow(InProcessWindow&&) = delete;
    InProcessWindow& operator=(InProcessWindow&&) = delete;
    virtual ~InProcessWindow() = default;

    /// @return Every workspace the window owns, in the order it took them.
    [[nodiscard]] virtual std::span<vtworkspace::WorkspaceId const> ownedWorkspaces() const noexcept = 0;
    /// @return The workspace the window shows.
    [[nodiscard]] virtual vtworkspace::WorkspaceId displayedWorkspace() const noexcept = 0;
    /// Shows @p workspace, one of the owned ones.
    /// @param workspace The workspace to show.
    virtual void displayWorkspace(vtworkspace::WorkspaceId workspace) = 0;
};

/// The WorkspaceDirectory of a window whose sessions run in this process: the workspaces it owns, in the
/// model's order, with the labels of a WorkspaceLabels store, the program status of each pane's session
/// and the git state a WorkspaceGitTracker knows.
///
/// DirectoryWorkspace::id is the WorkspaceId's value, DirectoryPane::key the pane's session id, and
/// DirectoryWorkspace::controlId stays zero: an in-process workspace is in no object registry.
class InProcessDirectory final: public window::IWorkspaceDirectorySource
{
  public:
    /// @param model The workspaces, tabs and panes. Borrowed; it must outlive the directory.
    /// @param owner The window whose workspaces are listed. Borrowed; as @p model.
    /// @param status Where the status of a pane comes from, by its session id. Borrowed; as @p model.
    /// @param labels The workspaces' labels. Borrowed; as @p model.
    /// @param git What is known of each workspace's repository; null where git is not tracked (a test, a
    ///        build without it). Borrowed; as @p model.
    /// @param changed Told by notifyChanged(); may be empty.
    InProcessDirectory(vtworkspace::SessionModel const& model,
                       InProcessWindow& owner,
                       window::PaneStatusLookup const& status,
                       WorkspaceLabels const& labels,
                       vthost::WorkspaceGitTracker const* git,
                       window::DirectoryChanged changed);

    /// @copydoc window::IWorkspaceDirectorySource::directory
    [[nodiscard]] window::WorkspaceDirectory directory() const override;

    /// Shows @p workspace in the window (InProcessWindow::displayWorkspace), through which it owns it.
    /// @param workspace The workspace's id.
    /// @return Refused for a workspace the window does not own.
    window::SelectResult select(uint64_t workspace) override;

    /// Tells the window's listeners that directory() lists something else than it did.
    void notifyChanged() const;

  private:
    vtworkspace::SessionModel const& _model;
    InProcessWindow& _owner;
    window::PaneStatusLookup const& _status;
    WorkspaceLabels const& _labels;
    vthost::WorkspaceGitTracker const* _git;
    window::DirectoryChanged _changed;
};

} // namespace contour::session
