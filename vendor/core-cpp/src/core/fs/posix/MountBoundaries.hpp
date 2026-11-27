// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Where the POSIX relation walk leaves a mount, and whether the mount it leaves hides real parents
/// that the other directory could be.
/// - A mount that shows a subtree of its file system -- a bind mount of a subdirectory or a btrfs
///   subvolume (Linux), a nullfs mount (FreeBSD) -- has a `..` at its root that leads to the
///   directory it is mounted on, so the directories of that file system above the subtree are never
///   seen. Each of them is on the left mount's own file system and reached through some other
///   mount; so a walk's `Unrelated` still stands where the other directory is on the left mount
///   itself, or on another file system.
/// - A macOS firmlink (`/Users` on the Data volume) leaves a mount below its root: its `..` is the
///   system volume's root. The directories skipped are the mount's own above the one left from,
///   and every directory on its mount point's path below `/` (`/System`, `/System/Volumes`, and
///   `/System/Volumes/Data`, the mount's root): read by identity where they can be, and any
///   directory may be one where they cannot.
/// - A file system that may show another's directories as its own (overlay, FUSE, nullfs, a network
///   share that may export this machine) has no identity that says what it shows: a walk that
///   leaves one, or meets a directory on one, decides only on that same mount.
/// - Walks up never see a mount BENEATH either directory: a bind mount of the other's file system
///   below one of them puts the other inside it all the same. So an `Unrelated` also scans the mount
///   table (@c IMountBoundaries::mountTable) for mounts beneath each, and stands only where every one
///   is of a file system that shows nothing of the other's.
/// One implementation per OS, chosen by CMake. Private to the fs module.

