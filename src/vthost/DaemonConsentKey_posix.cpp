// SPDX-License-Identifier: Apache-2.0
#ifndef _WIN32

    #include <core/Utils.hpp>

    #include <sys/file.h>
    #include <sys/stat.h>

    #include <array>
    #include <cerrno>
    #include <format>
    #include <memory>
    #include <ranges>
    #include <system_error>

    #include <fcntl.h>
    #include <unistd.h>

    #include <vthost/DaemonConsentKey.hpp>
    #include <vthost/FilePrivacy.hpp>

namespace vthost
{

namespace
{
    /// Whether a missing consent directory is made.
    enum class DirectoryCreation : uint8_t
    {
        OpenExisting = 0, ///< A window reads: a missing directory is an error.
        CreateIfMissing,  ///< The daemon writes: it makes one, 0700.
    };

    /// @return Why @p what failed on @p path, with errno's text.
    [[nodiscard]] std::string failureOf(std::string_view what, std::filesystem::path const& path)
    {
        return std::format("cannot {} '{}': {}",
                           what,
                           path.string(),
                           std::error_code { errno, std::generic_category() }.message());
    }

    /// A directory held open; closed as it goes.
    class OpenDirectory
    {
      public:
        explicit OpenDirectory(int fd) noexcept: _fd { fd } {}
        OpenDirectory(OpenDirectory const&) = delete;
        OpenDirectory& operator=(OpenDirectory const&) = delete;
        OpenDirectory(OpenDirectory&& other) noexcept: _fd { std::exchange(other._fd, -1) } {}
        OpenDirectory& operator=(OpenDirectory&&) = delete;
        ~OpenDirectory()
        {
            if (_fd >= 0)
                ::close(_fd);
        }
        [[nodiscard]] int fd() const noexcept { return _fd; }

      private:
        int _fd;
    };

    /// Opens @p directory without following a link, and refuses it unless it is a directory of this user's,
    /// closed to everyone else. The daemon, which makes it, closes it to everyone else itself.
    [[nodiscard]] std::expected<OpenDirectory, std::string> openConsentDirectory(
        std::filesystem::path const& directory, DirectoryCreation creation)
    {
        if (creation == DirectoryCreation::CreateIfMissing)
        {
            auto error = std::error_code {};
            std::filesystem::create_directories(directory.parent_path(), error);
            if (::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST)
                return std::unexpected(failureOf("make", directory));
        }
        auto const fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0)
        {
            if (errno == ELOOP || errno == ENOTDIR)
                return std::unexpected(std::format(
                    "'{}' is not a directory of its own (a link?); it is not used", directory.string()));
            return std::unexpected(failureOf("open", directory));
        }
        auto held = OpenDirectory { fd };
        struct stat status {};
        if (::fstat(fd, &status) != 0)
            return std::unexpected(failureOf("inspect", directory));
        if (!S_ISDIR(status.st_mode))
            return std::unexpected(std::format("'{}' is not a directory", directory.string()));
        if (creation == DirectoryCreation::CreateIfMissing && status.st_uid == ::geteuid()
            && (status.st_mode & 077) != 0 && ::fchmod(fd, 0700) == 0)
            status.st_mode &= ~static_cast<mode_t>(077);
        if (auto checked =
                checkPrivateFile(FileOwnership { .ownerId = status.st_uid,
                                                 .mode = static_cast<std::uint32_t>(status.st_mode & 07777) },
                                 ::geteuid(),
                                 directory);
            !checked)
            return std::unexpected(std::move(checked.error()));
        return held;
    }

    /// Writes all of @p content to @p fd.
    [[nodiscard]] bool writeAll(int fd, std::string_view content) noexcept
    {
        while (!content.empty())
        {
            auto const written = ::write(fd, content.data(), content.size());
            if (written < 0)
            {
                if (errno == EINTR)
                    continue;
                return false;
            }
            content.remove_prefix(static_cast<std::size_t>(written));
        }
        return true;
    }

