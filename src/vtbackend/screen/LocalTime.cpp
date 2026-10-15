// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/LocalTime.hpp>

#include <ctime>

namespace vtbackend
{

std::chrono::local_seconds systemLocalTime(std::chrono::system_clock::time_point when) noexcept
{
    namespace chrono = std::chrono;

    // Through the C library rather than std::chrono::current_zone(): the time-zone database is not
    // available in every standard library this builds against (Apple's libc++ has none), while
    // localtime_r/localtime_s are. They also honour TZ, which current_zone() ignores on some libraries.
    auto const whole = chrono::floor<chrono::seconds>(when);
    auto const asUtc = chrono::local_seconds { whole.time_since_epoch() };
    auto const timeT = chrono::system_clock::to_time_t(whole);
    auto civil = std::tm {};
#ifdef _WIN32
    if (localtime_s(&civil, &timeT) != 0)
        return asUtc;
#else
    if (localtime_r(&timeT, &civil) == nullptr)
        return asUtc;
#endif
    auto const date = chrono::year { civil.tm_year + 1900 }
                      / chrono::month { static_cast<unsigned>(civil.tm_mon + 1) }
                      / chrono::day { static_cast<unsigned>(civil.tm_mday) };
    return chrono::local_days { date } + chrono::hours { civil.tm_hour } + chrono::minutes { civil.tm_min }
           + chrono::seconds { civil.tm_sec };
}

} // namespace vtbackend
