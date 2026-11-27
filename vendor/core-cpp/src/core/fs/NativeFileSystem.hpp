// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The operating system's @c IDescriptorFileSystem: descriptors over real files and directories,
/// every system call made on an injected blocking executor.

#include <core/Random.hpp>
#include <core/async/IExecutor.hpp>
#include <core/fs/Descriptor.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/FsExport.hpp>
#include <core/fs/PathPolicy.hpp>
#include <core/platform/Clock.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace core::fs
{

/// How many symbolic links one resolution follows before it answers `Loop`, as Linux's own limit.
inline constexpr std::size_t DefaultMaxSymlinkHops = 40;

/// How many parents one @c IDescriptorFileSystem::relation walks from each directory before it
/// answers `Loop`: deeper than any real tree, low enough that a crafted one costs a bounded walk.
inline constexpr std::size_t DefaultMaxAncestorDepth = 4'096;

/// How a path is resolved beneath the descriptor it is relative to.
enum class BeneathStrategy : std::uint8_t
{
    Best,          ///< What this system does best: `openat2(RESOLVE_BENEATH)` where Linux has it, else
                   ///< @c ComponentWalk.
    ComponentWalk, ///< One component at a time, over held directory handles, on every system.
};

/// What a native file system is built with. Fixed at construction.
struct NativeFileSystemOptions
{
    std::size_t maxSymlinkHops = DefaultMaxSymlinkHops;  ///< How many links one resolution follows before
                                                         ///< it answers `Loop`. Under `Best` on Linux
                                                         ///< the kernel counts the links before the
                                                         ///< final component against its own 40, so a
                                                         ///< lower limit makes `Best` walk instead.
    BeneathStrategy strategy = BeneathStrategy::Best;    ///< How paths are resolved.
    std::size_t maxReadChunk = std::size_t { 1 } << 20U; ///< The most one read returns, whatever a
                                                         ///< caller asks for.
    PathPolicy const* policy = nullptr; ///< The name rules; null is @c nativePathPolicy(). Must
                                        ///< outlive the file system and its descriptors.
    std::size_t maxAncestorDepth = DefaultMaxAncestorDepth; ///< Parents one relation() walks from each
                                                            ///< directory before it answers `Loop`.
};

/// Builds the operating system's file system.
///
/// Every operation of it and of its descriptors runs its system calls on @p blocking, through
/// @c async::offload, and resumes on @p home. Each such call holds the native handle it uses by
/// shared ownership, so a handle is closed only once its descriptor and every call still running
/// on it are gone: a descriptor dropped while a call is parked never lets that call reach another
/// file that reused the handle's number. The close itself is queued on @p blocking too, wherever
/// the last owner lets go.
///
/// Teardown: drain or stop @p blocking before @p home is destroyed. A close still queued when
/// @p blocking is destroyed runs there and then.
///
/// A path of the `*At` operations is resolved beneath the descriptor, never by the kernel from
/// anywhere else: `..` above it, an absolute path or link target and a link that leaves it are
/// `NotPermitted`, whichever @c BeneathStrategy resolves it. The final component is never resolved
/// by the kernel: every final call is made without following, on the directory that holds it. A
/// name holding what the kernel would split on (`/`, and `\` and `:` on Windows) is `Invalid`
/// whatever @c NativeFileSystemOptions::policy allows.
///
/// On Windows:
/// - A junction, a mount point and every other reparse point that names another object by an
///   absolute path is a link that is never followed (`NotPermitted`); a symbolic link is followed
///   only where its target is relative. `readlinkAt` of such a link is `NotPermitted` too, where
///   POSIX returns an absolute target's text: no host path is handed out.
/// - `symlinkAt` makes a directory link where the target names a directory beneath the descriptor
///   (or ends in a separator, `.` or `..`), so that the system follows it too. It writes the link's
///   reparse point itself, which takes the symbolic-link privilege
///   (`SeCreateSymbolicLinkPrivilege`), and is `NotPermitted` without it.
/// - A directory link is a directory to the system: `linkAt` of one and `renameAt` of a file over
///   one are `IsDirectory`, where POSIX links and replaces it as any file.
/// - `renameAt` of a directory over an existing empty directory is refused by the system, where
///   POSIX replaces it.
/// - A reparse point that names nothing else (a cloud placeholder, a deduplicated file) opens
///   through its filter as the file it is, by the handle that was checked and refusing any reparse
///   the object manager would follow. A filter that serves its point by redirecting the open
///   elsewhere (a container's placeholders) is therefore refused (`NotPermitted`): safety first.
///   Before Windows 10 1803, which cannot refuse that reparse, such a point does not open at all.
/// - Every open identifies the process to a server it reaches (a named pipe, a share) at most at
///   identification level, so no server can impersonate it.
/// - `relation` reads each directory's final path by volume GUID and opens every ancestor on it, and
///   every folder its volume is mounted on, for its file id alone. Identities count only on a local
///   NTFS or ReFS volume: FAT, exFAT, any other file system and every share (whatever file system
///   the server reports) answer `Unsupported`. A rename that moved either directory or any ancestor
///   met during the walk answers `Busy`.
///
/// @param home Where every operation resumes. Must outlive the file system and its descriptors.
/// @param blocking Where every system call runs. Must outlive them too.
/// @param wallClock What `SetToNow` reads where the system takes an explicit time (Windows). Must
///        outlive them too.
/// @param random Keys metadata hashes with a secret drawn once, here, so a hash reveals none of
///        the file identity, size or times it is computed from.
/// @param options What the file system is built with.
/// @return The file system; `Invalid` for a `maxReadChunk` of 0.
[[nodiscard]] CORE_FS_API FsResult<std::unique_ptr<IDescriptorFileSystem>> makeNativeDescriptorFileSystem(
    async::IExecutor& home,
    async::IExecutor& blocking,
    platform::IWallClock const& wallClock,
    IRandom& random,
    NativeFileSystemOptions options = {});

} // namespace core::fs