    /// How often keep() makes its temporary file again when a clean-up removed it before it was held.
    constexpr auto KeepAttempts = 3;

    /// @return flock(@p fd, @p operation), retried across signals.
    [[nodiscard]] int lockRetryingInterrupts(int fd, int operation) noexcept
    {
        auto result = ::flock(fd, operation);
        while (result == -1 && errno == EINTR)
            result = ::flock(fd, operation);
        return result;
    }

    /// @return Whether @p fd still is the file @p name in the directory @p dir: what a clean-up's unlink
    ///         makes untrue.
    [[nodiscard]] bool isFileAt(int fd, int dir, std::string const& name) noexcept
    {
        struct stat opened {};
        struct stat named {};
        return ::fstat(fd, &opened) == 0 && ::fstatat(dir, name.c_str(), &named, AT_SYMLINK_NOFOLLOW) == 0
               && opened.st_dev == named.st_dev && opened.st_ino == named.st_ino;
    }

    /// A key file held open; the descriptor's exclusive flock() is the hold. Opened O_CLOEXEC, so no child
    /// -- nor the image a live upgrade execs, which keeps its key again -- carries it.
    class DescriptorHold final: public ConsentKeyHold
    {
      public:
        explicit DescriptorHold(int fd) noexcept: _fd { fd } {}
        DescriptorHold(DescriptorHold const&) = delete;
        DescriptorHold(DescriptorHold&&) = delete;
        DescriptorHold& operator=(DescriptorHold const&) = delete;
        DescriptorHold& operator=(DescriptorHold&&) = delete;
        ~DescriptorHold() override { ::close(_fd); }

        [[nodiscard]] int fd() const noexcept { return _fd; }

      private:
        int _fd;
    };

