// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace core::platform
{

/// Platform-independent file entry returned by FileInfoProvider.
///
/// Fields beyond @c name carry standard `stat(2)` metadata. The block/identity
/// fields (@c blocks, @c dev, @c ino) enable true disk-usage accounting, hardlink
/// deduplication, and cross-filesystem detection; platforms that cannot supply
/// them leave the documented sentinel defaults (@c blocks = -1, @c dev/@c ino = 0).
struct FileEntry
{
    std::string name {};          ///< File name (not full path).
    int64_t size = 0;             ///< Apparent size in bytes (st_size).
    int64_t blocks = -1;          ///< Allocated 512-byte blocks (st_blocks); -1 if unknown.
    int64_t mode = 0;             ///< Permission bits (e.g. 0755).
    int64_t mtime = 0;            ///< Last modification time as epoch seconds.
    uint64_t dev = 0;             ///< Device id (st_dev) for cross-filesystem detection; 0 if unknown.
    uint64_t ino = 0;             ///< Inode number (st_ino) for hardlink dedup; 0 if unknown.
    bool isDir = false;           ///< Whether this entry is a directory.
    bool isSymlink = false;       ///< Whether this entry is a symbolic link (from lstat, not followed).
    std::string symlinkTarget {}; ///< Target path of a symbolic link (verbatim, possibly relative);
                                  ///< empty if not a symlink or if the target could not be read.
    std::string path {};          ///< Absolute path of the entry, forward-slash normalized; empty if
                                  ///< it could not be determined. Consumers that must address the
                                  ///< entry itself (rather than display it) need this, because
                                  ///< @c name alone is meaningless once the listing is passed around.
};

/// Abstract interface for listing directory contents.
///
/// Implementations provide platform-specific directory enumeration.
/// Inject via constructor for testability (the mock in tests, nativeFileInfoProvider() in production).
class FileInfoProvider
{
  public:
    virtual ~FileInfoProvider() = default;

    /// Lists filesystem entries matching the given path.
    /// @param path A directory path (lists contents), a single file path (returns one entry),
    ///             or a glob pattern like "*.md" (returns matching entries in the parent directory).
    /// @return A vector of FileEntry structs, sorted by name.
    [[nodiscard]] virtual std::vector<FileEntry> listDirectory(std::string const& path) const = 0;
};

/// @brief Creates this operating system's own FileInfoProvider, for a composition root.
///
/// On Windows it is the provider over `std::filesystem`, which reports the read-only flag as the
/// permissions and has no block count, device or inode (the documented sentinels). On Linux,
/// macOS and the BSDs it is the POSIX provider, which describes each entry with `lstat(2)`: every
/// field, symlinks as links (not followed) with their targets, and the block count, device and
/// inode. Under Emscripten it is the POSIX provider too, over Emscripten's virtual filesystem
/// (in memory unless the program mounts another); its `readlink()` resolves a relative symlink
/// target against the link's directory (3.1.56 at least), so @c FileEntry::symlinkTarget is that
/// absolute path there. Both implementations are private (`posix/`, `windows/`), so this is the
/// way to reach them.
///
/// @return The provider, owned by the caller.
[[nodiscard]] std::unique_ptr<FileInfoProvider> nativeFileInfoProvider();

} // namespace core::platform
