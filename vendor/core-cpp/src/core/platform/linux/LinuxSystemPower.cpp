// SPDX-License-Identifier: Apache-2.0
#include <core/platform/SystemPower.hpp>
#include <core/platform/detail/LogindPlan.hpp>

#include <sys/eventfd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <expected>
#include <limits>
#include <memory>
#include <thread>
#include <utility>

#include <poll.h>
#include <unistd.h>

#include <systemd/sd-bus.h>

namespace core::platform
{

namespace
{
    [[nodiscard]] std::uint64_t monotonicMicroseconds() noexcept
    {
        auto now = timespec {};
        ::clock_gettime(CLOCK_MONOTONIC, &now);
        return (static_cast<std::uint64_t>(now.tv_sec) * 1'000'000U)
               + (static_cast<std::uint64_t>(now.tv_nsec) / 1'000U);
    }

    /// How long the reconnect wait sleeps between two tries of a poll() that failed outright.
    constexpr auto PollRetrySlice = std::chrono::milliseconds { 100 };

    /// Closes a bus connection without flushing it: a hung bus must not hold up a reconnect or the
    /// destructor, and nothing queued on it (the AddMatch call) is worth waiting for.
    struct BusCloser
    {
        void operator()(sd_bus* bus) const noexcept { sd_bus_close_unref(bus); }
    };

    using BusPtr = std::unique_ptr<sd_bus, BusCloser>;

    /// What the worker found while waiting.
    enum class Wait : std::uint8_t
    {
        TimedOut = 0,      ///< Nothing but the timeout: go on.
        ShutdownRequested, ///< The monitor is being destroyed.
    };

    /// What became of the match on PrepareForSleep, as logind's bus answered the asynchronous AddMatch.
    enum class MatchState : std::uint8_t
    {
        Pending = 0, ///< AddMatch has not been answered yet.
        Installed,   ///< The bus accepted the match.
        Refused,     ///< The bus refused it: treated as a lost bus.
    };

    /// What one turn of serving the bus ended with.
    enum class Served : std::uint8_t
    {
        Continue = 0,      ///< Keep serving.
        BusFailed,         ///< The connection is gone (or the match refused): reconnect.
        ShutdownRequested, ///< The monitor is being destroyed.
    };

    /// System sleep/wake notifications from logind's PrepareForSleep signal, read over sd-bus on a
    /// private thread. A lost system bus is reconnected with a capped exponential backoff
    /// (detail::reconnectDelay), so delivery pauses while the bus is unavailable rather than stopping.
    class LinuxSystemPowerMonitor final: public ISystemPowerMonitor
    {
      public:
        LinuxSystemPowerMonitor() = default;
        LinuxSystemPowerMonitor(LinuxSystemPowerMonitor const&) = delete;
        LinuxSystemPowerMonitor(LinuxSystemPowerMonitor&&) = delete;
        LinuxSystemPowerMonitor& operator=(LinuxSystemPowerMonitor const&) = delete;
        LinuxSystemPowerMonitor& operator=(LinuxSystemPowerMonitor&&) = delete;

        /// Stops and joins the thread first, so no callback runs once the bus is closed or the
        /// registry is destroyed.
        ~LinuxSystemPowerMonitor() override
        {
            if (_thread.joinable())
            {
                auto const one = std::uint64_t { 1 };
                while (::write(_shutdown, &one, sizeof(one)) == -1 && errno == EINTR)
                    ;
                _thread.join();
            }
            _bus.reset();
            if (_shutdown != -1)
                ::close(_shutdown);
        }

        /// Connects to the system bus, asks it to match PrepareForSleep and starts the reading thread.
        /// Does not wait for the bus to answer, so a hung bus cannot hold up the caller.
        /// @return Whether the connection could be opened.
        [[nodiscard]] std::expected<void, PlatformError> start()
        {
            _shutdown = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
            if (_shutdown == -1)
                return std::unexpected(PlatformError::NotificationRegistrationFailed);
            connect();
            if (_bus == nullptr)
                return std::unexpected(PlatformError::NotificationRegistrationFailed);
            _thread = std::thread { [this] { run(); } };
            return {};
        }

        [[nodiscard]] PowerSubscription subscribe(Handler handler) override
        {
            return _registry.subscribe(std::move(handler));
        }

      private:
        /// Opens the system bus and queues the AddMatch for PrepareForSleep, without waiting for it.
        /// Leaves _bus null when either fails.
        void connect() noexcept
        {
            auto* bus = static_cast<sd_bus*>(nullptr);
            if (sd_bus_open_system(&bus) < 0)
                return;
            auto owned = BusPtr { bus };
            _match = MatchState::Pending;
            auto const matched = sd_bus_match_signal_async(owned.get(),
                                                           nullptr,
                                                           "org.freedesktop.login1",
                                                           "/org/freedesktop/login1",
                                                           "org.freedesktop.login1.Manager",
                                                           "PrepareForSleep",
                                                           &onPrepareForSleep,
                                                           &onMatchInstalled,
                                                           this);
            if (matched >= 0)
                _bus = std::move(owned);
        }

        static int onMatchInstalled(sd_bus_message* reply, void* userdata, sd_bus_error* /*error*/)
        {
            auto& self = *static_cast<LinuxSystemPowerMonitor*>(userdata);
            if (sd_bus_message_is_method_error(reply, nullptr) != 0)
            {
                self._match = MatchState::Refused;
                return 0;
            }
            self._match = MatchState::Installed;
            self._failedAttempts = 0; // the bus works again: the next loss starts the backoff over
            return 0;
        }

        static int onPrepareForSleep(sd_bus_message* message, void* userdata, sd_bus_error* /*error*/)
        {
            auto sleeping = 0;
            if (sd_bus_message_read(message, "b", &sleeping) < 0)
                return 0;
            auto& self = *static_cast<LinuxSystemPowerMonitor*>(userdata);
            auto const state =
                sleeping != 0 ? detail::LogindSleepState::Sleeping : detail::LogindSleepState::Waking;
            self._registry.dispatch(detail::eventForPrepareForSleep(state));
            return 0;
        }

        /// Serves the bus until the shutdown eventfd is signalled; reconnects a bus that failed.
        void run()
        {
            while (true)
            {
                if (_bus == nullptr)
                {
                    if (reconnect() == Wait::ShutdownRequested)
                        return;
                    continue;
                }
                switch (serve())
                {
                    case Served::Continue: break;
                    case Served::BusFailed: _bus.reset(); break;
                    case Served::ShutdownRequested: return;
                }
            }
        }

        /// Waits out the backoff, then tries once to connect again; on failure _bus stays null, and the
        /// next turn backs off further.
        /// @return ShutdownRequested if the monitor was destroyed while waiting.
        [[nodiscard]] Wait reconnect()
        {
            auto const delay = detail::reconnectDelay(_failedAttempts);
            if (_failedAttempts < std::numeric_limits<std::uint32_t>::max())
                ++_failedAttempts;
            if (waitForShutdown(delay) == Wait::ShutdownRequested)
                return Wait::ShutdownRequested;
            connect();
            return Wait::TimedOut;
        }

        /// Sleeps for @p delay, or until the shutdown eventfd is signalled.
        [[nodiscard]] Wait waitForShutdown(std::chrono::milliseconds delay) const
        {
            auto const deadline = monotonicMicroseconds()
                                  + static_cast<std::uint64_t>(
                                      std::chrono::duration_cast<std::chrono::microseconds>(delay).count());
            while (true)
            {
                auto const timeout = detail::pollTimeoutMilliseconds(deadline, monotonicMicroseconds());
                if (timeout == 0)
                    return Wait::TimedOut;
                auto fd = pollfd { .fd = _shutdown, .events = POLLIN, .revents = 0 };
                auto const ready = ::poll(&fd, 1, timeout);
                if (ready > 0 && (fd.revents & POLLIN) != 0)
                    return Wait::ShutdownRequested;
                if (ready < 0 && errno != EINTR)
                {
                    // poll() itself failed: neither give up on the delay, which would turn a persistent
                    // failure into reconnects back to back, nor stop watching for shutdown. Read the
                    // non-blocking eventfd directly, then sleep a slice and try again.
                    if (shutdownSignalled())
                        return Wait::ShutdownRequested;
                    std::this_thread::sleep_for(
                        std::min(std::chrono::milliseconds { timeout }, PollRetrySlice));
                }
            }
        }

        /// Whether the destructor has written the shutdown eventfd, read without poll(). Consumes the
        /// count, which is harmless: the thread returns as soon as it sees it.
        [[nodiscard]] bool shutdownSignalled() const noexcept
        {
            auto count = std::uint64_t { 0 };
            return ::read(_shutdown, &count, sizeof(count)) == static_cast<ssize_t>(sizeof(count));
        }

        /// Processes what the bus has, then waits for it or for the shutdown eventfd.
        [[nodiscard]] Served serve()
        {
            auto* const bus = _bus.get();
            auto processed = 0;
            do
                processed = sd_bus_process(bus, nullptr);
            while (processed > 0);
            auto deadline = std::uint64_t { 0 };
            if (processed < 0 || _match == MatchState::Refused || sd_bus_get_timeout(bus, &deadline) < 0)
                return Served::BusFailed;
            auto const events = sd_bus_get_events(bus);
            if (events < 0)
                return Served::BusFailed;

            auto fds = std::array {
                pollfd { .fd = sd_bus_get_fd(bus), .events = static_cast<short>(events), .revents = 0 },
                pollfd { .fd = _shutdown, .events = POLLIN, .revents = 0 },
            };
            auto const timeout = detail::pollTimeoutMilliseconds(deadline, monotonicMicroseconds());
            auto const ready = ::poll(fds.data(), fds.size(), timeout);
            if ((fds[1].revents & POLLIN) != 0)
                return Served::ShutdownRequested;
            if (ready < 0 && errno != EINTR)
                return Served::BusFailed;
            return Served::Continue;
        }

        // Declared first, so it outlives the bus and the thread that dispatch into it.
        detail::PowerHandlerRegistry _registry;
        int _shutdown = -1;
        BusPtr _bus;
        // Touched by start() before the thread exists, then by the thread alone.
        MatchState _match = MatchState::Pending;
        std::uint32_t _failedAttempts = 0;
        std::thread _thread;
    };
} // namespace

std::expected<std::unique_ptr<ISystemPowerMonitor>, PlatformError> makeNativeSystemPowerMonitor()
{
    auto monitor = std::make_unique<LinuxSystemPowerMonitor>();
    if (auto started = monitor->start(); !started)
        return std::unexpected(started.error());
    return monitor;
}

} // namespace core::platform
