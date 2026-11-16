// SPDX-License-Identifier: Apache-2.0
//
// core-cpp#49 over a real terminal: the runtime reading a pseudo-terminal whose master side has
// closed, with SIGHUP ignored -- the process state the tuidu migration found spinning at 100% CPU.
// The scripted cases in runtime/TuiRuntime_test.cpp pin the runtime's half; this one is the only
// case that reaches the production read, TerminalInput::readReadyInput(), on a handle that has
// actually hung up, and it is POSIX because that is where a pty is.

#include <core/async/Cancellation.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/tui/MockTerminalOutput.hpp>
#include <core/tui/Terminal.hpp>
#include <core/tui/posix/StandardStreamRedirect.hpp>
#include <core/tui/runtime/TerminalInputSource.hpp>
#include <core/tui/runtime/TuiRuntime.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

using namespace std::chrono_literals;
using core::tui::test::StandardStreamRedirect;

namespace
{

/// A file descriptor, closed on destruction.
class OwnedFd
{
  public:
    explicit OwnedFd(int fd) noexcept: _fd(fd) {}
    ~OwnedFd() { reset(); }

    OwnedFd(OwnedFd const&) = delete;
    OwnedFd& operator=(OwnedFd const&) = delete;
    OwnedFd(OwnedFd&&) = delete;
    OwnedFd& operator=(OwnedFd&&) = delete;

    [[nodiscard]] int get() const noexcept { return _fd; }

    /// Closes the descriptor now.
    void reset() noexcept
    {
        if (_fd >= 0)
            ::close(_fd);
        _fd = -1;
    }

  private:
    int _fd;
};

/// Ignores SIGHUP until destroyed, as the reporter's process did: with the default disposition a
/// hangup of the controlling terminal kills the process, and there is nothing left to spin.
class IgnoreSighup
{
  public:
    IgnoreSighup() noexcept
    {
        struct sigaction ignore {};
        ignore.sa_handler = SIG_IGN;
        sigemptyset(&ignore.sa_mask);
        ::sigaction(SIGHUP, &ignore, &_previous);
    }

    ~IgnoreSighup() { ::sigaction(SIGHUP, &_previous, nullptr); }

    IgnoreSighup(IgnoreSighup const&) = delete;
    IgnoreSighup& operator=(IgnoreSighup const&) = delete;
    IgnoreSighup(IgnoreSighup&&) = delete;
    IgnoreSighup& operator=(IgnoreSighup&&) = delete;

  private:
    struct sigaction _previous {};
};

/// Forwards to the production source and counts its reads, which is what a spin is made of.
class CountingInputSource: public core::tui::runtime::InputSource
{
  public:
    explicit CountingInputSource(core::tui::runtime::InputSource& inner) noexcept: _inner(inner) {}

    [[nodiscard]] core::platform::NativeHandle inputHandle() const noexcept override
    {
        return _inner.inputHandle();
    }
    [[nodiscard]] core::platform::NativeHandle resizeHandle() const noexcept override
    {
        return _inner.resizeHandle();
    }
    [[nodiscard]] std::vector<core::tui::InputEvent> takePending() override { return _inner.takePending(); }
    [[nodiscard]] std::vector<core::tui::InputEvent> readReady() override
    {
        ++_reads;
        return _inner.readReady();
    }
    [[nodiscard]] bool inputClosed() const noexcept override { return _inner.inputClosed(); }
    [[nodiscard]] std::optional<core::tui::InputEvent> readResize() override { return _inner.readResize(); }
    [[nodiscard]] std::vector<core::tui::InputEvent> flushPartial() override { return _inner.flushPartial(); }
    [[nodiscard]] bool consumeReports(std::vector<core::tui::InputEvent>& events) override
    {
        return _inner.consumeReports(events);
    }

    [[nodiscard]] std::size_t reads() const noexcept { return _reads; }

  private:
    core::tui::runtime::InputSource& _inner;
    std::size_t _reads = 0;
};

/// Waits for one input event; sets @p cancelled when the wait ends in a cancellation instead.
core::async::Task<void> awaitEventOrCancellation(core::tui::runtime::TuiRuntime* runtime, bool* cancelled)
{
    try
    {
        std::ignore = co_await runtime->nextEvent();
    }
    catch (core::async::OperationCancelled const&)
    {
        *cancelled = true;
    }
}

/// What the runtime did with the hung-up terminal, recorded while it ran -- before the teardown,
/// which cancels a flow that is still waiting and would make a spin look like an ending.
struct Observed
{
    std::size_t turns = 0;
    std::size_t reads = 0;
    bool cancelled = false;
    bool inputClosed = false;
};

/// How many turns the runtime gets. A correct one needs three; the spin reads on every one.
constexpr auto TurnBound = std::size_t { 50 };

} // namespace

TEST_CASE("core-cpp#49: a pty whose master closed ends the runtime's input within a bounded number of turns",
          "[TuiRuntime][hangup][posix]")
{
    auto master = OwnedFd { ::posix_openpt(O_RDWR | O_NOCTTY) };
    if (master.get() < 0 || ::grantpt(master.get()) != 0 || ::unlockpt(master.get()) != 0)
        SKIP("no pseudo-terminal could be opened");
    auto slaveName = std::array<char, 256> {};
    if (::ptsname_r(master.get(), slaveName.data(), slaveName.size()) != 0)
        SKIP("the pseudo-terminal has no slave name");
    auto slave = OwnedFd { ::open(slaveName.data(), O_RDWR | O_NOCTTY) };
    if (slave.get() < 0)
        SKIP("the pseudo-terminal's slave could not be opened");

    auto observed = Observed {};
    auto cancelled = false; // outlives the loop, which the waiting flow writes it from
    {
        auto const ignoreSighup = IgnoreSighup {};
        auto const redirect = StandardStreamRedirect { STDIN_FILENO, slave.get() };
        if (!redirect.isRedirected())
            SKIP("standard input could not be redirected to the pseudo-terminal");
        slave.reset();  // standard input holds it now
        master.reset(); // the hangup

        // Not initialized: no raw mode, no protocol sequences on the test's own output, no
        // capability queries. Its input handle is standard input either way.
        auto terminal = core::tui::Terminal { std::make_unique<core::tui::MockTerminalOutput>() };
        auto production = core::tui::runtime::TerminalInputSource { terminal };
        auto source = CountingInputSource { production };
        auto const backend = core::net::makeDefaultBackend();
        auto loop = core::net::EventLoop { *backend };
        {
            auto runtime = core::tui::runtime::TuiRuntime { loop, source };
            loop.spawn(awaitEventOrCancellation(&runtime, &cancelled));
            while (!cancelled && observed.turns < TurnBound)
            {
                std::ignore = loop.runOnce(10ms);
                ++observed.turns;
            }
            observed.cancelled = cancelled;
            observed.reads = source.reads();
            observed.inputClosed = runtime.inputClosed();
        }
    } // standard input and SIGHUP are restored here, before anything is asserted

    INFO("turns: " << observed.turns << ", reads: " << observed.reads);
    CHECK(observed.cancelled);
    CHECK(observed.inputClosed);
    CHECK(observed.turns < TurnBound);
    // Linux answers the first read with EIO, so one read is the end. A system that answers with an
    // end of file first and reports the hangup only to poll(2) still needs just the one.
    CHECK(observed.reads == 1);
}
