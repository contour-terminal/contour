// SPDX-License-Identifier: Apache-2.0
#include <core/platform/SystemPower.hpp>
#include <core/platform/detail/PowerMessagePlan.hpp>

#include <expected>
#include <memory>
#include <utility>

#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <dispatch/dispatch.h>

namespace core::platform
{

namespace
{
    /// Reads IOKit's message number as the vocabulary of the pure planner.
    [[nodiscard]] detail::PowerMessage classify(natural_t messageType) noexcept
    {
        switch (messageType)
        {
            case kIOMessageCanSystemSleep: return detail::PowerMessage::CanSystemSleep;
            case kIOMessageSystemWillSleep: return detail::PowerMessage::WillSleep;
            case kIOMessageSystemWillNotSleep: return detail::PowerMessage::WillNotSleep;
            case kIOMessageSystemHasPoweredOn: return detail::PowerMessage::HasPoweredOn;
            default: return detail::PowerMessage::Other;
        }
    }

    /// System sleep/wake notifications from IOKit, delivered on a private serial dispatch queue.
    class DarwinSystemPowerMonitor final: public ISystemPowerMonitor
    {
      public:
        DarwinSystemPowerMonitor() = default;
        DarwinSystemPowerMonitor(DarwinSystemPowerMonitor const&) = delete;
        DarwinSystemPowerMonitor(DarwinSystemPowerMonitor&&) = delete;
        DarwinSystemPowerMonitor& operator=(DarwinSystemPowerMonitor const&) = delete;
        DarwinSystemPowerMonitor& operator=(DarwinSystemPowerMonitor&&) = delete;

        /// Tears the IOKit registration down on the queue itself, so it cannot overlap a callback: one
        /// that was running has returned, and no later one can reach IOAllowPowerChange on a closed
        /// connection or the registry once it is destroyed. Must not run on the queue (a handler must not
        /// destroy its monitor), as dispatch_sync() onto the current queue deadlocks.
        ~DarwinSystemPowerMonitor() override
        {
            if (_queue == nullptr)
                return;
            dispatch_sync(_queue, ^{
              if (_notifier != IO_OBJECT_NULL)
                  IODeregisterForSystemPower(&_notifier);
              if (_rootPort != MACH_PORT_NULL)
                  IOServiceClose(_rootPort);
              if (_port != nullptr)
                  IONotificationPortDestroy(_port);
            });
            dispatch_release(_queue);
        }

        /// Registers with IOKit.
        /// @return Whether the registration succeeded.
        [[nodiscard]] std::expected<void, PlatformError> start()
        {
            _queue = dispatch_queue_create("core.platform.system-power", DISPATCH_QUEUE_SERIAL);
            if (_queue == nullptr)
                return std::unexpected(PlatformError::NotificationRegistrationFailed);
            _rootPort = IORegisterForSystemPower(this, &_port, &onMessage, &_notifier);
            if (_rootPort == MACH_PORT_NULL)
                return std::unexpected(PlatformError::NotificationRegistrationFailed);
            IONotificationPortSetDispatchQueue(_port, _queue);
            return {};
        }

        [[nodiscard]] PowerSubscription subscribe(Handler handler) override
        {
            return _registry.subscribe(std::move(handler));
        }

      private:
        static void onMessage(void* refcon, io_service_t /*service*/, natural_t messageType, void* argument)
        {
            auto& self = *static_cast<DarwinSystemPowerMonitor*>(refcon);
            auto const action = detail::planPowerMessage(classify(messageType));
            if (action.event)
                self._registry.dispatch(*action.event);
            if (action.acknowledge == detail::PowerAcknowledgement::Yes)
                IOAllowPowerChange(self._rootPort, reinterpret_cast<intptr_t>(argument));
        }

        // Declared first, so it outlives the IOKit registration that dispatches into it.
        detail::PowerHandlerRegistry _registry;
        dispatch_queue_t _queue = nullptr;
        IONotificationPortRef _port = nullptr;
        io_connect_t _rootPort = MACH_PORT_NULL;
        io_object_t _notifier = IO_OBJECT_NULL;
    };
} // namespace

std::expected<std::unique_ptr<ISystemPowerMonitor>, PlatformError> makeNativeSystemPowerMonitor()
{
    auto monitor = std::make_unique<DarwinSystemPowerMonitor>();
    if (auto started = monitor->start(); !started)
        return std::unexpected(started.error());
    return monitor;
}

} // namespace core::platform
