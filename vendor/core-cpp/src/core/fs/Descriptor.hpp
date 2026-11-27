// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The descriptor-relative file system: @c IDescriptorFileSystem opens a preopened directory, and
/// every @c IDescriptor operation is an @c async::Task relative to the descriptor it is called on.
///
/// What every implementation guarantees:
/// - **No path leaves its descriptor.** An absolute path, a `..` above the descriptor, a symbolic
///   link whose target leaves it or is absolute: each is `NotPermitted`. A final symbolic link is
///   followed only where the call says @c SymlinkPolicy::Follow.
/// - **Rights are the descriptor's.** Each operation needs the @c DescriptorFlags its row of
///   @c rightsTable() names, and a child opened by @c IDescriptor::openAt carries no right its parent
///   could not grant (@c checkOpenRights). A refusal is `NotPermitted`, before anything is touched.
/// - **Nothing blocks the caller.** The blocking call runs on the executor the file system was built
///   with (@c async::offload), and the caller resumes on its home executor.
/// - **Paths are UTF-8**, relative, and checked against the file system's @c PathPolicy.

#include <core/async/Task.hpp>
#include <core/fs/AccessScope.hpp>
#include <core/fs/FsError.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace core::fs
{

/// What a descriptor or a directory entry refers to.
enum class DescriptorType : std::uint8_t
{
    Unknown,         ///< Something the file system does not classify.
    BlockDevice,     ///< A block device.
    CharacterDevice, ///< A character device.
    Directory,       ///< A directory.
    Fifo,            ///< A named pipe.
    SymbolicLink,    ///< A symbolic link.
    RegularFile,     ///< A regular file.
    Socket,          ///< A socket.
};

/// The rights and the write-integrity mode a descriptor carries. A bitmask: combine with `|`, test
/// with @c hasFlag.
enum class DescriptorFlags : std::uint8_t
{
    None = 0,                      ///< No right.
    Read = 0b1,                    ///< Read a file's bytes or a directory's entries; resolve paths.
    Write = 0b10,                  ///< Write a file's bytes. A directory never carries it.
    FileIntegritySync = 0b100,     ///< Writes complete with file integrity (`O_SYNC`). Not a right.
    DataIntegritySync = 0b1000,    ///< Writes complete with data integrity (`O_DSYNC`). Not a right.
    RequestedWriteSync = 0b1'0000, ///< Reads wait for pending writes (`O_RSYNC`). Not a right.
    MutateDirectory = 0b10'0000,   ///< Create, remove, rename and link entries in a directory.
};

/// @param a The first mask.
/// @param b The second mask.
/// @return Both masks' flags.
[[nodiscard]] constexpr DescriptorFlags operator|(DescriptorFlags a, DescriptorFlags b) noexcept
{
    return static_cast<DescriptorFlags>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

/// @param set The mask to test.
/// @param flag The flag to look for; @c DescriptorFlags::None is never present.
/// @return Whether @p set carries @p flag.
[[nodiscard]] constexpr bool hasFlag(DescriptorFlags set, DescriptorFlags flag) noexcept
{
    return (static_cast<std::uint8_t>(set) & static_cast<std::uint8_t>(flag)) != 0;
}

/// How an @c IDescriptor::openAt treats what it opens. A bitmask: combine with `|`, test with
/// @c hasFlag.
enum class OpenFlags : std::uint8_t
{
    None = 0,          ///< Open what exists.
    Create = 0b1,      ///< Create a regular file where nothing exists.
    Directory = 0b10,  ///< Fail unless the object is a directory.
    Exclusive = 0b100, ///< With @c Create: fail where something exists.
    Truncate = 0b1000, ///< Truncate a regular file to zero bytes.
    /// With @c Create and @c Exclusive (else `Invalid`): the file is created so that no other user
    /// can reach it at any moment, under its final name or before it:
    /// - **Linux:** mode 0600 is given to `openat` itself; a default ACL cannot widen it, since
    ///   each entry it hands down is capped by the mode (the mask by the group bits).
    /// - **macOS, FreeBSD:** an inherited ACL (extended; NFSv4) could, so the file is made inside a
    ///   fresh staging directory beside it: that directory is created 0700, its inherited ACL
    ///   stripped and its access verified before anything is created in it, so another user who
    ///   opened it early holds a descriptor that searches nothing. The file is created there,
    ///   stripped and verified too, then linked to its final name (which fails, as `Exclusive`
    ///   says, where something exists) and the staging directory removed.
    /// - **Windows:** owned by the process token's user, with a protected DACL (nothing inherited)
    ///   granting that user alone, given to `NtCreateFile` itself.
    ///
    /// Nothing has been written to it when the call returns. Where any step fails, what was made is
    /// removed through the parent's handle and the call fails (`NotPermitted` where the access read
    /// back is not @c AccessScope::OwnerOnly, else what refused the step).
    OwnerOnly = 0b1'0000,
};

/// Who may reach a directory @c IDescriptor::createDirectoryAt creates.
enum class CreationAccess : std::uint8_t
{
    Default, ///< What the system gives: POSIX 0777 less the umask; Windows the inherited DACL.
    /// No other user can reach it at any moment, as for @c OpenFlags::OwnerOnly: Linux gives 0700 to
    /// `mkdirat`; macOS and FreeBSD make it inside a stripped, verified 0700 staging directory, strip
    /// and verify it too, then rename it over an empty placeholder made with the final name (so an
    /// existing entry is `Exist`); Windows gives it the same protected DACL, its entry inheritable.
    /// Where a step fails, what was made is removed and the call fails.
    OwnerOnly,
};

/// @param a The first mask.
/// @param b The second mask.
/// @return Both masks' flags.
[[nodiscard]] constexpr OpenFlags operator|(OpenFlags a, OpenFlags b) noexcept
{
    return static_cast<OpenFlags>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

/// @param set The mask to test.
/// @param flag The flag to look for; @c OpenFlags::None is never present.
/// @return Whether @p set carries @p flag.
[[nodiscard]] constexpr bool hasFlag(OpenFlags set, OpenFlags flag) noexcept
{
    return (static_cast<std::uint8_t>(set) & static_cast<std::uint8_t>(flag)) != 0;
}

/// Whether a path's final component is followed when it is a symbolic link. Links before it are
/// always followed, beneath the descriptor.
enum class SymlinkPolicy : std::uint8_t
{
    NoFollow, ///< Operate on the link itself; an open of one is `Loop`.
    Follow,   ///< Operate on what it names.
};

/// How the caller intends to access a range of a file (`posix_fadvise`). Advisory only.
enum class Advice : std::uint8_t
{
    Normal,     ///< No particular pattern.
    Sequential, ///< In order, from low offsets to high.
    Random,     ///< In no particular order.
    WillNeed,   ///< Soon.
    DontNeed,   ///< Not soon.
    NoReuse,    ///< Once.
};

/// Whether a read reached the end of what it reads.
enum class StreamEnd : std::uint8_t
{
    More,    ///< There may be more after the bytes returned.
    Reached, ///< The bytes returned end at the end of the file.
};

/// A point in time, as seconds and nanoseconds since the Unix epoch.
struct FileTime
{
    std::int64_t seconds = 0;      ///< Whole seconds since 1970-01-01T00:00:00Z.
    std::uint32_t nanoseconds = 0; ///< Below a second, `[0, 1e9)`.

    /// Ordered by instant.
    auto operator<=>(FileTime const&) const = default;
};

/// A @c NewTimestamp that leaves the time as it is.
struct NoChange
{
};

/// A @c NewTimestamp that sets the time to the file system's clock.
struct SetToNow
{
};

/// What @c IDescriptor::setTimes does with one of the times.
using NewTimestamp = std::variant<NoChange, SetToNow, FileTime>;

/// What a descriptor refers to, and its size and times.
struct DescriptorStat
{
    DescriptorType type = DescriptorType::Unknown; ///< What it is.
    std::uint64_t linkCount = 0;                   ///< How many hard links name it.
    std::uint64_t size = 0;                        ///< Its size in bytes.
    std::optional<FileTime> accessed;              ///< Last access, where the system keeps it.
    std::optional<FileTime> modified;              ///< Last modification of its data.
    std::optional<FileTime> statusChanged;         ///< Last change of its metadata.
    /// Who may reach it, read from the handle the stat was made through (@c classifyAccess).
    ///
    /// **nullopt means "cannot judge", never "probably fine"**: an ACL that cannot be read, a
    /// Windows object whose DACL withholds `READ_CONTROL`, a Linux, FreeBSD or macOS
    /// @c IDescriptor::statAt, which holds no handle to read an ACL or a file system from, an object
    /// on Linux whose NFS or SMB mount or NFSv4 ACL the mode does not bound, and every object on
    /// NetBSD and on POSIX systems this module reads no ACL of. A consumer that guards a
    /// secret with this (a key directory, a key file) must refuse nullopt as it refuses
    /// @c AccessScope::Shared: fail closed. An object created owner-only (@c OpenFlags::OwnerOnly,
    /// @c CreationAccess::OwnerOnly) reads @c AccessScope::OwnerOnly through its handle when it is
    /// returned.
    std::optional<AccessScope> access;
};

/// One entry of a directory.
struct DirectoryEntry
{
    DescriptorType type = DescriptorType::Unknown; ///< What it is.
    std::string name;                              ///< Its name, in UTF-8.
};

/// A 128-bit hash of an object's metadata: equal for two descriptors of one unchanged object,
/// different once it changes.
struct MetadataHash
{
    std::uint64_t lower = 0; ///< The low 64 bits.
    std::uint64_t upper = 0; ///< The high 64 bits.

    /// Compared as a pair.
    auto operator<=>(MetadataHash const&) const = default;
};

/// What a read returns.
struct ReadChunk
{
    std::vector<std::byte> bytes;    ///< The bytes read; fewer than asked for near the end.
    StreamEnd end = StreamEnd::More; ///< Whether they end at the end of the file.
};

/// The entries of a directory, read in batches.
///
/// Each @c IDescriptor::readDirectory opens a stream of its own, positioned at the directory's
/// start, and the stream holds one batch of entries at a time, never the whole listing. A stream
/// outlives the descriptor it came from. An entry neither added nor removed while the directory is
/// read comes exactly once; one added or removed meanwhile may come or not, as the system has it. One
/// call at a time: @c next is called again once the call before it has completed, and the stream
/// outlives every call to it that is still pending.
class IDirectoryStream
{
  public:
    IDirectoryStream() = default;
    IDirectoryStream(IDirectoryStream const&) = delete;
    IDirectoryStream(IDirectoryStream&&) = delete;
    IDirectoryStream& operator=(IDirectoryStream const&) = delete;
    IDirectoryStream& operator=(IDirectoryStream&&) = delete;
    virtual ~IDirectoryStream() = default;

    /// @return The next entry, nullopt at the end; never "." or "..". An unrepresentable name is
    ///         IllegalByteSequence for that entry, and the following call continues after it.
    [[nodiscard]] virtual async::Task<FsResult<std::optional<DirectoryEntry>>> next() = 0;
};

/// An open file or directory, and every operation relative to it.
///
/// A descriptor passed as `newDirectory` or `other` is borrowed for the call; it must be one of the
/// same file system instance, or the answer is `CrossDevice` (`false` for @c isSameObject).
class IDescriptor
{
  public:
    IDescriptor() = default;
    IDescriptor(IDescriptor const&) = delete;
    IDescriptor(IDescriptor&&) = delete;
    IDescriptor& operator=(IDescriptor const&) = delete;
    IDescriptor& operator=(IDescriptor&&) = delete;
    virtual ~IDescriptor() = default;

    /// Reads up to @p maxBytes from @p offset. Needs `Read`.
    /// @param maxBytes How many bytes at most; clamped to the file system's `maxReadChunk`.
    /// @param offset Where to start.
    /// @return The bytes, and whether they reach the end of the file.
    [[nodiscard]] virtual async::Task<FsResult<ReadChunk>> read(std::size_t maxBytes,
                                                                std::uint64_t offset) = 0;

    /// Writes @p bytes at @p offset, extending the file as needed. Needs `Write`.
    /// @param bytes What to write.
    /// @param offset Where.
    /// @return How many bytes were written.
    [[nodiscard]] virtual async::Task<FsResult<std::size_t>> write(std::vector<std::byte> bytes,
                                                                   std::uint64_t offset) = 0;

    /// Writes @p bytes at the end of the file. Needs `Write`.
    /// @param bytes What to write.
    /// @return How many bytes were written.
    [[nodiscard]] virtual async::Task<FsResult<std::size_t>> append(std::vector<std::byte> bytes) = 0;

    /// Tells the system how a range will be accessed.
    /// @param offset Where the range starts.
    /// @param length How long it is; 0 to the end.
    /// @param advice How.
    /// @return Nothing, or why the advice was refused.
    [[nodiscard]] virtual async::Task<FsResult<void>> advise(std::uint64_t offset,
                                                             std::uint64_t length,
                                                             Advice advice) = 0;

    /// @return Nothing once the file's data is on stable storage (`fdatasync`).
    [[nodiscard]] virtual async::Task<FsResult<void>> syncData() = 0;

    /// @return Nothing once the file's data and metadata are on stable storage (`fsync`).
    [[nodiscard]] virtual async::Task<FsResult<void>> sync() = 0;

    /// @return The flags the descriptor was opened with.
    [[nodiscard]] virtual DescriptorFlags flags() const noexcept = 0;

    /// @return What the descriptor refers to.
    [[nodiscard]] virtual async::Task<FsResult<DescriptorType>> type() = 0;

    /// Truncates or extends the file to @p size bytes. Needs `Write`.
    /// @param size The new size.
    /// @return Nothing, or why not.
    [[nodiscard]] virtual async::Task<FsResult<void>> setSize(std::uint64_t size) = 0;

    /// Sets the access and modification times. Needs `Write`, or `MutateDirectory` on a directory.
    /// @param accessed The new access time.
    /// @param modified The new modification time.
    /// @return Nothing, or why not.
    [[nodiscard]] virtual async::Task<FsResult<void>> setTimes(NewTimestamp accessed,
                                                               NewTimestamp modified) = 0;

    /// @return What the descriptor refers to, its size and its times.
    [[nodiscard]] virtual async::Task<FsResult<DescriptorStat>> stat() = 0;

    /// @return The hash of the object's metadata.
    [[nodiscard]] virtual async::Task<FsResult<MetadataHash>> metadataHash() = 0;

    /// Opens a stream over the directory's entries. Needs `Read`.
    /// @return The stream.
    [[nodiscard]] virtual async::Task<FsResult<std::unique_ptr<IDirectoryStream>>> readDirectory() = 0;

    /// Opens @p path beneath this directory. Needs `Read`; the child's flags are held to
    /// @c checkOpenRights.
    /// @param policy Whether a final symbolic link is followed.
    /// @param path The relative path.
    /// @param open How to open it.
    /// @param flags The rights and mode the new descriptor carries.
    /// @return The new descriptor.
    [[nodiscard]] virtual async::Task<FsResult<std::unique_ptr<IDescriptor>>> openAt(
        SymlinkPolicy policy, std::string path, OpenFlags open, DescriptorFlags flags) = 0;

    /// Creates the directory @p path. Needs `MutateDirectory`.
    /// @param path The relative path.
    /// @param access Who may reach it, given to the creating call itself.
    /// @return Nothing, or why not.
    [[nodiscard]] virtual async::Task<FsResult<void>> createDirectoryAt(std::string path,
                                                                        CreationAccess access) = 0;

    /// Reads what @p path refers to without opening it. Needs `Read`.
    /// @param policy Whether a final symbolic link is followed.
    /// @param path The relative path.
    /// @return What @p path refers to, its size and its times.
    [[nodiscard]] virtual async::Task<FsResult<DescriptorStat>> statAt(SymlinkPolicy policy,
                                                                       std::string path) = 0;

    /// Sets the times of @p path. Needs `MutateDirectory`.
    /// @param policy Whether a final symbolic link is followed.
    /// @param path The relative path.
    /// @param accessed The new access time.
    /// @param modified The new modification time.
    /// @return Nothing, or why not.
    [[nodiscard]] virtual async::Task<FsResult<void>> setTimesAt(SymlinkPolicy policy,
                                                                 std::string path,
                                                                 NewTimestamp accessed,
                                                                 NewTimestamp modified) = 0;

    /// Creates the hard link @p newPath beneath @p newDirectory to @p oldPath beneath this one. Needs
    /// `MutateDirectory` on both.
    /// @param policy @c SymlinkPolicy::Follow is `Invalid`.
    /// @param oldPath The existing object.
    /// @param newDirectory From the same file system, or the answer is CrossDevice (D11).
    /// @param newPath The new name.
    /// @return Nothing, or why not.
    [[nodiscard]] virtual async::Task<FsResult<void>> linkAt(SymlinkPolicy policy,
                                                             std::string oldPath,
                                                             IDescriptor* newDirectory,
                                                             std::string newPath) = 0;

    /// Reads a symbolic link's target. Needs `Read`.
    /// @param path The relative path of the link.
    /// @return Its target, as stored; `NotPermitted` for a target that names a root
    ///         (@c isRootedLinkTarget), which no walk follows and no `symlinkAt` makes.
    [[nodiscard]] virtual async::Task<FsResult<std::string>> readlinkAt(std::string path) = 0;

    /// Removes the empty directory @p path. Needs `MutateDirectory`.
    /// @param path The relative path.
    /// @return Nothing, or why not.
    [[nodiscard]] virtual async::Task<FsResult<void>> removeDirectoryAt(std::string path) = 0;

    /// Renames @p oldPath beneath this directory to @p newPath beneath @p newDirectory. Needs
    /// `MutateDirectory` on both.
    /// @param oldPath The existing object.
    /// @param newDirectory From the same file system, or the answer is CrossDevice (D11).
    /// @param newPath The new name.
    /// @return Nothing, or why not.
    [[nodiscard]] virtual async::Task<FsResult<void>> renameAt(std::string oldPath,
                                                               IDescriptor* newDirectory,
                                                               std::string newPath) = 0;

    /// Creates the symbolic link @p newPath holding @p target. Needs `MutateDirectory`.
    /// @param target What the link names; an absolute one is `NotPermitted`.
    /// @param newPath The relative path of the link.
    /// @return Nothing, or why not.
    [[nodiscard]] virtual async::Task<FsResult<void>> symlinkAt(std::string target, std::string newPath) = 0;

    /// Removes the non-directory @p path. Needs `MutateDirectory`.
    /// @param path The relative path.
    /// @return Nothing, or why not.
    [[nodiscard]] virtual async::Task<FsResult<void>> unlinkFileAt(std::string path) = 0;

    /// Hashes the metadata of @p path without opening it. Needs `Read`.
    /// @param policy Whether a final symbolic link is followed.
    /// @param path The relative path.
    /// @return The hash of its metadata.
    [[nodiscard]] virtual async::Task<FsResult<MetadataHash>> metadataHashAt(SymlinkPolicy policy,
                                                                             std::string path) = 0;

    /// A bool because the name asks the question; a foreign descriptor is never the same object.
    /// @param other The descriptor to compare with.
    /// @return Whether both refer to one object.
    [[nodiscard]] virtual async::Task<FsResult<bool>> isSameObject(IDescriptor* other) = 0;
};

/// How two opened directories relate in the real directory tree. `Same` is zero, so a
/// zero-initialized value refuses in a guard that admits only `Unrelated`.
enum class Relation : std::uint8_t
{
    Same,                ///< One directory.
    FirstContainsSecond, ///< The first is a real ancestor of the second.
    SecondContainsFirst, ///< The second is a real ancestor of the first.
    Unrelated,           ///< Neither lies beneath the other.
};

/// Opens preopened directories, the roots every other descriptor descends from.
class IDescriptorFileSystem
{
  public:
    IDescriptorFileSystem() = default;
    IDescriptorFileSystem(IDescriptorFileSystem const&) = delete;
    IDescriptorFileSystem(IDescriptorFileSystem&&) = delete;
    IDescriptorFileSystem& operator=(IDescriptorFileSystem const&) = delete;
    IDescriptorFileSystem& operator=(IDescriptorFileSystem&&) = delete;
    virtual ~IDescriptorFileSystem() = default;

    /// Opens @p hostDirectory as a root. The host chose it, so it is not held to a @c PathPolicy.
    /// @param hostDirectory The directory, in the host's own terms.
    /// @param flags The rights the root carries; `Write` is `IsDirectory`, as on any directory.
    /// @return The root descriptor.
    [[nodiscard]] virtual async::Task<FsResult<std::unique_ptr<IDescriptor>>> openPreopen(
        std::filesystem::path hostDirectory, DescriptorFlags flags) = 0;

    /// Decides whether @p first is, contains or lies beneath @p second on the opened handles: each
    /// one's real parents are walked up to the root and compared by identity (device and inode;
    /// volume serial and file id on Windows), never by path text. So a component of the path either
    /// was opened by that is a symbolic link, a junction or a mount of a whole file system changes
    /// nothing, and nor does a rename after the open: what counts is where the object is now. A
    /// mount that shows only a subtree of its file system -- a bind mount of a subdirectory, a btrfs
    /// subvolume, a nullfs mount -- hides the directories of that file system above the subtree
    /// from a walk that leaves it; a macOS firmlink (`/Users`) leaves the Data volume below its
    /// root and skips that volume's directories above it and every directory on its mount point's
    /// path (`/System`, `/System/Volumes`), which are on the system volume. A containment the walks
    /// meet is always real; an `Unrelated` stands where the other directory is none of what was
    /// skipped (known by identity, or `Unsupported` where it cannot be read), and is on the mount
    /// left, on another file system, or, where Linux's mountinfo shows it, beneath a part of the
    /// file system that is not above the hidden subtree.
    /// A file system that may show another's directories as its own -- overlay, any FUSE file
    /// system, nullfs, unionfs, a network share -- decides only on its own mount: a walk that leaves
    /// one, or a directory on one, is otherwise `Unsupported`, sshfs included. So is any of these
    /// facts a system cannot read: macOS and the BSDs say nowhere which subtree a mount shows.
    /// A walk up never meets a mount BENEATH either directory, which can put the other inside it
    /// (a bind mount of a key directory under a granted one). So on POSIX an `Unrelated` also scans
    /// the mount table: each mount point's path is followed from the root one component at a time,
    /// never through a link, each component told apart by identity before it is entered and
    /// entered only as that directory (a rename between the two is `Busy`); a mount point strictly
    /// beneath either directory is opened, and the answer is `Unsupported` where it opens onto
    /// another mount than the table's, is of the other directory's file system, or either may show
    /// another's. The table is read again after the scan, and a difference is `Busy`.
    /// Each walk is checked after it is done: on POSIX both are walked again, on Windows every
    /// ancestor met is asked for its path again, and any change is `Busy`. A directory moved away and back
    /// between the two passes (ABA) is not seen on POSIX; each chain still is one that existed.
    /// **The answer is the layout when it was asked**: a mount made beneath either directory after
    /// it is not seen, so a host asks at the moment it grants (Isoline's preopen grant, P14a,
    /// evaluates the relation at grant time) and holds the grant to what it decided then.
    ///
    /// Both descriptors are read on the caller's thread, before the returned task first suspends;
    /// neither needs to outlive the task.
    /// @param first A directory of this file system.
    /// @param second A directory of this file system.
    /// @return How they relate; `NotDirectory` where either is not a directory; `CrossDevice` for a
    ///         descriptor of another file system (D11); `Unsupported` where the file system cannot
    ///         give stable identities or a mount of a subtree may hide one from the other (fail
    ///         closed); `Loop` past @c NativeFileSystemOptions::maxAncestorDepth parents; `Busy`
    ///         where a rename during the walk could have changed the answer; or what refused opening
    ///         a parent (`Access` on one the user may not search).
    [[nodiscard]] virtual async::Task<FsResult<Relation>> relation(IDescriptor const& first,
                                                                   IDescriptor const& second) = 0;
};

} // namespace core::fs
