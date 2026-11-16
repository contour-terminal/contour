// SPDX-License-Identifier: Apache-2.0
// The POSIX TerminalChannel over a pseudo-terminal: the test holds the master, the channel opens the
// slave by name, as it opens /dev/tty in production.
#include <core/tui/InputEvent.hpp>
#include <core/tui/TerminalChannel.hpp>
#include <core/tui/posix/TerminalChannelPosix.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

using namespace std::chrono_literals;
using core::tui::ChannelAccess;
using core::tui::DecModeReport;
using core::tui::InputEvent;
using core::tui::OscResponse;
using core::tui::TerminalChannel;
using core::tui::TerminalChannelError;

namespace
{

class OwnedFd
{
  public:
    explicit OwnedFd(int fd) noexcept: _fd(fd) {}
    ~OwnedFd()
    {
        if (_fd >= 0)
            ::close(_fd);
    }
    OwnedFd(OwnedFd const&) = delete;
    OwnedFd& operator=(OwnedFd const&) = delete;
    OwnedFd(OwnedFd&&) = delete;
    OwnedFd& operator=(OwnedFd&&) = delete;

    [[nodiscard]] int get() const noexcept { return _fd; }

  private:
    int _fd;
};

/// A pseudo-terminal pair: the master end, and the slave's name and an fd onto it to inspect it by.
struct Pty
{
    OwnedFd master { ::posix_openpt(O_RDWR | O_NOCTTY) };
    std::array<char, 256> slaveName {};
    std::optional<OwnedFd> slave;

    [[nodiscard]] auto open() -> bool
    {
        if (master.get() < 0 || ::grantpt(master.get()) != 0 || ::unlockpt(master.get()) != 0)
            return false;
        if (::ptsname_r(master.get(), slaveName.data(), slaveName.size()) != 0)
            return false;
        slave.emplace(::open(slaveName.data(), O_RDWR | O_NOCTTY));
        return slave->get() >= 0;
    }

    [[nodiscard]] auto localFlags() const -> tcflag_t
    {
        auto attributes = termios {};
        ::tcgetattr(slave->get(), &attributes);
        return attributes.c_lflag;
    }

    /// Reads what the slave side wrote, waiting at most one second for the first byte.
    [[nodiscard]] auto readFromMaster() const -> std::string
    {
        auto watch = pollfd { .fd = master.get(), .events = POLLIN, .revents = 0 };
        if (::poll(&watch, 1, 1000) <= 0)
            return {};
        auto buffer = std::array<char, 256> {};
        auto const n = ::read(master.get(), buffer.data(), buffer.size());
        return n > 0 ? std::string(buffer.data(), static_cast<std::size_t>(n)) : std::string {};
    }

    void writeToMaster(std::string_view bytes) const
    {
        REQUIRE(::write(master.get(), bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
    }
};

/// Polls @p channel until an event arrives, for at most one second of real time.
auto firstEvent(TerminalChannel& channel) -> std::optional<InputEvent>
{
    auto const deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto events = channel.poll(100);
        REQUIRE(events.has_value());
        if (!events->empty())
            return events->front();
    }
    return std::nullopt; // nothing within a second: the reply never arrived
}

} // namespace

TEST_CASE("tui.TerminalChannel: the terminal neither echoes nor buffers lines while the channel is open",
          "[posix]")
{
    auto pty = Pty {};
    if (!pty.open())
        SKIP("no pseudo-terminal could be opened");
    REQUIRE((pty.localFlags() & ICANON) != 0);

    {
        auto channel = core::tui::openTerminalChannelAt(pty.slaveName.data(), ChannelAccess::ReadWrite);
        REQUIRE(channel.has_value());
        CHECK((*channel)->access() == ChannelAccess::ReadWrite);
        auto const flags = pty.localFlags();
        CHECK((flags & ICANON) == 0);
        CHECK((flags & ECHO) == 0);
        CHECK((flags & ISIG) != 0); // Ctrl+C still interrupts
    }

    auto const restored = pty.localFlags();
    CHECK((restored & ICANON) != 0);
    CHECK((restored & ECHO) != 0);
}

TEST_CASE("tui.TerminalChannel: what the channel writes reaches the terminal", "[posix]")
{
    auto pty = Pty {};
    if (!pty.open())
        SKIP("no pseudo-terminal could be opened");
    auto channel = core::tui::openTerminalChannelAt(pty.slaveName.data(), ChannelAccess::ReadWrite);
    REQUIRE(channel.has_value());

    REQUIRE((*channel)->write("\033[?5522$p").has_value());
    CHECK(pty.readFromMaster() == "\033[?5522$p");
}

TEST_CASE("tui.TerminalChannel: the terminal's replies come back decoded, OSC included", "[posix]")
{
    auto pty = Pty {};
    if (!pty.open())
        SKIP("no pseudo-terminal could be opened");
    auto channel = core::tui::openTerminalChannelAt(pty.slaveName.data(), ChannelAccess::ReadWrite);
    REQUIRE(channel.has_value());

    pty.writeToMaster("\033[?5522;2$y");
    auto const decMode = firstEvent(**channel);
    REQUIRE(decMode.has_value());
    auto const* report = std::get_if<DecModeReport>(&*decMode);
    REQUIRE(report != nullptr);
    CHECK(report->mode == 5522);
    CHECK(report->status == 2);

    pty.writeToMaster("\033]5522;type=write:status=DONE\033\\");
    auto const status = firstEvent(**channel);
    REQUIRE(status.has_value());
    auto const* osc = std::get_if<OscResponse>(&*status);
    REQUIRE(osc != nullptr);
    CHECK(osc->payload == "5522;type=write:status=DONE");
}

TEST_CASE("tui.TerminalChannel: discarding pending input drops a reply left over from an earlier exchange",
          "[posix]")
{
    auto pty = Pty {};
    if (!pty.open())
        SKIP("no pseudo-terminal could be opened");

    // A status that arrived after the previous copy gave up waiting, still in the terminal's input.
    pty.writeToMaster("\033]5522;type=write:status=DONE\033\\");
    std::this_thread::sleep_for(50ms); // let the line discipline queue it before the channel opens

    auto channel = core::tui::openTerminalChannelAt(pty.slaveName.data(), ChannelAccess::ReadWrite);
    REQUIRE(channel.has_value());
    (*channel)->discardPendingInput();
    CHECK_FALSE(firstEvent(**channel).has_value());
}

TEST_CASE("tui.TerminalChannel: a terminal that cannot be opened is NoTerminal", "[posix]")
{
    auto const channel = core::tui::openTerminalChannelAt("/nonexistent/tty", std::nullopt);
    REQUIRE_FALSE(channel.has_value());
    CHECK(channel.error() == TerminalChannelError::NoTerminal);
}

TEST_CASE("tui.TerminalChannel: the controlling terminal opens, or there is none", "[posix]")
{
    // Under CI there is usually no controlling terminal; at a developer's desk there is one.
    auto const channel = core::tui::openControllingTerminal();
    if (channel.has_value())
        CHECK(*channel != nullptr);
    else
        CHECK(channel.error() == TerminalChannelError::NoTerminal);
}
