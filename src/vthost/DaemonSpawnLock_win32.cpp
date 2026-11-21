// SPDX-License-Identifier: Apache-2.0
#ifdef _WIN32

    #include <memory>
    #include <system_error>

    #include <windows.h>

    #include <vthost/DaemonSpawn.hpp>

namespace vthost
{

namespace
{
    /// A lock file held open; the handle's LockFileEx() range is the lock.
    class HandleSpawnLock final: public DaemonSpawnLock
    {
      public:
        explicit HandleSpawnLock(HANDLE handle) noexcept: _handle { handle } {}
        HandleSpawnLock(HandleSpawnLock const&) = delete;
        HandleSpawnLock(HandleSpawnLock&&) = delete;
        HandleSpawnLock& operator=(HandleSpawnLock const&) = delete;
        HandleSpawnLock& operator=(HandleSpawnLock&&) = delete;
        ~HandleSpawnLock() override { ::CloseHandle(_handle); } // Closing releases the range.

      private:
        HANDLE _handle;
    };
} // namespace

std::expected<std::unique_ptr<DaemonSpawnLock>, SpawnLockError> tryLockDaemonSpawn(
    std::filesystem::path const& lockFile)
{
    auto error = std::error_code {};
    std::filesystem::create_directories(lockFile.parent_path(), error);
    // Not inheritable (no SECURITY_ATTRIBUTES), so the daemon spawned under it does not carry it; a reparse
    // point is opened as itself and then refused, never followed.
    auto* const handle = ::CreateFileW(lockFile.c_str(),
                                       GENERIC_READ | GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr,
                                       OPEN_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                       nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return std::unexpected(SpawnLockError::Io);
    auto lock = std::make_unique<HandleSpawnLock>(handle);
    auto attributes = FILE_ATTRIBUTE_TAG_INFO {};
    if (::GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) == FALSE
        || (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return std::unexpected(SpawnLockError::Io);
    auto overlapped = OVERLAPPED {};
    if (::LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlapped)
        == FALSE)
        return std::unexpected(::GetLastError() == ERROR_LOCK_VIOLATION ? SpawnLockError::Held
                                                                        : SpawnLockError::Io);
    return lock;
}

} // namespace vthost

#endif
