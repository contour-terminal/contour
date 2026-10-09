// SPDX-License-Identifier: Apache-2.0
#include <vtpty/SdBusScopeBus.hpp>

#include <vtpty/ScopeRequestEncoding.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <string_view>
#include <type_traits>
#include <variant>

#include <systemd/sd-bus.h>

using namespace std::chrono;

namespace vtpty
{

namespace
{
    constexpr auto SystemdService = "org.freedesktop.systemd1";
    constexpr auto SystemdObject = "/org/freedesktop/systemd1";
    constexpr auto ManagerInterface = "org.freedesktop.systemd1.Manager";
    constexpr auto JobInterface = "org.freedesktop.systemd1.Job";

    /// Owns a reference to an sd_bus_message.
    struct MessageDeleter
    {
        void operator()(sd_bus_message* message) const noexcept { sd_bus_message_unref(message); }
    };
    using Message = std::unique_ptr<sd_bus_message, MessageDeleter>;

    /// Owns an sd_bus_slot; releasing a match's slot removes the match.
    struct SlotDeleter
    {
        void operator()(sd_bus_slot* slot) const noexcept { sd_bus_slot_unref(slot); }
    };
    using Slot = std::unique_ptr<sd_bus_slot, SlotDeleter>;

    /// Owns what an sd_bus_error points to.
    class BusError
    {
      public:
        BusError() = default;
        ~BusError() { sd_bus_error_free(&_error); }
        BusError(BusError const&) = delete;
        BusError& operator=(BusError const&) = delete;
        BusError(BusError&&) = delete;
        BusError& operator=(BusError&&) = delete;

        [[nodiscard]] sd_bus_error* get() noexcept { return &_error; }

        /// @return The D-Bus error name, or empty when there is none.
        [[nodiscard]] std::string_view name() const noexcept
        {
            return _error.name != nullptr ? std::string_view { _error.name } : std::string_view {};
        }

      private:
        sd_bus_error _error {}; // == SD_BUS_ERROR_NULL, which is a C compound literal
    };

    /// @return @p duration in microseconds, as sd-bus counts timeouts; never negative.
    [[nodiscard]] std::uint64_t microsecondsOf(steady_clock::duration duration) noexcept
    {
        return static_cast<std::uint64_t>(
            std::max<std::int64_t>(0, duration_cast<microseconds>(duration).count()));
    }

    /// Appends @p property to @p message as one `(sv)`.
    /// @return What sd-bus returned: negative on failure.
    int appendProperty(sd_bus_message* message, ScopeProperty const& property)
    {
        auto const name = std::string { property.name };
        return std::visit(
            [&](auto const& value) -> int {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, std::string>)
                    return sd_bus_message_append(message, "(sv)", name.c_str(), "s", value.c_str());
                else if constexpr (std::is_same_v<T, std::uint64_t>)
                    return sd_bus_message_append(message, "(sv)", name.c_str(), "t", value);
                else if constexpr (std::is_same_v<T, PidList>)
                    return sd_bus_message_append(message, "(sv)", name.c_str(), "au", 1U, value.pid);
                else
                    return sd_bus_message_append(message, "(sv)", name.c_str(), "ah", 1U, value.pidfd);
            },
            property.value);
    }

    /// The JobRemoved signal handler: hands each removed job's unit and result to the SdBusScopeBus.
    int onJobRemoved(sd_bus_message* message, void* userdata, sd_bus_error* /*error*/)
    {
        auto id = std::uint32_t {};
        char const* job = nullptr;
        char const* unit = nullptr;
        char const* result = nullptr;
        if (sd_bus_message_read(message, "uoss", &id, &job, &unit, &result) >= 0 && unit != nullptr
            && result != nullptr)
            static_cast<SdBusScopeBus*>(userdata)->jobRemoved(unit, result);
        return 0;
    }
} // namespace

void SdBusScopeBus::BusDeleter::operator()(sd_bus* bus) const noexcept
{
    sd_bus_flush_close_unref(bus);
}

SdBusScopeBus::SdBusScopeBus(BusHandle bus, core::platform::IClock const& clock) noexcept:
    _bus { std::move(bus) }, _clock { clock }
{
}

std::expected<std::unique_ptr<ScopeBus>, ScopeError> SdBusScopeBus::connect(
    core::platform::IClock const& clock)
{
    // Only whether there is a user bus at all. Whether systemd is on it is answered by the first
    // request: ServiceUnknown is Unavailable there, and anything else is reported as what it is.
    sd_bus* raw = nullptr;
    if (sd_bus_open_user(&raw) < 0)
        return std::unexpected(ScopeError::Unavailable);
    return std::make_unique<SdBusScopeBus>(BusHandle { raw }, clock);
}

