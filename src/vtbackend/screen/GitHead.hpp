// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/FileSystem.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
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

/// Reads a HEAD file's contents.
/// @param contents The file's text; trailing whitespace is ignored.
/// @return `ref: refs/heads/<n>` gives a Branch `<n>`, any other `ref: refs/<r>` an OtherRef `<r>`, and a
///         SHA-1 or SHA-256 object id (40 or 64 hex digits) a Detached head of its first
///         @ref DetachedHeadDigits; anything else nullopt.
[[nodiscard]] std::optional<GitHead> parseGitHead(std::string_view contents);

/// Finds the HEAD file of the repository @p directory is in, as git does: the first `.git` walking up to
/// the root decides.
/// @param fileSystem Where to look.
/// @param directory Where to start; a path without a root directory finds nothing.
/// @return `.git/HEAD` when that `.git` is a directory holding it as a regular file; `<p>/HEAD` when it is
///         a file (a worktree or submodule) of at most @ref MaxGitMetadataSize bytes whose first line is
///         `gitdir: <p>`, @p p relative to the directory holding the `.git` file; nullopt otherwise.
[[nodiscard]] std::optional<std::filesystem::path> findGitHeadFile(
    core::platform::FileSystem const& fileSystem, std::filesystem::path const& directory);

/// The branch `{Git}` shows, read through a filesystem only when it may have changed.
///
/// Per frame refresh() costs nothing: once it has read for a directory it reads again only after
/// invalidate() -- which the terminal calls when a command ends (OSC 133 D) and on every working-directory
/// report (OSC 7) -- or for another directory. Even then it walks to HEAD and stats it, and re-reads only
/// when HEAD's path or modification time changed.
///
/// Not thread-safe: the terminal calls every member under its state lock.
class GitHeadCache
{
  public:
    /// @param fileSystem Where the repository is read. Borrowed: it must outlive the cache.
    explicit GitHeadCache(core::platform::FileSystem const& fileSystem);

    /// Says the branch may have changed, so that the next refresh() looks again.
    void invalidate() noexcept;

    /// Brings branch() up to date for @p directory, unless it already is for it and nothing invalidated it.
    /// @param directory The directory whose repository to show; nullopt for none (no branch).
    void refresh(std::optional<std::filesystem::path> const& directory);

    /// @return The branch, ref or short object id, made safe to show; empty for none.
    [[nodiscard]] std::string const& branch() const noexcept { return _branch; }

  private:
    /// Whether the cache may answer for its directory without looking.
    enum class GitHeadFreshness : uint8_t
    {
        Stale = 0, ///< Look on the next refresh().
        Fresh,     ///< Nothing invalidated it since it last looked.
    };

    /// What identifies a HEAD file's contents without reading them.
    struct HeadKey
    {
        std::filesystem::path head;               ///< The HEAD file.
        std::filesystem::file_time_type mtime {}; ///< Its last modification.

        bool operator==(HeadKey const&) const = default;
    };

    /// The key of the HEAD file @p directory's repository has, or nullopt for none.
    [[nodiscard]] std::optional<HeadKey> keyFor(std::filesystem::path const& directory) const;

    /// What the HEAD file @p head says, made safe to show; empty when it is unreadable, too large or not
    /// git's.
    [[nodiscard]] std::string branchIn(std::filesystem::path const& head) const;

    core::platform::FileSystem const& _fileSystem;
    GitHeadFreshness _freshness = GitHeadFreshness::Stale;
    std::optional<std::filesystem::path> _directory;
    std::optional<HeadKey> _key;
    std::string _branch;
};

} // namespace vtbackend
