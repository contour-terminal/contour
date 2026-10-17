// SPDX-License-Identifier: Apache-2.0
#include <vtpty/UnixUtils.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef __linux__
    #include <sys/syscall.h>
#endif

#include <fcntl.h>
#include <unistd.h>

using namespace std::string_literals;

namespace vtpty
{

UnixPipe::UnixPipe(unsigned flags): pfd { -1, -1 }
{
#ifdef __linux__
    if (pipe2(pfd.data(), static_cast<int>(flags)) < 0)
        throw std::runtime_error { "Failed to create PTY pipe. "s + std::generic_category().message(errno) };
#else
    if (pipe(pfd.data()) < 0)
        throw std::runtime_error { "Failed to create PTY pipe. "s + std::generic_category().message(errno) };
    for (auto const fd: pfd)
    {
        if (auto const applied = util::applyCreationFlags(fd, flags); !applied)
        {
            close();
            throw std::runtime_error { "Failed to configure PTY pipe. "s + applied.error().message() };
        }
    }
#endif
}

} // namespace vtpty

namespace vtpty::util
{

namespace
{
    /// Closes every descriptor above @p keepFd with close_range().
    /// @return Whether the kernel did; not where the call is missing.
    [[nodiscard]] bool closeByKernel([[maybe_unused]] int keepFd) noexcept
    {
#ifdef HAVE_CLOSE_RANGE
        return ::close_range(static_cast<unsigned>(keepFd) + 1, ~0U, 0) == 0;
#elif defined(__linux__) && defined(SYS_close_range)
        // A C library before glibc 2.34 has no wrapper, while the kernel may have the call.
        return ::syscall(SYS_close_range, static_cast<unsigned>(keepFd) + 1, ~0U, 0U) == 0;
#else
        return false;
#endif
    }

#ifdef __linux__
    /// @return The descriptor @p name spells in /proc/self/fd, or -1 for one that spells none ("." and "..").
    [[nodiscard]] int descriptorNamed(std::string_view name) noexcept
    {
        if (name.empty())
            return -1;
        auto value = 0;
        for (auto const digit: name)
        {
            if (digit < '0' || digit > '9')
                return -1;
            value = (value * 10) + (digit - '0');
        }
        return value;
    }

    /// Where a getdents64 record keeps its length and its name: struct linux_dirent64, the kernel's own
    /// layout (d_ino and d_off, 8 bytes each; d_reclen, 2; d_type, 1; then d_name), not the C library's.
    constexpr auto RecordLengthOffset = std::size_t { 16 };
    constexpr auto NameOffset = std::size_t { 19 };

    /// Closes every open descriptor above @p keepFd, as /proc/self/fd lists them.
    /// @return Whether the listing was read to its end, and a last pass found nothing left to close.
    [[nodiscard]] bool closeListed(int keepFd) noexcept
    {
        auto const directory = ::open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory < 0)
            return false;

        // On the stack: nothing between fork() and exec() may allocate. Closing what was listed can change
        // what a later read lists, so the listing is read again until a pass finds nothing left to close --
        // a few passes at most: a listing that keeps naming descriptors is not trusted, and the bounded sweep
        // takes over.
        constexpr auto MaxPasses = 4;
        auto buffer = std::array<char, 4096> {};
        auto listed = true;
        auto closedAny = true;
        auto passes = 0;
        while (listed && closedAny && passes < MaxPasses)
        {
            ++passes;
            closedAny = false;
            ::lseek(directory, 0, SEEK_SET);
            while (true)
            {
                auto const count = ::syscall(SYS_getdents64, directory, buffer.data(), buffer.size());
                if (count == 0)
                    break;
                if (count < 0)
                {
                    if (errno == EINTR)
                        continue;
                    listed = false;
                    break;
                }
                auto offset = std::size_t { 0 };
                while (std::cmp_less(offset, count))
                {
                    auto recordLength = std::uint16_t {};
                    std::memcpy(
                        &recordLength, buffer.data() + offset + RecordLengthOffset, sizeof(recordLength));
                    auto const fd = descriptorNamed(buffer.data() + offset + NameOffset);
                    if (fd > keepFd && fd != directory)
                    {
                        ::close(fd);
                        closedAny = true;
                    }
                    offset += recordLength;
                }
            }
        }
        ::close(directory);
        return listed && !closedAny;
    }
#endif

    /// Closes every descriptor number above @p keepFd and below @p limit, open or not.
    void closeBounded(int keepFd, int limit) noexcept
    {
        for (auto const fd: std::views::iota(keepFd + 1, std::max(limit, keepFd + 1)))
            ::close(fd);
    }
} // namespace

int openDescriptorLimit() noexcept
{
    constexpr auto Indeterminate = 1024;
    auto const limit = ::sysconf(_SC_OPEN_MAX);
    if (limit <= 0)
        return Indeterminate;
    return static_cast<int>(std::min<long>(limit, std::numeric_limits<int>::max()));
}

void closeDescriptorsAbove(int keepFd, int limit, DescriptorSweep first) noexcept
{
    // From the way tried first on, each way that is missing hands over to the next.
    switch (first)
    {
        case DescriptorSweep::Kernel:
            if (closeByKernel(keepFd))
                return;
            [[fallthrough]];
        case DescriptorSweep::Listing:
#ifdef __linux__
            if (closeListed(keepFd))
                return;
#endif
            [[fallthrough]];
        case DescriptorSweep::Bounded: closeBounded(keepFd, limit); return;
    }
}

} // namespace vtpty::util
