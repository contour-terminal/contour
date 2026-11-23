// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/graphics/KittyGraphics.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

/// The policy that decides which local files a kitty graphics command may name (`t=f`, `t=t`).
///
/// Pure: it judges paths and file attributes it is handed, and touches no filesystem. The platform adapter
/// (KittyMediumSource) asks it at each step of a read, in the order the adapter documents.
namespace vtbackend::kitty_graphics
{

/// Whether a terminal reads files a kitty graphics command names at all.
enum class FileTransmission : uint8_t
{
    Disabled = 0, ///< Every file medium is refused.
    Enabled = 1,  ///< `t=f` and `t=t` are read, within the rest of the policy.
};

/// How a platform's filesystems compare path components.
enum class PathCase : uint8_t
{
    Sensitive = 0,   ///< Byte for byte (Linux, macOS as the policy judges it).
    Insensitive = 1, ///< ASCII letters case-folded (Windows). A component differing only in the case of a
                     ///< non-ASCII letter does not match, so such a spelling is refused rather than admitted.
};

/// What a terminal may read on behalf of a kitty graphics command. Supplied at construction by the
/// composition root, which owns the platform's answers (the temp roots, how paths compare).
struct MediaPolicy
{
    /// Whether file media are read at all.
    FileTransmission transmission = FileTransmission::Disabled;
    /// Canonical temp directories: the only roots of `t=t`, and roots of `t=f` too. Deletion after a read
    /// happens only inside these.
    std::vector<std::filesystem::path> temporaryRoots;
    /// Further roots of `t=t` alone (Linux `/dev/shm`, as kitty allows): never read by `t=f`, and exempt from
    /// the system-tree refusal for `t=t` only. Deletion after a read happens inside these too.
    std::vector<std::filesystem::path> temporaryFileOnlyRoots;
    /// Canonical further roots `t=f` may read below (default `~/.claude/plugins/data`). Never deleted from.
    std::vector<std::filesystem::path> allowedRoots;
    /// The largest file read, in bytes.
    size_t sizeLimit = 0;
    /// How path components compare against the roots: the platform's own rule.
    PathCase pathCase = PathCase::Sensitive;
};

/// Why a file medium was refused. Every refusal reaches the application as the same MediaRefusalReply;
/// the reason is for tests and logs only.
enum class MediaRefusal : uint8_t
{
    Disabled = 0,      ///< The policy reads no file media, or the bytes did not come from a local process.
    UnsupportedMedium, ///< `t=s` (shared memory), which is never read, or a medium that names no file.
    OutsideRoots,      ///< The canonical path lies under no allowed root.
    HiddenComponent,   ///< A path component below the root starts with `.`.
    SystemPath,        ///< Under `/proc`, `/sys` or `/dev` (but `/dev/shm` for `t=t`).
    NotRegularFile,    ///< The opened file is a directory, FIFO, device, socket...
    WrongOwner,        ///< The opened file belongs to another user.
    MultipleLinks,     ///< The opened file has more than one hard link.
    TooLarge,          ///< The opened file exceeds the size limit.
    Unreadable,        ///< It does not exist, vanished, was swapped, or reading it failed.
    SymbolicLink,      ///< A component below the root is a symbolic link, never followed.
    PathTooLong,       ///< The path is longer than MaxFilePathLength.
    ReservedName,     ///< Windows: a component it reads as other than a name (a stream, a device, `..`, ...).
    UnsupportedDrive, ///< Windows: not on a fixed or RAM disk (a share, removable or optical drive, a
                      ///< device).
};

/// The longest path, in bytes, a file medium may name: Linux's PATH_MAX less its terminating NUL. A longer
/// one names no file any platform could open, and is refused before it is looked at.
inline constexpr auto MaxFilePathLength = size_t { 4095 };

/// The one reply every refused file medium gets, whatever the reason: inside the allowed roots it still
/// tells whether a named file exists and fits, outside them nothing is learnt.
inline constexpr std::string_view MediaRefusalReply = "EBADF:file not readable";

/// What kind of file an opened descriptor refers to, as far as the policy cares.
enum class FileKind : uint8_t
{
    Other = 0,   ///< Anything but a regular file.
    Regular = 1, ///< A regular file.
};

/// Who owns an opened file, as the medium source decides it from the identity it was constructed with.
enum class FileOwner : uint8_t
{
    Other = 0,  ///< Anyone but the reader: refused.
    Reader = 1, ///< The user the terminal reads as.
};

/// The attributes of an opened file (its `fstat`), as the policy judges them.
struct OpenedFile
{
    FileKind kind = FileKind::Other;
    FileOwner owner = FileOwner::Other;
    uint64_t linkCount = 0;
    uint64_t size = 0;
};

/// A requested path the policy accepted as spelled, and where a reader walks it from.
struct RequestedPath
{
    std::filesystem::path path;                   ///< Lexically normal, absolute.
    std::filesystem::path root;                   ///< Its enclosingRoot().
    std::vector<std::filesystem::path> belowRoot; ///< Its components below @c root; never empty.
};

/// Whether a file is deleted once it was read.
enum class DeletionAfterRead : uint8_t
{
    Keep = 0,   ///< Left where it is.
    Delete = 1, ///< Unlinked: a `t=t` file inside a temp root.
};

/// Refuses a request before any filesystem is touched: file media disabled, or a medium that is not a file.
/// @param policy The terminal's media policy.
/// @param medium The command's `t=`.
/// @return Nothing when a file of this medium may be looked at; the refusal otherwise.
[[nodiscard]] std::expected<void, MediaRefusal> checkMedium(MediaPolicy const& policy,
                                                            Medium medium) noexcept;

/// Refuses a request before any filesystem is touched: checkMedium(), then a path longer than
/// MaxFilePathLength.
/// @param policy The terminal's media policy.
/// @param medium The command's `t=`.
/// @param path The path the command names, as it arrived.
/// @return Nothing when the path may be looked at; the refusal otherwise.
[[nodiscard]] std::expected<void, MediaRefusal> checkRequest(MediaPolicy const& policy,
                                                             Medium medium,
                                                             std::string_view path) noexcept;

/// The prelude every reader runs before it touches the filesystem: checkRequest(), a path that is empty or
/// holds a NUL refused, the UTF-8 @p path decoded (never through a narrow code page), made lexically normal,
/// judged by checkPath() as spelled, and its enclosingRoot() found.
/// @param policy The terminal's media policy.
/// @param medium The command's `t=`.
/// @param path The path as the application sent it, UTF-8.
/// @return What to walk; the refusal otherwise.
[[nodiscard]] std::expected<RequestedPath, MediaRefusal> requestedPath(MediaPolicy const& policy,
                                                                       Medium medium,
                                                                       std::string_view path);

/// Judges a path against the policy (checkMedium() first). Asked twice by a reader: of the requested path in
/// its lexically normal form before anything resolves it -- so that no lookup leaves the roots, an automount
/// or a network filesystem say -- and of the canonical path, which is final.
/// @param policy The terminal's media policy.
/// @param realPath The canonical path (symbolic links resolved), or the lexically normal requested one.
/// @param medium The command's `t=`: `t=t` reads below the temp roots only, `t=f` below the allow-list too.
/// @return Nothing when the path may be opened; the refusal otherwise.
[[nodiscard]] std::expected<void, MediaRefusal> checkPath(MediaPolicy const& policy,
                                                          std::filesystem::path const& realPath,
                                                          Medium medium);

/// The root a reader walks down from to reach @p path: the deepest root eligible for @p medium that @p path
/// lies below with no hidden component in between.
/// @param policy The terminal's media policy.
/// @param path A lexically normal, absolute path checkPath() accepted.
/// @param medium The command's `t=`.
/// @return The root, or nullopt when there is none.
[[nodiscard]] std::optional<std::filesystem::path> enclosingRoot(MediaPolicy const& policy,
                                                                 std::filesystem::path const& path,
                                                                 Medium medium);

/// Judges an opened file's attributes against the policy.
/// @param policy The terminal's media policy.
/// @param file What `fstat` reported of the opened descriptor.
/// @return Nothing when the file may be read; the refusal otherwise.
[[nodiscard]] std::expected<void, MediaRefusal> checkOpenedFile(MediaPolicy const& policy,
                                                                OpenedFile const& file) noexcept;

/// Decides whether a file that was read is deleted: only a `t=t` file the policy accepts, which lies inside a
/// temp root, never one inside an allow-list root, and whose full path carries `tty-graphics-protocol`:
/// kitty's rule against deleting files not meant for it. A `t=t` file without the marker is read all the
/// same.
/// @param policy The terminal's media policy.
/// @param realPath The canonical path that was read.
/// @param medium The command's `t=`.
/// @return Whether to unlink @p realPath.
[[nodiscard]] DeletionAfterRead deletionAfterRead(MediaPolicy const& policy,
                                                  std::filesystem::path const& realPath,
                                                  Medium medium);

/// Whether a foreground process is a remote-shell client (`ssh`, `mosh-client`, `telnet`, ...): while one is,
/// the bytes come from another host, whatever the terminal's contexts say, and no local file is read for
/// them.
/// @param processName The process name as the PTY reports it; only its last path component counts (either
///                    separator), compared without a trailing `.exe` and ignoring ASCII case, as Windows
///                    names `ssh.exe` or `SSH.EXE` what POSIX names `ssh`.
/// @return Whether it names a remote-shell client.
[[nodiscard]] bool isRemoteShellClient(std::string_view processName) noexcept;

} // namespace vtbackend::kitty_graphics
