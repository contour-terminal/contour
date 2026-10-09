// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ScopeRequestEncoding.hpp>

#include <algorithm>
#include <array>
#include <cerrno>

namespace vtpty
{

namespace
{
    /// A return code of a failed call, and what it says about the scope.
    struct ReturnCodeMeaning
    {
        int returnCode;
        ScopeError error;
    };

    constexpr auto ReturnCodeMeanings = std::array {
        ReturnCodeMeaning { .returnCode = -ETIMEDOUT, .error = ScopeError::TimedOut },
        ReturnCodeMeaning { .returnCode = -ECONNRESET, .error = ScopeError::Disconnected },
        ReturnCodeMeaning { .returnCode = -ENOTCONN, .error = ScopeError::Disconnected },
        ReturnCodeMeaning { .returnCode = -EPIPE, .error = ScopeError::Disconnected },
        ReturnCodeMeaning { .returnCode = -ESHUTDOWN, .error = ScopeError::Disconnected },
        // The connection cannot pass file descriptors: PIDFDs cannot work, PIDs can.
        ReturnCodeMeaning { .returnCode = -EOPNOTSUPP, .error = ScopeError::Unsupported },
    };

    /// A D-Bus error name a failed call answered with, and what it says about the scope.
    struct ErrorNameMeaning
    {
        std::string_view name;
        ScopeError error;
    };

    constexpr auto ErrorNameMeanings = std::array {
        // Nobody owns org.freedesktop.systemd1 on this bus: there is no systemd user instance.
        ErrorNameMeaning { .name = "org.freedesktop.DBus.Error.ServiceUnknown",
                           .error = ScopeError::Unavailable },
        ErrorNameMeaning { .name = "org.freedesktop.DBus.Error.NameHasNoOwner",
                           .error = ScopeError::Unavailable },
        // systemd's answer to a property it does not know: "Cannot set property ..., or unknown
        // property." InvalidArgs is deliberately absent: it means an invalid request (a unit name,
        // a kernel thread), which another protocol would not fix.
        ErrorNameMeaning { .name = "org.freedesktop.DBus.Error.PropertyReadOnly",
                           .error = ScopeError::Unsupported },
    };
} // namespace

std::vector<ScopeProperty> scopeProperties(ScopeRequest const& request)
{
    auto properties = std::vector<ScopeProperty> {
        { .name = "Description", .value = request.description },
        { .name = "Slice", .value = request.slice },
        { .name = "CollectMode", .value = std::string { "inactive-or-failed" } },
    };
    auto const current = request.protocol == ScopeProtocol::Current;
    if (current)
        properties.push_back({ .name = "OOMPolicy", .value = std::string { "continue" } });
    if (current && request.pidfd >= 0)
        properties.push_back({ .name = "PIDFDs", .value = PidFdList { .pidfd = request.pidfd } });
    else
        properties.push_back(
            { .name = "PIDs", .value = PidList { .pid = static_cast<std::uint32_t>(request.pid) } });
    if (request.memoryLimit)
    {
        properties.push_back({ .name = "MemoryMax", .value = request.memoryLimit->maxBytes });
        properties.push_back({ .name = "MemorySwapMax", .value = request.memoryLimit->swapMaxBytes });
    }
    return properties;
}

ScopeError classifyScopeFailure(int returnCode, std::string_view errorName) noexcept
{
    if (auto const known = std::ranges::find(ReturnCodeMeanings, returnCode, &ReturnCodeMeaning::returnCode);
        known != ReturnCodeMeanings.end())
        return known->error;
    if (auto const known = std::ranges::find(ErrorNameMeanings, errorName, &ErrorNameMeaning::name);
        known != ErrorNameMeanings.end())
        return known->error;
    return ScopeError::Refused;
}

} // namespace vtpty
