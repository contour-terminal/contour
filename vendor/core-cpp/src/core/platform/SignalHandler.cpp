// SPDX-License-Identifier: Apache-2.0
#include <core/platform/SignalHandler.hpp>

#include <core/platform/Wakeup.hpp>

#include <atomic>
#include <csignal>
#include <optional>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <unistd.h>
    #ifdef __linux__
        #include <sys/signalfd.h>
    #endif
#endif

namespace core::platform
{

namespace
{
    // The handler's state is process-wide, because signal dispositions are. File-scope rather
    // than static members of SignalHandler, so its header names none of it.
    SignalCallback* currentCallback = nullptr;
    int currentSignalFd = -1;

    /// The signal fd as a @c NativeHandle, set only where one exists. An optional rather than a
    /// handle initialized to @c InvalidHandle, which on Windows is not a constant expression, so a
    /// namespace-scope copy of it would be initialized dynamically, in an order no other
    /// translation unit can rely on.
    std::optional<NativeHandle> currentSignalHandle;
    std::atomic<bool> sigintPending { false };
    std::atomic<Wakeup*> interruptWakeup { nullptr };

    /// Signals the registered interrupt wakeup, if any. Safe from a handler.
    void signalInterruptWakeup() noexcept
    {
        // Loaded atomically: registration may race with a handler/console thread.
        if (auto* const wakeup = interruptWakeup.load())
            wakeup->signal();
    }

#ifdef _WIN32
    /// Win32 console control handler. Intercepts Ctrl+C / Ctrl+Break so the
    /// process survives (records a pending interrupt and returns TRUE); lets the
    /// default handler process other control events (e.g. CTRL_CLOSE_EVENT).
    BOOL WINAPI consoleCtrlHandler(DWORD ctrlType)
    {
        if (SignalHandler::isInterruptCtrlEvent(ctrlType))
        {
            // Record the interrupt so in-process work polling hasPendingSigint()
            // can abort, and return TRUE so the process is not terminated. The
            // foreground child shares the console process group and receives its own
            // CTRL_C_EVENT from the OS; background jobs are shielded by being created
            // in a separate console process group.
            sigintPending.store(true);
            signalInterruptWakeup();
            return TRUE;
        }
        return FALSE;
    }
#endif

#if !defined(__linux__) && !defined(_WIN32)
    std::atomic<bool> sigChldPending { false };
    std::atomic<bool> sigTstpPending { false };
    std::atomic<bool> sigContPending { false };

    // Traditional signal handlers for the POSIX systems without signalfd.
    void sigchldHandler(int /*sig*/)
    {
        sigChldPending.store(true);
    }

    void sigtstpHandler(int /*sig*/)
    {
        sigTstpPending.store(true);
    }

    void sigcontHandler(int /*sig*/)
    {
        sigContPending.store(true);
    }

    void sigintHandler(int /*sig*/)
    {
        sigintPending.store(true);
        signalInterruptWakeup();
    }
#endif
} // namespace

int SignalHandler::initialize(SignalCallback* callback)
{
    currentCallback = callback;

#ifdef _WIN32
    // Windows has no POSIX signals. Install a console control handler so that
    // Ctrl+C / Ctrl+Break interrupt the foreground child instead of terminating
    // this process. The handler records a pending interrupt and returns TRUE,
    // preventing the default handler from calling ExitProcess on it.
    SetConsoleCtrlHandler(&consoleCtrlHandler, TRUE);
    return -1;
#elifdef __linux__
    // Block signals so they can be received via signalfd
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGTSTP);
    sigaddset(&mask, SIGCONT);
    sigaddset(&mask, SIGINT);
    pthread_sigmask(SIG_BLOCK, &mask, nullptr);

    // Ignore SIGTTOU so tcsetpgrp() doesn't stop this process when transferring terminal control
    signal(SIGTTOU, SIG_IGN);

    // Create signalfd for receiving signals as file descriptor events
    currentSignalFd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (currentSignalFd >= 0)
        currentSignalHandle = currentSignalFd;
    return currentSignalFd;
#else
    // macOS/BSD: Use traditional signal handlers
    struct sigaction sa {};
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);

    // SIGCHLD: Child process state change
    sa.sa_handler = sigchldHandler;
    sigaction(SIGCHLD, &sa, nullptr);

    // SIGTSTP: Terminal stop (Ctrl+Z from parent or kill -TSTP)
    sa.sa_handler = sigtstpHandler;
    sigaction(SIGTSTP, &sa, nullptr);

    // SIGCONT: Continue after stop
    sa.sa_handler = sigcontHandler;
    sigaction(SIGCONT, &sa, nullptr);

    // Ignore SIGTTOU so tcsetpgrp() doesn't stop this process when transferring terminal control
    signal(SIGTTOU, SIG_IGN);

    // Handle SIGINT via flag: in-process work (a sleep, say) polls it to support Ctrl+C.
    // A terminal in raw mode delivers Ctrl+C as a keypress, not as this signal.
    sa.sa_handler = sigintHandler;
    sigaction(SIGINT, &sa, nullptr);

    sigChldPending.store(false);
    sigTstpPending.store(false);
    sigContPending.store(false);
    sigintPending.store(false);
    return -1;
#endif
}