    /// The OS's consent directory, reached through a descriptor of it.
    class PosixConsentKeyFiles final: public IConsentKeyFiles
    {
      public:
        [[nodiscard]] std::expected<std::unique_ptr<ConsentKeyHold>, std::string> keep(
            std::filesystem::path const& directory,
            std::string_view name,
            std::string_view content) const override
        {
            auto opened = openConsentDirectory(directory, DirectoryCreation::CreateIfMissing);
            if (!opened)
                return std::unexpected(std::move(opened.error()));
            auto const dir = opened->fd();
            auto const target = std::string { name };
            auto const temporary = target + ".tmp";
            for ([[maybe_unused]] auto const attempt: std::views::iota(0, KeepAttempts))
            {
                // A crash of this run may have left one.
                ::unlinkat(dir, temporary.c_str(), 0);
                auto const fd = ::openat(
                    dir, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
                if (fd < 0)
                    return std::unexpected(failureOf("create", directory / temporary));
                auto hold = std::make_unique<DescriptorHold>(fd);
                // Held before it holds anything: another run's clean-up that got to it first removed it, and
                // it is made again.
                if (lockRetryingInterrupts(fd, LOCK_EX | LOCK_NB) != 0 || !isFileAt(fd, dir, temporary))
                    continue;
                // Private while still empty, so the key never exists within anyone else's reach.
                if (::fchmod(fd, 0600) != 0 || !writeAll(fd, content) || ::fsync(fd) != 0)
                {
                    auto const failure = failureOf("write", directory / temporary);
                    ::unlinkat(dir, temporary.c_str(), 0);
                    return std::unexpected(failure);
                }
                // The hold goes with the file to its name: the lock is the open file's, not the name's.
                if (::renameat(dir, temporary.c_str(), dir, target.c_str()) != 0)
                {
                    auto const failure = failureOf("rename", directory / temporary);
                    ::unlinkat(dir, temporary.c_str(), 0);
                    return std::unexpected(failure);
                }
                ::fsync(dir);
                return hold;
            }
            return std::unexpected(
                std::format("'{}' kept being removed as it was made", (directory / temporary).string()));
        }

        [[nodiscard]] std::expected<std::string, std::string> read(std::filesystem::path const& directory,
                                                                   std::string_view name) const override
        {
            auto opened = openConsentDirectory(directory, DirectoryCreation::OpenExisting);
            if (!opened)
                return std::unexpected(std::move(opened.error()));
            auto const file = directory / name;
            auto const fd =
                ::openat(opened->fd(), std::string { name }.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0)
                return std::unexpected(failureOf("open", file));
            auto const closed = core::Finally { [fd] { ::close(fd); } };
            struct stat status {};
            if (::fstat(fd, &status) != 0)
                return std::unexpected(failureOf("inspect", file));
            if (!S_ISREG(status.st_mode))
                return std::unexpected(std::format("'{}' is not a regular file", file.string()));
            if (auto checked = checkPrivateFile(
                    FileOwnership { .ownerId = status.st_uid,
                                    .mode = static_cast<std::uint32_t>(status.st_mode & 07777) },
                    ::geteuid(),
                    file);
                !checked)
                return std::unexpected(std::move(checked.error()));
            auto content = std::string {};
            auto buffer = std::array<char, 256> {};
            while (true)
            {
                auto const count = ::read(fd, buffer.data(), buffer.size());
                if (count < 0)
                {
                    if (errno == EINTR)
                        continue;
                    return std::unexpected(failureOf("read", file));
                }
                if (count == 0)
                    return content;
                content.append(buffer.data(), static_cast<std::size_t>(count));
            }
        }

        [[nodiscard]] std::expected<std::vector<std::string>, std::string> list(
            std::filesystem::path const& directory) const override
        {
            auto opened = openConsentDirectory(directory, DirectoryCreation::OpenExisting);
            if (!opened)
                return std::unexpected(std::move(opened.error()));
            // Listed by name, once the directory itself passed the checks: what a name leads to is reached
            // only through the descriptor checked above (openat, unlinkat).
            auto names = std::vector<std::string> {};
            auto error = std::error_code {};
            for (auto const& entry: std::filesystem::directory_iterator(directory, error))
                names.push_back(entry.path().filename().string());
            if (error)
                return std::unexpected(
                    std::format("cannot list '{}': {}", directory.string(), error.message()));
            return names;
        }

        [[nodiscard]] std::expected<KeyFileRemoval, std::string> removeUnlessHeld(
            std::filesystem::path const& directory, std::string_view name) const override
        {
            auto opened = openConsentDirectory(directory, DirectoryCreation::OpenExisting);
            if (!opened)
                return std::unexpected(std::move(opened.error()));
            auto const dir = opened->fd();
            auto const entry = std::string { name };
            // O_NONBLOCK: whatever was planted there, opening it never waits.
            auto const fd = ::openat(dir, entry.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0)
            {
                if (errno == ENOENT)
                    return KeyFileRemoval::Removed;
                // A link, or what cannot be opened, holds no run's key: it is only a name to remove.
                if (::unlinkat(dir, entry.c_str(), 0) != 0 && errno != ENOENT)
                    return std::unexpected(failureOf("remove", directory / name));
                return KeyFileRemoval::Removed;
            }
            auto const held = DescriptorHold { fd };
            // A live run holds its key file for as long as it runs: one this cannot lock is that run's.
            if (lockRetryingInterrupts(fd, LOCK_EX | LOCK_NB) != 0)
            {
                if (errno == EWOULDBLOCK)
                    return KeyFileRemoval::Held;
                return std::unexpected(failureOf("lock", directory / name));
            }
            // Locked, and still the file of that name: no run can take it now, for it is held here.
            if (isFileAt(fd, dir, entry) && ::unlinkat(dir, entry.c_str(), 0) != 0 && errno != ENOENT)
                return std::unexpected(failureOf("remove", directory / name));
            return KeyFileRemoval::Removed;
        }
    };
} // namespace

IConsentKeyFiles const& nativeConsentKeyFiles()
{
    static auto const files = PosixConsentKeyFiles {};
    return files;
}

} // namespace vthost

#endif
