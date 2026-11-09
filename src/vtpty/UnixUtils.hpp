// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/ControllingTerminal.hpp>
#include <vtpty/InputEcho.hpp>

#include <gsl/pointers>

#include <array>
#include <cerrno>
#include <cstdint>
#include <expected>
#include <string_view>
#include <system_error>

#ifdef __APPLE__
    #include <util.h>
#else
    #include <termios.h>
#endif

#include <fcntl.h>
#include <unistd.h>

namespace vtpty::util
{

termios getTerminalSettings(int fd) noexcept;
termios constructTerminalSettings(int fd) noexcept;

/// Applies @p tio to the terminal on @p fd at once, and flushes its queues once it applies. A terminal that
/// refuses the settings is left as it is.
/// @param fd The terminal's descriptor.
/// @param tio The settings.
void applyTerminalSettings(int fd, termios const& tio) noexcept;

/// Gives the terminal on @p fd the settings a session's program starts with: applyTerminalSettings() of
/// constructTerminalSettings().
/// @param fd The terminal's slave descriptor.
void configureTerminal(int fd) noexcept;

/// Decides what the terminal does with typed text from its local flags. Echo wins; without it, line editing
/// still on is a password prompt (Hidden), and both off is a program drawing its own echo (Raw).
/// @param localFlags A termios' c_lflag.
/// @return Echoed, Hidden or Raw; never Unknown.
[[nodiscard]] constexpr InputEcho inputEchoOf(tcflag_t localFlags) noexcept
{
    if ((localFlags & ECHO) != 0)
        return InputEcho::Echoed;
    if ((localFlags & ICANON) != 0)
        return InputEcho::Hidden;
    return InputEcho::Raw;
}

/// Marks @p fd close-on-exec -- a descriptor flag, set with F_SETFD -- so that no process this one spawns
/// inherits it.
/// @param fd The descriptor.
/// @return Nothing; or why the flag could not be set.
[[nodiscard]] std::expected<void, std::error_code> setCloseOnExec(int fd) noexcept;

/// Makes reads and writes on @p fd return rather than wait, keeping its other status flags.
/// @param fd The descriptor.
/// @return Nothing; or why the flag could not be set.
[[nodiscard]] std::expected<void, std::error_code> setNonBlocking(int fd) noexcept;

/// Applies @p flags, as pipe2() and open() take them, to an open descriptor: O_CLOEXEC through
/// setCloseOnExec(), O_NONBLOCK through setNonBlocking(). For a pipe made where pipe2() is missing.
/// @param fd The descriptor.
/// @param flags O_CLOEXEC and O_NONBLOCK, or neither.
/// @return Nothing; or why a flag could not be set -- invalid_argument for any other flag, as pipe2()
///         refuses one.
[[nodiscard]] std::expected<void, std::error_code> applyCreationFlags(int fd, unsigned flags) noexcept;

/// Makes @p slaveFd the calling process's controlling terminal and standard streams: what login_tty() does,
/// with the signal mask emptied and the dispositions a shell expects reset to default first.
/// @param slaveFd The terminal's slave descriptor; left open, also where it is above 2.
/// @param ctty Whether the terminal becomes the controlling terminal (TIOCSCTTY); a sandbox such as
///        flatpak forbids it.
/// @return Nothing; or why the terminal could not be made the controlling one. The standard streams are
///         on the terminal either way: it is the program's terminal even where it cannot control it.
[[nodiscard]] std::expected<void, std::error_code> loginTerminal(int slaveFd,
                                                                 ControllingTerminal ctty) noexcept;

/// What a descriptor written by writeAll() is, as its caller knows it: said rather than asked of the kernel
/// on every write.
enum class DescriptorKind : std::uint8_t
{
    /// A pipe, a terminal or a file: written with write().
    Stream,
    /// A socket: written with send(), so that a peer gone away is an error rather than a SIGPIPE --
    /// MSG_NOSIGNAL where send() takes it, the socket's own SO_NOSIGPIPE (makeGate()) elsewhere.
    Socket,
};

/// Writes @p bytes to @p fd in full, retrying on short writes and EINTR.
/// @param fd The descriptor.
/// @param kind What @p fd is, which decides how it is written.
/// @param bytes What to write.
/// @return Nothing; or why not everything could be written -- broken_pipe where the descriptor took nothing.
[[nodiscard]] std::expected<void, std::error_code> writeAll(int fd,
                                                            DescriptorKind kind,
                                                            std::string_view bytes) noexcept;

/// Waits for the child @p pid to end, and reaps it, retrying on EINTR.
/// @param pid The child.
/// @return Its wait status; or why it could not be waited for (ECHILD: not a child, or reaped already).
[[nodiscard]] std::expected<int, std::error_code> waitForExit(pid_t pid) noexcept;

void saveClose(gsl::not_null<int*> fd) noexcept;
void saveDup2(int a, int b) noexcept;

/// How closeDescriptorsAbove() finds the descriptors to close, the fastest first.
enum class DescriptorSweep : std::uint8_t
{
    Kernel,  ///< close_range(): the kernel closes them all (Linux 5.9, FreeBSD 12.2, glibc 2.34).
    Listing, ///< Linux: the open ones, listed from /proc/self/fd with getdents64 into a buffer on the stack.
    Bounded, ///< Every number up to the limit, open or not.
};

/// @return One above the highest descriptor this process may open: sysconf(_SC_OPEN_MAX), or 1024 where it
///         is indeterminate. Not async-signal-safe -- POSIX.1-2008 took sysconf() off that list -- so read in
///         a single-threaded process, as the spawn trampoline is, or before a fork().
[[nodiscard]] int openDescriptorLimit() noexcept;

/// Closes every descriptor above @p keepFd, before an exec(): the spawn trampoline's, ahead of its program's,
/// and the daemon's own on a live upgrade.
///
/// Async-signal-safe -- no allocation and no exception -- so that it stays fit for a child forked from a
/// multi-threaded process, which may call only async-signal-safe functions (fork(2) NOTES; POSIX XSH
/// fork()), although the trampoline that calls it is single-threaded: Contour's threaded processes spawn
/// through it rather than fork (@see SpawnTrampoline.hpp). It calls close(), open() and
/// lseek() of POSIX's async-signal-safe list; close_range() -- the C library's wrapper, or syscall(), which
/// is not POSIX, where the library has none -- and getdents64 through syscall(): system calls that touch no
/// library state but errno. A way that is not there -- close_range() before Linux 5.9, /proc/self/fd off
/// Linux -- hands over to the next.
/// @param keepFd The highest descriptor kept.
/// @param limit One above the highest descriptor that can be open, from openDescriptorLimit().
/// @param first The way tried first; tests force the slower ones with it.
void closeDescriptorsAbove(int keepFd, int limit, DescriptorSweep first = DescriptorSweep::Kernel) noexcept;

// {{{ impl
inline termios getTerminalSettings(int fd) noexcept
{
    termios tio {};
    tcgetattr(fd, &tio);
    return tio;
}

inline void applyTerminalSettings(int fd, termios const& tio) noexcept
{
    if (tcsetattr(fd, TCSANOW, &tio) == 0)
        tcflush(fd, TCIOFLUSH);
}

inline termios constructTerminalSettings(int fd) noexcept
{
    auto tio = getTerminalSettings(fd);

    // input flags
#ifdef IUTF8
    // Input is UTF-8; this allows character-erase to be properly applied in cooked mode.
    tio.c_iflag |= IUTF8;
#endif

    // special characters
    tio.c_cc[VMIN] = 1;  // Report as soon as 1 character is available.
    tio.c_cc[VTIME] = 0; // Disable timeout (no need).
    // tio.c_iflag &= ~ISTRIP; // Disable stripping 8th bit off the bytes.
    // tio.c_iflag &= ~INLCR;  // Disable NL-to-CR mapping
    // tio.c_iflag &= ~ICRNL;  // Disable CR-to-NL mapping
    // tio.c_iflag &= ~IXON;   // Disable control flow
    // tio.c_iflag &= ~BRKINT; // disable signal-break handling

    return tio;
}

/// @return The calling thread's errno, as an error code.
[[nodiscard]] inline std::error_code lastError() noexcept
{
    return std::error_code { errno, std::generic_category() };
}

/// Whether a descriptor survives an exec.
enum class ExecInheritance : std::uint8_t
{
    CloseOnExec = 0, ///< FD_CLOEXEC set: the exec closes it.
    KeepOnExec,      ///< FD_CLOEXEC clear: the exec keeps it.
};

/// Sets or clears @p fd's FD_CLOEXEC flag, leaving its other descriptor flags as they are.
/// @param fd The descriptor.
/// @param inheritance Whether an exec closes or keeps it.
/// @return Nothing, or why fcntl() refused.
[[nodiscard]] inline std::expected<void, std::error_code> setExecInheritance(
    int fd, ExecInheritance inheritance) noexcept
{
    auto const flags = fcntl(fd, F_GETFD);
    if (flags < 0)
        return std::unexpected(lastError());
    auto const updated =
        inheritance == ExecInheritance::CloseOnExec ? (flags | FD_CLOEXEC) : (flags & ~FD_CLOEXEC);
    if (fcntl(fd, F_SETFD, updated) < 0)
        return std::unexpected(lastError());
    return {};
}

inline std::expected<void, std::error_code> setCloseOnExec(int fd) noexcept
{
    return setExecInheritance(fd, ExecInheritance::CloseOnExec);
}

inline std::expected<void, std::error_code> setNonBlocking(int fd) noexcept
{
    auto const flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return std::unexpected(lastError());
    return {};
}

inline std::expected<void, std::error_code> applyCreationFlags(int fd, unsigned flags) noexcept
{
    using Setter = std::expected<void, std::error_code> (*)(int) noexcept;
    struct CreationFlag
    {
        unsigned flag; ///< As pipe2() and open() take it.
        Setter apply;  ///< What sets it on an open descriptor.
    };
    auto const known = std::array {
        CreationFlag { .flag = static_cast<unsigned>(O_CLOEXEC), .apply = &setCloseOnExec },
        CreationFlag { .flag = static_cast<unsigned>(O_NONBLOCK), .apply = &setNonBlocking },
    };

    auto rest = flags;
    for (auto const& [flag, apply]: known)
    {
        if ((flags & flag) == 0)
            continue;
        rest &= ~flag;
        if (auto const applied = apply(fd); !applied)
            return applied;
    }
    if (rest != 0)
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    return {};
}

inline void saveClose(gsl::not_null<int*> fd) noexcept
{
    if (*fd != -1)
    {
        ::close(*fd);
        *fd = -1;
    }
}

inline void saveDup2(int a, int b) noexcept
{
    while (dup2(a, b) == -1 && (errno == EBUSY || errno == EINTR))
        ;
}
// }}}

} // namespace vtpty::util

