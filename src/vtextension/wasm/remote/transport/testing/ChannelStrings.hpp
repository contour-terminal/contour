// SPDX-License-Identifier: Apache-2.0
//
// Test-only: how a failed check prints a channel's errors and the results that carry them.
#pragma once

#include <catch2/catch_tostring.hpp>

#include <algorithm>
#include <array>
#include <expected>
#include <string>
#include <string_view>
#include <type_traits>

#include <vtextension/wasm/remote/transport/Channel.hpp>

namespace vtextension::wasm::remote::transport::testing
{

/// A ChannelError and its name.
struct ChannelErrorName
{
    ChannelError error;
    std::string_view name;
};

/// Every ChannelError, named as it is spelled.
inline constexpr auto ChannelErrorNames = std::array {
    ChannelErrorName { .error = ChannelError::Closed, .name = "Closed" },
    ChannelErrorName { .error = ChannelError::Io, .name = "Io" },
    ChannelErrorName { .error = ChannelError::Refused, .name = "Refused" },
};

/// @return @p error's name; `ChannelError(<value>)` for a value no row names.
[[nodiscard]] inline std::string nameOf(ChannelError error)
{
    auto const row = std::ranges::find(ChannelErrorNames, error, &ChannelErrorName::error);
    if (row != ChannelErrorNames.end())
        return std::string { row->name };
    return "ChannelError(" + std::to_string(static_cast<int>(error)) + ")";
}

} // namespace vtextension::wasm::remote::transport::testing

/// Prints a ChannelError by its name.
template <>
struct Catch::StringMaker<vtextension::wasm::remote::transport::ChannelError>
{
    /// @return @p error's name.
    static std::string convert(vtextension::wasm::remote::transport::ChannelError error)
    {
        return vtextension::wasm::remote::transport::testing::nameOf(error);
    }
};

/// Prints a channel operation's result: its value, or the ChannelError it failed with.
template <typename T>
struct Catch::StringMaker<std::expected<T, vtextension::wasm::remote::transport::ChannelError>>
{
    /// @return `value <value>` (`value` alone for void), or `error <name>`.
    static std::string convert(
        std::expected<T, vtextension::wasm::remote::transport::ChannelError> const& result)
    {
        if (!result)
            return "error " + vtextension::wasm::remote::transport::testing::nameOf(result.error());
        if constexpr (std::is_void_v<T>)
            return "value";
        else
            return "value " + Catch::StringMaker<T>::convert(*result);
    }
};

/// Prints the failure a check expects: `error <name>`.
template <>
struct Catch::StringMaker<std::unexpected<vtextension::wasm::remote::transport::ChannelError>>
{
    /// @return `error <name>`.
    static std::string convert(
        std::unexpected<vtextension::wasm::remote::transport::ChannelError> const& failure)
    {
        return "error " + vtextension::wasm::remote::transport::testing::nameOf(failure.error());
    }
};
