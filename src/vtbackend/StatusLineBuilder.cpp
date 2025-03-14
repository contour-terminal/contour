// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Color.h>
#include <vtbackend/StatusLineBuilder.h>
#include <vtbackend/Terminal.h>

#include <crispy/interpolated_string.h>
#include <crispy/utils.h>

#include <libunicode/convert.h>

#include <range/v3/range/conversion.hpp>
#include <range/v3/view/join.hpp>
#include <range/v3/view/transform.hpp>

#include <chrono>
#include <cstdio>
#include <format>
#include <optional>

using namespace std::string_view_literals;

#if defined(_WIN32)
    #define popen  _popen
    #define pclose _pclose
#endif

namespace vtbackend
{

namespace // helper functions
{
    std::string_view modeString(ViMode mode) noexcept
    {
        switch (mode)
        {
            case ViMode::Normal: return "NORMAL"sv;
            case ViMode::Insert: return "INSERT"sv;
            case ViMode::Visual: return "VISUAL"sv;
            case ViMode::VisualLine: return "VISUAL LINE"sv;
            case ViMode::VisualBlock: return "VISUAL BLOCK"sv;
        }
        crispy::unreachable();
    }
} // namespace

std::optional<RGBColor> tryParseColorAttribute(crispy::string_interpolation const& interpolation,
                                               std::string_view key)
{
    if (auto const i = interpolation.attributes.find(key); i != interpolation.attributes.end())
        return parseColor(i->second);

    return std::nullopt;
}

std::optional<StatusLineDefinitions::Item> makeStatusLineItem(
    crispy::interpolated_string_fragment const& fragment)
{
    if (std::holds_alternative<std::string_view>(fragment))
        return StatusLineDefinitions::Text { StatusLineDefinitions::Styles {},
                                             std::string(std::get<std::string_view>(fragment)) };

    auto const& interpolation = std::get<crispy::string_interpolation>(fragment);

    auto styles = StatusLineDefinitions::Styles {};

    auto constexpr FlagMappings = std::array {
        std::pair { "Bold", CellFlag::Bold },
        std::pair { "Faint", CellFlag::Faint },
        std::pair { "Italic", CellFlag::Italic },
        std::pair { "Underline", CellFlag::Underline },
        std::pair { "Blinking", CellFlag::Blinking },
        std::pair { "Inverse", CellFlag::Inverse },
        std::pair { "CrossedOut", CellFlag::CrossedOut },
        std::pair { "DoubleUnderline", CellFlag::DoublyUnderlined },
        std::pair { "CurlyUnderline", CellFlag::CurlyUnderlined },
        std::pair { "DottedUnderline", CellFlag::DottedUnderline },
        std::pair { "DashedUnderline", CellFlag::DashedUnderline },
        std::pair { "RapidBlinking", CellFlag::RapidBlinking },
        std::pair { "Overline", CellFlag::Overline },
    };

    for (auto&& [text, flag]: FlagMappings)
        if (interpolation.flags.count(text))
            styles.flags.enable(flag);

    styles.foregroundColor = tryParseColorAttribute(interpolation, "Color");
    styles.backgroundColor = tryParseColorAttribute(interpolation, "BackgroundColor");

    if (auto const i = interpolation.attributes.find("Left"); i != interpolation.attributes.end())
        styles.textLeft = i->second;

    if (auto const i = interpolation.attributes.find("Right"); i != interpolation.attributes.end())
        styles.textRight = i->second;

    if (interpolation.name == "CellSGR")
        return StatusLineDefinitions::CellSGR { styles };

    if (interpolation.name == "CellTextUTF8")
        return StatusLineDefinitions::CellTextUtf8 { styles };

    if (interpolation.name == "CellTextUTF32")
        return StatusLineDefinitions::CellTextUtf32 { styles };

    if (interpolation.name == "Clock")
        return StatusLineDefinitions::Clock { styles };

    if (interpolation.name == "Command")
    {
        if (interpolation.attributes.count("Program"))
        {
            return StatusLineDefinitions::Command {
                styles,
                std::string(interpolation.attributes.at("Program")),
            };
        }
        else
            return std::nullopt;
    }

    if (interpolation.name == "HistoryLineCount")
        return StatusLineDefinitions::HistoryLineCount { styles };

    if (interpolation.name == "Hyperlink")
        return StatusLineDefinitions::Hyperlink { styles };

    if (interpolation.name == "InputMode")
        return StatusLineDefinitions::InputMode { styles };

    if (interpolation.name == "ProtectedMode")
        return StatusLineDefinitions::ProtectedMode { styles };

    if (interpolation.name == "SearchMode")
        return StatusLineDefinitions::SearchMode { styles };

    if (interpolation.name == "SearchPrompt")
        return StatusLineDefinitions::SearchPrompt { styles };

    if (interpolation.name == "Title")
        return StatusLineDefinitions::Title { styles };

    if (interpolation.name == "Text")
        return StatusLineDefinitions::Text {
            styles,
            std::string(interpolation.attributes.at("text")),
        };

    if (interpolation.name == "VTType")
        return StatusLineDefinitions::VTType { styles };

    if (interpolation.name == "Tabs")
    {
        const std::optional<RGBColor> activeColor = tryParseColorAttribute(interpolation, "ActiveColor");
        const std::optional<RGBColor> activeBackground =
            tryParseColorAttribute(interpolation, "ActiveBackground");

        return StatusLineDefinitions::Tabs {
            styles,
            activeColor,
            activeBackground,
            std::nullopt, // separator
        };
    }

    return std::nullopt;
}

StatusLineSegment parseStatusLineSegment(std::string_view text)
{
    auto segment = StatusLineSegment {};

    // Parses a string like:
    // "{Clock:Bold,Italic,Color=#FFFF00} | {VTType} | {InputMode} {Search:Bold,Color=Yellow}"

    auto const interpolations = crispy::parse_interpolated_string(text);

    for (auto const& fragment: interpolations)
    {
        if (std::holds_alternative<std::string_view>(fragment))
        {
            segment.emplace_back(StatusLineDefinitions::Text {
                StatusLineDefinitions::Styles {}, std::string(std::get<std::string_view>(fragment)) });
        }
        else if (auto const item = makeStatusLineItem(std::get<crispy::string_interpolation>(fragment)))
        {
            segment.emplace_back(*item);
        }
    }

    return segment;
}

StatusLineDefinition parseStatusLineDefinition(std::string_view left,
                                               std::string_view middle,
                                               std::string_view right)
{
    return StatusLineDefinition {
        .left = parseStatusLineSegment(left),
        .middle = parseStatusLineSegment(middle),
        .right = parseStatusLineSegment(right),
    };
}

struct VTSerializer
{
    Terminal const& vt;
    StatusLineStyling styling;
    std::string result {};

