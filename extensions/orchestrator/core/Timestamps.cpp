// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/Timestamps.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string_view>

using namespace std::string_view_literals;

namespace orchestrator
{

namespace
{
    /// The length of `YYYY-MM-DDTHH:MM:SS`, the part every spelling shares.
    constexpr auto BaseLength = std::size_t { 19 };

    /// @return The value of the @p count decimal digits at @p at in @p text; std::nullopt if the text is too
    /// short
    ///         or one of them is not a digit.
    std::optional<unsigned> digitsAt(std::string_view text, std::size_t at, std::size_t count) noexcept
    {
        if (at + count > text.size())
            return std::nullopt;
        auto const digits = text.substr(at, count);
        if (!std::ranges::all_of(digits, [](char c) { return c >= '0' && c <= '9'; }))
            return std::nullopt;
        auto value = 0u;
        for (auto const c: digits)
            value = value * 10 + static_cast<unsigned>(c - '0');
        return value;
    }

    /// @return The offset of the zone @p zone (`Z`, `+HH:MM` or `+HHMM`) from UTC, in minutes; std::nullopt
    /// for
    ///         any other spelling or an hour or minute out of range.
    std::optional<int> offsetMinutesOf(std::string_view zone) noexcept
    {
        if (zone == "Z"sv)
            return 0;
        if (zone.size() != 5 && zone.size() != 6)
            return std::nullopt;
        if (zone[0] != '+' && zone[0] != '-')
            return std::nullopt;
        auto const hasColon = zone.size() == 6;
        if (hasColon && zone[3] != ':')
            return std::nullopt;
        auto const hours = digitsAt(zone, 1, 2);
        auto const minutes = digitsAt(zone, hasColon ? 4 : 3, 2);
        if (!hours || !minutes || *hours > 23 || *minutes > 59)
            return std::nullopt;
        auto const magnitude = static_cast<int>(*hours * 60 + *minutes);
        return zone[0] == '-' ? -magnitude : magnitude;
    }

    /// @return The fraction's milliseconds: the first three digits, padded with zeros; std::nullopt when the
    ///         fraction (the text after the `.`) is empty or holds anything but digits.
    std::optional<unsigned> millisecondsOf(std::string_view fraction) noexcept
    {
        auto const digitCount = fraction.find_first_not_of("0123456789"sv);
        auto const count = digitCount == std::string_view::npos ? fraction.size() : digitCount;
        if (count == 0 || count > 9 || count != fraction.size())
            return std::nullopt;
        auto milliseconds = 0u;
        for (auto const index: std::views::iota(std::size_t { 0 }, std::size_t { 3 }))
        {
            milliseconds *= 10;
            if (index < count)
                milliseconds += static_cast<unsigned>(fraction[index] - '0');
        }
        return milliseconds;
    }
} // namespace

std::optional<uint64_t> unixMsOf(std::string_view text) noexcept
{
    if (text.size() < BaseLength || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':'
        || text[16] != ':')
        return std::nullopt;
    auto const yearValue = digitsAt(text, 0, 4);
    auto const monthValue = digitsAt(text, 5, 2);
    auto const dayValue = digitsAt(text, 8, 2);
    auto const hourValue = digitsAt(text, 11, 2);
    auto const minuteValue = digitsAt(text, 14, 2);
    auto const secondValue = digitsAt(text, 17, 2);
    if (!yearValue || !monthValue || !dayValue || !hourValue || !minuteValue || !secondValue)
        return std::nullopt;
    if (*hourValue > 23 || *minuteValue > 59 || *secondValue > 59)
        return std::nullopt;

    auto const date = std::chrono::year_month_day { std::chrono::year { static_cast<int>(*yearValue) },
                                                    std::chrono::month { *monthValue },
                                                    std::chrono::day { *dayValue } };
    if (!date.ok())
        return std::nullopt;

    auto rest = text.substr(BaseLength);
    auto milliseconds = 0u;
    if (rest.starts_with('.'))
    {
        auto const fractionEnd = rest.find_first_not_of("0123456789"sv, 1);
        auto const fraction =
            rest.substr(1, fractionEnd == std::string_view::npos ? std::string_view::npos : fractionEnd - 1);
        auto const parsed = millisecondsOf(fraction);
        if (!parsed)
            return std::nullopt;
        milliseconds = *parsed;
        rest = fractionEnd == std::string_view::npos ? std::string_view {} : rest.substr(fractionEnd);
    }

    auto const offset = offsetMinutesOf(rest);
    if (!offset)
        return std::nullopt;

    auto const days = std::chrono::sys_days { date }.time_since_epoch().count();
    auto const localSeconds = static_cast<int64_t>(*hourValue) * 3600
                              + static_cast<int64_t>(*minuteValue) * 60 + static_cast<int64_t>(*secondValue);
    auto const unixSeconds = days * 86400 + localSeconds - static_cast<int64_t>(*offset) * 60;
    if (unixSeconds < 0)
        return std::nullopt;
    return static_cast<uint64_t>(unixSeconds) * 1000 + milliseconds;
}

} // namespace orchestrator
