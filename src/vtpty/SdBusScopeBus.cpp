// SPDX-License-Identifier: Apache-2.0
#include <vtpty/SdBusScopeBus.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <ranges>
#include <string_view>

#include <systemd/sd-bus.h>

using namespace std::chrono;

namespace vtpty
{

namespace
{
    constexpr auto SystemdService = "org.freedesktop.systemd1";
    constexpr auto SystemdObject = "/org/freedesktop/systemd1";
    constexpr auto ManagerInterface = "org.freedesktop.systemd1.Manager";

    /// Owns a reference to an sd_bus_message.
    struct MessageDeleter
    {
        void operator()(sd_bus_message* message) const noexcept { sd_bus_message_unref(message); }
    };
    using Message = std::unique_ptr<sd_bus_message, MessageDeleter>;

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
        [[nodiscard]] bool hasName(char const* name) const noexcept
        {
            return sd_bus_error_has_name(&_error, name) != 0;
        }

      private:
        sd_bus_error _error {}; // == SD_BUS_ERROR_NULL, which is a C compound literal
    };

    /// A return code of a failed call, and what it says about the scope.
    struct ReturnCodeMeaning
    {
        int returnCode;
        ScopeError error;
    };

    constexpr auto ReturnCodeMeanings = std::array {
        ReturnCodeMeaning { -ETIMEDOUT, ScopeError::TimedOut },
        ReturnCodeMeaning { -ECONNRESET, ScopeError::Disconnected },
        ReturnCodeMeaning { -ENOTCONN, ScopeError::Disconnected },
        ReturnCodeMeaning { -EPIPE, ScopeError::Disconnected },
        ReturnCodeMeaning { -ESHUTDOWN, ScopeError::Disconnected },
    };

    /// The D-Bus errors systemd answers a property it does not know with: PropertyReadOnly ("Cannot
    /// set property PIDFDs, or unknown property."), or InvalidArgs in some versions.
    constexpr auto UnknownPropertyErrors =
        std::array { SD_BUS_ERROR_PROPERTY_READ_ONLY, SD_BUS_ERROR_INVALID_ARGS };