void SignalHandler::restore()
{
#ifdef _WIN32
    // Deregister the console control handler installed in initialize() and clear any
    // interrupt it may have recorded, so a pending flag does not survive this teardown
    // into a later SignalHandler user.
    SetConsoleCtrlHandler(&consoleCtrlHandler, FALSE);
    sigintPending.store(false);
#elifdef __linux__
    if (currentSignalFd >= 0)
    {
        close(currentSignalFd);
        currentSignalFd = -1;
        currentSignalHandle.reset();
    }

    // Unblock signals
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGTSTP);
    sigaddset(&mask, SIGCONT);
    sigaddset(&mask, SIGINT);
    pthread_sigmask(SIG_UNBLOCK, &mask, nullptr);

    // Restore default handlers
    signal(SIGCHLD, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGCONT, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    signal(SIGINT, SIG_DFL);
#else
    // Restore default signal handlers
    signal(SIGCHLD, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGCONT, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    sigChldPending.store(false);
    sigTstpPending.store(false);
    sigContPending.store(false);
    sigintPending.store(false);
#endif

    currentCallback = nullptr;
    // The wakeup goes the way the callback does. It belongs to whoever called initialize(), and
    // once this returns nothing may call signal() on it again: processSignalFd() on Linux, the
    // SIGINT handler elsewhere and the Windows console handler all reach it through this
    // pointer, and all of them outlive the Wakeup a teardown is about to destroy.
    interruptWakeup.store(nullptr);
}

int SignalHandler::signalFd() noexcept
{
    return currentSignalFd;
}

NativeHandle SignalHandler::nativeHandle() noexcept
{
    return currentSignalHandle.value_or(InvalidHandle);
}

bool SignalHandler::processSignalFd()
{
#ifdef __linux__
    if (currentSignalFd < 0 || !currentCallback)
        return false;

    signalfd_siginfo info {};
    bool processed = false;

    // Read all pending signals from signalfd
    while (read(currentSignalFd, &info, sizeof(info)) == sizeof(info))
    {
        switch (info.ssi_signo)
        {
            case SIGCHLD:
                currentCallback->onSigchld();
                processed = true;
                break;
            case SIGTSTP:
                currentCallback->onSigtstp();
                processed = true;
                break;
            case SIGCONT:
                currentCallback->onSigcont();
                processed = true;
                break;
            case SIGINT:
                sigintPending.store(true);
                signalInterruptWakeup();
                processed = true;
                break;
            default: break;
        }
    }
    return processed;
#else
    return false;
#endif
}

void SignalHandler::processPendingSignals()
{
#if !defined(__linux__) && !defined(_WIN32)
    if (sigChldPending.exchange(false) && currentCallback)
        currentCallback->onSigchld();

    if (sigTstpPending.exchange(false) && currentCallback)
        currentCallback->onSigtstp();

    if (sigContPending.exchange(false) && currentCallback)
        currentCallback->onSigcont();
#endif
}

bool SignalHandler::hasPendingSigchld() noexcept
{
#if !defined(__linux__) && !defined(_WIN32)
    return sigChldPending.load();
#else
    return false;
#endif
}

void SignalHandler::clearPendingSigchld() noexcept
{
#if !defined(__linux__) && !defined(_WIN32)
    sigChldPending.store(false);
#endif
}

bool SignalHandler::hasPendingSigtstp() noexcept
{
#if !defined(__linux__) && !defined(_WIN32)
    return sigTstpPending.load();
#else
    return false;
#endif
}

void SignalHandler::clearPendingSigtstp() noexcept
{
#if !defined(__linux__) && !defined(_WIN32)
    sigTstpPending.store(false);
#endif
}

bool SignalHandler::hasPendingSigcont() noexcept
{
#if !defined(__linux__) && !defined(_WIN32)
    return sigContPending.load();
#else
    return false;
#endif
}

void SignalHandler::clearPendingSigcont() noexcept
{
#if !defined(__linux__) && !defined(_WIN32)
    sigContPending.store(false);
#endif
}

void SignalHandler::suspendSelf()
{
#ifndef _WIN32
    // Temporarily restore default SIGTSTP handling
    signal(SIGTSTP, SIG_DFL);

    #ifdef __linux__
    // Unblock SIGTSTP temporarily so we can actually be stopped
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTSTP);
    pthread_sigmask(SIG_UNBLOCK, &mask, nullptr);
    #endif

    // Re-raise SIGTSTP to actually stop ourselves
    raise(SIGTSTP);

    // When we get here, we've been resumed (SIGCONT was received)

    #ifdef __linux__
    // Re-block SIGTSTP for signalfd
    pthread_sigmask(SIG_BLOCK, &mask, nullptr);
    #else
    // Re-install our handler
    struct sigaction sa {};
    sa.sa_handler = sigtstpHandler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTSTP, &sa, nullptr);
    #endif
#endif
}

bool SignalHandler::hasPendingSigint() noexcept
{
    return sigintPending.load();
}

void SignalHandler::clearPendingSigint() noexcept
{
    sigintPending.store(false);
}

void SignalHandler::setInterruptWakeup(Wakeup* wakeup) noexcept
{
    interruptWakeup.store(wakeup);
}

void SignalHandler::simulateSigint() noexcept
{
    sigintPending.store(true);
}

bool SignalHandler::isInterruptCtrlEvent(unsigned long ctrlType) noexcept
{
#ifdef _WIN32
    return ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT;
#else
    // Mirror the Win32 CTRL_C_EVENT (0) and CTRL_BREAK_EVENT (1) values so the
    // interrupt policy can be unit-tested on any platform.
    constexpr unsigned long CtrlCEvent = 0;
    constexpr unsigned long CtrlBreakEvent = 1;
    return ctrlType == CtrlCEvent || ctrlType == CtrlBreakEvent;
#endif
}

} // namespace core::platform
