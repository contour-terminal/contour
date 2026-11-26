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

/// The names the user gave the workspaces of this process. A daemon's names live in its object registry;
/// an in-process workspace has none, so they are kept here.
class WorkspaceNames
{
  public:
    /// Gives @p workspace the name @p name; an empty name removes it.
    /// @param workspace The workspace.
    /// @param name Its name.
    void set(vtworkspace::WorkspaceId workspace, std::string name);

    /// @param workspace The workspace.
    /// @return Its name; empty when it has none.
    [[nodiscard]] std::string nameOf(vtworkspace::WorkspaceId workspace) const;

    /// Drops the name of @p workspace, which is gone.
    /// @param workspace The workspace.
    void forget(vtworkspace::WorkspaceId workspace);

  private:
    std::map<uint64_t, std::string> _names;
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
/// model's order, with the names of a WorkspaceNames store, the program status of each pane's session
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
    /// @param names The workspaces' names. Borrowed; as @p model.
    /// @param git What is known of each workspace's repository; null where git is not tracked (a test, a
    ///        build without it). Borrowed; as @p model.
    /// @param changed Told by notifyChanged(); may be empty.
    InProcessDirectory(vtworkspace::SessionModel const& model,
                       InProcessWindow& owner,
                       window::PaneStatusLookup const& status,
                       WorkspaceNames const& names,
                       vthost::WorkspaceGitTracker const* git,
                       window::DirectoryChanged changed);

    /// @copydoc window::IWorkspaceDirectorySource::directory
    [[nodiscard]] window::WorkspaceDirectory directory() const override;

    /// Shows @p workspace in the window (InProcessWindow::displayWorkspace), through which it owns it.
    /// @param workspace The workspace's id.
    /// @return Refused for a workspace the window does not own.
    window::SelectResult select(uint64_t workspace) override;

    /// @return FlagSource::Window: the window's own ledger marks its panes.
    [[nodiscard]] window::FlagSource flagSource() const override { return window::FlagSource::Window; }

    /// Nothing: the window's own ledger notes what its user saw (TabAttention).
    void markSeen(window::PaneKey /*pane*/, window::PaneSeen /*seen*/) override {}

    /// Nothing: no daemon watches the window's panes.
    void reportShown(window::WindowFocusState /*focus*/, std::vector<window::PaneKey> /*panes*/) override {}

    /// Tells the window's listeners that directory() lists something else than it did.
    void notifyChanged() const;

  private:
    vtworkspace::SessionModel const& _model;
    InProcessWindow& _owner;
    window::PaneStatusLookup const& _status;
    WorkspaceNames const& _names;
    vthost::WorkspaceGitTracker const* _git;
    window::DirectoryChanged _changed;
};

} // namespace contour::session
