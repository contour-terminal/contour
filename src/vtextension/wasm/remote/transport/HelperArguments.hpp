// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The extension host process's command line: which of the handles it inherited is which, whose child it is,
/// and the cap on its memory. Contour formats it, the helper parses it; both link this, and the helper links
/// nothing else of the launcher's to read its own arguments.
///
/// `--control=H --lanes=H,H,H,H --page=H --wake=H --parent=PID --data-bytes=N`, every option once. A handle
/// is its decimal value: a descriptor number on POSIX, a HANDLE's value on Windows.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vtextension/wasm/remote/transport/Channel.hpp>

namespace vtextension::wasm::remote
{

/// How many lanes a host serves: one per thread that may call into its instances at once.
inline constexpr std::size_t MaxLaneChannels = 4;

} // namespace vtextension::wasm::remote

namespace vtextension::wasm::remote::transport
{

/// What an extension host process is told on its command line.
struct HelperArguments
{
    NativeHandle control {};                            ///< The control channel's end.
    std::array<NativeHandle, MaxLaneChannels> lanes {}; ///< The lanes' ends.
    NativeHandle controlPage {};                        ///< The interrupt page (ControlPage).
    NativeHandle wake {};                               ///< The wake signal's receiving end.
    int64_t parentProcessId = 0;                        ///< The process that started it.
    uint64_t dataBytes = 0;                             ///< The cap on its data memory; 0: no cap.

    bool operator==(HelperArguments const&) const = default;
};

/// Why a command line does not parse.
enum class ArgumentError : uint8_t
{
    Unknown,   ///< An argument names no option.
    Duplicate, ///< An option is given twice.
    Missing,   ///< An option is not given.
    Malformed, ///< An option's value is not what it takes.
};

/// @param arguments What to tell the helper.
/// @return The command line, one `--name=value` argument per option.
[[nodiscard]] std::vector<std::string> formatHelperArguments(HelperArguments const& arguments);

/// @param arguments The command line after the program's name, each a `--name=value` argument.
/// @return What it tells the helper; or the first reason it does not parse.
[[nodiscard]] std::expected<HelperArguments, ArgumentError> parseHelperArguments(
    std::span<std::string_view const> arguments);

} // namespace vtextension::wasm::remote::transport
