// SPDX-License-Identifier: Apache-2.0
#include <vtextension/FileLocker.hpp>

#ifdef _WIN32

    #include <memory>

    #include <windows.h>

namespace vtextension
{

namespace
{
    /// A lock file held open; the handle's LockFileEx() range is the lock.
    class HandleLock final: public FileLock
    {
      public:
        explicit HandleLock(HANDLE handle) noexcept: _handle { handle } {}
        HandleLock(HandleLock const&) = delete;
        HandleLock(HandleLock&&) = delete;
        HandleLock& operator=(HandleLock const&) = delete;
        HandleLock& operator=(HandleLock&&) = delete;
        ~HandleLock() override { ::CloseHandle(_handle); } // Closing releases the range.

      private:
        HANDLE _handle;
    };
} // namespace

std::expected<std::unique_ptr<FileLock>, LockError> NativeFileLocker::tryLockExclusive(
    std::filesystem::path const& lockFile) const
{
    // Not inheritable (no SECURITY_ATTRIBUTES), so no child carries it; a reparse point is opened as itself
    // and then refused, never followed.
    auto* const handle = ::CreateFileW(lockFile.c_str(),
                                       GENERIC_READ | GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr,
                                       OPEN_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                       nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return std::unexpected(LockError::Io);
    auto attributes = FILE_ATTRIBUTE_TAG_INFO {};
    if (::GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) == FALSE
        || (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        ::CloseHandle(handle);
        return std::unexpected(LockError::Io);
    }
    auto overlapped = OVERLAPPED {};
    if (::LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlapped)
        == FALSE)
    {
        auto const reason = ::GetLastError();
        ::CloseHandle(handle);
        return std::unexpected(reason == ERROR_LOCK_VIOLATION ? LockError::Held : LockError::Io);
    }
    return std::make_unique<HandleLock>(handle);
}

} // namespace vtextension

#endif
