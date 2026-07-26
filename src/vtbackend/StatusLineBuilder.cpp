// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/StatusLineBuilder.h>

#include <vtbackend/CellFlags.h>
#include <vtbackend/Color.h>
#include <vtbackend/Terminal.h>

#include <crispy/interpolated_string.h>
#include <crispy/utils.h>

#include <libunicode/convert.h>

#include <chrono>
#include <concepts>
#include <cstdio>
#include <ctime>
#include <format>
#include <optional>
#include <ranges>
#include <system_error>
#include <type_traits>

using namespace std::string_view_literals;

#ifdef _WIN32
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
            case ViMode::Hint: return "HINT"sv;
        }
        crispy::unreachable();
    }
} // namespace

static std::optional<RGBColor> tryParseColorAttribute(crispy::string_interpolation const& interpolation,
                                                      std::string_view key)
{
    if (auto const i = interpolation.attributes.find(key); i != interpolation.attributes.end())
        return parseColor(i->second);

    return std::nullopt;
}

/// The value of attribute @p key, or nullopt when the interpolation does not carry it.
static std::optional<std::string> tryParseStringAttribute(crispy::string_interpolation const& interpolation,
                                                          std::string_view key)
{
    if (auto const i = interpolation.attributes.find(key); i != interpolation.attributes.end())
        return std::string(i->second);

    return std::nullopt;
}

/// Builds the item of type @p T that @p interpolation describes, given its already-parsed @p styles.
///
/// The three alternatives carrying payload beyond `Styles` are the only ones that need saying here;
/// every other placeholder is exactly its styles. Returns nullopt when a required attribute is
/// missing, which makes the placeholder unrecognized and so echoed verbatim by the caller.
template <typename T>
static std::optional<StatusLineDefinitions::Item> makeItemOfType(
    StatusLineDefinitions::Styles const& styles, crispy::string_interpolation const& interpolation)
{
    if constexpr (std::same_as<T, StatusLineDefinitions::Command>)
    {
        // Without a program there is nothing to run, so this is not a usable {Command}.
        if (auto program = tryParseStringAttribute(interpolation, "Program"))
            return StatusLineDefinitions::Command { styles, std::move(*program) };
        return std::nullopt;
    }
    else if constexpr (std::same_as<T, StatusLineDefinitions::Text>)
    {
        // A missing `text=` yields empty text rather than throwing: this parses user-authored config,
        // and a bare "{Text}" is a plausible typo that must not take the process down.
        return StatusLineDefinitions::Text {
            styles, tryParseStringAttribute(interpolation, "text").value_or(std::string {})
        };
    }
    else if constexpr (std::same_as<T, StatusLineDefinitions::Tabs>)
    {
        return StatusLineDefinitions::Tabs {
            styles,
            tryParseColorAttribute(interpolation, "ActiveColor"),
            tryParseColorAttribute(interpolation, "ActiveBackground"),
            tryParseStringAttribute(interpolation, "Separator"),
        };
    }
    else
        return T { styles };
}

/// The item @p interpolation describes, or nullopt when it names no known placeholder (or names one but
/// omits an attribute it cannot do without), in which case the caller echoes it verbatim.
static std::optional<StatusLineDefinitions::Item> makeStatusLineItem(
    crispy::string_interpolation const& interpolation)
{
    auto styles = StatusLineDefinitions::Styles {};

    for (auto const& [name, _, flag]: StatusLineDefinitions::CellFlagNames)
        if (interpolation.flags.contains(name))
            styles.flags.enable(flag);

    styles.foregroundColor = tryParseColorAttribute(interpolation, "Color");
    styles.backgroundColor = tryParseColorAttribute(interpolation, "BackgroundColor");
    styles.textLeft = tryParseStringAttribute(interpolation, "Left").value_or(std::string {});
    styles.textRight = tryParseStringAttribute(interpolation, "Right").value_or(std::string {});

    // Dispatch on the placeholder name via StatusLineDefinitions::ItemTraits, so the set of names
    // understood here is the same set the serializer writes and the settings page offers.
    auto result = std::optional<StatusLineDefinitions::Item> {};
    StatusLineDefinitions::forEachItemType([&](auto tag) {
        using T = decltype(tag)::type;
        if (result || interpolation.name != StatusLineDefinitions::ItemTraits<T>::Name)
            return;
        result = makeItemOfType<T>(styles, interpolation);
    });
    return result;
}

