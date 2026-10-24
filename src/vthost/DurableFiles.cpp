// SPDX-License-Identifier: Apache-2.0
#ifndef _WIN32

    #include <vthost/DurableFiles.hpp>

    #include <cerrno>
    #include <format>
    #include <system_error>

    #include <fcntl.h>
    #include <unistd.h>

namespace vthost
{

namespace
{
    [[nodiscard]] std::string lastError()
    {
        return std::system_category().message(errno);
    }

    /// Opens @p path read-only and flushes it: fsync() accepts a read-only descriptor for a file and for a
    /// directory alike, and flushes the inode, whichever descriptor wrote it.
    [[nodiscard]] std::expected<void, std::string> syncPath(std::filesystem::path const& path, int extraFlags)
    {
        auto const fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | extraFlags);
        if (fd < 0)
            return std::unexpected(std::format("cannot open {}: {}", path.string(), lastError()));
    #ifdef __APPLE__
        // macOS's fsync() reaches the drive but not past its cache; F_FULLFSYNC does, where the file system
        // supports it.
        auto const synced = ::fcntl(fd, F_FULLFSYNC) == 0 || ::fsync(fd) == 0;
    #else
        auto const synced = ::fsync(fd) == 0;
    #endif
        auto const error = synced ? std::string {} : lastError();
        ::close(fd);
        if (!synced)
            return std::unexpected(std::format("cannot sync {}: {}", path.string(), error));
        return {};
    }
} // namespace

std::expected<void, std::string> NativeDurableFiles::createExclusive(std::filesystem::path const& path) const
{
    auto const fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0)
        return std::unexpected(std::format("cannot create {}: {}", path.string(), lastError()));
    ::close(fd);
    return {};
}

std::expected<void, std::string> NativeDurableFiles::syncFile(std::filesystem::path const& path) const
{
    return syncPath(path, 0);
}

std::expected<void, std::string> NativeDurableFiles::replace(std::filesystem::path const& from,
                                                             std::filesystem::path const& to) const
{
    auto error = std::error_code {};
    std::filesystem::rename(from, to, error);
    if (error)
        return std::unexpected(
            std::format("cannot rename {} to {}: {}", from.string(), to.string(), error.message()));
    return {};
}

std::expected<void, std::string> NativeDurableFiles::syncDirectory(
    std::filesystem::path const& directory) const
{
    return syncPath(directory, O_DIRECTORY);
}

} // namespace vthost

#endif
