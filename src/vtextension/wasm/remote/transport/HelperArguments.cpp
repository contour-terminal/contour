// SPDX-License-Identifier: Apache-2.0
//
// The helper's command line, one row per option: its name, how its value is read into the arguments, and how
// it is written from them. A new option is a new row; nothing else changes.
#include <vtextension/wasm/remote/transport/HelperArguments.hpp>

#ifndef _WIN32
    #include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <bitset>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace vtextension::wasm::remote::transport
{

namespace
{
    /// @return @p text as a whole decimal number of type T; nothing if it is not one, or out of T's range.
    template <typename T>
    [[nodiscard]] std::expected<T, ArgumentError> numberOf(std::string_view text) noexcept
    {
        auto value = T {};
        auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (text.empty() || error != std::errc {} || end != text.data() + text.size())
            return std::unexpected(ArgumentError::Malformed);
        return value;
    }

    /// @return @p text as a handle; `Malformed` if it is none.
    [[nodiscard]] std::expected<NativeHandle, ArgumentError> handleOf(std::string_view text) noexcept
    {
#ifdef _WIN32
        return numberOf<uintptr_t>(text).transform(
            [](uintptr_t value) { return std::bit_cast<NativeHandle>(value); });
#else
        return numberOf<NativeHandle>(text).and_then(
            [](NativeHandle value) -> std::expected<NativeHandle, ArgumentError> {
                // 0, 1 and 2 are the standard streams, never a handle handed over.
                if (value <= STDERR_FILENO)
                    return std::unexpected(ArgumentError::Malformed);
                return value;
            });
#endif
    }

    /// @return @p handle's decimal value.
    [[nodiscard]] std::string textOf(NativeHandle handle)
    {
#ifdef _WIN32
        return std::to_string(std::bit_cast<uintptr_t>(handle));
#else
        return std::to_string(handle);
#endif
    }

    /// One option: `--name=value`.
    struct Option
    {
        std::string_view name;
        /// Reads @p value into the arguments.
        std::expected<void, ArgumentError> (*parse)(HelperArguments& arguments, std::string_view value);
        /// @return The option's value, from the arguments.
        std::string (*format)(HelperArguments const& arguments);
    };

    /// @return A row's parse for a single handle field.
    template <NativeHandle HelperArguments::* Field>
    [[nodiscard]] std::expected<void, ArgumentError> parseHandle(HelperArguments& arguments,
                                                                 std::string_view value)
    {
        return handleOf(value).transform([&](NativeHandle handle) { arguments.*Field = handle; });
    }

    /// @return A row's format for a single handle field.
    template <NativeHandle HelperArguments::* Field>
    [[nodiscard]] std::string formatHandle(HelperArguments const& arguments)
    {
        return textOf(arguments.*Field);
    }

    /// @return A row's parse for a number field.
    template <typename T, T HelperArguments::* Field>
    [[nodiscard]] std::expected<void, ArgumentError> parseNumber(HelperArguments& arguments,
                                                                 std::string_view value)
    {
        return numberOf<T>(value).transform([&](T number) { arguments.*Field = number; });
    }

    /// @return A row's format for a number field.
    template <typename T, T HelperArguments::* Field>
    [[nodiscard]] std::string formatNumber(HelperArguments const& arguments)
    {
        return std::to_string(arguments.*Field);
    }

    /// Reads the lanes: exactly MaxLaneChannels handles, comma-separated.
    [[nodiscard]] std::expected<void, ArgumentError> parseLanes(HelperArguments& arguments,
                                                                std::string_view value)
    {
        auto count = std::size_t { 0 };
        for (auto const part: std::views::split(value, ','))
        {
            if (count == MaxLaneChannels)
                return std::unexpected(ArgumentError::Malformed);
            auto const handle = handleOf(std::string_view { part });
            if (!handle)
                return std::unexpected(handle.error());
            arguments.lanes[count++] = *handle;
        }
        if (count != MaxLaneChannels)
            return std::unexpected(ArgumentError::Malformed);
        return {};
    }

    /// Writes the lanes, comma-separated.
    [[nodiscard]] std::string formatLanes(HelperArguments const& arguments)
    {
        auto text = std::string {};
        for (auto const lane: arguments.lanes)
        {
            if (!text.empty())
                text += ',';
            text += textOf(lane);
        }
        return text;
    }

    /// Every option, in the order the command line is written in.
    constexpr auto Options = std::array {
        Option { .name = "control",
                 .parse = &parseHandle<&HelperArguments::control>,
                 .format = &formatHandle<&HelperArguments::control> },
        Option { .name = "lanes", .parse = &parseLanes, .format = &formatLanes },
        Option { .name = "page",
                 .parse = &parseHandle<&HelperArguments::controlPage>,
                 .format = &formatHandle<&HelperArguments::controlPage> },
        Option { .name = "wake",
                 .parse = &parseHandle<&HelperArguments::wake>,
                 .format = &formatHandle<&HelperArguments::wake> },
        Option { .name = "parent",
                 .parse = &parseNumber<int64_t, &HelperArguments::parentProcessId>,
                 .format = &formatNumber<int64_t, &HelperArguments::parentProcessId> },
        Option { .name = "data-bytes",
                 .parse = &parseNumber<uint64_t, &HelperArguments::dataBytes>,
                 .format = &formatNumber<uint64_t, &HelperArguments::dataBytes> },
    };

    /// @return The row of the option @p name; nothing if none has it.
    [[nodiscard]] std::expected<std::size_t, ArgumentError> optionIndexOf(std::string_view name) noexcept
    {
        auto const found = std::ranges::find(Options, name, &Option::name);
        if (found == Options.end())
            return std::unexpected(ArgumentError::Unknown);
        return static_cast<std::size_t>(found - Options.begin());
    }
} // namespace

std::vector<std::string> formatHelperArguments(HelperArguments const& arguments)
{
    auto result = std::vector<std::string> {};
    result.reserve(Options.size());
    for (auto const& option: Options)
        result.push_back(std::format("--{}={}", option.name, option.format(arguments)));
    return result;
}

std::expected<HelperArguments, ArgumentError> parseHelperArguments(
    std::span<std::string_view const> arguments)
{
    constexpr auto Prefix = std::string_view { "--" };
    auto result = HelperArguments {};
    auto seen = std::bitset<Options.size()> {};
    for (auto const argument: arguments)
    {
        if (!argument.starts_with(Prefix))
            return std::unexpected(ArgumentError::Unknown);
        auto const option = argument.substr(Prefix.size());
        auto const equals = option.find('=');
        auto const index = optionIndexOf(option.substr(0, equals));
        if (!index)
            return std::unexpected(index.error());
        if (seen.test(*index))
            return std::unexpected(ArgumentError::Duplicate);
        seen.set(*index);
        if (equals == std::string_view::npos)
            return std::unexpected(ArgumentError::Malformed);
        if (auto const parsed = Options[*index].parse(result, option.substr(equals + 1)); !parsed)
            return std::unexpected(parsed.error());
    }
    if (!seen.all())
        return std::unexpected(ArgumentError::Missing);
    // Each role its own handle: two roles on one would have the helper read its control channel as a lane.
    auto handles = std::vector<NativeHandle> { result.control, result.controlPage, result.wake };
    handles.insert(handles.end(), result.lanes.begin(), result.lanes.end());
    std::ranges::sort(handles);
    if (std::ranges::adjacent_find(handles) != handles.end())
        return std::unexpected(ArgumentError::Malformed);
    return result;
}

} // namespace vtextension::wasm::remote::transport
