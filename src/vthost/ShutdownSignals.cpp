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
#include <future>
#include <memory>
#include <tuple>
#include <utility>

#ifdef _WIN32
    #include <algorithm>
    #include <array>
#endif

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
        /// A SIGTERM ends nothing by itself here, so the daemon's last write needs no waiting for.
        [[nodiscard]] std::unique_ptr<Watch> watch(std::function<void()> onSignal,
                                                   std::shared_future<void> /*lastWrite*/) override
        {
            return std::make_unique<SigwaitWatch>(std::move(onSignal));
        }
    };

#else

    /// What a console-control event asks of the handler once the daemon has been told.
    enum class ConsoleEventEnd : std::uint8_t
    {
        Returns = 0,     ///< Ctrl+C, Ctrl+Break: the process lives on until the daemon has ended.
        AwaitsLastWrite, ///< Windows ends the process as soon as the handler returns.
    };

    /// One console-control event a daemon ends on.
    struct ConsoleEventRow
    {
        DWORD type;
        ConsoleEventEnd end;
    };

    /// Every console-control event a daemon ends on; each arrives as ShutdownCause::Signal.
    constexpr auto ConsoleEvents = std::to_array<ConsoleEventRow>({
        { .type = CTRL_C_EVENT, .end = ConsoleEventEnd::Returns },
        { .type = CTRL_BREAK_EVENT, .end = ConsoleEventEnd::Returns },
        { .type = CTRL_CLOSE_EVENT, .end = ConsoleEventEnd::AwaitsLastWrite },
        { .type = CTRL_LOGOFF_EVENT, .end = ConsoleEventEnd::AwaitsLastWrite },
        { .type = CTRL_SHUTDOWN_EVENT, .end = ConsoleEventEnd::AwaitsLastWrite },
    });

    /// What the watching daemon is told through, and what it fulfils once its last write is done.
    struct ConsoleShutdown
    {
        std::function<void()> onSignal;
        std::shared_future<void> lastWrite;
    };

    /// The console-control shutdown hook: SetConsoleCtrlHandler's callback is a plain C function invoked on
    /// an arbitrary thread with no user data pointer, so the watching daemon registers its (thread-safe)
    /// shutdown poster in this one process-wide slot.
    std::atomic<ConsoleShutdown const*> consoleShutdown = nullptr;

    BOOL WINAPI consoleCtrlHandler(DWORD type)
    {
        auto const row = std::ranges::find(ConsoleEvents, type, &ConsoleEventRow::type);
        if (row == ConsoleEvents.end())
            return FALSE;
        auto const* const shutdown = consoleShutdown.load();
        if (shutdown == nullptr)
            return TRUE;
        // Copied before the post: the watch, and the slot's target with it, may be destroyed once the
        // daemon has shut down, while this thread still waits.
        auto const lastWrite = shutdown->lastWrite;
        shutdown->onSignal();
        if (row->end == ConsoleEventEnd::AwaitsLastWrite && lastWrite.valid())
            std::ignore = lastWrite.wait_for(LastWriteGrace);
        return TRUE;
    }

    /// Ctrl+C, Ctrl+Break, closing the console window, logging off and shutting the system down.
    class ConsoleControlWatch final: public ShutdownSignals::Watch
    {
      public:
        ConsoleControlWatch(std::function<void()> onSignal, std::shared_future<void> lastWrite):
            _shutdown { .onSignal = std::move(onSignal), .lastWrite = std::move(lastWrite) }
        {
            consoleShutdown = &_shutdown;
            ::SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
        }

        ConsoleControlWatch(ConsoleControlWatch const&) = delete;
        ConsoleControlWatch& operator=(ConsoleControlWatch const&) = delete;
        ConsoleControlWatch(ConsoleControlWatch&&) = delete;
        ConsoleControlWatch& operator=(ConsoleControlWatch&&) = delete;

        ~ConsoleControlWatch() override
        {
            // The slot first: a handler the console thread starts from here on finds nothing to call. One
            // that loaded the pointer just before may still call onSignal as it is destroyed — a window
            // SetConsoleCtrlHandler offers no way to close, since it does not wait for running handlers.
            consoleShutdown = nullptr;
            ::SetConsoleCtrlHandler(consoleCtrlHandler, FALSE);
        }

      private:
        ConsoleShutdown _shutdown;
    };

    /// The process's console-control events.
    class ProcessSignals final: public ShutdownSignals
    {
      public:
        [[nodiscard]] std::unique_ptr<Watch> watch(std::function<void()> onSignal,
                                                   std::shared_future<void> lastWrite) override
        {
            return std::make_unique<ConsoleControlWatch>(std::move(onSignal), std::move(lastWrite));
        }
    };

#endif
} // namespace

std::shared_ptr<ShutdownSignals> processShutdownSignals()
{
    return std::make_shared<ProcessSignals>();
}

std::unique_ptr<ShutdownSignals::Watch> NoShutdownSignals::watch(std::function<void()> /*onSignal*/,
                                                                 std::shared_future<void> /*lastWrite*/)
{
    return std::make_unique<InertWatch>();
}

} // namespace vthost