    std::string makeTextColor(std::optional<RGBColor> const& color, std::string_view defaultSequence = {})
    {
        if (!color)
            return std::string(defaultSequence);

        return std::format("\033[38:2:{}:{}:{}m", color->red, color->green, color->blue);
    }

    std::string makeBackgroundColor(std::optional<RGBColor> const& color,
                                    std::string_view defaultSequence = {})
    {
        if (!color)
            return std::string(defaultSequence);

        return std::format("\033[48:2:{}:{}:{}m", color->red, color->green, color->blue);
    }

    void applyStyles(StatusLineDefinitions::Styles const& styles) // {{{
    {
        if (styling == StatusLineStyling::Disabled)
            return;

        result += makeTextColor(styles.foregroundColor);
        result += makeBackgroundColor(styles.backgroundColor);

        result += styles.flags.reduce(std::string {}, [](std::string&& result, CellFlag flag) -> std::string {
            switch (flag)
            {
                case CellFlag::None: return result;
                case CellFlag::Bold: return std::move(result) + "\033[1m";
                case CellFlag::Italic: return std::move(result) + "\033[3m";
                case CellFlag::Underline: return std::move(result) + "\033[4m";
                case CellFlag::DottedUnderline: return std::move(result) + "\033[4:1m";
                case CellFlag::CurlyUnderlined: return std::move(result) + "\033[4:3m";
                case CellFlag::DoublyUnderlined: return std::move(result) + "\033[4:4m";
                case CellFlag::DashedUnderline: return std::move(result) + "\033[4:5m";
                case CellFlag::Blinking: return std::move(result) + "\033[5m";
                case CellFlag::RapidBlinking: return std::move(result) + "\033[6m";
                case CellFlag::Inverse: return std::move(result) + "\033[7m";
                case CellFlag::Hidden: return std::move(result) + "\033[8m";
                case CellFlag::CrossedOut: return std::move(result) + "\033[9m";
                case CellFlag::Framed: return std::move(result) + "\033[51m";
                case CellFlag::Encircled: return std::move(result) + "\033[52m";
                case CellFlag::Overline: return std::move(result) + "\033[53m";
                case CellFlag::Faint: return std::move(result) + "\033[2m";
                case CellFlag::CharacterProtected:
                default: return result;
            }
        });
    } // }}}

