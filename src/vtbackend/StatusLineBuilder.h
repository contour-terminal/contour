// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/CellFlags.h>
#include <vtbackend/Color.h>

#include <string>
#include <variant>
#include <vector>

namespace vtbackend
{

namespace StatusLineDefinitions
{
    struct Styles
    {
        std::optional<RGBColor> foregroundColor;
        std::optional<RGBColor> backgroundColor;
        CellFlags flags;
        std::string textLeft;
        std::string textRight;
    };

    // clang-format off
    struct Title: Styles {};
    struct CellSGR: Styles {};
    struct CellTextUtf32: Styles {};
    struct CellTextUtf8: Styles {};
    struct Clock: Styles {};
    struct Command: Styles { std::string command; };
    struct HistoryLineCount: Styles {};
    struct Hyperlink: Styles {};
    struct InputMode: Styles {};
    struct ProtectedMode: Styles {};
    struct SearchMode: Styles {};
    struct SearchPrompt: Styles {};
    struct Text: Styles { std::string text; };
    struct TraceMode: Styles {};
    struct VTType: Styles {};

    struct Tabs: Styles
    {
        std::optional<RGBColor> activeColor;
        std::optional<RGBColor> activeBackground;
        std::optional<std::string> separator;
    };

    using Item = std::variant<
        CellSGR,
        CellTextUtf32,
        CellTextUtf8,
        Clock,
        Command,
        HistoryLineCount,
        Hyperlink,
        InputMode,
        ProtectedMode,
        SearchMode,
        SearchPrompt,
        Text,
        Title,
        TraceMode,
        VTType,
        Tabs
    >;
    // clang-format on
} // namespace StatusLineDefinitions

using StatusLineSegment = std::vector<StatusLineDefinitions::Item>;

struct StatusLineDefinition
{
    StatusLineSegment left;
    StatusLineSegment middle;
    StatusLineSegment right;
};

// "{Clock:Bold,Italic,Color=#FFFF00} | {VTType} | {InputMode} {SearchPrompt:Bold,Color=Yellow}"
StatusLineSegment parseStatusLineSegment(std::string_view text);

StatusLineDefinition parseStatusLineDefinition(std::string_view left,
                                               std::string_view middle,
                                               std::string_view right);

enum class StatusLineStyling : uint8_t
{
    Disabled,
    Enabled
};

class Terminal;
std::string serializeToVT(Terminal const& vt, StatusLineSegment const& segment, StatusLineStyling styling);

} // namespace vtbackend
