// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Where the workspace snapshot lives and how it is written and read: atomically, durably, privately, and
/// without ever losing the last good workspace.
///
/// A write goes to a private temporary sibling, which is synced to the disk before it is renamed over the
/// file, and the rename is synced too (IDurableFiles): a daemon crash, a power loss or a kernel crash at any
/// point leaves either the previous snapshot or the new one, whole. The temporary file is created
/// exclusively, so a link planted in its place is not followed; the content is then written by name, which
/// would follow a link swapped in between, but only the user can do that inside the `0700` directory.
///
/// A restore never destroys the file it reads. beginRestore() moves `<file>` to `<file>.restored`; the first
/// successful periodic write after a restore that realized something deletes it (confirmRestored()). A
/// restore that realized nothing leaves it for one more attempt at the next start, then sets it aside as
/// `<file>.failed`.

#include <core/platform/FileSystem.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <vthost/DurableFiles.hpp>
#include <vthost/FilePrivacy.hpp>
#include <vthost/SessionSnapshot.hpp>

namespace vthost
{

/// What load() found.
enum class LoadVerdict : std::uint8_t
{
    Absent = 0,  ///< No snapshot file.
    Empty,       ///< A readable snapshot of nothing: the daemon was stopped, or held no session.
    Restorable,  ///< A workspace to bring back.
    SetAside,    ///< The file was damaged and has been renamed to `<file>.corrupt`.
    Unsupported, ///< Written by a newer format; left in place.
    Unreadable,  ///< The file could not be read at all.
};

/// Which file a load read.
enum class RestoreSource : std::uint8_t
{
    Snapshot = 0,   ///< `<file>`.
    PendingRestore, ///< `<file>.restored`, left by a restore that realized nothing.
};

/// What a restore came to.
enum class RestoreOutcome : std::uint8_t
{
    NothingRealized = 0,
    Realized,
};

/// The result of load().
struct LoadResult
{
    LoadVerdict verdict = LoadVerdict::Absent;
    RestoreSource source = RestoreSource::Snapshot;
    std::optional<WorkspaceSnapshot> snapshot {}; ///< Set for Restorable only.
    std::size_t droppedHistories = 0;
    std::string detail {}; ///< For a log line: what was wrong, when anything was.
};

/// The step of a store operation that failed.
enum class StoreStep : std::uint8_t
{
    CreateDirectory = 0,
    Restrict,
    WriteTemporary,
    SyncTemporary, ///< The temporary file could not be flushed to the disk; the previous snapshot stays.
    Replace,
    SyncDirectory, ///< The rename landed but was not flushed to the disk; a power loss may undo it.
};

/// Why a store operation failed.
struct StoreFailure
{
    StoreStep step;
    std::string detail;
};

/// One snapshot file.
class SessionSnapshotStore
{
  public:
    /// @param fileSystem The file system the snapshot lives on.
    /// @param privacy Makes what the store creates the user's alone.
    /// @param durability Flushes a write to the disk before and after it replaces the snapshot.
    /// @param path The snapshot file.
    SessionSnapshotStore(core::platform::FileSystem const& fileSystem,
                         IFilePrivacy const& privacy,
                         IDurableFiles const& durability,
                         std::filesystem::path path);

    /// Replaces the snapshot with @p snapshot: a private temporary sibling, synced, then renamed over the
    /// file, and the rename synced. On a failure before the rename the previous snapshot stays and no
    /// temporary file is left.
    /// @param snapshot The workspace.
    [[nodiscard]] std::expected<void, StoreFailure> write(WorkspaceSnapshot const& snapshot) const;

    /// Writes an empty snapshot.
    [[nodiscard]] std::expected<void, StoreFailure> clear() const;

    /// Writes an empty snapshot and drops `<file>.restored`, which load() would otherwise prefer: what a
    /// stop that discards the workspace leaves, also when no daemon runs to write it.
    [[nodiscard]] std::expected<void, StoreFailure> discard() const;

    /// Reads `<file>.restored` when present, else `<file>`. A damaged file is renamed `.corrupt`.
    [[nodiscard]] LoadResult load() const;

    /// Moves `<file>` to `<file>.restored` before a restore realizes it; a no-op for a load that came
    /// from `.restored`.
    /// @param source What load() read.
    [[nodiscard]] std::expected<void, StoreFailure> beginRestore(RestoreSource source) const;

    /// Closes a restore. A realized one leaves `.restored` for confirmRestored(); one that realized
    /// nothing, on its second attempt (@p source PendingRestore), renames it `<file>.failed`.
    /// @param source What load() read.
    /// @param outcome Whether any pane came back.
    [[nodiscard]] std::expected<void, StoreFailure> endRestore(RestoreSource source,
                                                               RestoreOutcome outcome) const;

    /// Deletes `<file>.restored`; the first successful periodic write after a realized restore calls it.
    [[nodiscard]] std::expected<void, StoreFailure> confirmRestored() const;

    /// @return The snapshot file's path.
    [[nodiscard]] std::filesystem::path const& path() const noexcept { return _path; }

  private:
    [[nodiscard]] LoadResult loadFile(std::filesystem::path const& file, RestoreSource source) const;
    [[nodiscard]] std::filesystem::path sibling(std::string_view suffix) const;

    core::platform::FileSystem const& _fileSystem;
    IFilePrivacy const& _privacy;
    IDurableFiles const& _durability;
    std::filesystem::path _path;
};

/// `<stateDirectory>/sessions/<label>.snapshot`.
/// @param stateDirectory The daemon's state directory (`daemonStateDirectory(...)`, StatePath.hpp).
/// @param label The session label.
/// @return The path; nullopt for a label that is empty or holds `/`, `\`, `:`, NUL or `..`.
[[nodiscard]] std::optional<std::filesystem::path> sessionSnapshotPath(
    std::filesystem::path const& stateDirectory, std::string_view label);

/// The snapshot file of the daemon labelled @p label that listens on @p socket: its label's own
/// (sessionSnapshotPath) when @p socket is the one its label would run on, else
/// `<stateDirectory>/sessions/<label>@<hash>.snapshot`, the hash being the 16 hex digits of the FNV-1a 64 of
/// the socket's path. A daemon at a socket of its own, beside the one its label runs, so never restores or
/// overwrites that one's workspace.
/// @param stateDirectory The daemon's state directory.
/// @param label The session label.
/// @param socket The socket the daemon listens on; canonical, as @p labelSocket.
/// @param labelSocket The socket its label would run on (labelSocketPath, SocketPath.hpp).
/// @return The path; nullopt where sessionSnapshotPath gives none.
[[nodiscard]] std::optional<std::filesystem::path> daemonSnapshotPath(
    std::filesystem::path const& stateDirectory,
    std::string_view label,
    std::filesystem::path const& socket,
    std::filesystem::path const& labelSocket);

} // namespace vthost
