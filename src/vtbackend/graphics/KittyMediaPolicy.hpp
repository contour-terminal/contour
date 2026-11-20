// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/graphics/KittyGraphics.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
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

/// What a terminal may read on behalf of a kitty graphics command. Supplied at construction by the
/// composition root, which owns the platform's answers (the temp roots, the user id).
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
    /// The user a readable file must be owned by: the reading process's own.
    uint32_t userId = 0;
    /// The largest file read, in bytes.
    size_t sizeLimit = 0;
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
    NotTemporaryName,  ///< A `t=t` file whose name lacks `tty-graphics-protocol`.
    NotRegularFile,    ///< The opened file is a directory, FIFO, device, socket...
    WrongOwner,        ///< The opened file belongs to another user.
    MultipleLinks,     ///< The opened file has more than one hard link.
    TooLarge,          ///< The opened file exceeds the size limit.
    Unreadable,        ///< It does not exist, vanished, was swapped, or reading it failed.
};

/// The one reply every refused file medium gets, whatever the reason: inside the allowed roots it still
/// tells whether a named file exists and fits, outside them nothing is learnt.
inline constexpr std::string_view MediaRefusalReply = "EBADF:file not readable";

/// What kind of file an opened descriptor refers to, as far as the policy cares.
enum class FileKind : uint8_t
{
    Other = 0,   ///< Anything but a regular file.
    Regular = 1, ///< A regular file.
};

/// The attributes of an opened file (its `fstat`), as the policy judges them.
struct OpenedFile
{
    FileKind kind = FileKind::Other;
    uint32_t ownerId = 0;
    uint64_t linkCount = 0;
    uint64_t size = 0;
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

/// Judges an opened file's attributes against the policy.
/// @param policy The terminal's media policy.
/// @param file What `fstat` reported of the opened descriptor.
/// @return Nothing when the file may be read; the refusal otherwise.
[[nodiscard]] std::expected<void, MediaRefusal> checkOpenedFile(MediaPolicy const& policy,
                                                                OpenedFile const& file) noexcept;

/// Decides whether a file that was read is deleted: only a `t=t` file the policy accepts, which lies inside a
/// temp root, never one inside an allow-list root.
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
/// @param processName The process name as the PTY reports it; only its last path component counts.
/// @return Whether it names a remote-shell client.
[[nodiscard]] bool isRemoteShellClient(std::string_view processName) noexcept;

} // namespace vtbackend::kitty_graphics
