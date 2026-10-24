// SPDX-License-Identifier: Apache-2.0

// winsock2.h MUST precede windows.h, mirroring every Win32 TU of this module.
// clang-format off
#ifdef _WIN32
    #include <winsock2.h>
    #include <windows.h>
#endif
// clang-format on

#include <vthost/ShutdownSignals.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <tuple>
#include <utility>

#ifndef _WIN32
    #include <csignal>
    #include <thread>

    #include <pthread.h>
    #include <unistd.h>
#endif

namespace vthost
{

namespace
{
    /// A watch that watches nothing.
    class InertWatch final: public ShutdownSignals::Watch
    {
    };

#ifndef _WIN32

    /// Where a SigwaitWatch is: still waiting, woken by a signal, or released by its destructor. One
    /// atomic state rather than two flags, so the signal and the release cannot both win.
    enum class SigwaitState : std::uint8_t
    {
        Waiting = 0, ///< Nothing happened yet.
        Signalled,   ///< A signal arrived; @c onSignal was called.
        Released,    ///< The watch is being destroyed; whatever wakes the thread now is ignored.
    };

    /// SIGINT/SIGTERM without async-signal-safety hazards: they are blocked on the constructing thread —
    /// which every thread it starts afterwards inherits — and consumed by a dedicated sigwait thread, which
    /// hands them on through @c onSignal (a post onto the loop, its only thread-safe entry point).
    class SigwaitWatch final: public ShutdownSignals::Watch
    {
      public:
        explicit SigwaitWatch(std::function<void()> onSignal):
            _signals { blockedShutdownSignals() }, _onSignal { std::move(onSignal) }, _thread { [this] {
                auto sig = 0;
                sigwait(&_signals, &sig);
                auto expected = SigwaitState::Waiting;
                if (_state.compare_exchange_strong(expected, SigwaitState::Signalled))
                    _onSignal();
            } }
        {
        }

        SigwaitWatch(SigwaitWatch const&) = delete;
        SigwaitWatch& operator=(SigwaitWatch const&) = delete;
        SigwaitWatch(SigwaitWatch&&) = delete;
        SigwaitWatch& operator=(SigwaitWatch&&) = delete;

        ~SigwaitWatch() override
        {
            // Unblock the sigwait thread when the shutdown came from elsewhere — a stop request or the
            // last-session exit, for which no signal was ever delivered. If it already consumed a signal,
            // nothing is sent.
            //
            // raise(3) targets the calling thread (pthread_kill(pthread_self(), …)) — the watcher thread's
            // sigwait dequeues from the PROCESS queue and never sees a thread-directed signal, so the join
            // below would hang. kill(getpid(), …) is process-directed.
            auto expected = SigwaitState::Waiting;
            if (_state.compare_exchange_strong(expected, SigwaitState::Released))
                std::ignore = ::kill(::getpid(), SIGTERM);
            _thread.join();
        }

      private:
        /// Blocks SIGINT and SIGTERM on the calling thread.
        /// @return The set blocked, which the sigwait thread waits on.
        [[nodiscard]] static sigset_t blockedShutdownSignals() noexcept
        {
            auto signals = sigset_t {};
            sigemptyset(&signals);
            sigaddset(&signals, SIGINT);
            sigaddset(&signals, SIGTERM);
            pthread_sigmask(SIG_BLOCK, &signals, nullptr);
            return signals;
        }

        sigset_t _signals;
        std::atomic<SigwaitState> _state { SigwaitState::Waiting };
        std::function<void()> _onSignal;
        std::thread _thread; ///< Last member: started only once everything it reads is initialized.
    };

    /// The process's SIGINT/SIGTERM.
    class ProcessSignals final: public ShutdownSignals
    {
      public:
        [[nodiscard]] std::unique_ptr<Watch> watch(std::function<void()> onSignal) override
        {
            return std::make_unique<SigwaitWatch>(std::move(onSignal));
        }
    };

#else

    /// The console-control shutdown hook: SetConsoleCtrlHandler's callback is a plain C function invoked on
    /// an arbitrary thread with no user data pointer, so the watching daemon registers its (thread-safe)
    /// shutdown poster in this one process-wide slot.
    std::atomic<std::function<void()>*> consoleShutdown = nullptr;

    BOOL WINAPI consoleCtrlHandler(DWORD type)
    {
        if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT && type != CTRL_CLOSE_EVENT)
            return FALSE;
        if (auto* shutdown = consoleShutdown.load())
            (*shutdown)();
        return TRUE;
    }

    /// Ctrl+C, Ctrl+Break and closing the console window.
    class ConsoleControlWatch final: public ShutdownSignals::Watch
    {
      public:
        explicit ConsoleControlWatch(std::function<void()> onSignal): _onSignal { std::move(onSignal) }
        {
            consoleShutdown = &_onSignal;
            ::SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
        }

        ConsoleControlWatch(ConsoleControlWatch const&) = delete;
        ConsoleControlWatch& operator=(ConsoleControlWatch const&) = delete;
        ConsoleControlWatch(ConsoleControlWatch&&) = delete;
        ConsoleControlWatch& operator=(ConsoleControlWatch&&) = delete;

        ~ConsoleControlWatch() override
        {
            // The slot first: a handler the console thread starts from here on finds nothing to call. One
            // that loaded the pointer just before may still call _onSignal as it is destroyed — a window
            // SetConsoleCtrlHandler offers no way to close, since it does not wait for running handlers.
            consoleShutdown = nullptr;
            ::SetConsoleCtrlHandler(consoleCtrlHandler, FALSE);
        }

      private:
        std::function<void()> _onSignal;
    };

    /// The process's console-control events.
    class ProcessSignals final: public ShutdownSignals
    {
      public:
        [[nodiscard]] std::unique_ptr<Watch> watch(std::function<void()> onSignal) override
        {
            return std::make_unique<ConsoleControlWatch>(std::move(onSignal));
        }
    };

#endif
} // namespace

std::shared_ptr<ShutdownSignals> processShutdownSignals()
{
    return std::make_shared<ProcessSignals>();
}

std::unique_ptr<ShutdownSignals::Watch> NoShutdownSignals::watch(std::function<void()> /*onSignal*/)
{
    return std::make_unique<InertWatch>();
}

} // namespace vthost
