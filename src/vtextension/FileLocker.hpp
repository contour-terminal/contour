// SPDX-License-Identifier: Apache-2.0
//
// Exclusive advisory locks on files, behind an interface: core::platform::FileSystem has no locking, and the
// tests take locks without a disk.
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>

namespace vtextension
{

/// Why a lock was not taken.
enum class LockError : uint8_t
{
    Held, ///< Someone else holds it.
    Io,   ///< The lock file could not be opened or locked.
};

/// A lock taken; it is released when this is destroyed.
class FileLock
{
  public:
    FileLock() = default;
    FileLock(FileLock const&) = delete;
    FileLock(FileLock&&) = delete;
    FileLock& operator=(FileLock const&) = delete;
    FileLock& operator=(FileLock&&) = delete;
    virtual ~FileLock() = default;
};

/// Takes exclusive advisory locks.
class IFileLocker
{
  public:
    IFileLocker() = default;
    IFileLocker(IFileLocker const&) = delete;
    IFileLocker(IFileLocker&&) = delete;
    IFileLocker& operator=(IFileLocker const&) = delete;
    IFileLocker& operator=(IFileLocker&&) = delete;
    virtual ~IFileLocker() = default;

    /// Takes the exclusive lock of @p lockFile without waiting, creating the file (owner-only) if missing.
    /// @param lockFile The file the lock is on; a symbolic link there is refused, never followed.
    /// @return The lock, held until it is destroyed; `Held` if anyone holds it, even this process through
    ///         another call; `Io` otherwise.
    [[nodiscard]] virtual std::expected<std::unique_ptr<FileLock>, LockError> tryLockExclusive(
        std::filesystem::path const& lockFile) const = 0;
};

/// The operating system's locks: on POSIX, the file opened `O_NOFOLLOW | O_CLOEXEC` and `flock(LOCK_EX |
/// LOCK_NB)` (on NFS, Linux maps flock() to fcntl() byte-range locks); on Windows, a handle no child inherits
/// and `LockFileEx` with `LOCKFILE_FAIL_IMMEDIATELY`.
class NativeFileLocker final: public IFileLocker
{
  public:
    [[nodiscard]] std::expected<std::unique_ptr<FileLock>, LockError> tryLockExclusive(
        std::filesystem::path const& lockFile) const override;
};

} // namespace vtextension
