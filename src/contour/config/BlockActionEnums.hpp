// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ConfigEnumInfo.hpp>

#include <vtbackend/shell/CommandBlockAddress.hpp>

#include <array>
#include <cstdint>
#include <span>

namespace contour::actions
{

/// The format CopySelection extracts a selection in.
enum class CopyFormat : uint8_t
{
    Text = 0, ///< The text, with its whitespace and newlines, but no formatting.
    HTML,     ///< HTML.
    VT,       ///< The text with the VT sequences that render it.
    PNG,      ///< An image.
};

/// What CopySelection copies when nothing is selected.
enum class CopyFallback : uint8_t
{
    None = 0,          ///< Nothing new: the empty selection is copied, as it always was.
    LastCommandOutput, ///< The output of the most recently finished command (OSC 133).
};

/// Where OpenCommandOutput shows a command's output.
enum class OutputPlacement : uint8_t
{
    Split = 0, ///< A new pane stacked below, running the pager over a private file.
    Tab,       ///< A new tab, likewise.
    Detached,  ///< No pane: the output is piped as plain text into the program's standard input.
};

/// Whether the file OpenCommandOutput writes keeps the output's colours.
enum class OutputFormat : uint8_t
{
    Sgr = 0, ///< With SGR sequences, for a pager that renders them (`less -R`).
    Plain,   ///< Text only.
};

} // namespace contour::actions

namespace contour::config
{

namespace detail
{
    /// Which block a block action addresses: `target:` in input_mapping.
    inline constexpr auto CommandBlockTargetTable = std::array {
        ConfigEnumInfo<vtbackend::CommandBlockTarget> {
            vtbackend::CommandBlockTarget::Pointer, "pointer", "Under the mouse pointer" },
        ConfigEnumInfo<vtbackend::CommandBlockTarget> {
            vtbackend::CommandBlockTarget::Cursor, "cursor", "At the cursor" },
        ConfigEnumInfo<vtbackend::CommandBlockTarget> {
            vtbackend::CommandBlockTarget::Last, "last", "The last finished command" },
    };

    /// Which part of a block: `part:` in input_mapping. `input` is the prompt with the command typed at
    /// it -- the user's word for that part, where the engine's is "prompt".
    inline constexpr auto CommandBlockPartTable = std::array {
        ConfigEnumInfo<vtbackend::CommandBlockPart> {
            vtbackend::CommandBlockPart::Prompt, "input", "The prompt and the command" },
        ConfigEnumInfo<vtbackend::CommandBlockPart> {
            vtbackend::CommandBlockPart::Output, "output", "The output" },
        ConfigEnumInfo<vtbackend::CommandBlockPart> {
            vtbackend::CommandBlockPart::PromptAndOutput, "all", "The command and its output" },
    };

    /// The format CopySelection copies in: `format:` in input_mapping. Its tokens are written upper-case,
    /// as they always were, and read ignoring case.
    inline constexpr auto CopyFormatTable = std::array {
        ConfigEnumInfo<actions::CopyFormat> { actions::CopyFormat::Text, "Text", "Plain text" },
        ConfigEnumInfo<actions::CopyFormat> { actions::CopyFormat::HTML, "HTML", "HTML" },
        ConfigEnumInfo<actions::CopyFormat> { actions::CopyFormat::VT, "VT", "VT sequences" },
        ConfigEnumInfo<actions::CopyFormat> { actions::CopyFormat::PNG, "PNG", "PNG image" },
    };

    /// What CopySelection copies when nothing is selected: `fallback:` in input_mapping.
    inline constexpr auto CopyFallbackTable = std::array {
        ConfigEnumInfo<actions::CopyFallback> { actions::CopyFallback::None, "none", "Nothing" },
        ConfigEnumInfo<actions::CopyFallback> {
            actions::CopyFallback::LastCommandOutput, "last_command_output", "The last command's output" },
    };

    /// Where OpenCommandOutput shows the output: `placement:` in input_mapping.
    inline constexpr auto OutputPlacementTable = std::array {
        ConfigEnumInfo<actions::OutputPlacement> {
            actions::OutputPlacement::Split, "split", "In a split pane" },
        ConfigEnumInfo<actions::OutputPlacement> { actions::OutputPlacement::Tab, "tab", "In a new tab" },
        ConfigEnumInfo<actions::OutputPlacement> {
            actions::OutputPlacement::Detached, "detached", "Piped into a program" },
    };

    /// Whether OpenCommandOutput keeps the colours: `format:` in input_mapping.
    inline constexpr auto OutputFormatTable = std::array {
        ConfigEnumInfo<actions::OutputFormat> { actions::OutputFormat::Sgr, "sgr", "With colours" },
        ConfigEnumInfo<actions::OutputFormat> { actions::OutputFormat::Plain, "plain", "Plain text" },
    };
} // namespace detail

template <>
constexpr std::span<ConfigEnumInfo<vtbackend::CommandBlockTarget> const> configEnumValues() noexcept
{
    return detail::CommandBlockTargetTable;
}

template <>
constexpr std::span<ConfigEnumInfo<vtbackend::CommandBlockPart> const> configEnumValues() noexcept
{
    return detail::CommandBlockPartTable;
}

template <>
constexpr std::span<ConfigEnumInfo<actions::CopyFormat> const> configEnumValues() noexcept
{
    return detail::CopyFormatTable;
}

template <>
constexpr std::span<ConfigEnumInfo<actions::CopyFallback> const> configEnumValues() noexcept
{
    return detail::CopyFallbackTable;
}

template <>
constexpr std::span<ConfigEnumInfo<actions::OutputPlacement> const> configEnumValues() noexcept
{
    return detail::OutputPlacementTable;
}

template <>
constexpr std::span<ConfigEnumInfo<actions::OutputFormat> const> configEnumValues() noexcept
{
    return detail::OutputFormatTable;
}

} // namespace contour::config
