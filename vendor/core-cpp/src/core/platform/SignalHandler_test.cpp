// SPDX-License-Identifier: Apache-2.0
#include <core/platform/SignalHandler.hpp>
#include <core/platform/Wakeup.hpp>

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

#ifndef _WIN32
    #include <csignal>

    #include <poll.h>
#endif

using core::platform::SignalHandler;

#ifndef _WIN32
namespace
{
/// A callback that records nothing: processSignalFd() refuses to run without one.
class SilentSignalCallback final: public core::platform::SignalCallback
{
  public:
    void onSigchld() override {}
    void onSigtstp() override {}
    void onSigcont() override {}
};

/// Puts the process-wide signal disposition back, however the test case leaves.
///
/// initialize() blocks signals and opens a signalfd; a REQUIRE that throws past restore() would
/// otherwise leave SIGINT blocked for every test that runs after it.
struct RestoreSignals
{
    ~RestoreSignals() { SignalHandler::restore(); }
};

/// @return Whether @p wakeup has been signalled. Does not block.
[[nodiscard]] bool isSignalled(core::platform::Wakeup const& wakeup)
{
    auto descriptor = pollfd { .fd = wakeup.nativeHandle(), .events = POLLIN, .revents = 0 };
    return ::poll(&descriptor, 1, 0) == 1;
}

/// Raises SIGINT and lets the handler see it, whichever mechanism this system uses.
/// @return True if the interrupt was raised and drained.
[[nodiscard]] bool raiseInterrupt()
{
    if (::raise(SIGINT) != 0)
        return false;
    #ifdef __linux__
    // Linux blocks the signal and delivers it through the signalfd, which only a read drains;
    // elsewhere the sigaction handler has already run, inline with raise().
    return SignalHandler::processSignalFd();
    #else
    return true;
    #endif
}
} // namespace
#endif

TEST_CASE("SignalHandler records an interrupt until it is cleared", "[platform][signal]")
{
    SignalHandler::clearPendingSigint();
    CHECK_FALSE(SignalHandler::hasPendingSigint());

    SignalHandler::simulateSigint();
    CHECK(SignalHandler::hasPendingSigint());
    // Reading the flag does not consume it: in-process work polls it until the program has
    // handled the interrupt and cleared it.
    CHECK(SignalHandler::hasPendingSigint());

    SignalHandler::clearPendingSigint();
    CHECK_FALSE(SignalHandler::hasPendingSigint());
}

TEST_CASE("SignalHandler treats Ctrl+C and Ctrl+Break as interrupts, and nothing else", "[platform][signal]")
{
    // The Win32 console control types: CTRL_C_EVENT 0, CTRL_BREAK_EVENT 1, CTRL_CLOSE_EVENT 2,
    // CTRL_LOGOFF_EVENT 5, CTRL_SHUTDOWN_EVENT 6. A close must still reach the default handler,
    // which ends the process as the user asked.
    CHECK(SignalHandler::isInterruptCtrlEvent(0));
    CHECK(SignalHandler::isInterruptCtrlEvent(1));
    CHECK_FALSE(SignalHandler::isInterruptCtrlEvent(2));
    CHECK_FALSE(SignalHandler::isInterruptCtrlEvent(5));
    CHECK_FALSE(SignalHandler::isInterruptCtrlEvent(6));
}

#ifndef _WIN32
TEST_CASE("SignalHandler::restore deregisters the interrupt wakeup", "[platform][signal]")
{
    // restore() cleared the callback but never the wakeup, so once the Wakeup was destroyed the
    // signalfd reader (Linux), the SIGINT handler (the other POSIX systems) and the Windows
    // console handler all still held a pointer to it and called signal() on it. The asymmetry is
    // invisible from the outside, because restore() looks complete.
    auto const guard = RestoreSignals {}; // Declared first, so it is destroyed last.
    auto wakeup = core::platform::Wakeup {};
    auto callback = SilentSignalCallback {};

    [[maybe_unused]] auto const firstFd = SignalHandler::initialize(&callback);
    #ifdef __linux__
    REQUIRE(firstFd >= 0);
    #endif
    SignalHandler::setInterruptWakeup(&wakeup);

    // While it is registered, an interrupt reaches it -- otherwise the check below would pass
    // for the wrong reason.
    REQUIRE(raiseInterrupt());
    REQUIRE(SignalHandler::hasPendingSigint());
    REQUIRE(isSignalled(wakeup));

    SignalHandler::restore();
    wakeup.reset();
    SignalHandler::clearPendingSigint();

    // A second user of the process-wide handler, which registered no wakeup of its own.
    [[maybe_unused]] auto const secondFd = SignalHandler::initialize(&callback);
    #ifdef __linux__
    REQUIRE(secondFd >= 0);
    #endif
    REQUIRE(raiseInterrupt());

    CHECK(SignalHandler::hasPendingSigint()); // The interrupt did arrive...
    CHECK_FALSE(isSignalled(wakeup));         // ...and reached no wakeup, this one deregistered.

    SignalHandler::clearPendingSigint();
}
#endif

namespace
{
/// A callback that records nothing, for the handle case below, which runs on every platform.
class NoSignalCallback final: public core::platform::SignalCallback
{
  public:
    void onSigchld() override {}
    void onSigtstp() override {}
    void onSigcont() override {}
};
} // namespace

// The handle type is the question, so it is asked of the compiler: what the runtime's
// `TuiRuntimeOptions::signalFd` takes is a `NativeHandle`, and `initialize()`'s `int` is not one on
// Windows.
static_assert(std::is_same_v<decltype(SignalHandler::nativeHandle()), core::platform::NativeHandle>);

TEST_CASE("SignalHandler::nativeHandle is the signal fd as a NativeHandle, or InvalidHandle",
          "[platform][signal]")
{
    // Every caller of initialize() converted its int for the runtime, and on Windows an int is the
    // wrong type for a handle. nativeHandle() answers in the runtime's type: the signalfd where
    // there is one (Linux), InvalidHandle where there is none -- the other platforms, and any
    // platform before initialize() or after restore().
    auto callback = NoSignalCallback {};
    [[maybe_unused]] auto const fd = SignalHandler::initialize(&callback);
    auto const whileInitialized = SignalHandler::nativeHandle();
    auto const fdWhileInitialized = SignalHandler::signalFd();
    SignalHandler::restore();
    auto const afterRestore = SignalHandler::nativeHandle();

    CHECK(fdWhileInitialized == fd);
    CHECK((whileInitialized == core::platform::InvalidHandle) == (fd < 0));
    CHECK(afterRestore == core::platform::InvalidHandle);
}
