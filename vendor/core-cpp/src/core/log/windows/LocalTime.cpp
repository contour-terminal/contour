// SPDX-License-Identifier: Apache-2.0
#include <core/log/detail/LocalTime.hpp>

namespace core::log::detail
{

std::tm localTime(std::time_t time) noexcept
{
    // The arguments are the other way round from localtime_r's, which is the whole reason this
    // is two files rather than one with a branch in it.
    auto result = std::tm {};
    (void) ::localtime_s(&result, &time);
    return result;
}

} // namespace core::log::detail
