// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/PlatformError.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>

namespace core::platform
{

/// A change in the power state of the machine.
enum class SystemPowerEvent : std::uint8_t
{
    WillSleep = 0, ///< The machine is about to sleep.
    DidWake = 1,   ///< The machine has woken up.
};

namespace detail
{
    class PowerHandlerRegistryState;
} // namespace detail

/// A live subscription to an ISystemPowerMonitor.
///
/// Move-only. Destroying (or resetting) it unsubscribes, and waits out a handler that is running for it on
/// another thread, so the handler is no longer running once the destructor returned. Captures by reference
/// are then safe to destroy; captures by value may be destroyed a moment later, on the dispatching thread,
/// when a dispatch still holds the handler. A handler may drop its own subscription: that does not wait.
///
/// The token keeps the monitor's handler table alive, so it may outlive the monitor that issued it.
class PowerSubscription
{
  public:
    /// An empty token, subscribed to nothing.
    PowerSubscription() noexcept = default;
    PowerSubscription(PowerSubscription const&) = delete;
    PowerSubscription& operator=(PowerSubscription const&) = delete;

    /// Takes over the subscription of another token, leaving it empty.
    PowerSubscription(PowerSubscription&& other) noexcept;

    /// Unsubscribes the current subscription, then takes over the one of another token.
    PowerSubscription& operator=(PowerSubscription&& other) noexcept;

    /// Unsubscribes.
    ~PowerSubscription();

  private:
    friend class detail::PowerHandlerRegistryState;

    PowerSubscription(std::shared_ptr<detail::PowerHandlerRegistryState> state, std::uint64_t id) noexcept;

    void release() noexcept;

    std::shared_ptr<detail::PowerHandlerRegistryState> _state;
    std::uint64_t _id = 0;
};

/// Where system sleep/wake notifications come from: the operating system, or a test double.
///
/// Events are hints, not a protocol. A DidWake may arrive without a WillSleep before it; on Linux a
/// WillSleep may arrive late, after the system has begun to suspend, as the monitor takes no inhibitor
/// lock; delivery may pause while the system bus is unavailable (Linux); and a monitor that was created
/// successfully does not guarantee that any event ever arrives.
///
/// Handlers run on a platform thread that is not the caller's: the monitor's own on macOS and Linux, the
/// power manager's on Windows. Consumer contract for a handler:
/// - It must not throw. A handler that throws terminates the process, on every backend and in the test
///   double: there is no caller to hand the exception to.
/// - It must not destroy a monitor, its own or another (macOS waits for its queue from the queue, Linux
///   joins its own thread, Windows takes a process-wide lock the handler already holds), nor create one
///   (Windows takes that lock again).
/// - It must never block waiting on a thread that may be destroying its subscription or its monitor:
///   both wait for a running handler to return, on every backend.
/// - It should return promptly and marshal the real work onto the consumer's own loop. On macOS the
///   system does not sleep until every WillSleep handler has returned (up to IOKit's 30 s timeout).
///
/// To implement this interface, dispatch through detail::PowerHandlerRegistry: it is the supported way,
/// as PowerSubscription's constructor is reachable only through it.
class ISystemPowerMonitor
{
  public:
    /// Receives each power event. Must not be empty, and must not throw (see the class comment).
    using Handler = std::function<void(SystemPowerEvent)>;

    ISystemPowerMonitor() = default;
    ISystemPowerMonitor(ISystemPowerMonitor const&) = delete;
    ISystemPowerMonitor(ISystemPowerMonitor&&) = delete;
    ISystemPowerMonitor& operator=(ISystemPowerMonitor const&) = delete;
    ISystemPowerMonitor& operator=(ISystemPowerMonitor&&) = delete;
    virtual ~ISystemPowerMonitor() = default;

    /// Registers a handler for power events.
    /// Handlers run on a platform thread that is not the caller's; marshal to your own loop. A handler
    /// subscribed from inside another handler first sees the event after the one being dispatched.
    /// @param handler Called for every event until the returned token is destroyed. Must not be empty.
    /// @return The token that keeps the handler subscribed.
    [[nodiscard]] virtual PowerSubscription subscribe(Handler handler) = 0;
};

namespace detail
{
    /// Thread-safe handler table every ISystemPowerMonitor implementation dispatches through, and the
    /// supported way to write one: PowerSubscription is constructible only from here.
    /// Tokens it hands out keep its state alive, so they may safely outlive the registry.
    class PowerHandlerRegistry
    {
      public:
        PowerHandlerRegistry();

        /// Adds a handler.
        /// @param handler Called by dispatch() until the returned token is destroyed. Must not be empty
        ///                (a precondition, checked with Require()).
        /// @return The token that removes the handler again.
        [[nodiscard]] PowerSubscription subscribe(ISystemPowerMonitor::Handler handler);

        /// Calls every live handler, in subscription order, on the calling thread.
        ///
        /// A handler may subscribe or unsubscribe, its own subscription included; one subscribed during the
        /// dispatch is not called for this event. A handler may also destroy the registry's owner: the
        /// state stays alive until the dispatch has returned.
        ///
        /// `noexcept` is the exception policy of every monitor: a handler that throws terminates the
        /// process rather than unwinding into the operating system's callback, which is the trade
        /// core::async::DetachedTask and core::net::EventLoop::onHostPump make, for the same reason.
        /// @param event What happened.
        void dispatch(SystemPowerEvent event) noexcept;

        /// The number of live subscriptions.
        [[nodiscard]] std::size_t size() const;

      private:
        std::shared_ptr<PowerHandlerRegistryState> _state;
    };
} // namespace detail

/// Creates the platform's system power monitor.
/// @return The monitor, or the reason this platform (or session) cannot report power events;
///         PlatformError::NotImplemented where there is no backend.
[[nodiscard]] std::expected<std::unique_ptr<ISystemPowerMonitor>, PlatformError>
makeNativeSystemPowerMonitor();

} // namespace core::platform