std::expected<void, ScopeError> SdBusScopeBus::startScope(ScopeRequest const& request, milliseconds deadline)
{
    auto const until = _clock.now() + deadline;

    // Hear about this unit's job only, and only for as long as this request lasts. Unfiltered, every
    // JobRemoved systemd broadcasts while some other client subscribes would queue up on this
    // connection between spawns. Installed asynchronously: the bus handles it before the call below.
    auto const rule =
        std::format("type='signal',sender='{}',path='{}',interface='{}',member='JobRemoved',arg2='{}'",
                    SystemdService,
                    SystemdObject,
                    ManagerInterface,
                    request.unitName);
    sd_bus_slot* rawSlot = nullptr;
    if (auto const added =
            sd_bus_add_match_async(_bus.get(), &rawSlot, rule.c_str(), &onJobRemoved, nullptr, this);
        added < 0)
        return std::unexpected(classifyScopeFailure(added, {}));
    auto const match = Slot { rawSlot };

    sd_bus_message* rawMessage = nullptr;
    if (auto const created = sd_bus_message_new_method_call(
            _bus.get(), &rawMessage, SystemdService, SystemdObject, ManagerInterface, "StartTransientUnit");
        created < 0)
        return std::unexpected(classifyScopeFailure(created, {}));
    auto const message = Message { rawMessage };
    auto* const m = message.get();

    // Every append reports failure the same way; collect them rather than branch after each.
    auto appended = true;
    auto const append = [&appended](int returnCode) {
        appended = appended && returnCode >= 0;
    };
    append(sd_bus_message_append(m, "ss", request.unitName.c_str(), "fail"));
    append(sd_bus_message_open_container(m, 'a', "(sv)"));
    for (auto const& property: scopeProperties(request))
        append(appendProperty(m, property));
    append(sd_bus_message_close_container(m));
    append(sd_bus_message_append(m, "a(sa(sv))", 0U));
    if (!appended)
        return std::unexpected(ScopeError::Refused);

    _awaitedUnit = request.unitName;
    _awaitedResult.reset();

    auto error = BusError {};
    sd_bus_message* rawReply = nullptr;
    auto const called =
        sd_bus_call(_bus.get(), m, microsecondsOf(until - _clock.now()), error.get(), &rawReply);
    auto const reply = Message { rawReply };
    // A call that times out before its reply leaves no job path to cancel: systemd may still start
    // the scope, after the child was released. The deadline makes that rare, and the breaker then
    // keeps the next spawns from adding to it.
    if (called < 0)
        return std::unexpected(classifyScopeFailure(called, error.name()));

    char const* job = nullptr;
    if (sd_bus_message_read(reply.get(), "o", &job) < 0 || job == nullptr)
        return std::unexpected(ScopeError::Refused);
    auto const jobPath = std::string { job };

    auto awaited = awaitJob(until);
    if (!awaited && awaited.error() == ScopeError::TimedOut)
        cancelJob(jobPath);
    return awaited;
}

std::expected<void, ScopeError> SdBusScopeBus::awaitJob(core::platform::SteadyTimePoint until)
{
    while (!_awaitedResult)
    {
        auto const processed = sd_bus_process(_bus.get(), nullptr);
        if (processed < 0)
            return std::unexpected(ScopeError::Disconnected);
        if (processed > 0)
            continue; // something was dispatched; it may have been our job
        auto const now = _clock.now();
        if (now >= until)
            return std::unexpected(ScopeError::TimedOut);
        if (sd_bus_wait(_bus.get(), microsecondsOf(until - now)) < 0)
            return std::unexpected(ScopeError::Disconnected);
    }
    if (*_awaitedResult != "done")
        return std::unexpected(ScopeError::Refused);
    return {};
}

void SdBusScopeBus::cancelJob(std::string const& jobPath) noexcept
{
    // The child is released unplaced. Were the job to finish now, the scope would take the shell but
    // not what it forked meanwhile, splitting the session across two cgroups. Fire and forget: the
    // child goes either way, and a job that already finished simply refuses.
    (void) sd_bus_call_method_async(
        _bus.get(), nullptr, SystemdService, jobPath.c_str(), JobInterface, "Cancel", nullptr, nullptr, "");
    (void) sd_bus_flush(_bus.get());
}

void SdBusScopeBus::jobRemoved(std::string_view unit, std::string_view result)
{
    if (unit == _awaitedUnit)
        _awaitedResult = std::string { result };
}

std::shared_ptr<ProcessPlacement> makeSystemdScopePlacement(SystemdScopeConfig config)
{
    auto& clock = core::platform::defaultSteadyClock();
    return std::make_shared<SystemdScopePlacement>(
        std::move(config), [&clock] { return SdBusScopeBus::connect(clock); }, clock);
}

} // namespace vtpty
