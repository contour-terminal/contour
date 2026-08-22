// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/StatusLineBuilder.hpp>

#include <vtbackend/core/CellFlags.hpp>
#include <vtbackend/core/Color.hpp>
#include <vtbackend/screen/Terminal.hpp>

#include <crispy/InterpolatedString.hpp>
#include <crispy/Utils.hpp>

#include <libunicode/convert.h>

#include <chrono>
#include <concepts>
#include <cstdio>
#include <ctime>
#include <format>
#include <iterator>
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

static std::optional<RGBColor> tryParseColorAttribute(crispy::StringInterpolation const& interpolation,
                                                      std::string_view key)
{
    if (auto const i = interpolation.attributes.find(key); i != interpolation.attributes.end())
        return parseColor(i->second);

    return std::nullopt;
}

/// The value of attribute @p key, or nullopt when the interpolation does not carry it.
static std::optional<std::string> tryParseStringAttribute(crispy::StringInterpolation const& interpolation,
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
    StatusLineDefinitions::Styles const& styles, crispy::StringInterpolation const& interpolation)
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
    else if constexpr (std::same_as<T, StatusLineDefinitions::Context>)
    {
        auto item = StatusLineDefinitions::Context {};
        static_cast<StatusLineDefinitions::Styles&>(item) = styles;
        if (auto const verbosity = tryParseStringAttribute(interpolation, "Verbosity"))
            item.verbosity = StatusLineDefinitions::contextVerbosityFrom(*verbosity);
        item.separator = tryParseStringAttribute(interpolation, "Separator");
        if (auto const width = tryParseStringAttribute(interpolation, "MaxWidth"))
            if (auto const columns = crispy::toInteger<10, int>(*width); columns && *columns > 0)
                item.maxWidth = ColumnCount(*columns);
        return item;
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
    crispy::StringInterpolation const& interpolation)
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

    auto const interpolations = crispy::parseInterpolatedString(text);

    // An un-styled literal Text item wrapping the given source bytes; used both for the plain-text
    // fragments and for the verbatim echo of an unrecognized placeholder.
    auto const literalText = [](std::string_view s) {
        return StatusLineDefinitions::Text { StatusLineDefinitions::Styles {}, std::string(s) };
    };

    for (auto const& fragment: interpolations)
    {
        if (std::holds_alternative<std::string_view>(fragment))
            segment.emplace_back(literalText(std::get<std::string_view>(fragment)));
        // Reached by const reference out of the variant (no copy): binding the StringInterpolation by
        // value here would deep-copy its flag set and attribute map.
        else if (auto const item = makeStatusLineItem(std::get<crispy::StringInterpolation>(fragment)))
            segment.emplace_back(*item);
        else
            // An unrecognized placeholder is echoed verbatim (its exact original `{...}` slice) rather than
            // dropped, so the user sees what they typed — matching expandTabLabel's tab-strip handling so
            // both surfaces treat unknown placeholders identically.
            segment.emplace_back(literalText(std::get<crispy::StringInterpolation>(fragment).whole));
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
            crispy::ignoreUnused(this);

            // TODO: Find a more convenient way; The following is printing the time in UTC,
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

            // The SCROLLABLE count, not the raw history depth: the scroll offset is bounded by that
            // count, so dividing by the depth yields an indicator that can never reach 100% and quotes
            // a distance the user cannot travel. @see Viewport::scrollableLineCount, which every other
            // consumer of the bound already reads.
            auto const scrollable = vt.viewport().scrollableLineCount();

            // Zero is the whole reason for the guard as well as an uninteresting answer: with the
            // history entirely inside collapsed folds there is nothing to be a percentage of.
            if (vt.viewport().scrollOffset().value && unbox(scrollable) > 0)
            {
                auto const pct = double(vt.viewport().scrollOffset()) / scrollable.as<double>();
                return std::format("{}/{} {:3}%", vt.viewport().scrollOffset(), scrollable, int(pct * 100));
            }
            else
                return std::format("{}", scrollable);
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

        std::string visit(StatusLineDefinitions::Progress const&)
        {
            // Read without locking, as every other visit() here does: buildStatusLineText runs on the
            // terminal's own thread with _stateMutex held.
            auto const progress = vt.progress();
            switch (progress.state)
            {
                // Nothing to say, so the segment collapses -- the same gating ProtectedMode and
                // TraceMode use, which is what keeps the default status line clean.
                case ProgressState::Inactive: return {};
                case ProgressState::Normal: return std::format("{}%", progress.percentage);
                case ProgressState::Error: return std::format("ERROR {}%", progress.percentage);
                case ProgressState::Paused: return std::format("PAUSED {}%", progress.percentage);
                // No percentage: an indeterminate operation has no meaningful one to show, and the
                // number carried across the transition belongs to whatever ran before it.
                case ProgressState::Indeterminate: return "BUSY";
            }
            crispy::unreachable();
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
            if (!vt.search().pattern.empty())
                return " SEARCH";

            return {};
        }

        std::string visit(StatusLineDefinitions::SearchPrompt const&)
        {
            // A read-only echo of the active pattern, not an edit field any more: search is typed
            // into the find bar now. The block character this used to append was a drawn stand-in for
            // a caret, and echoing one where nothing can be typed would only mislead.
            if (vt.search().pattern.empty())
                return {};

            return std::format("Search: {}",
                               unicode::convert_to<char>(std::u32string_view(vt.search().pattern)));
        }

        std::string visit(StatusLineDefinitions::Command const& item)
        {
            crispy::ignoreUnused(this);

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
            crispy::ignoreUnused(this);
            return item.text;
        }

        std::string visit(StatusLineDefinitions::VTType const&)
        {
            return std::format("{}", vt.operatingLevel());
        }

        /// How one context type reads in a breadcrumb.
        ///
        /// A TABLE rather than a switch chain, and the prefix column is why: `container:foobar` needs
        /// its type in front because a bare name reads like a hostname, while `zeta` and `root` are
        /// self-describing in position. That rule lives here, one row per type, rather than being
        /// re-decided at each call site.
        struct SegmentRule
        {
            ContextType type;
            std::string_view prefix; ///< "" for none.
            /// Fields tried in order; the first non-empty one is shown.
            std::array<ContextField, 2> fields;
        };

        /// Which field to show for each type. WHETHER a type is shown at the default verbosity is not
        /// a column here: that is isBoundaryContext(), and stating it twice is how the tint and the
        /// breadcrumb would come to disagree about what a boundary is.
        static constexpr auto SegmentRules = std::array {
            SegmentRule { ContextType::Remote, "", { ContextField::TargetHost, ContextField::Hostname } },
            SegmentRule {
                ContextType::Container, "container:", { ContextField::Container, ContextField::Comm } },
            SegmentRule { ContextType::Vm, "vm:", { ContextField::Vm, ContextField::Comm } },
            SegmentRule { ContextType::Elevate, "", { ContextField::TargetUser, ContextField::User } },
            SegmentRule {
                ContextType::ChangePrivileges, "", { ContextField::TargetUser, ContextField::User } },
            // A session names WHERE, which is why the issue's example reads "zeta > container:foobar
            // > root > vim": the hostname comes from the outermost context.
            SegmentRule { ContextType::Session, "", { ContextField::Hostname, ContextField::User } },
            SegmentRule { ContextType::Boot, "", { ContextField::Hostname, ContextField::Comm } },
            SegmentRule { ContextType::Service, "", { ContextField::Comm, ContextField::User } },
            SegmentRule { ContextType::Shell, "", { ContextField::Comm, ContextField::User } },
            SegmentRule { ContextType::Command, "", { ContextField::Comm, ContextField::CommandLine } },
            SegmentRule { ContextType::App, "", { ContextField::Comm, ContextField::User } },
            SegmentRule { ContextType::Subcontext, "", { ContextField::TargetUser, ContextField::User } },
        };

        static_assert(SegmentRules.size() == ContextTypeList.size(),
                      "A ContextType was added or removed. Give it a SegmentRule row, or the breadcrumb "
                      "falls back to comm= for it without anything saying so.");

        /// The value @p field names on @p record, or empty when it was not supplied.
        ///
        /// Generated from VTBACKEND_CONTEXT_FIELDS, so a sixteenth textual field is reachable from a
        /// SegmentRule the moment it exists. Written out by hand, a row naming a field the switch had
        /// forgotten yielded a silently empty segment.
        [[nodiscard]] static std::string_view fieldOf(TerminalContext const& record, ContextField field)
        {
            if (!(record.present & field).any())
                return {};
            switch (field)
            {
// Only the textual kinds have a std::string to view; Enum and Uint64 fall to the default below.
#define VTBACKEND_CONTEXT_FIELD_OF_Text(Name, Member) \
    case ContextField::Name: return record.Member;
#define VTBACKEND_CONTEXT_FIELD_OF_OptionalText(Name, Member) \
    case ContextField::Name: return record.Member;
#define VTBACKEND_CONTEXT_FIELD_OF_Id128(Name, Member) \
    case ContextField::Name: return record.Member;
#define VTBACKEND_CONTEXT_FIELD_OF_Uint64(Name, Member)
#define VTBACKEND_CONTEXT_FIELD_OF_Enum(Name, Member)
#define VTBACKEND_CONTEXT_FIELD_OF(Name, Bit, Spelling, Member, Kind, Max) \
    VTBACKEND_CONTEXT_FIELD_OF_##Kind(Name, Member)
                VTBACKEND_CONTEXT_FIELDS(VTBACKEND_CONTEXT_FIELD_OF)
#undef VTBACKEND_CONTEXT_FIELD_OF
#undef VTBACKEND_CONTEXT_FIELD_OF_Enum
#undef VTBACKEND_CONTEXT_FIELD_OF_Uint64
#undef VTBACKEND_CONTEXT_FIELD_OF_Id128
#undef VTBACKEND_CONTEXT_FIELD_OF_OptionalText
#undef VTBACKEND_CONTEXT_FIELD_OF_Text
                default: return {};
            }
        }

        /// Strips anything a terminal would INTERPRET.
        ///
        /// Defence in depth, and not theoretical: this string is handed to writeToScreenInternal(),
        /// which parses it. The sequence decoder already rejects control bytes, so one arriving here
        /// means a parser bug -- and without this, that bug would be an injection able to clear the
        /// status screen from a field any program on the tty can write.
        [[nodiscard]] static std::string sanitized(std::string_view text)
        {
            auto out = std::string {};
            out.reserve(text.size());
            for (auto const ch: text)
                if (static_cast<unsigned char>(ch) >= 0x20 && static_cast<unsigned char>(ch) != 0x7f)
                    out.push_back(ch);
            return out;
        }

        /// How @p record reads in a breadcrumb at @p verbosity, or empty when it says nothing worth
        /// showing.
        [[nodiscard]] static std::string segmentFor(TerminalContext const& record,
                                                    TerminalContext const* outermost,
                                                    StatusLineDefinitions::ContextVerbosity verbosity)
        {
            // Plain `auto`, never `auto const*`: std::array's const_iterator is a raw pointer only in
            // libstdc++. The MSVC STL wraps it in a class, so the pointer form fails to deduce there --
            // which is exactly why .clang-tidy disables readability-qualified-auto.
            auto const rule = std::ranges::find_if(
                SegmentRules, [&](SegmentRule const& r) { return r.type == record.type; });
            auto const known = rule != SegmentRules.end();

            // The same predicate the background tint consults, so the two can never disagree about what
            // a boundary is. An untyped context is not one, which is what the old `known &&` said.
            if (verbosity == StatusLineDefinitions::ContextVerbosity::Boundaries
                && !isBoundaryContext(record.type))
                return {};

            // run0 while already root, or an elevate to yourself, is not news -- and suppressing it
            // removes the commonest honest false positive, so a SPOOFED one has to actually differ
            // before it appears at all.
            if (known && (record.type == ContextType::Elevate || record.type == ContextType::ChangePrivileges)
                && outermost != nullptr && !record.targetUser.empty() && record.targetUser == outermost->user)
                return {};

            auto value = std::string_view {};
            if (known)
            {
                for (auto const field: rule->fields)
                    if (value = fieldOf(record, field); !value.empty())
                        break;
            }
            else
                value = fieldOf(record, ContextField::Comm);

            if (value.empty())
                return {};
            return (known ? std::string { rule->prefix } : std::string {}) + sanitized(value);
        }

        /// Joins @p segments, eliding the MIDDLE when the result would exceed @p maxWidth columns.
        [[nodiscard]] static std::string elide(std::vector<std::string> const& segments,
                                               std::string const& separator,
                                               ColumnCount maxWidth)
        {
            auto const widthOf = [](std::string_view text) {
                // Counted in COLUMNS, not bytes: a CJK container name is two columns per codepoint and
                // three bytes, and measuring bytes would elide text that fits.
                auto columns = 0;
                for (auto const codepoint: unicode::convert_to<char32_t>(text))
                    columns += static_cast<int>(unicode::width(codepoint));
                return columns;
            };

            auto const join = [&](std::vector<std::string> const& parts) {
                return crispy::joinWith(parts, separator);
            };

            auto candidate = join(segments);
            if (widthOf(candidate) <= unbox<int>(maxWidth))
                return candidate;

            // Keep both ends: the first answers *where*, the last answers *who*.
            if (segments.size() > 2)
            {
                auto elided = std::vector<std::string> { segments.front(), "…", segments.back() };
                candidate = join(elided);
                if (widthOf(candidate) <= unbox<int>(maxWidth))
                    return candidate;
            }

            // Still too wide: the innermost alone, cut at its own START so its tail -- the part that
            // identifies it -- survives.
            //
            // Cut by CODEPOINT, never by byte: a container name is UTF-8, and erasing one byte at a
            // time leaves the string starting on a continuation byte, which is mojibake on screen and
            // an invalid sequence handed to writeToScreenInternal(). The ellipsis is budgeted for
            // rather than added afterwards, because prepending it to a string already at maxWidth
            // overflows the width by exactly the column the caller allotted.
            auto const codepoints = unicode::convert_to<char32_t>(std::string_view { segments.back() });
            auto const budget = unbox<int>(maxWidth) - 1; // one column for the leading '…'
            if (budget <= 0)
                return {};

            auto kept = size_t {};
            auto columns = 0;
            for (auto index = codepoints.size(); index-- > 0;)
            {
                columns += static_cast<int>(unicode::width(codepoints[index]));
                if (columns > budget)
                    break;
                kept = codepoints.size() - index;
            }
            if (kept == 0)
                return {};

            auto tail = std::string { "…" };
            unicode::convert_to<char>(std::u32string_view { codepoints }.substr(codepoints.size() - kept),
                                      std::back_inserter(tail));
            return tail;
        }

        std::string visit(StatusLineDefinitions::Context const& item)
        {
            auto const& stack = vt.contexts();
            auto const chain = stack.chain();
            if (chain.empty())
                return {};

            auto const* const outermost = chain.front().record.get();
            auto segments = std::vector<std::string> {};

            if (item.verbosity == StatusLineDefinitions::ContextVerbosity::Active)
            {
                if (auto text = segmentFor(
                        *chain.back().record, outermost, StatusLineDefinitions::ContextVerbosity::Full);
                    !text.empty())
                    segments.push_back(std::move(text));
            }
            else
                for (auto const& entry: chain)
                    if (auto text = segmentFor(*entry.record, outermost, item.verbosity); !text.empty())
                        segments.push_back(std::move(text));

            // Nothing to say, so the segment COLLAPSES -- and operator() drops textLeft/textRight with
            // it, so no orphan divider is left behind. That is what keeps this affordable in the
            // default status line: an ordinary session pays nothing for it.
            if (segments.empty())
                return {};

            return elide(segments, item.separator.value_or(" › "), item.maxWidth);
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
    /// crispy::parseInterpolation splits a placeholder at its *first* colon -- everything before it
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

                if constexpr (std::same_as<T, StatusLineDefinitions::Context>)
                {
                    // Only what is NOT the default: a config Contour writes must be one Contour reads
                    // back, and one it wrote must not gain noise. The default is read off a
                    // default-constructed item rather than restated, so it cannot drift from the header.
                    if (auto const name = StatusLineDefinitions::contextVerbosityName(v.verbosity);
                        !name.empty())
                        attributes.add("Verbosity", std::string { name });
                    if (v.separator)
                        attributes.add("Separator", *v.separator);
                    if (v.maxWidth != StatusLineDefinitions::Context {}.maxWidth)
                        attributes.add("MaxWidth", std::to_string(unbox(v.maxWidth)));
                }

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
