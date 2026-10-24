// SPDX-License-Identifier: Apache-2.0
#include <vthost/StdioPreamble.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <ranges>

namespace vthost
{

namespace
{
    /// Every line the relay may begin with.
    constexpr auto PreambleStarts = std::array { StdioPreambleReady, StdioPreambleErrorPrefix };

    /// @return The length of the longest tail of @p received that a preamble line could still begin with.
    [[nodiscard]] std::size_t partialPreambleLength(std::string_view received)
    {
        auto const longest =
            std::ranges::max(PreambleStarts | std::views::transform(&std::string_view::size));
        for (auto const length: std::views::iota(std::size_t { 1 }, std::min(longest, received.size() + 1))
                                    | std::views::reverse)
        {
            auto const tail = received.substr(received.size() - length);
            if (std::ranges::any_of(PreambleStarts,
                                    [&](std::string_view start) { return start.starts_with(tail); }))
                return length;
        }
        return 0;
    }

    /// @return @p scan, unless @p skipped is more junk than the scan limit allows.
    [[nodiscard]] PreambleScan withinLimit(std::size_t skipped, PreambleScan scan)
    {
        if (skipped > StdioPreambleScanLimit)
            return PreambleOverflow {};
        return scan;
    }
} // namespace

PreambleScan scanPreamble(std::string_view received)
{
    auto const ready = received.find(StdioPreambleReady);
    auto const error = received.find(StdioPreambleErrorPrefix);

    if (ready != std::string_view::npos && ready < error)
    {
        auto const extra = received.size() - ready - StdioPreambleReady.size();
        if (extra != 0)
            return withinLimit(ready, PreambleTrailing { .extra = extra });
        return withinLimit(ready, PreambleReady { .skipped = ready });
    }

    if (error != std::string_view::npos)
    {
        auto const messageStart = error + StdioPreambleErrorPrefix.size();
        auto const newline = received.find('\n', messageStart);
        if (newline == std::string_view::npos)
            return withinLimit(error, PreambleIncomplete {});
        return withinLimit(error,
                           PreambleRefused { .message = std::string { received.substr(
                                                 messageStart, newline - messageStart) },
                                             .skipped = error });
    }

    // The limit counts junk, not a preamble that has only begun to arrive.
    return withinLimit(received.size() - partialPreambleLength(received), PreambleIncomplete {});
}

std::string stdioPreambleError(std::string_view message)
{
    auto line = std::string { message };
    std::ranges::replace(line, '\r', ' ');
    std::ranges::replace(line, '\n', ' ');
    return std::format("{}{}\n", StdioPreambleErrorPrefix, line);
}

} // namespace vthost
