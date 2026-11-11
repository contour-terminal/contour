// SPDX-License-Identifier: Apache-2.0
#include <vthost/ReleaseNumbers.hpp>

#include <algorithm>
#include <charconv>
#include <ranges>

namespace vthost
{

std::optional<std::vector<std::uint32_t>> parseReleaseNumbers(std::string_view release)
{
    auto numbers = std::vector<std::uint32_t> {};
    while (!release.empty() && release.front() >= '0' && release.front() <= '9')
    {
        auto value = std::uint32_t {};
        auto const [end, error] = std::from_chars(release.data(), release.data() + release.size(), value);
        if (error != std::errc {})
            return std::nullopt;
        numbers.push_back(value);
        release.remove_prefix(static_cast<size_t>(end - release.data()));
        if (release.size() >= 2 && release.front() == '.' && release[1] >= '0' && release[1] <= '9')
            release.remove_prefix(1);
        else
            break;
    }
    if (numbers.empty())
        return std::nullopt;
    return numbers;
}

std::optional<std::strong_ordering> compareReleaseNumbers(std::string_view a, std::string_view b)
{
    auto const first = parseReleaseNumbers(a);
    auto const second = parseReleaseNumbers(b);
    if (!first || !second)
        return std::nullopt;
    auto const width = std::max(first->size(), second->size());
    for (auto const index: std::views::iota(size_t { 0 }, width))
    {
        auto const x = index < first->size() ? (*first)[index] : std::uint32_t { 0 };
        auto const y = index < second->size() ? (*second)[index] : std::uint32_t { 0 };
        if (auto const order = x <=> y; std::is_neq(order))
            return order;
    }
    return std::strong_ordering::equal;
}

} // namespace vthost
