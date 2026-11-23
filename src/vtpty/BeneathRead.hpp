// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/DirectoryHandle.hpp>
#include <vtpty/OwnedFd.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>

// A regular file read beneath a directory handle, one component at a time, following no link and never
// blocking: the read an extension's `file-read` is served by, confined to the directory it was granted.
namespace vtpty
{

/// Why a file beneath a directory was not read.
enum class BeneathReadError : uint8_t
{
    DirectoryMissing, ///< A directory on the path does not exist.
    NotRegular,       ///< The path names no regular file: nothing, a directory, a pipe, a device, a socket.
    Unreadable,       ///< The file or a directory on the way may not be opened or read.
    TooLarge,         ///< More than the limit was read.
    Escapes,          ///< The path leaves the directory: a link, a second name, another device, `..`.
    Unsupported,      ///< This platform has no reads beneath a directory handle.
    Fenced,           ///< A directory on the path is one the caller fenced off: another root's.
};

/// A file, as it was read.
struct BeneathFile
{
    std::string bytes;           ///< Its contents.
    uint64_t modifiedUnixMs = 0; ///< When it was last modified, in milliseconds since the Unix epoch.

    [[nodiscard]] bool operator==(BeneathFile const&) const = default;
};

/// What an entry is, as `fstat` tells it.
enum class FileKind : uint8_t
{
    Other = 0, ///< Anything else: a pipe, a device, a socket.
    Regular,   ///< A regular file.
    Directory, ///< A directory.
    Symlink,   ///< A symbolic link, looked at rather than followed.
};

/// Which inode an entry is: what tells two spellings of one directory apart from two directories.
struct FileIdentity
{
    uint64_t device = 0;    ///< The device it lives on (`st_dev`; Windows: the volume serial number).
    uint64_t inode = 0;     ///< Its inode on that device (`st_ino`; Windows: the file id's lower half).
    uint64_t inodeHigh = 0; ///< The upper half of a 128-bit file id (Windows ReFS); 0 on POSIX and NTFS.

    [[nodiscard]] bool operator==(FileIdentity const&) const = default;
};

/// What `fstat` (Windows: a handle's file information) tells of an entry, as far as a read beneath a
/// directory needs it.
struct FileStatus
{
    FileKind kind = FileKind::Other; ///< What it is.
    FileIdentity identity {};        ///< Which inode it is.
    uint64_t links = 0;     ///< How many names it has (`st_nlink`; Windows: 0 once its deletion is pending).
    uint64_t sizeBytes = 0; ///< Its size when asked (`st_size`; Windows: EndOfFile): a hint, never a bound.
    uint64_t modifiedUnixMs = 0; ///< When it was last modified, in milliseconds since the Unix epoch.
};

/// What an entry beneath a directory is opened as.
enum class BeneathTarget : uint8_t
{
    Directory = 0, ///< A directory on the way: `O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC`.
    File,          ///< The file: `O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC`.
};

/// How a name beneath a directory is resolved.
enum class BeneathResolution : uint8_t
{
    /// One `openat(O_NOFOLLOW)` per component, the device compared after it is opened.
    PerComponent = 0,
    /// Linux's `openat2(RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV | RESOLVE_NO_MAGICLINKS)`: a
    /// mount crossing is refused before the mounted filesystem is asked. PerComponent where the kernel has
    /// no `openat2` (`ENOSYS`), and on every other system.
    Kernel,
};

/// The system calls a read beneath a directory makes: the seam a test injects an errno, a device, a size or
/// an identity through.
class IBeneathSystem
{
  public:
    IBeneathSystem() = default;
    IBeneathSystem(IBeneathSystem const&) = delete;
    IBeneathSystem(IBeneathSystem&&) = delete;
    IBeneathSystem& operator=(IBeneathSystem const&) = delete;
    IBeneathSystem& operator=(IBeneathSystem&&) = delete;
    virtual ~IBeneathSystem() = default;

