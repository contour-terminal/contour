// SPDX-License-Identifier: Apache-2.0
#include <core/platform/SystemPower.hpp>
#include <core/platform/detail/PowerBroadcastPlan.hpp>

#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

// powrprof.h needs the types of windows.h but does not include it, so the order is not alphabetical.
// clang-format off
#include <windows.h>
#include <powrprof.h>
// clang-format on

namespace core::platform
{

namespace
{
    static_assert(detail::PowerBroadcastSuspend == PBT_APMSUSPEND);
    static_assert(detail::PowerBroadcastResumeSuspend == PBT_APMRESUMESUSPEND);
    static_assert(detail::PowerBroadcastResumeAutomatic == PBT_APMRESUMEAUTOMATIC);

    class WindowsSystemPowerMonitor;

    /// The monitors that can still be called back, by the opaque id given to the power manager as the
    /// callback context. The callback never dereferences a pointer the power manager hands back; it looks
    /// the id up here, so a callback that arrives after its monitor is gone finds nothing.
    struct LiveMonitors
    {
        std::mutex mutex;
        std::unordered_map<std::uintptr_t, WindowsSystemPowerMonitor*> byId;
        std::atomic<std::uintptr_t> lastId = 0;
    };

    [[nodiscard]] LiveMonitors& liveMonitors()
    {
        // Leaked on purpose: the power manager may still call back on its own thread while the process
        // exits, after the static destructors have run, and must then find a live mutex and map, not
        // destroyed ones.
        static auto* const monitors = new LiveMonitors {};
        return *monitors;
    }

    /// System sleep/wake notifications from the Windows power manager, delivered on a thread the power
    /// manager owns.
    ///
    /// A handler runs while a process-wide lock is held, so it must not destroy a monitor (this one or
    /// another): the destructor takes that lock.
    class WindowsSystemPowerMonitor final: public ISystemPowerMonitor
    {
      public:
        WindowsSystemPowerMonitor() = default;
        WindowsSystemPowerMonitor(WindowsSystemPowerMonitor const&) = delete;
        WindowsSystemPowerMonitor(WindowsSystemPowerMonitor&&) = delete;
        WindowsSystemPowerMonitor& operator=(WindowsSystemPowerMonitor const&) = delete;
        WindowsSystemPowerMonitor& operator=(WindowsSystemPowerMonitor&&) = delete;

        /// Cancels the registration, then removes the monitor from the live set.
        ///
        /// Microsoft does not document that PowerUnregisterSuspendResumeNotification waits for a callback
        /// that is running, and this does not rely on it. A callback dispatches while holding the lock of
        /// the live set, and the removal below takes the same lock, so it returns only after a dispatch in
        /// progress has finished; a callback arriving later looks its id up, finds nothing and returns.
        ~WindowsSystemPowerMonitor() override
        {
            if (_registration != nullptr)
                PowerUnregisterSuspendResumeNotification(_registration);
            if (_id != 0)
            {
                auto& live = liveMonitors();
                auto const lock = std::scoped_lock { live.mutex };
                live.byId.erase(_id);
            }
        }

        /// Registers with the power manager.
        /// @return Whether the registration succeeded.
        [[nodiscard]] std::expected<void, PlatformError> start()
        {
            auto& live = liveMonitors();
            _id = ++live.lastId;
            {
                auto const lock = std::scoped_lock { live.mutex };
                live.byId.emplace(_id, this);
            }
            _parameters.Callback = &onBroadcast;
            _parameters.Context = reinterpret_cast<PVOID>(_id);
            if (PowerRegisterSuspendResumeNotification(
                    DEVICE_NOTIFY_CALLBACK, reinterpret_cast<HANDLE>(&_parameters), &_registration)
                != ERROR_SUCCESS)
            {
                _registration = nullptr; // the destructor removes the id
                return std::unexpected(PlatformError::NotificationRegistrationFailed);
            }
            return {};
        }

        [[nodiscard]] PowerSubscription subscribe(Handler handler) override
        {
            return _registry.subscribe(std::move(handler));
        }

      private:
        static ULONG CALLBACK onBroadcast(PVOID context, ULONG type, PVOID /*setting*/) noexcept
        {
            auto const event = detail::eventForPowerBroadcast(type);
            if (!event)
                return ERROR_SUCCESS;
            auto& live = liveMonitors();
            auto const lock = std::scoped_lock { live.mutex };
            auto const found = live.byId.find(reinterpret_cast<std::uintptr_t>(context));
            if (found == live.byId.end())
                return ERROR_SUCCESS;
            // dispatch() is noexcept: a throwing handler terminates rather than unwinding into the power
            // manager.
            found->second->_registry.dispatch(*event);
            return ERROR_SUCCESS;
        }

        // Declared first, so it outlives the registration that dispatches into it.
        detail::PowerHandlerRegistry _registry;
        std::uintptr_t _id = 0;
        DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS _parameters {};
        HPOWERNOTIFY _registration = nullptr;
    };
} // namespace

std::expected<std::unique_ptr<ISystemPowerMonitor>, PlatformError> makeNativeSystemPowerMonitor()
{
    auto monitor = std::make_unique<WindowsSystemPowerMonitor>();
    if (auto started = monitor->start(); !started)
        return std::unexpected(started.error());
    return monitor;
}

} // namespace core::platform
