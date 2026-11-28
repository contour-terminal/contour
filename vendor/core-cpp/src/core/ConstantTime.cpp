// SPDX-License-Identifier: Apache-2.0
#include <core/ConstantTime.hpp>

#include <core/ValueBarrier.hpp>

#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>

namespace core
{

bool constantTimeEquals(std::span<std::byte const> lhs, std::span<std::byte const> rhs) noexcept
{
    // The length is public (see the header): only the bytes are compared in constant time.
    if (lhs.size() != rhs.size())
        return false;
    auto difference = std::uint8_t { 0 };
    for (auto const index: std::views::iota(std::size_t { 0 }, lhs.size()))
        difference = detail::valueBarrier(
            static_cast<std::uint8_t>(difference | std::to_integer<std::uint8_t>(lhs[index] ^ rhs[index])));
    return detail::valueBarrier(difference) == 0;
}

void secureZero(std::span<std::byte> bytes) noexcept
{
    // Every store through a volatile lvalue is performed as written, needed or not.
    auto* const target = static_cast<std::byte volatile*>(bytes.data());
    for (auto const index: std::views::iota(std::size_t { 0 }, bytes.size()))
        target[index] = std::byte { 0 };
    detail::memoryBarrier(bytes.data());
}

} // namespace core
