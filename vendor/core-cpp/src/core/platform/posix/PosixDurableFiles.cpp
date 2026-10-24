// SPDX-License-Identifier: Apache-2.0
#include <core/platform/detail/DurableFiles.hpp>

#include <cerrno>
#include <cstdio>

#include <fcntl.h>
#include <unistd.h>

namespace core::platform::detail
{

namespace
{
    /// @return The reason the last system call failed.
    [[nodiscard]] std::error_code lastError() noexcept
    {
        return { errno, std::generic_category() };
    }

    /// @brief Opens @p path read-only and flushes it.
    ///
    /// fsync() accepts a read-only descriptor, for a file and for a directory alike, and flushes the
    /// inode whichever descriptor wrote it.
    ///
    /// @param path The file or directory to flush.
    /// @param openFlags What to open it with besides O_RDONLY and O_CLOEXEC.
    /// @return Nothing, or why it could not be opened or flushed.
    [[nodiscard]] std::expected<void, std::error_code> syncPath(std::filesystem::path const& path,
                                                                int openFlags)
    {
        auto const fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | openFlags);
        if (fd < 0)
            return std::unexpected(lastError());
#ifdef F_FULLFSYNC
        // macOS's fsync() reaches the drive but not past its cache; F_FULLFSYNC does, where the file
        // system supports it, and fsync() is the fallback where it does not.
        auto const synced = ::fcntl(fd, F_FULLFSYNC) == 0 || ::fsync(fd) == 0;
#else
        auto const synced = ::fsync(fd) == 0;
#endif
        auto const error = synced ? std::error_code {} : lastError();
        ::close(fd);
        if (!synced)
            return std::unexpected(error);
        return {};
    }
} // namespace

std::expected<void, std::error_code> createNewFile(std::filesystem::path const& path)
{
    // O_EXCL fails on anything there; with O_NOFOLLOW that includes a symbolic link, dangling or not.
    auto const fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0)
        return std::unexpected(lastError());
    ::close(fd);
    return {};
}

std::expected<void, std::error_code> syncFile(std::filesystem::path const& path)
{
    return syncPath(path, 0);
}

std::expected<void, std::error_code> replaceFile(std::filesystem::path const& from,
                                                 std::filesystem::path const& to)
{
    if (std::rename(from.c_str(), to.c_str()) != 0)
        return std::unexpected(lastError());
    return {};
}

std::expected<void, std::error_code> syncDirectory(std::filesystem::path const& directory)
{
    return syncPath(directory, O_DIRECTORY);
}

} // namespace core::platform::detail
