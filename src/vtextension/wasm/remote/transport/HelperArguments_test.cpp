// SPDX-License-Identifier: Apache-2.0
//
// The command line that names, to the extension host process, the handles it inherited: what is formatted
// parses back to the same values, and every way a command line can be wrong is refused for its reason.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vtextension/wasm/remote/transport/HelperArguments.hpp>

using vtextension::wasm::remote::transport::ArgumentError;
using vtextension::wasm::remote::transport::formatHelperArguments;
using vtextension::wasm::remote::transport::HelperArguments;
using vtextension::wasm::remote::transport::NativeHandle;
using vtextension::wasm::remote::transport::parseHelperArguments;

namespace
{

/// @return @p number as a handle value of this platform.
[[nodiscard]] NativeHandle handleOf(uint64_t number) noexcept
{
#ifdef _WIN32
    return std::bit_cast<NativeHandle>(static_cast<uintptr_t>(number));
#else
    return static_cast<NativeHandle>(number);
#endif
}

/// @return @p arguments parsed, as the views a process's argv gives.
[[nodiscard]] std::expected<HelperArguments, ArgumentError> parse(std::vector<std::string> const& arguments)
{
    auto views = std::vector<std::string_view> {};
    for (auto const& argument: arguments)
        views.emplace_back(argument);
    return parseHelperArguments(std::span<std::string_view const> { views });
}

/// @return A complete, valid command line: what the rows below each spoil in one way.
[[nodiscard]] std::vector<std::string> validArguments()
{
    return formatHelperArguments(HelperArguments {
        .control = handleOf(3),
        .lanes = { handleOf(4), handleOf(5), handleOf(6), handleOf(7) },
        .controlPage = handleOf(8),
        .wake = handleOf(9),
        .parentProcessId = 4242,
        .dataBytes = 1024,
    });
}

/// @return validArguments() with @p option in place of the one of its name, or appended if none has it.
[[nodiscard]] std::vector<std::string> validWith(std::string const& option)
{
    auto arguments = validArguments();
    auto const prefix = option.substr(0, option.find('=') + 1);
    auto const same = std::ranges::find_if(
        arguments, [&](std::string const& argument) { return argument.starts_with(prefix); });
    if (same == arguments.end())
        arguments.push_back(option);
    else
        *same = option;
    return arguments;
}

/// @return validArguments() without the option whose `--name=` prefix is @p prefix.
[[nodiscard]] std::vector<std::string> validWithout(std::string_view prefix)
{
    auto arguments = validArguments();
    std::erase_if(arguments, [&](std::string const& argument) { return argument.starts_with(prefix); });
    return arguments;
}

/// @return validArguments() with @p option once more at its end.
[[nodiscard]] std::vector<std::string> validAnd(std::string const& option)
{
    auto arguments = validArguments();
    arguments.push_back(option);
    return arguments;
}

} // namespace

TEST_CASE("HelperArguments.roundTrip", "[vtextension]")
{
    SECTION("POSIX-style numbers")
    {
        auto const arguments =
            HelperArguments { .control = handleOf(3),
                              .lanes = { handleOf(4), handleOf(5), handleOf(6), handleOf(7) },
                              .controlPage = handleOf(8),
                              .wake = handleOf(9),
                              .parentProcessId = 12345,
                              .dataBytes = uint64_t { 1 } << 30 };
        CHECK(parse(formatHelperArguments(arguments)) == arguments);
    }
    SECTION("large values, and no cap")
    {
#ifdef _WIN32
        // Handle values as Windows hands them out: pointer-sized, beyond 32 bits.
        constexpr auto Large = uint64_t { 0x7fff'0000'0000 };
#else
        // A descriptor is an int.
        constexpr auto Large = uint64_t { 0x7fff'0000 };
#endif
        auto const arguments = HelperArguments {
            .control = handleOf(Large + 0x1234),
            .lanes = { handleOf(0x2a4), handleOf(0x2a8), handleOf(Large + 0xfff0), handleOf(Large + 4) },
            .controlPage = handleOf(Large + 0xfffc),
            .wake = handleOf(0x10),
            .parentProcessId = 0x7fff'ffff'ffff,
            .dataBytes = 0
        };
        CHECK(parse(formatHelperArguments(arguments)) == arguments);
    }
}

TEST_CASE("HelperArguments.rejects", "[vtextension]")
{
    struct Row
    {
        std::string_view name;
        std::vector<std::string> arguments;
        ArgumentError error;
    };
    auto const rows = std::vector<Row> {
        { .name = "an unknown option", .arguments = validAnd("--bogus=1"), .error = ArgumentError::Unknown },
        { .name = "a repeated option", .arguments = validAnd("--wake=9"), .error = ArgumentError::Duplicate },
        { .name = "a missing option", .arguments = validWithout("--wake="), .error = ArgumentError::Missing },
        { .name = "too few lanes",
          .arguments = validWith("--lanes=4,5,6"),
          .error = ArgumentError::Malformed },
        { .name = "a parent that is no number",
          .arguments = validWith("--parent=x"),
          .error = ArgumentError::Malformed },
        { .name = "two roles on one handle",
          .arguments = validWith("--wake=3"),
          .error = ArgumentError::Malformed },
#ifndef _WIN32
        { .name = "a standard stream as a handle",
          .arguments = validWith("--page=2"),
          .error = ArgumentError::Malformed },
#endif
        { .name = "a negative cap",
          .arguments = validWith("--data-bytes=-1"),
          .error = ArgumentError::Malformed },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        CHECK(parse(row.arguments) == std::unexpected(row.error));
    }
}