StatusLineSegment parseStatusLineSegment(std::string_view text)
{
    auto segment = StatusLineSegment {};

    // Parses a string like:
    // "{Clock:Bold,Italic,Color=#FFFF00} | {VTType} | {InputMode} {Search:Bold,Color=Yellow}"

    auto const interpolations = crispy::parse_interpolated_string(text);

    // An un-styled literal Text item wrapping the given source bytes; used both for the plain-text
    // fragments and for the verbatim echo of an unrecognized placeholder.
    auto const literalText = [](std::string_view s) {
        return StatusLineDefinitions::Text { StatusLineDefinitions::Styles {}, std::string(s) };
    };

    for (auto const& fragment: interpolations)
    {
        if (std::holds_alternative<std::string_view>(fragment))
            segment.emplace_back(literalText(std::get<std::string_view>(fragment)));
        // Reached by const reference out of the variant (no copy): binding the string_interpolation by
        // value here would deep-copy its flag set and attribute map.
        else if (auto const item = makeStatusLineItem(std::get<crispy::string_interpolation>(fragment)))
            segment.emplace_back(*item);
        else
            // An unrecognized placeholder is echoed verbatim (its exact original `{...}` slice) rather than
            // dropped, so the user sees what they typed — matching expandTabLabel's tab-strip handling so
            // both surfaces treat unknown placeholders identically.
            segment.emplace_back(literalText(std::get<crispy::string_interpolation>(fragment).whole));
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

namespace
{
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

            result +=
                styles.flags.reduce(std::string {}, [](std::string&& result, CellFlag flag) -> std::string {
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

            std::string result;
            bool first = true;
            for (char32_t ch: cellText32)
            {
                if (!first)
                    result += " ";
                result += std::format("U+{:04X}", static_cast<uint32_t>(ch));
                first = false;
            }
            return result;
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
            std::tm tmBuf {};
#ifdef _WIN32
            localtime_s(&tmBuf, &nowTimeT);
#else
            localtime_r(&nowTimeT, &tmBuf);
#endif
            std::stringstream out;
            out << std::put_time(&tmBuf, "%H:%M");
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
            // Trace mode is off in Normal execution; render nothing then (mirrors ProtectedMode's gating), so
            // the default status line's {TraceMode} segment is empty unless tracing is actually active.
            if (vt.executionMode() == ExecutionMode::Normal)
                return {};

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
                result = std::generic_category().message(errno);
            return result;
        }

        std::string visit(StatusLineDefinitions::Text const& item)
        {
            crispy::ignore_unused(this);
            return item.text;
        }

        std::string visit(StatusLineDefinitions::VTType const&)
        {
            return std::format("{}", vt.operatingLevel());
        }

        std::string visit(StatusLineDefinitions::Tabs const& tabs)
        {
            auto const tabsInfo = vt.guiTabsInfoForStatusLine();

            std::string fragment;
            for (auto const position: std::views::iota(1u, tabsInfo.tabs.size() + 1))
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
} // namespace

std::string serializeToVT(Terminal const& vt, StatusLineSegment const& segment, StatusLineStyling styling)
{
    auto serializer = VTSerializer { .vt = vt, .styling = styling };
    for (auto const& item: segment)
        serializer(item);
    return serializer.result;
}

namespace
{
    /// Writes a placeholder's attribute list, emitting the ':' that separates the placeholder name
    /// from its first attribute and the ',' before each subsequent one.
    ///
    /// crispy::parse_interpolation splits a placeholder at its *first* colon -- everything before it
    /// is the name -- then splits what follows on commas. Emitting a comma where the colon belongs
    /// therefore folds the entire attribute list into the name, which matches no placeholder and is
    /// echoed back as literal text: the item and every style on it are destroyed by one round trip.
    /// Owning the separator in one place is what stops that from being writable by accident.
    struct AttributeWriter
    {
        std::string& out;
        bool first = true;

        /// Appends a bare flag, e.g. "Bold".
        void add(std::string_view entry)
        {
            out += first ? ':' : ',';
            first = false;
            out += entry;
        }

        /// Appends a "key=value" attribute.
        void add(std::string_view key, std::string_view value)
        {
            add(key);
            out += '=';
            out += value;
        }

        /// Appends "key=#RRGGBB" when @p color holds a value; a no-op otherwise.
        void addColor(std::string_view key, std::optional<RGBColor> color)
        {
            // formatColor() is documented as the exact inverse of parseColor(), which is what
            // tryParseColorAttribute() reads these back with -- so the two ends of the round trip are
            // one function apart rather than two spellings that have to be kept agreeing.
            if (color)
                add(key, formatColor(*color));
        }
    };

    /// Whether @p text survives a round trip as a placeholder attribute value.
    ///
    /// A template has no escaping, so a ',' reads back as an attribute separator and a brace as a
    /// placeholder boundary. See the note on @ref serializeStatusLineSegment.
    [[nodiscard]] bool isAttributeSafe(std::string_view text) noexcept
    {
        return text.find_first_of(",{}") == std::string_view::npos;
    }

    /// Appends the attributes every item shares: its flags, colors and side adornments.
    void appendStyles(AttributeWriter& attributes, StatusLineDefinitions::Styles const& styles)
    {
        for (auto const& [name, _, flag]: StatusLineDefinitions::CellFlagNames)
            if (styles.flags.test(flag))
                attributes.add(name);

        attributes.addColor("Color", styles.foregroundColor);
        attributes.addColor("BackgroundColor", styles.backgroundColor);

        if (!styles.textLeft.empty())
            attributes.add("Left", styles.textLeft);

        if (!styles.textRight.empty())
            attributes.add("Right", styles.textRight);
    }
} // namespace

std::string serializeStatusLineSegment(StatusLineSegment const& segment)
{
    auto result = std::string {};

    for (auto const& item: segment)
    {
        std::visit(
            [&](auto const& v) {
                using T = std::decay_t<decltype(v)>;

                if constexpr (std::same_as<T, StatusLineDefinitions::Text>)
                {
                    auto const unstyled = !v.foregroundColor && !v.backgroundColor && !v.flags.any()
                                          && v.textLeft.empty() && v.textRight.empty();
                    // A bare literal is the form parseStatusLineSegment produces for the plain text
                    // between placeholders, and the only form that can hold a comma or a brace at
                    // all -- so text that cannot be an attribute is emitted this way even when that
                    // costs its styling, because the styled form would come back corrupted instead.
                    if (unstyled || !isAttributeSafe(v.text))
                    {
                        // Verbatim, with no brace escaping: a template has none, so a doubled brace
                        // would not read back as one -- it would gain a brace on every save.
                        // Verbatim round-trips exactly, because "a{b" reads back as the literal "a"
                        // followed by the unterminated "{b" echoed verbatim, and the two concatenate
                        // to the text we started with.
                        result += v.text;
                        return;
                    }
                }

                result += '{';
                result += StatusLineDefinitions::ItemTraits<T>::Name;
                auto attributes = AttributeWriter { .out = result };

                if constexpr (std::same_as<T, StatusLineDefinitions::Text>)
                    attributes.add("text", v.text);
                else if constexpr (std::same_as<T, StatusLineDefinitions::Command>)
                    attributes.add("Program", v.command);

                appendStyles(attributes, v);

                if constexpr (std::same_as<T, StatusLineDefinitions::Tabs>)
                {
                    attributes.addColor("ActiveColor", v.activeColor);
                    attributes.addColor("ActiveBackground", v.activeBackground);
                    if (v.separator)
                        attributes.add("Separator", *v.separator);
                }

                result += '}';
            },
            item);
    }

    return result;
}

} // namespace vtbackend
