// SPDX-License-Identifier: Apache-2.0
#include <vthost/handover/HandoverToken.hpp>

#include <core/Utils.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <ranges>

namespace vthost::handover
{

namespace
{
    /// What a POSIX token starts with, the descriptor following it.
    constexpr auto DescriptorKey = std::string_view { "fd=" };

    /// What a Windows token starts with, the handles following it.
    constexpr auto HandlesKey = std::string_view { "handles=" };

    /// What separates the handles in a token.
    constexpr auto HandleSeparator = ',';

    /// The lowest descriptor a token may name: 0 to 2 are the daemon's own standard streams.
    constexpr auto LowestHandedDescriptor = std::int64_t { 3 };

    /// The highest descriptor a token may name: a descriptor is an int.
    constexpr auto HighestHandedDescriptor = std::int64_t { std::numeric_limits<int>::max() };

    /// @param text A handle's number, in decimal.
    /// @return The handle; nullopt for text that is no plain decimal number, or one too large.
    [[nodiscard]] std::optional<vtpty::CrossingHandle> handleOf(std::string_view text) noexcept
    {
        return core::toInteger<10, std::int64_t>(text).transform(
            [](std::int64_t value) { return vtpty::CrossingHandle { value }; });
    }
} // namespace

std::string handoverToken(vtpty::CrossingHandle memory)
{
    return std::format("{}{}", DescriptorKey, memory.value);
}

std::string handoverToken(SuccessorHandles const& handles)
{
    return std::format("{}{}{}{}{}{}{}{}",
                       HandlesKey,
                       handles.image.value,
                       HandleSeparator,
                       handles.ready.value,
                       HandleSeparator,
                       handles.commit.value,
                       HandleSeparator,
                       handles.predecessor.value);
}

std::string handoverArgument(std::string_view token)
{
    return std::format("{}{}", HandoverPrefix, token);
}

std::optional<std::string_view> handoverTokenOf(std::string_view argument) noexcept
{
    if (!argument.starts_with(HandoverPrefix))
        return std::nullopt;
    return argument.substr(HandoverPrefix.size());
}

std::optional<vtpty::CrossingHandle> parseHandoverDescriptor(std::string_view token) noexcept
{
    if (!token.starts_with(DescriptorKey))
        return std::nullopt;
    auto const handle = handleOf(token.substr(DescriptorKey.size()));
    if (!handle || handle->value < LowestHandedDescriptor || handle->value > HighestHandedDescriptor)
        return std::nullopt;
    return handle;
}

std::optional<SuccessorHandles> parseHandoverHandles(std::string_view token) noexcept
{
    if (!token.starts_with(HandlesKey))
        return std::nullopt;
    auto values = std::array<vtpty::CrossingHandle, 4> {};
    auto count = std::size_t { 0 };
    for (auto const piece: std::views::split(token.substr(HandlesKey.size()), HandleSeparator))
    {
        if (count == values.size())
            return std::nullopt;
        auto const handle = handleOf(std::string_view { piece.begin(), piece.end() });
        if (!handle || handle->value <= 0)
            return std::nullopt;
        values.at(count++) = *handle;
    }
    if (count != values.size())
        return std::nullopt;
    return SuccessorHandles {
        .image = values[0], .ready = values[1], .commit = values[2], .predecessor = values[3]
    };
}

} // namespace vthost::handover
