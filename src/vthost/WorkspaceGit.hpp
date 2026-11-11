// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The git state of a workspace, as the sidebar shows it: the branch its active pane's directory is on, and
/// how far that branch is ahead of and behind its upstream. Looked up off the loop thread, never under a
/// terminal's lock, and told to the loop only when it changed.

#include <vtbackend/screen/GitHead.hpp>

#include <core/Environment.hpp>
#include <core/async/IExecutor.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vthost/CommandRunner.hpp>
#include <vtworkspace/Primitives.hpp>

namespace vthost
{

/// What a workspace's directory is in, git-wise.
struct GitInfo
{
    vtbackend::GitHeadKind kind = vtbackend::GitHeadKind::Branch; ///< What @ref branch names.
    std::string branch {}; ///< The branch or ref name, or a detached HEAD's short object id.
    /// Commits the branch has that its upstream has not; nullopt when it has no upstream, or is no branch.
    std::optional<uint32_t> ahead {};
    /// Commits its upstream has that the branch has not; nullopt as @ref ahead.
    std::optional<uint32_t> behind {};

    bool operator==(GitInfo const&) const = default;
};

/// Looks up the git state of a directory. Synchronous: it runs on an executor, never on the loop thread.
class IGitStatusProbe
{
  public:
    IGitStatusProbe() = default;
    IGitStatusProbe(IGitStatusProbe const&) = delete;
    IGitStatusProbe(IGitStatusProbe&&) = delete;
    IGitStatusProbe& operator=(IGitStatusProbe const&) = delete;
    IGitStatusProbe& operator=(IGitStatusProbe&&) = delete;
    virtual ~IGitStatusProbe() = default;

    /// @param cwd A directory, possibly inside a repository.
    /// @return What git says about it; nullopt outside a repository. Callable from any thread.
    [[nodiscard]] virtual std::optional<GitInfo> probe(std::filesystem::path const& cwd) const = 0;
};

/// Parses what `git rev-list --left-right --count @{upstream}...HEAD` printed: `<behind>\t<ahead>\n`, the
/// left side being what only the upstream has.
/// @param standardOutput The command's output.
/// @return {behind, ahead}; nullopt for anything but two counts.
[[nodiscard]] std::optional<std::pair<uint32_t, uint32_t>> parseLeftRightCount(
    std::string_view standardOutput);

/// The environment git runs under for a probe: what it needs to find its configuration, and
/// `GIT_OPTIONAL_LOCKS=0`, so that a probe never takes the index lock a user's own git needs, and
/// `GIT_TERMINAL_PROMPT=0` with an empty `GIT_ASKPASS`, so that it never waits for a prompt.
/// @param environment The daemon's environment.
/// @return "NAME=VALUE" entries, for makeNativeCommandRunner().
[[nodiscard]] std::vector<std::string> gitProbeEnvironment(core::Environment const& environment);

/// The probe that reads HEAD through @p fileSystem and asks git for the counts through @p commands.
/// @param fileSystem Where repositories are read; borrowed, and called from the executor's threads.
/// @param commands Runs `git`; borrowed, and used from the executor's threads only.
/// @return The probe. Without an upstream, or with no git to run, the branch is told without counts.
[[nodiscard]] std::unique_ptr<IGitStatusProbe> makeGitStatusProbe(vtbackend::GitFileSystem const& fileSystem,
                                                                  CommandRunner& commands);

/// What a SessionHost needs to track its workspaces' git state; borrowed, and all of it must outlive the host
/// (an executor is joined before the probe it runs goes).
struct WorkspaceGitServices
{
    IGitStatusProbe const& probe;     ///< Looks a directory up.
    core::async::IExecutor& executor; ///< Where it does.
};

/// Tracks the git state of workspaces: one probe in flight per workspace, run on an executor and handed back
/// to the loop thread. Every member but the destructor runs on the loop thread.
///
/// The executor must be joined (or idle) before the probe is destroyed, as a probe queued behind the
/// destruction still runs.
class WorkspaceGitTracker
{
  public:
    /// Hands a callable to the loop thread; callable from any thread.
    using Post = std::function<void(std::function<void()>)>;
    /// Told, on the loop thread, that a workspace's state changed; nullopt when it left its repository.
    using Changed = std::function<void(vtworkspace::WorkspaceId, std::optional<GitInfo> const&)>;

    /// @param probe Looks a directory up; borrowed.
    /// @param executor Where it does; borrowed.
    /// @param post How a result gets back to the loop thread.
    /// @param onChanged Told of each change, on the loop thread.
    WorkspaceGitTracker(IGitStatusProbe const& probe,
                        core::async::IExecutor& executor,
                        Post post,
                        Changed onChanged);
    ~WorkspaceGitTracker();

    WorkspaceGitTracker(WorkspaceGitTracker const&) = delete;
    WorkspaceGitTracker(WorkspaceGitTracker&&) = delete;
    WorkspaceGitTracker& operator=(WorkspaceGitTracker const&) = delete;
    WorkspaceGitTracker& operator=(WorkspaceGitTracker&&) = delete;

    /// Looks @p cwd up for @p workspace. One probe runs per workspace at a time: asked again meanwhile, the
    /// tracker probes once more, for the newest directory, when it ends.
    /// @param workspace The workspace.
    /// @param cwd The directory of its active pane.
    void refresh(vtworkspace::WorkspaceId workspace, std::filesystem::path cwd);

    /// Drops what is known of @p workspace, and ignores a probe still running for it.
    /// @param workspace The workspace.
    void forget(vtworkspace::WorkspaceId workspace);

    /// @param workspace The workspace.
    /// @return What was last told of it; nullopt when it is in no repository, or was never looked up.
    [[nodiscard]] std::optional<GitInfo> infoOf(vtworkspace::WorkspaceId workspace) const;

  private:
    struct State;
    std::shared_ptr<State> _state;
};

} // namespace vthost