    /// @return Why a call that returned @p returnCode with @p error did not create the scope.
    [[nodiscard]] ScopeError classify(int returnCode, BusError const& error) noexcept
    {
        if (auto const known =
                std::ranges::find(ReturnCodeMeanings, returnCode, &ReturnCodeMeaning::returnCode);
            known != ReturnCodeMeanings.end())
            return known->error;
        if (std::ranges::any_of(UnknownPropertyErrors, [&](char const* name) { return error.hasName(name); }))
            return ScopeError::UnknownProperty;
        return ScopeError::Refused;
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

    /// @return @p duration in microseconds, as sd-bus counts timeouts; never negative.
    [[nodiscard]] std::uint64_t microsecondsOf(steady_clock::duration duration) noexcept
    {
        return static_cast<std::uint64_t>(
            std::max<std::int64_t>(0, duration_cast<microseconds>(duration).count()));
    }
} // namespace

void SdBusScopeBus::BusDeleter::operator()(sd_bus* bus) const noexcept
{
    sd_bus_flush_close_unref(bus);
}

SdBusScopeBus::SdBusScopeBus(BusHandle bus) noexcept: _bus { std::move(bus) }
{
}

std::expected<std::unique_ptr<ScopeBus>, ScopeError> SdBusScopeBus::connect(milliseconds timeout)
{
    sd_bus* raw = nullptr;
    if (sd_bus_open_user(&raw) < 0)
        return std::unexpected(ScopeError::Unavailable);
    auto handle = BusHandle { raw };
    if (sd_bus_set_method_call_timeout(raw, microsecondsOf(timeout)) < 0)
        return std::unexpected(ScopeError::Unavailable);

    // JobRemoved is how a request learns that its scope exists. Subscribing also proves that a
    // systemd instance is on the bus: without one it fails here, not at the first session.
    auto error = BusError {};
    if (sd_bus_call_method(
            raw, SystemdService, SystemdObject, ManagerInterface, "Subscribe", error.get(), nullptr, "")
        < 0)
        return std::unexpected(ScopeError::Unavailable);

    auto bus = std::make_unique<SdBusScopeBus>(std::move(handle));
    if (sd_bus_match_signal(raw,
                            nullptr,
                            SystemdService,
                            SystemdObject,
                            ManagerInterface,
                            "JobRemoved",
                            &onJobRemoved,
                            bus.get())
        < 0)
        return std::unexpected(ScopeError::Unavailable);
    return bus;
}

std::expected<void, ScopeError> SdBusScopeBus::startScope(ScopeRequest const& request, milliseconds deadline)
{
    auto const until = steady_clock::now() + deadline;

    sd_bus_message* rawMessage = nullptr;
    if (sd_bus_message_new_method_call(
            _bus.get(), &rawMessage, SystemdService, SystemdObject, ManagerInterface, "StartTransientUnit")
        < 0)
        return std::unexpected(ScopeError::Disconnected);
    auto const message = Message { rawMessage };
    auto* const m = message.get();

    // Every append reports failure the same way; collect them rather than branch after each.
    auto appended = true;
    auto const append = [&appended](int returnCode) {
        appended = appended && returnCode >= 0;
    };
    append(sd_bus_message_append(m, "ss", request.unitName.c_str(), "fail"));
    append(sd_bus_message_open_container(m, 'a', "(sv)"));
    append(sd_bus_message_append(m, "(sv)", "Description", "s", request.description.c_str()));
    append(sd_bus_message_append(m, "(sv)", "Slice", "s", request.slice.c_str()));
    append(sd_bus_message_append(m, "(sv)", "OOMPolicy", "s", "continue"));
    append(sd_bus_message_append(m, "(sv)", "CollectMode", "s", "inactive-or-failed"));
    switch (request.reference)
    {
        case ProcessReference::Pid:
            append(
                sd_bus_message_append(m, "(sv)", "PIDs", "au", 1U, static_cast<std::uint32_t>(request.pid)));
            break;
        case ProcessReference::PidFd:
            append(sd_bus_message_append(m, "(sv)", "PIDFDs", "ah", 1U, request.pidfd));
            break;
    }
    if (request.memoryLimit)
    {
        append(sd_bus_message_append(m, "(sv)", "MemoryMax", "t", request.memoryLimit->maxBytes));
        append(sd_bus_message_append(m, "(sv)", "MemorySwapMax", "t", request.memoryLimit->swapMaxBytes));
    }
    append(sd_bus_message_close_container(m));
    append(sd_bus_message_append(m, "a(sa(sv))", 0U));
    if (!appended)
        return std::unexpected(ScopeError::Refused);

    _awaitedUnit = request.unitName;
    _awaitedResult.reset();

    auto error = BusError {};
    sd_bus_message* rawReply = nullptr;
    auto const returnCode =
        sd_bus_call(_bus.get(), m, microsecondsOf(until - steady_clock::now()), error.get(), &rawReply);
    auto const reply = Message { rawReply };
    if (returnCode < 0)
        return std::unexpected(classify(returnCode, error));
    return awaitJob(until);
}

std::expected<void, ScopeError> SdBusScopeBus::awaitJob(steady_clock::time_point until)
{
    while (!_awaitedResult)
    {
        auto const processed = sd_bus_process(_bus.get(), nullptr);
        if (processed < 0)
            return std::unexpected(ScopeError::Disconnected);
        if (processed > 0)
            continue; // something was dispatched; it may have been our job
        auto const now = steady_clock::now();
        if (now >= until)
            return std::unexpected(ScopeError::TimedOut);
        if (sd_bus_wait(_bus.get(), microsecondsOf(until - now)) < 0)
            return std::unexpected(ScopeError::Disconnected);
    }
    if (*_awaitedResult != "done")
        return std::unexpected(ScopeError::Refused);
    return {};
}

void SdBusScopeBus::jobRemoved(std::string_view unit, std::string_view result)
{
    if (unit == _awaitedUnit)
        _awaitedResult = std::string { result };
}

} // namespace vtpty
