// SPDX-License-Identifier: Apache-2.0
#include <core/platform/CpuCounters.hpp>
#include <core/platform/detail/UnavailableCpuCounterSource.hpp>

#include <memory>

namespace core::platform
{

std::unique_ptr<ICpuCounterSource> makeDefaultCpuCounterSource()
{
    return std::make_unique<detail::UnavailableCpuCounterSource>(
        "CPU counters are not supported on this platform");
}

} // namespace core::platform
