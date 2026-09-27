// SPDX-License-Identifier: Apache-2.0
#include <core/log/detail/LocalTime.hpp>

namespace core::log::detail
{

std::tm localTime(std::time_t time) noexcept
{
    // Zeroed first, so a conversion that fails answers the same thing on either platform:
    // localtime_r leaves the buffer alone and returns nullptr, where localtime_s zeroes it.
    auto result = std::tm {};
    (void) ::localtime_r(&time, &result);
    return result;
}

} // namespace core::log::detail
