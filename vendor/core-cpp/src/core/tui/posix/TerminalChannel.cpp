// SPDX-License-Identifier: Apache-2.0
// The POSIX TerminalChannel: the terminal device opened beside standard I/O, with echo and line
// buffering off for as long as the channel is open.
#include <core/tui/TerminalChannel.hpp>

#include <core/tui/VtParser.hpp>
#include <core/tui/posix/PosixIO.hpp>
#include <core/tui/posix/TerminalChannelPosix.hpp>

#include <array>
#include <cerrno>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <tuple>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

namespace core::tui
{

namespace
{
    /// @brief An open terminal device, and the mode to give back when it is closed.
    class PosixTerminalChannel final: public TerminalChannel
    {
      public:
        /// @param fd The open terminal device; owned from here on.
        /// @param access Whether to read it. Only a reader changes the terminal's mode: a process
        ///               outside the foreground group that tried would be stopped by SIGTTOU.
        PosixTerminalChannel(int fd, ChannelAccess access): _fd(fd), _access(access)
        {
            auto saved = termios {};
            if (_access == ChannelAccess::WriteOnly || ::tcgetattr(_fd, &saved) != 0)
                return;
            auto raw = saved;
            raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO); // ISIG stays: Ctrl+C still interrupts
            raw.c_cc[VMIN] = 0;
            raw.c_cc[VTIME] = 0;
            if (::tcsetattr(_fd, TCSANOW, &raw) == 0)
                _restore = saved;
        }

        ~PosixTerminalChannel() override
        {
            // TCSANOW, not TCSADRAIN: only local flags change, which do not affect bytes already
            // written, and a drain waits for a reader -- forever on a terminal nobody reads (macOS
            // ptys wait for the master to read even an echo).
            if (_restore)
                std::ignore = ::tcsetattr(_fd, TCSANOW, &*_restore);
            ::close(_fd);
        }

        PosixTerminalChannel(PosixTerminalChannel const&) = delete;
        auto operator=(PosixTerminalChannel const&) -> PosixTerminalChannel& = delete;
        PosixTerminalChannel(PosixTerminalChannel&&) = delete;
        auto operator=(PosixTerminalChannel&&) -> PosixTerminalChannel& = delete;

        [[nodiscard]] auto access() const noexcept -> ChannelAccess override { return _access; }

        [[nodiscard]] auto write(std::string_view bytes) -> std::expected<void, TerminalChannelError> override
        {
            if (safeWrite(_fd, bytes.data(), bytes.size()) < 0)
                return std::unexpected { TerminalChannelError::IoError };
            return {};
        }

        [[nodiscard]] auto poll(int timeoutMs)
            -> std::expected<std::vector<InputEvent>, TerminalChannelError> override
        {
            auto watch = pollfd { .fd = _fd, .events = POLLIN, .revents = 0 };
            auto const ready = ::poll(&watch, 1, timeoutMs);
            if (ready < 0 && errno != EINTR)
                return std::unexpected { TerminalChannelError::IoError };
            if (ready <= 0)
                return std::vector<InputEvent> {}; // timeout, or a signal: the caller's deadline decides

            auto buffer = std::array<char, 4096> {};
            auto const count = safeRead(_fd, buffer.data(), buffer.size());
            if (count < 0 && errno != EAGAIN)
                return std::unexpected { TerminalChannelError::IoError };
            if (count <= 0)
                return std::vector<InputEvent> {};
            return _parser.feed(std::string_view { buffer.data(), static_cast<std::size_t>(count) });
        }

        void discardPendingInput() override
        {
            if (_access == ChannelAccess::ReadWrite)
                std::ignore = ::tcflush(_fd, TCIFLUSH);
        }

      private:
        int _fd;
        ChannelAccess _access;
        std::optional<termios> _restore; ///< The mode to give back, when this channel changed it.
        VtParser _parser { VtParser::Options { .osc = VtParser::OscRecognition::Response } };
    };

    /// @brief Read-write when this process is the terminal's foreground process group.
    auto foregroundAccess(int fd) noexcept -> ChannelAccess
    {
        return ::tcgetpgrp(fd) == ::getpgrp() ? ChannelAccess::ReadWrite : ChannelAccess::WriteOnly;
    }
} // namespace

auto openTerminalChannelAt(char const* path, std::optional<ChannelAccess> access) -> TerminalChannelResult
{
    auto const fd = ::open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0)
        return std::unexpected { TerminalChannelError::NoTerminal };
    return std::make_unique<PosixTerminalChannel>(fd, access ? *access : foregroundAccess(fd));
}

auto openControllingTerminal() -> TerminalChannelResult
{
    return openTerminalChannelAt("/dev/tty", std::nullopt);
}

} // namespace core::tui