    /// Opens one entry beneath @p directory, following no link (and, resolved by the kernel, crossing no
    /// mount).
    /// @param directory An open directory.
    /// @param name One component.
    /// @param target What it is opened as.
    /// @return The open entry; or the errno the open failed with.
    [[nodiscard]] virtual std::expected<OwnedFd, int> open(int directory,
                                                           std::string const& name,
                                                           BeneathTarget target) = 0;

    /// Looks at one entry beneath @p directory without opening it for reading and without following it
    /// (resolved by the kernel: an `O_PATH` descriptor, so a mount point is refused, not asked).
    /// @param directory An open directory.
    /// @param name One component.
    /// @return What it is (a link is FileKind::Symlink); or the errno the look failed with.
    [[nodiscard]] virtual std::expected<FileStatus, int> statusAt(int directory, std::string const& name) = 0;

    /// @param descriptor An open descriptor.
    /// @return What it is; or the errno `fstat` failed with.
    [[nodiscard]] virtual std::expected<FileStatus, int> status(int descriptor) = 0;
};

/// @param resolution How names are resolved.
/// @return The system calls of this machine. Windows: every call fails with `ENOSYS`.
[[nodiscard]] std::unique_ptr<IBeneathSystem> makeBeneathSystem(BeneathResolution resolution);

/// @return The system calls of this machine, resolved by the kernel where it can (BeneathResolution::Kernel).
[[nodiscard]] IBeneathSystem& nativeBeneathSystem() noexcept;

/// Reads a regular file beneath an open directory. Non-copyable.
class IBeneathReader
{
  public:
    IBeneathReader() = default;
    IBeneathReader(IBeneathReader const&) = delete;
    IBeneathReader(IBeneathReader&&) = delete;
    IBeneathReader& operator=(IBeneathReader const&) = delete;
    IBeneathReader& operator=(IBeneathReader&&) = delete;
    virtual ~IBeneathReader() = default;

    /// Walks @p components beneath @p root, each directory opened as BeneathTarget::Directory beneath the one
    /// above it and refused when it is one of @p fenced or on another device than the root's. The last is
    /// looked at first (IBeneathSystem::statusAt) and opened only when it is a regular file, as
    /// BeneathTarget::File; its descriptor is then `fstat`ed for a regular file with one name
    /// (`st_nlink == 1`) on the root's device. At most @p maxBytes + 1 bytes are read: more than @p maxBytes
    /// is `TooLarge`, whatever `fstat`'s size said.
    /// @param root The directory to read beneath; the path is never resolved again from its name.
    /// @param components The path below @p root, one component each: none empty, `.`, `..`, or holding a
    ///        `/` or a NUL (any such is `Escapes`).
    /// @param maxBytes The most the file may hold.
    /// @param fenced Directories the walk may not enter, by identity (`Fenced`): another root's, however the
    ///        path spells them.
    /// @return The file's bytes and modification time; or why it was not read.
    [[nodiscard]] virtual std::expected<BeneathFile, BeneathReadError> read(
        DirectoryHandle const& root,
        std::span<std::string_view const> components,
        std::size_t maxBytes,
        std::span<FileIdentity const> fenced) = 0;

    /// Walks to a directory beneath @p root as read() walks to the file's: each component opened by the one
    /// above it, following no link, on the root's device (on Linux crossing no mount, so a mount point's
    /// filesystem is never asked).
    /// @param root An open directory.
    /// @param components The directory below @p root, one component each; none for @p root itself.
    /// @return Which inode it is; or why it could not be reached (`Unsupported` where there are no
    ///         directory handles).
    [[nodiscard]] virtual std::expected<FileIdentity, BeneathReadError> identityOf(
        DirectoryHandle const& root, std::span<std::string_view const> components) = 0;
};

/// @param system The system calls the reader makes; outlives the reader.
/// @return A reader over this machine's filesystem through @p system; Windows: one that answers
///         `Unsupported`.
[[nodiscard]] std::unique_ptr<IBeneathReader> makeBeneathReader(IBeneathSystem& system);

/// @return The reader over this machine's filesystem, through nativeBeneathSystem(). Windows: every read
///         answers `Unsupported`.
[[nodiscard]] IBeneathReader& nativeBeneathReader() noexcept;

} // namespace vtpty