    std::string operator()(StatusLineDefinitions::Item const& item)
    {
        std::visit(
            [this](auto const& item) {
                if (auto const text = visit(item); !text.empty())
                {
                    if constexpr (std::is_same_v<decltype(item), StatusLineDefinitions::Text>)
                        result += text;
                    else
                    {
                        if (styling == StatusLineStyling::Enabled)
                        {
                            result += SGRSAVE();
                            applyStyles(item);
                        }
                        result += item.textLeft;
                        result += text;
                        result += item.textRight;
                        if (styling == StatusLineStyling::Enabled)
                            result += SGRRESTORE();
                    }
                }
            },
            item);
        return result;
    }

    std::string operator()(StatusLineSegment const& segment)
    {
        std::string result;
        for (auto const& item: segment)
            result += std::visit(*this, item);
        return result;
    }

    // {{{
    std::string visit(StatusLineDefinitions::Title const&) { return vt.windowTitle(); }

    std::string visit(StatusLineDefinitions::CellSGR const&)
    {
        auto const currentMousePosition = vt.currentMousePosition();
        auto const cellFlags = vt.currentScreen().cellFlagsAt(currentMousePosition);
        return std::format("{}", cellFlags);
    }

    std::string visit(StatusLineDefinitions::CellTextUtf32 const&)
    {
        auto const currentMousePosition = vt.currentMousePosition();
        if (!vt.contains(currentMousePosition))
            return {};

        auto const cellText = vt.currentScreen().cellTextAt(currentMousePosition);
        auto const cellText32 = unicode::convert_to<char32_t>(std::string_view(cellText));

        return ranges::views::transform(
                   cellText32, [](char32_t ch) { return std::format("U+{:04X}", static_cast<uint32_t>(ch)); })
               | ranges::views::join(" ") | ranges::to<std::string>;
    }

    std::string visit(StatusLineDefinitions::CellTextUtf8 const&)
    {
        auto const currentMousePosition = vt.currentMousePosition();
        if (!vt.contains(currentMousePosition))
            return {};
        return crispy::escape(vt.currentScreen().cellTextAt(currentMousePosition));
    }

    std::string visit(StatusLineDefinitions::Clock const&)
    {
        crispy::ignore_unused(this);

        // TODO: Find a more convinient way; The following is printing the time in UTC,
        //       but we need it in local time.
        // return std::format("{:%H:%M}", std::chrono::system_clock::now());

        auto now = std::chrono::system_clock::now();
        std::time_t const nowTimeT = std::chrono::system_clock::to_time_t(now);
        std::tm const* tm = std::localtime(&nowTimeT);
        std::stringstream out;
        out << std::put_time(tm, "%H:%M");
        return out.str();
    }

