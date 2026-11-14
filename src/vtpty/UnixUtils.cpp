// SPDX-License-Identifier: Apache-2.0
#include <vtpty/UnixUtils.hpp>

#include <algorithm>
#include <array>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>

#ifdef __linux__
    #include <sys/syscall.h>
#endif

#ifdef __APPLE__
    #include <sys/param.h>

    #include <libproc.h>
#endif

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <fcntl.h>
#include <unistd.h>

// POSIX requires no header to declare `environ`; each platform is left to differ, and all three we
// build on do.
//
// macOS: a shared library cannot link against the symbol directly, so it goes through
// _NSGetEnviron(). Linux: glibc's <unistd.h> declares it, so nothing is needed. FreeBSD: its
// <unistd.h> does NOT declare it in the configuration this tree builds with, which is why the
// FreeBSD job failed with "use of undeclared identifier 'environ'" -- so the declaration is
// supplied here, as POSIX intends. `extern "C"` rather than a bare `extern`: the symbol has C
// linkage, and declaring it with C++ linkage would name a different entity and fail to link.
#ifdef __APPLE__
    #include <crt_externs.h>
    #define environ (*_NSGetEnviron())
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
extern "C" char** environ;
#endif

namespace vtpty
{

std::expected<UnixPipe, std::error_code> UnixPipe::open(unsigned flags)
{
    auto pipe = UnixPipe::none();
#ifdef __linux__
    if (pipe2(pipe.pfd.data(), static_cast<int>(flags)) < 0)
        return std::unexpected { std::error_code { errno, std::generic_category() } };
#else
    if (::pipe(pipe.pfd.data()) < 0)
        return std::unexpected { std::error_code { errno, std::generic_category() } };
    for (auto const fd: pipe.pfd)
        if (auto const applied = util::applyCreationFlags(fd, flags); !applied)
            return std::unexpected { applied.error() };
#endif
    return pipe;
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

void configureTerminal(int fd) noexcept
{
    applyTerminalSettings(fd, constructTerminalSettings(fd));
}

std::expected<void, std::error_code> writeAll(int fd, DescriptorKind kind, std::string_view bytes) noexcept
{
#ifdef MSG_NOSIGNAL
    constexpr auto NoSigPipeSendFlags = MSG_NOSIGNAL;
#else
    constexpr auto NoSigPipeSendFlags = 0; // makeGate() set SO_NOSIGPIPE on its sockets instead
#endif
    auto const writeSome = [&]() -> ssize_t {
        switch (kind)
        {
            case DescriptorKind::Stream: return ::write(fd, bytes.data(), bytes.size());
            case DescriptorKind::Socket: return ::send(fd, bytes.data(), bytes.size(), NoSigPipeSendFlags);
        }
        return ::write(fd, bytes.data(), bytes.size());
    };
    while (!bytes.empty())
    {
        auto const written = writeSome();
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return std::unexpected(written < 0 ? lastError() : std::make_error_code(std::errc::broken_pipe));
        bytes.remove_prefix(static_cast<std::size_t>(written));
    }
    return {};
}

std::expected<int, std::error_code> waitForExit(pid_t pid) noexcept
{
    auto status = 0;
    while (::waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            return std::unexpected(lastError());
    return status;
}

std::expected<void, std::error_code> loginTerminal(int slaveFd, ControllingTerminal ctty) noexcept
{
    // This is doing what login_tty() is doing, too.
    // But doing it ourselves allows for a little more flexibility.

    sigset_t signals;
    sigemptyset(&signals);
    pthread_sigmask(SIG_SETMASK, &signals, nullptr);

    // clang-format off
    struct sigaction act {};
    act.sa_handler = SIG_DFL;
    // clang-format on

    for (auto const signo: { SIGCHLD, SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGALRM })
        sigaction(signo, &act, nullptr);

    setsid();

    // A failure to take the terminal on as the controlling one is reported once the standard streams are on
    // it regardless, which are the program's way to its terminal either way.
    auto controlling = std::expected<void, std::error_code> {};
#ifdef TIOCSCTTY
    if (ctty == ControllingTerminal::Acquire && ioctl(slaveFd, TIOCSCTTY, nullptr) == -1)
        controlling = std::unexpected(lastError());
#else
    (void) ctty;
#endif

    for (int const fd: { 0, 1, 2 })
    {
        if (slaveFd != fd)
            ::close(fd);
        saveDup2(slaveFd, fd);
    }

    return controlling;
}

std::optional<std::string> processNameOf(pid_t pid)
{
#if defined(__linux__) || defined(__APPLE__)
    if (pid <= 0)
        return std::nullopt;
    #ifdef __linux__
    // Streamed: /proc reports a size of zero, so a read sized by the file's length reads nothing.
    auto in = std::ifstream { std::format("/proc/{}/comm", pid) };
    auto name = std::string {};
    if (!std::getline(in, name) || name.empty())
        return std::nullopt;
    return name;
    #else
    auto name = std::array<char, (2 * MAXCOMLEN) + 1> {};
    if (proc_name(pid, name.data(), static_cast<uint32_t>(name.size())) <= 0)
        return std::nullopt;
    return std::string { name.data() };
    #endif
#else
    std::ignore = pid;
    return std::nullopt;
#endif
}

std::optional<std::string> workingDirectoryOf(pid_t pid)
{
#ifdef __linux__
    auto ec = std::error_code {};
    auto const cwd = std::filesystem::read_symlink(std::format("/proc/{}/cwd", pid), ec);
    if (ec)
        return std::nullopt;
    return cwd.string();
#elifdef __APPLE__
    auto vpi = proc_vnodepathinfo {};
    if (proc_pidinfo(pid, PROC_PIDVNODEPATHINFO, 0, &vpi, sizeof(vpi)) <= 0)
        return std::nullopt;
    return std::string { vpi.pvi_cdir.vip_path };
#else
    std::ignore = pid;
    return std::nullopt;
#endif
}

char**& processEnvironment() noexcept
{
    return environ;
}

std::vector<std::string_view> environmentEntries()
{
    auto entries = std::vector<std::string_view> {};
    auto const* const variables = processEnvironment();
    if (variables == nullptr) // clearenv() leaves no array at all
        return entries;
    for (auto const index: std::views::iota(std::size_t { 0 })
                               | std::views::take_while([&](auto i) { return variables[i] != nullptr; }))
        entries.emplace_back(variables[index]);
    return entries;
}

} // namespace vtpty::util
