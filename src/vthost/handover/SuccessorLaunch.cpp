// SPDX-License-Identifier: Apache-2.0
#include <vthost/handover/SuccessorLaunch.hpp>

#include <array>
#include <charconv>
#include <cstddef>
#include <format>
#include <ranges>
#include <system_error>

namespace vthost::handover
{

namespace
{
    /// What separates the handles in `--handover-handles=`.
    constexpr auto HandleSeparator = ',';
} // namespace

std::string handoverHandlesArgument(SuccessorHandles const& handles)
{
    return std::format("{}{}{}{}{}{}{}{}",
                       HandoverHandlesPrefix,
                       handles.image,
                       HandleSeparator,
                       handles.ready,
                       HandleSeparator,
                       handles.commit,
                       HandleSeparator,
                       handles.predecessor);
}

std::optional<SuccessorHandles> parseHandoverHandles(std::string_view value)
{
    auto values = std::array<vtpty::HandleValue, 4> {};
    auto count = std::size_t { 0 };
    for (auto const piece: std::views::split(value, HandleSeparator))
    {
        auto const text = std::string_view { piece.begin(), piece.end() };
        if (count == values.size())
            return std::nullopt;
        auto& handle = values.at(count++);
        auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), handle);
        if (error != std::errc {} || end != text.data() + text.size() || handle == 0)
            return std::nullopt;
    }
    if (count != values.size())
        return std::nullopt;
    return SuccessorHandles {
        .image = values[0], .ready = values[1], .commit = values[2], .predecessor = values[3]
    };
}

} // namespace vthost::handover
