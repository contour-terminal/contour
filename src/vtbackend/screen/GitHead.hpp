// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/async/IExecutor.hpp>
#include <core/platform/FileSystem.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace vtbackend
{

/// What a repository's HEAD names.
enum class GitHeadKind : uint8_t
{
    Branch = 0, ///< A local branch, `ref: refs/heads/<name>`.
    Detached,   ///< A commit, named by its abbreviated object id.
    OtherRef,   ///< Any other ref, `ref: refs/<name>` -- a remote-tracking branch, a tag.
};

/// A repository's HEAD as `{Git}` shows it.
struct GitHead
{
    GitHeadKind kind = GitHeadKind::Branch; ///< What @ref name is.
    std::string name {};                    ///< The branch or ref below its prefix, or the short object id.

    bool operator==(GitHead const&) const = default;
};

/// The largest HEAD or `.git` file read, in bytes; a larger one is not git's and is ignored.
inline constexpr std::uintmax_t MaxGitMetadataSize = 4096;

/// How many hex digits of a detached HEAD's object id are shown, as `git log --oneline` does.
inline constexpr std::size_t DetachedHeadDigits = 7;

/// The branch a reftable repository's HEAD file names: a stub that is no branch, kept so that an older git
/// refuses the repository rather than misreading it.
inline constexpr std::string_view ReftableStubBranch = ".invalid";

/// Reads a HEAD file's contents.
///
/// Only the files backend's HEAD is understood. A reftable repository (`git init --ref-format=reftable`,
/// planned as git 3.0's default) keeps HEAD's target in `<gitdir>/reftable/`, whose binary records this does
/// not decode: its stub `ref: refs/heads/.invalid` reads as nullopt, and GitHeadCache shows no branch for any
/// repository with a `reftable` directory.
/// @param contents The file's text; trailing whitespace is ignored.
/// @return `ref: refs/heads/<n>` gives a Branch `<n>`, any other `ref: refs/<r>` an OtherRef `<r>`, and a
///         SHA-1 or SHA-256 object id (40 or 64 hex digits) a Detached head of its first
///         @ref DetachedHeadDigits; anything else nullopt.
[[nodiscard]] std::optional<GitHead> parseGitHead(std::string_view contents);

/// What a path is, as one probe of it says.
enum class GitPathKind : uint8_t
{
    Absent = 0,  ///< Nothing is there, or a component of the path is no directory.
    Directory,   ///< A directory, or a link to one.
    RegularFile, ///< A regular file, or a link to one.
    Other,       ///< Anything else -- a socket, a device, a path that cannot be probed.
};

/// What one probe of a path says: what is there, and which filesystem holds it.
struct GitPathStatus
{
    GitPathKind kind = GitPathKind::Absent; ///< What the path names.
    /// The filesystem holding it, comparable only with another of the same GitFileSystem; nullopt where
    /// that cannot be told, which never ends a walk.
    std::optional<std::uint64_t> device {};

    bool operator==(GitPathStatus const&) const = default;
};

/// The filesystem as repository discovery reads it: one probe per path, and a bounded read.
///
/// A seam of its own rather than core::platform::FileSystem, which answers "what is this" in up to three
/// calls and cannot say which filesystem a path is on -- what a walk needs to stop at a mount boundary as
/// git does. Implementations are called from a background executor and must be safe to call from any
/// thread.
class GitFileSystem
{
  public:
    GitFileSystem() = default;
    GitFileSystem(GitFileSystem const&) = delete;
    GitFileSystem(GitFileSystem&&) = delete;
    GitFileSystem& operator=(GitFileSystem const&) = delete;
    GitFileSystem& operator=(GitFileSystem&&) = delete;
    virtual ~GitFileSystem() = default;

    /// Probes @p path once, following links.
    /// @param path The path.
    /// @return What it is, and its filesystem.
    [[nodiscard]] virtual GitPathStatus status(std::filesystem::path const& path) const = 0;

    /// Reads a small file of git's.
    /// @param path The file.
    /// @return Its contents when it is a regular file of at most @ref MaxGitMetadataSize bytes; else nullopt.
    [[nodiscard]] virtual std::optional<std::string> readMetadata(
        std::filesystem::path const& path) const = 0;
};

/// This machine's filesystem: a `stat(2)` per probe, its `st_dev` the device, and reads through @p files.
/// Windows tells no device, so a walk there is not stopped at a volume boundary.
class NativeGitFileSystem final: public GitFileSystem
{
  public:
    /// @param files Where files are read. Borrowed: it must outlive this.
    explicit NativeGitFileSystem(core::platform::FileSystem const& files): _files { files } {}

    [[nodiscard]] GitPathStatus status(std::filesystem::path const& path) const override;
    [[nodiscard]] std::optional<std::string> readMetadata(std::filesystem::path const& path) const override;

  private:
    core::platform::FileSystem const& _files;
};

/// What a terminal's `{Git}` needs: where the lookups run, and what they read.
struct GitHeadServices
{
    /// Where the walk and the read run: a thread a hung mount may block, never one holding a terminal's lock.
    /// It must be joined before @ref fileSystem is destroyed.
    core::async::IExecutor& executor;
    GitFileSystem const& fileSystem; ///< What the lookups read. Must outlive every lookup in flight.
};

/// Finds the HEAD file of the repository @p directory is in, as git does: the first `.git` walking up to
/// the root decides, and the walk does not leave the filesystem it started on (git's default, without
/// `GIT_DISCOVERY_ACROSS_FILESYSTEM`).
///
/// Lexical: a symlinked directory is walked by its own parents, not its target's. OSC 7 reports the logical
/// `$PWD`, so a shell in `~/link` pointing into a repository finds no repository, where git, which walks the
/// physical path, does. Resolving it would cost a realpath on every lookup.
///
/// Each level costs two probes: the directory, for its device, and its `.git`.
/// @param fileSystem Where to look.
/// @param directory Where to start; a path without a root directory finds nothing.
/// @return `.git/HEAD` when that `.git` is a directory holding it as a regular file; `<p>/HEAD` when it is
///         a file (a worktree or submodule) of at most @ref MaxGitMetadataSize bytes whose first line is
///         `gitdir: <p>`, @p p relative to the directory holding the `.git` file; nullopt otherwise.
[[nodiscard]] std::optional<std::filesystem::path> findGitHeadFile(GitFileSystem const& fileSystem,
                                                                   std::filesystem::path const& directory);

/// What `{Git}` shows for @p directory: the branch, ref or short object id, made safe to show.
/// @param fileSystem Where to look.
/// @param directory The directory whose repository to describe.
/// @return The text; empty outside a repository, for a reftable one, or when HEAD cannot be read.
[[nodiscard]] std::string describeGitHead(GitFileSystem const& fileSystem,
                                          std::filesystem::path const& directory);

/// The branch `{Git}` shows, looked up on a background executor and never under a terminal's lock.
///
/// A frame calls refresh(), which does no I/O: once a lookup for a directory has started it starts another
/// only after invalidate() -- which the terminal calls when a command ends (OSC 133 D, or a daemon's record
/// of one) and on every working-directory report (OSC 7) -- or for another directory. The lookup runs on
/// the services' executor; when its answer differs from what is shown, the cache publishes it and asks the
/// owner for a frame. Until then the previous answer stays, so a frame never waits and a branch shows one
/// frame after it is known.
///
/// invalidate(), refresh() and branch() are called under the terminal's lock; the lookup takes only the
/// cache's own mutex, after the terminal's in lock order.
class GitHeadCache
{
  public:
    /// @param services Where lookups run and what they read; see GitHeadServices for the lifetime rules.
    /// @param onBranchChanged Called on the executor, with the cache's mutex held and the terminal's NOT
    ///                        held, when a lookup changed branch(). Must be cheap, must not throw, and must
    ///                        not take the terminal's lock or call into this cache.
    GitHeadCache(GitHeadServices services, std::function<void()> onBranchChanged);

    /// Stops telling the owner: a lookup in flight finishes, unheard.
    ~GitHeadCache();

    GitHeadCache(GitHeadCache const&) = delete;
    GitHeadCache(GitHeadCache&&) = delete;
    GitHeadCache& operator=(GitHeadCache const&) = delete;
    GitHeadCache& operator=(GitHeadCache&&) = delete;

    /// Says the branch may have changed, so that the next refresh() looks again.
    void invalidate() noexcept;

    /// Starts a lookup for @p directory unless one already ran or runs for it and nothing invalidated it.
    /// Never blocks on I/O.
    /// @param directory The directory whose repository to show; nullopt for none (no branch, at once).
    void refresh(std::optional<std::filesystem::path> const& directory);

    /// @return The last answer, made safe to show; empty for none.
    [[nodiscard]] std::string branch() const;

  private:
    struct State;
    std::shared_ptr<State> _state;
};

} // namespace vtbackend
