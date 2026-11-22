// SPDX-License-Identifier: Apache-2.0
#include <vtextension/FileLocker.hpp>

#ifndef _WIN32

    #include <sys/file.h>

    #include <cerrno>
    #include <memory>

    #include <fcntl.h>
    #include <unistd.h>

namespace vtextension
{

namespace
{
    /// A lock file held open; the descriptor's flock() is the lock.
    class DescriptorLock final: public FileLock
    {
      public:
        explicit DescriptorLock(int fd) noexcept: _fd { fd } {}
        DescriptorLock(DescriptorLock const&) = delete;
        DescriptorLock(DescriptorLock&&) = delete;
        DescriptorLock& operator=(DescriptorLock const&) = delete;
        DescriptorLock& operator=(DescriptorLock&&) = delete;
        ~DescriptorLock() override { ::close(_fd); }

      private:
        int _fd;
    };
} // namespace

std::expected<std::unique_ptr<FileLock>, LockError> NativeFileLocker::tryLockExclusive(
    std::filesystem::path const& lockFile) const
{
    // O_NOFOLLOW: a symbolic link planted as the lock file fails (ELOOP) rather than being followed.
    auto const fd = ::open(lockFile.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd == -1)
        return std::unexpected(LockError::Io);
    // flock() locks the open file description, so a second open() in this very process is refused as well.
    if (::flock(fd, LOCK_EX | LOCK_NB) == -1)
    {
        auto const reason = errno;
        ::close(fd);
        return std::unexpected(reason == EWOULDBLOCK ? LockError::Held : LockError::Io);
    }
    return std::make_unique<DescriptorLock>(fd);
}

} // namespace vtextension

#endif
