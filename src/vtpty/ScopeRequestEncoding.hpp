// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/ScopeBus.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

/// @file
/// The decisions SdBusScopeBus makes, without sd-bus: which properties a scope request carries, and
/// what a failed call means. Pure, so both are table-tested.

namespace vtpty
{

/// `PIDs=[pid]`.
struct PidList
{
    std::uint32_t pid;
};

/// `PIDFDs=[pidfd]`.
struct PidFdList
{
    int pidfd;
};

/// One property of a StartTransientUnit request: D-Bus `(sv)`.
struct ScopeProperty
{
    std::string_view name;
    std::variant<std::string, std::uint64_t, PidList, PidFdList> value;
};

/// @param request The scope to ask for.
/// @return The properties to send for it, in the protocol it names.
[[nodiscard]] std::vector<ScopeProperty> scopeProperties(ScopeRequest const& request);

/// @param returnCode The negative errno a failed sd-bus call returned.
/// @param errorName  The D-Bus error name it answered with, or empty when there is none.
/// @return Why the scope was not created.
[[nodiscard]] ScopeError classifyScopeFailure(int returnCode, std::string_view errorName) noexcept;

} // namespace vtpty