#include <core/fs/FsError.hpp>
#include <core/fs/FsExport.hpp>
#include <core/fs/detail/Ancestry.hpp>
#include <core/fs/detail/OwnedHandle.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace core::fs::detail
{

/// Whether a mount's file system may show another file system's directories as its own.
enum class Reexposure : std::uint8_t
{
    Possible,   ///< Overlay, FUSE, nullfs, a network share, or a type that cannot be read.
    Impossible, ///< A file system that shows only its own directories.
};

/// What a relation walk knows about one mount: each part is nullopt where this system cannot tell
/// it, which decides nothing (fail closed).
struct MountView
{
    /// Names the mount; one name, one mount. Linux: its mount id; macOS and the BSDs: where it is
    /// mounted and its file system id.
    std::optional<std::string> mount;
    /// Names the file system the mount shows a directory of: Linux's superblock device from
    /// mountinfo, never a btrfs subvolume's `st_dev`; elsewhere `st_dev`.
    std::optional<std::uint64_t> fileSystem;
    /// The directory of that file system the mount shows, as an absolute path: `/` for all of it.
    std::optional<std::string> root;
    /// Whether its file system may show another's directories.
    Reexposure reexposure = Reexposure::Possible;

    [[nodiscard]] bool operator==(MountView const&) const = default;
};

/// Where a walk left a mount.
enum class MountExit : std::uint8_t
{
    BelowRoot, ///< From a directory below its root (a macOS firmlink), or where that cannot be told.
    AtRoot,    ///< From its root, to the directory it is mounted on.
};

/// How much of what a step below a mount's root skipped is known.
enum class SkippedSet : std::uint8_t
{
    Unknown, ///< Nothing could be read: any directory may have been skipped.
    AtLeast, ///< @c MountCrossing::skipped, and the mount's own directories above the one left from.
    Exactly, ///< @c MountCrossing::skipped and nothing else: the one left from is a child of the root.
};

/// One step of a walk that left a mount.
struct MountCrossing
{
    MountView mount;                       ///< The mount left.
    MountExit exit = MountExit::BelowRoot; ///< Where it was left.
    /// Below the root only: the directories the step skipped, by identity -- each directory on the
    /// mount point's path below `/`, the last of them the mount's root as that path reaches it.
    /// Ignored at the root.
    std::vector<ObjectIdentity> skipped;
    SkippedSet skippedSet = SkippedSet::Unknown; ///< How much of what was skipped @c skipped is.

    [[nodiscard]] bool operator==(MountCrossing const&) const = default;
};

/// Whether the other directory's walk passed through the directory a crossing left from.
enum class PassesThrough : std::uint8_t
{
    No,
    Yes,
};

/// The other directory of a relation, as a crossing's check sees it.
struct HidingCandidate
{
    MountView mount;                          ///< The mount it is on.
    ObjectIdentity identity;                  ///< Its own identity.
    PassesThrough passes = PassesThrough::No; ///< Whether its walk passed the one the crossing left from.
};

/// Decides whether @p other could be one of the directories @p left skipped or hides.
/// @param left A mount a walk left.
/// @param other The other directory.
/// @return For a crossing below its root that @p other's walk did not pass: true where nothing of
///         what was skipped could be read, where @p other is one of the directories known skipped,
///         or (unless all skipped is known) where @p other is on the mount left. On the mount left,
///         false otherwise. Then true where either may re-expose another file system (unless
///         @p left shows its own file system whole, from its root); false where the two are on
///         different file systems, or (left at its root) @p other shows a directory not above
///         @p left's root; true otherwise, and wherever a part needed is unknown.
[[nodiscard]] CORE_FS_API bool mayHide(MountCrossing const& left, HidingCandidate const& other);

/// Opens @p name in @p directory as a directory, never following a link, and only as the directory
/// a stat of the name identified: the beneath scan's step, between whose `fstatat` and `openat` a
/// rename can put another directory under the name.
/// @param directory Where @p name is.
/// @param name One component.
/// @param expected What the stat of @p name answered.
/// @return The opened directory; `Busy` where it is not @p expected; otherwise what refused the open.
[[nodiscard]] CORE_FS_API FsResult<UniqueHandle> openNamedAs(int directory,
                                                             std::string const& name,
                                                             ObjectIdentity const& expected);

/// How a @c ReexposingType's name matches a mount's type.
enum class TypeMatch : std::uint8_t
{
    Exact,  ///< The type is the name.
    Family, ///< The type starts with the name (`fuse` for `fuse.sshfs` and `fuseblk`).
};

/// A file system type, or a family of them, that may re-expose another file system.
struct ReexposingType
{
    std::string_view name;              ///< The type, or the prefix of a family.
    TypeMatch match = TypeMatch::Exact; ///< How @c name matches.
};

/// @param table The types a system names as re-exposing.
/// @param type A mount's type.
/// @return `Possible` where @p type is in @p table, or empty (unreadable); `Impossible` otherwise.
[[nodiscard]] CORE_FS_API Reexposure reexposureOf(std::span<ReexposingType const> table,
                                                  std::string_view type);

/// One row of this process's mount table, as the scan for mounts beneath a directory reads it.
struct MountTableRow
{
    /// Where the mount is, absolute, as this process sees it.
    std::string path;
    /// Its name, as @c MountView::mount spells it for a directory on it; nullopt where it has none.
    std::optional<std::string> mount;

    [[nodiscard]] bool operator==(MountTableRow const&) const = default;
};

/// Reads the mounts of a relation walk.
class IMountBoundaries
{
  public:
    IMountBoundaries() = default;
    IMountBoundaries(IMountBoundaries const&) = delete;
    IMountBoundaries(IMountBoundaries&&) = delete;
    IMountBoundaries& operator=(IMountBoundaries const&) = delete;
    IMountBoundaries& operator=(IMountBoundaries&&) = delete;
    virtual ~IMountBoundaries() = default;

    /// Checks the step from @p child to @p parent, its `..`.
    /// @param child A directory the walk is at.
    /// @param parent What its `..` opened.
    /// @return Nullopt where both are on one mount; otherwise the mount @p child is on, which the
    ///         step leaves, and whether @p child is its root; or what refused reading them.
    [[nodiscard]] virtual FsResult<std::optional<MountCrossing>> crossing(int child, int parent) const = 0;

    /// @param handle A directory.
    /// @return The mount it is on, or what refused reading it.
    [[nodiscard]] virtual FsResult<MountView> mountOf(int handle) const = 0;

    /// @return Every mount this process sees, in the table's order; or what refused reading the
    ///         table. Nothing may be missing from it: a row that does not parse refuses the whole.
    [[nodiscard]] virtual FsResult<std::vector<MountTableRow>> mountTable() const = 0;
};

/// @return This system's boundaries, made when a file system is built (`linux/`, `bsd/` or
///         `posix/MountBoundaries.cpp`); null where they cannot be read, so that every relation is
///         `Unsupported`.
[[nodiscard]] CORE_FS_API std::shared_ptr<IMountBoundaries const> makeMountBoundaries();

} // namespace core::fs::detail