namespace vtpty
{

struct UnixPipe
{
    std::array<int, 2> pfd;

    explicit UnixPipe(unsigned flags = 0);
    UnixPipe(UnixPipe&&) noexcept;
    UnixPipe& operator=(UnixPipe&&) noexcept;
    UnixPipe(UnixPipe const&) = delete;
    UnixPipe& operator=(UnixPipe const&) = delete;
    ~UnixPipe();

    [[nodiscard]] bool good() const noexcept { return pfd[0] != -1 && pfd[1] != -1; }

    [[nodiscard]] int reader() const noexcept { return pfd[0]; }
    [[nodiscard]] int writer() const noexcept { return pfd[1]; }

    void closeReader() noexcept;
    void closeWriter() noexcept;

    void close();
};

// {{{ UnixPipe
inline UnixPipe::UnixPipe(UnixPipe&& v) noexcept: pfd { v.pfd[0], v.pfd[1] }
{
    v.pfd[0] = -1;
    v.pfd[1] = -1;
}

inline UnixPipe& UnixPipe::operator=(UnixPipe&& v) noexcept
{
    close();
    pfd[0] = v.pfd[0];
    pfd[1] = v.pfd[1];
    v.pfd[0] = -1;
    v.pfd[1] = -1;
    return *this;
}

inline UnixPipe::~UnixPipe()
{
    close();
}

inline void UnixPipe::close()
{
    closeReader();
    closeWriter();
}

inline void UnixPipe::closeReader() noexcept
{
    util::saveClose(&pfd[0]); // NOLINT(readability-container-data-pointer)
}

inline void UnixPipe::closeWriter() noexcept
{
    util::saveClose(&pfd[1]);
}
// }}}

} // namespace vtpty
