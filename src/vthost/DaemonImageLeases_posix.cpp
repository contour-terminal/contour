// SPDX-License-Identifier: Apache-2.0
#include <vthost/DaemonImage.hpp>

#ifndef _WIN32

    #include <core/Utf8Path.hpp>

    #include <sys/file.h>
    #include <sys/stat.h>

    #include <algorithm>
    #include <cerrno>
    #include <filesystem>
    #include <format>
    #include <ranges>
    #include <system_error>

    #include <fcntl.h>
    #include <unistd.h>

namespace vthost
{

namespace
{
    /// How often hold() starts again on a lease file that a prune unlinked while it waited. A prune
    /// unlinks a lease file only after removing its image, so one retry suffices unless prunes keep
    /// racing it; the bound stops a pathological loop, not a plausible one.
    constexpr auto HoldAttempts = 8;

    /// How many descriptor numbers adoptInherited() asks about. Numbers are handed out lowest first, so an
    /// inherited lease, opened early by the image before the exec, lies far below this.
    constexpr auto DescriptorsScanned = 65536L;

    /// A lease file held open; the descriptor's flock() is the lease.
    class FileLease final: public ImageLease
    {
      public:
        explicit FileLease(int fd) noexcept: _fd { fd } {}
        FileLease(FileLease const&) = delete;
        FileLease& operator=(FileLease const&) = delete;
        FileLease(FileLease&&) = delete;
        FileLease& operator=(FileLease&&) = delete;
        ~FileLease() override { ::close(_fd); }

        [[nodiscard]] std::error_code keepAcrossExec() override
        {
            auto const flags = ::fcntl(_fd, F_GETFD);
            if (flags == -1 || ::fcntl(_fd, F_SETFD, flags & ~FD_CLOEXEC) == -1)
                return { errno, std::generic_category() };
            return {};
        }

      private:
        int _fd;
    };

    /// @return The last call's error, as text.
    [[nodiscard]] std::string lastError()
    {
        return std::generic_category().message(errno);
    }

    /// @return Whether @p fd still is the file at @p path: what a prune's unlink, or a replacement
    ///         after it, makes untrue.
    [[nodiscard]] bool isFileAt(int fd, std::filesystem::path const& path) noexcept
    {
        struct stat opened {};
        struct stat named {};
        return ::fstat(fd, &opened) == 0 && ::stat(path.c_str(), &named) == 0 && opened.st_dev == named.st_dev
               && opened.st_ino == named.st_ino;
    }

    /// @return flock(@p fd, @p operation), retried across signals.
    [[nodiscard]] int lockRetryingInterrupts(int fd, int operation) noexcept
    {
        auto result = ::flock(fd, operation);
        while (result == -1 && errno == EINTR)
            result = ::flock(fd, operation);
        return result;
    }

    class NativeImageLeases final: public ImageLeases
    {
      public:
        [[nodiscard]] std::expected<std::unique_ptr<ImageLease>, StagingError> hold(
            std::filesystem::path const& leaseFile) override
        {
            for ([[maybe_unused]] auto const attempt: std::views::iota(0, HoldAttempts))
            {
                // O_CLOEXEC, so no shell, opener or helper this process starts inherits the lease
                // (a hand-over across execv clears it right before, @see makeNativeImageLeases).
                auto const fd = ::open(leaseFile.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
                if (fd == -1)
                    return failure(leaseFile, lastError());
                if (lockRetryingInterrupts(fd, LOCK_SH) == -1)
                {
                    auto const reason = lastError();
                    ::close(fd);
                    return failure(leaseFile, reason);
                }
                if (isFileAt(fd, leaseFile))
                    return std::make_unique<FileLease>(fd);
                ::close(fd); // claimed and unlinked while this waited: lease the file there now
            }
            return failure(leaseFile, "the lease file kept being replaced");
        }

        [[nodiscard]] std::unique_ptr<ImageLease> claim(std::filesystem::path const& leaseFile) override
        {
            auto const fd = ::open(leaseFile.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
            if (fd == -1)
                return nullptr;
            if (lockRetryingInterrupts(fd, LOCK_EX | LOCK_NB) == -1 || !isFileAt(fd, leaseFile))
            {
                ::close(fd);
                return nullptr;
            }
            return std::make_unique<FileLease>(fd);
        }

        [[nodiscard]] LeaseState probe(std::filesystem::path const& leaseFile) override
        {
            auto const fd = ::open(leaseFile.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd == -1)
                return errno == ENOENT ? LeaseState::Free : LeaseState::Held;
            auto const state =
                lockRetryingInterrupts(fd, LOCK_EX | LOCK_NB) == 0 ? LeaseState::Free : LeaseState::Held;
            ::close(fd); // and with it the exclusive lock, if it was taken
            return state;
        }

        [[nodiscard]] std::unique_ptr<ImageLease> adoptInherited(
            std::filesystem::path const& leaseFile) override
        {
            // Whatever number the descriptor had before the exec, it has now. Each number is asked in turn,
            // rather than /dev/fd listed: FreeBSD's shows 0 to 2 only, unless fdescfs is mounted on it.
            // Only one without FD_CLOEXEC can have been inherited; every lease of this process's own is
            // opened with it.
            auto const openMax = ::sysconf(_SC_OPEN_MAX);
            auto const limit =
                static_cast<int>(openMax > 0 ? std::min(openMax, DescriptorsScanned) : DescriptorsScanned);
            for (auto const fd: std::views::iota(0, limit))
            {
                auto const flags = ::fcntl(fd, F_GETFD);
                if (flags == -1 || (flags & FD_CLOEXEC) != 0 || !isFileAt(fd, leaseFile))
                    continue;
                if (::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1)
                    return nullptr;
                return std::make_unique<FileLease>(fd);
            }
            return nullptr;
        }

      private:
        [[nodiscard]] static std::unexpected<StagingError> failure(std::filesystem::path const& leaseFile,
                                                                   std::string const& reason)
        {
            return std::unexpected(
                StagingError { .code = StagingErrorCode::LeaseFailed,
                               .context = std::format("{}: {}", core::pathToUtf8(leaseFile), reason) });
        }
    };
} // namespace

std::unique_ptr<ImageLeases> makeNativeImageLeases()
{
    return std::make_unique<NativeImageLeases>();
}

} // namespace vthost

#endif
