// SPDX-License-Identifier: Apache-2.0
#ifndef _WIN32

    #include <sys/file.h>

    #include <cerrno>
    #include <memory>

    #include <fcntl.h>
    #include <unistd.h>

    #include <vthost/Daemon.hpp>
    #include <vthost/DaemonSpawn.hpp>

namespace vthost
{

namespace
{
    /// A lock file held open; the descriptor's flock() is the lock.
    class DescriptorSpawnLock final: public DaemonSpawnLock
    {
      public:
        explicit DescriptorSpawnLock(int fd) noexcept: _fd { fd } {}
        DescriptorSpawnLock(DescriptorSpawnLock const&) = delete;
        DescriptorSpawnLock(DescriptorSpawnLock&&) = delete;
        DescriptorSpawnLock& operator=(DescriptorSpawnLock const&) = delete;
        DescriptorSpawnLock& operator=(DescriptorSpawnLock&&) = delete;
        ~DescriptorSpawnLock() override { ::close(_fd); }

      private:
        int _fd;
    };
} // namespace

std::expected<std::unique_ptr<DaemonSpawnLock>, SpawnLockError> tryLockDaemonSpawn(
    std::filesystem::path const& lockFile)
{
    // Its directory is the socket's, made owner-only as the daemon's listener wants it.
    if (!prepareSocketDirectory(lockFile))
        return std::unexpected(SpawnLockError::Io);
    // O_NOFOLLOW: a link planted as the lock file is refused, not followed. O_CLOEXEC: the daemon spawned
    // under the lock must not carry it, or it would hold every later client's start for its whole life.
    auto const fd = ::open(lockFile.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd == -1)
        return std::unexpected(SpawnLockError::Io);
    auto lock = std::make_unique<DescriptorSpawnLock>(fd);
    // flock() locks the open file description, so a second open() in this very process is refused as well.
    auto result = ::flock(fd, LOCK_EX | LOCK_NB);
    while (result == -1 && errno == EINTR)
        result = ::flock(fd, LOCK_EX | LOCK_NB);
    if (result == -1)
        return std::unexpected(errno == EWOULDBLOCK ? SpawnLockError::Held : SpawnLockError::Io);
    return lock;
}

} // namespace vthost

#endif