    std::string visit(StatusLineDefinitions::HistoryLineCount const&)
    {
        if (!vt.isPrimaryScreen())
            return {};

        if (vt.viewport().scrollOffset().value)
        {
            auto const pct =
                double(vt.viewport().scrollOffset()) / double(vt.primaryScreen().historyLineCount());
            return std::format("{}/{} {:3}%",
                               vt.viewport().scrollOffset(),
                               vt.primaryScreen().historyLineCount(),
                               int(pct * 100));
        }
        else
            return std::format("{}", vt.primaryScreen().historyLineCount());
    }

    std::string visit(StatusLineDefinitions::Hyperlink const&)
    {
        if (auto const hyperlink = vt.currentScreen().hyperlinkAt(vt.currentMousePosition()))
            return std::format("{}", hyperlink->uri);

        return {};
    }

    std::string visit(StatusLineDefinitions::InputMode const&)
    {
        return std::string(modeString(vt.inputHandler().mode()));
    }

    std::string visit(StatusLineDefinitions::ProtectedMode const&)
    {
        if (vt.allowInput())
            return {};

        return " (PROTECTED)";
    }

    std::string visit(StatusLineDefinitions::TraceMode const&)
    {
        std::string result;

        result += "TRACING";

        if (!vt.traceHandler().pendingSequences().empty())
            result += std::format(" (#{}): {}",
                                  vt.traceHandler().pendingSequences().size(),
                                  vt.traceHandler().pendingSequences().front());
        return result;
    }

    std::string visit(StatusLineDefinitions::SearchMode const&)
    {
        if (!vt.search().pattern.empty() || vt.inputHandler().isEditingSearch())
            return " SEARCH";

        return {};
    }

    std::string visit(StatusLineDefinitions::SearchPrompt const&)
    {
        if (vt.inputHandler().isEditingSearch())
            return std::format("Search: {}█",
                               unicode::convert_to<char>(std::u32string_view(vt.search().pattern)));

        if (vt.inputHandler().isEditingPrompt())
            return std::format("{}{}█", vt.prompt().prompt, vt.prompt().text);

        return {};
    }

    std::string visit(StatusLineDefinitions::Command const& item)
    {
        crispy::ignore_unused(this);

        std::string result;
        if (FILE* fp = popen(item.command.c_str(), "r"); fp)
        {
            char buffer[256] {};
            while (fgets(buffer, sizeof(buffer), fp) != nullptr)
            {
                result += buffer;
            }
            pclose(fp);

            // Only keep first line
            if (auto const pos = result.find('\n'); pos != std::string::npos)
                result.erase(pos);
        }
        else
            result = std::strerror(errno);
        return result;
    }

    std::string visit(StatusLineDefinitions::Text const& item)
    {
        crispy::ignore_unused(this);
        return item.text;
    }

    std::string visit(StatusLineDefinitions::VTType const&) { return std::format("{}", vt.terminalId()); }

    std::string visit(StatusLineDefinitions::Tabs const& tabs)
    {
        auto const tabsInfo = vt.guiTabsInfoForStatusLine();

        std::string fragment;
        for (const auto position: std::views::iota(1u, tabsInfo.tabs.size() + 1))
        {
            if (!fragment.empty())
                fragment += tabs.separator.value_or("|");

            auto const isActivePosition = position == tabsInfo.activeTabPosition;
            auto const activePositionStylized =
                isActivePosition && (tabs.activeColor || tabs.activeBackground);

            if (activePositionStylized)
            {
                fragment += SGRSAVE();
                fragment += makeTextColor(tabs.activeColor);
                fragment += makeBackgroundColor(tabs.activeBackground);
            }

            if (tabsInfo.tabs[position - 1].name)
                fragment += tabsInfo.tabs[position - 1].name.value();
            else
                fragment += std::to_string(position);

            if (activePositionStylized)
                fragment += SGRRESTORE();
        }
        return fragment;
    }
    // }}}
};

std::string serializeToVT(Terminal const& vt, StatusLineSegment const& segment, StatusLineStyling styling)
{
    auto serializer = VTSerializer { .vt = vt, .styling = styling };
    for (auto const& item: segment)
        serializer(item);
    return serializer.result;
}

} // namespace vtbackend
