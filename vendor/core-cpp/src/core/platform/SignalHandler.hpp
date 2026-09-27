// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/Types.hpp>

namespace core::platform
{

class Wakeup;

/// Abstract callback interface for signal notifications.
///
/// Implement this interface to receive signal callbacks from SignalHandler: a program that
/// runs jobs in the foreground and background implements it to handle job control signals.
class SignalCallback
{
  public:
    virtual ~SignalCallback() = default;

    /// Called when SIGCHLD is received (child process state change).
    virtual void onSigchld() = 0;

    /// Called when SIGTSTP is received (terminal stop, e.g. Ctrl+Z).
    virtual void onSigtstp() = 0;

    /// Called when SIGCONT is received (continue after stop).
    virtual void onSigcont() = 0;
};

/// Signal handling using signalfd (Linux) or traditional handlers (other POSIX).
///
/// On Linux, we use signalfd to convert SIGCHLD, SIGTSTP, and SIGCONT signals
/// to file descriptor events, which eliminates race conditions and allows
/// integration with poll().
///
/// On macOS/BSD, we use traditional signal handlers with atomic flags.
///
/// The state is process-wide, because signal dispositions are: every member is static, and the
/// flags live in the implementation file.
///
/// Supported signals:
/// - SIGCHLD: Child process state change (exit, stop, continue)
/// - SIGTSTP: Terminal stop signal (Ctrl+Z at the controlling terminal, or kill -TSTP)
/// - SIGCONT: Continue after stop (when resumed via fg or kill -CONT)
class SignalHandler
{
  public:
    /// Initializes signal handling.
    ///
    /// **The `int` is kept for compatibility**, and it is the wrong type for what a caller does
    /// with it: `core::tui::runtime::TuiRuntimeOptions::signalFd` is a @c NativeHandle, which on
    /// Windows is a `HANDLE`, so every caller converted. Ask @c nativeHandle for the runtime's type
    /// instead. (Changing this return type is a 0.3.0 matter: it breaks every caller.)
    /// @param callback Pointer to the callback receiver for signal notifications
    /// @return signalfd file descriptor on Linux (for poll), -1 on other platforms
    [[nodiscard]] static int initialize(SignalCallback* callback);

    /// Restores default signal handling.
    static void restore();

    /// Returns the signalfd file descriptor (-1 if not using signalfd).
    [[nodiscard]] static int signalFd() noexcept;

    /// Returns the signal fd as the handle type the TUI runtime watches
    /// (`TuiRuntimeOptions::signalFd`), with no conversion at the call site.
    /// @return The signalfd on Linux while initialized; @c InvalidHandle on every other platform,
    ///         and before @c initialize or after @c restore.
    [[nodiscard]] static NativeHandle nativeHandle() noexcept;

    /// Processes pending signals from signalfd (call when fd is readable).
    ///
    /// @return true if any signals were processed
    static bool processSignalFd();

    /// Processes pending signals for non-signalfd platforms.
    /// Call this periodically in the main loop.
    static void processPendingSignals();

    /// Checks if SIGCHLD was received (for platforms without signalfd).
    [[nodiscard]] static bool hasPendingSigchld() noexcept;

    /// Clears the pending SIGCHLD flag.
    static void clearPendingSigchld() noexcept;

    /// Checks if SIGTSTP was received (for platforms without signalfd).
    [[nodiscard]] static bool hasPendingSigtstp() noexcept;

    /// Clears the pending SIGTSTP flag.
    static void clearPendingSigtstp() noexcept;

    /// Checks if SIGCONT was received (for platforms without signalfd).
    [[nodiscard]] static bool hasPendingSigcont() noexcept;

    /// Clears the pending SIGCONT flag.
    static void clearPendingSigcont() noexcept;

    /// Checks if SIGINT was received (works on all platforms).
    [[nodiscard]] static bool hasPendingSigint() noexcept;

    /// Clears the pending SIGINT flag.
    static void clearPendingSigint() noexcept;

    /// Registers a wakeup signalled whenever a SIGINT / Ctrl+C is recorded.
    ///
    /// This lets an event loop blocked in poll()/WaitForMultipleObjects() wake
    /// promptly on Ctrl+C instead of waiting for its timeout — in particular it
    /// closes the Windows gap where the console control handler only set a flag.
    /// Signalling happens from a signal handler / console-handler thread, so the
    /// wakeup must outlive the registration and its signal() must be safe to call
    /// from that context (the self-pipe / eventfd / Win32 event all are).
    /// @param wakeup The wakeup to signal on interrupt, or nullptr to clear.
    static void setInterruptWakeup(Wakeup* wakeup) noexcept;

    /// Simulates a SIGINT by setting the pending flag. For testing only.
    static void simulateSigint() noexcept;

    /// Temporarily restores default SIGTSTP handling and re-raises the signal.
    ///
    /// A program calls this when it suspends itself (Ctrl+Z) to actually stop the process.
    /// After the process is resumed (SIGCONT), the custom handler is reinstalled.
    static void suspendSelf();

    /// Decides whether a Windows console control event should be treated as a
    /// user interrupt that the program handles itself (keeping the process alive)
    /// rather than letting the default handler terminate the process.
    ///
    /// Exposed as a pure function so the policy is unit-testable without
    /// registering a real OS console control handler.
    ///
    /// @param ctrlType The Win32 console control type (e.g. CTRL_C_EVENT).
    /// @return true for Ctrl+C / Ctrl+Break, false otherwise.
    [[nodiscard]] static bool isInterruptCtrlEvent(unsigned long ctrlType) noexcept;
};

} // namespace core::platform
