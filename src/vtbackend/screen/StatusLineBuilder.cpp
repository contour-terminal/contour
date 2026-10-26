// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/StatusLineBuilder.hpp>

#include <vtbackend/core/CellFlags.hpp>
#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/WorkingDirectory.hpp>
#include <vtbackend/grid/GraphemeClusters.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/shell/CommandLineSanitizer.hpp>

#include <crispy/InterpolatedString.hpp>

#include <core/Utils.hpp>

#include <libunicode/convert.h>

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

using namespace std::string_view_literals;

namespace vtbackend
{

namespace // helper functions
{
    /// @p text with its SGR sequences (`ESC [ ... m`, the only escapes an extension's text holds) removed.
    [[nodiscard]] std::string withoutSgr(std::string_view text)
    {
        auto result = std::string {};
        result.reserve(text.size());
        while (!text.empty())
        {
            auto const escape = text.find("\033[");
            result += text.substr(0, escape);
            if (escape == std::string_view::npos)
                break;
            // A CSI sequence ends at its final byte, the first in '@'..'~' after the introducer.
            auto const parameters = text.substr(escape + 2);
            auto const final = std::ranges::find_if(parameters, [](char c) { return c >= '@' && c <= '~'; });
            if (final == parameters.end())
                break;
            text = std::string_view { std::next(final), parameters.end() };
        }
        return result;
    }

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
        core::unreachable();
    }

    /// @p text, cut at its END to at most @p maxWidth columns with a trailing "…" when it is wider.
    /// @param text The text, valid UTF-8.
    /// @param maxWidth The widest the result may be, in columns.
    /// @param policy How the status screen measures a grapheme cluster.
    /// @return @p text itself, or its head and "…"; never cut inside a grapheme cluster.
    [[nodiscard]] std::string elideEnd(std::string_view text, ColumnCount maxWidth, ClusterWidthPolicy policy)
    {
        if (columnsOnTheGrid(text, policy) <= maxWidth)
            return std::string { text };

        // The ellipsis is budgeted for, as elideMiddle() budgets for its own.
        auto const budget = maxWidth - ColumnCount(1);
        auto head = std::string {};
        auto columns = ColumnCount(0);
        for (auto const& cluster: graphemeClustersOf(text, policy))
        {
            columns += cluster.columns;
            if (columns > budget)
                break;
            head += cluster.text;
        }
        return head + "…";
    }

    /// Joins @p segments, eliding the MIDDLE when the result would exceed @p maxWidth columns.
    /// @param segments The parts, outermost first.
    /// @param separator What goes between two parts.
    /// @param maxWidth The widest the result may be, in columns.
    /// @param policy How the status screen measures a grapheme cluster.
    /// @return The joined parts; the first, "…" and the last; or the last alone cut at its start.
    [[nodiscard]] std::string elideMiddle(std::vector<std::string> const& segments,
                                          std::string_view separator,
                                          ColumnCount maxWidth,
                                          ClusterWidthPolicy policy)
    {
        auto const join = [&](std::vector<std::string> const& parts) {
            return core::joinWith(parts, separator);
        };

        auto candidate = join(segments);
        if (columnsOnTheGrid(candidate, policy) <= maxWidth)
            return candidate;

        // Keep both ends: the first answers *where*, the last answers *who*.
        if (segments.size() > 2)
        {
            auto elided = std::vector<std::string> { segments.front(), "…", segments.back() };
            candidate = join(elided);
            if (columnsOnTheGrid(candidate, policy) <= maxWidth)
                return candidate;
        }

        // Still too wide: the innermost alone, cut at its own START so its tail -- the part that
        // identifies it -- survives.
        //
        // Cut by GRAPHEME CLUSTER, never by byte or codepoint: a byte cut leaves mojibake and an invalid
        // sequence for writeToScreenInternal(), and a codepoint cut can keep a combining mark without its
        // base, or half a ZWJ sequence or flag. The ellipsis is budgeted for rather than added afterwards,
        // because prepending it to a string already at maxWidth overflows the width by exactly the column
        // the caller allotted.
        auto const budget = maxWidth - ColumnCount(1); // one column for the leading '…'
        auto const clusters = graphemeClustersOf(segments.back(), policy);
        auto kept = std::size_t { 0 };
        auto columns = ColumnCount(0);
        for (auto const& cluster: clusters | std::views::reverse)
        {
            columns += cluster.columns;
            if (columns > budget)
                break;
            ++kept;
        }
        if (kept == 0)
            return {};

        auto tail = std::string { "…" };
        for (auto const& cluster: clusters | std::views::drop(clusters.size() - kept))
            tail += cluster.text;
        return tail;
    }
} // namespace

namespace
{
    /// Whether @p text spells @p name, ignoring ASCII case. An ASCII fold, not std::tolower: that reads the
    /// locale, and a Turkish one does not fold 'I' to 'i'.
    [[nodiscard]] bool spellsIgnoringAsciiCase(std::string_view text, std::string_view name) noexcept
    {
        auto const asciiLower = [](char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        };
        return std::ranges::equal(text, name, {}, asciiLower, asciiLower);
    }
} // namespace

namespace StatusLineDefinitions
{
    std::optional<ViMode> inputModeFrom(std::string_view name) noexcept
    {
        auto const spells = [&](InputModeName const& row) {
            return spellsIgnoringAsciiCase(name, row.name);
        };
        if (auto const row = std::ranges::find_if(InputModeNames, spells); row != InputModeNames.end())
            return row->mode;
        return std::nullopt;
    }

    std::string_view inputModeName(ViMode mode) noexcept
    {
        return InputModeNames[static_cast<std::size_t>(mode)].name;
    }

    RGBColorPair modeChipColors(RGBColorPair modePair, ColorPalette const& palette) noexcept
    {
        if (contrastRatio(modePair.foreground, modePair.background) >= MinimumTextContrast)
            return modePair;
        return RGBColorPair { .foreground = legibleInkOn(palette, modePair.background),
                              .background = modePair.background };
    }

    std::optional<ItemColor> parseItemColor(std::string_view text)
    {
        auto const spells = [&](ColorRoleName const& r) {
            return spellsIgnoringAsciiCase(text, r.name);
        };
        if (auto const row = std::ranges::find_if(ColorRoleNames, spells); row != ColorRoleNames.end())
            return ItemColor { row->role };
        if (auto const rgb = parseColor(text))
            return ItemColor { *rgb };
        return std::nullopt;
    }

    std::string formatItemColor(ItemColor const& color)
    {
        if (auto const* const role = std::get_if<ColorRole>(&color))
            return std::string { ColorRoleNames[static_cast<std::size_t>(*role)].name };
        return formatColor(std::get<RGBColor>(color));
    }

    RGBColor roleColor(ColorRole role, ColorPalette const& palette) noexcept
    {
        return palette.indexedColor(ColorRoleNames[static_cast<std::size_t>(role)].paletteIndex);
    }

    RGBColor resolveItemColor(ItemColor const& color, ColorPalette const& palette, RGBColor ground) noexcept
    {
        auto const* const role = std::get_if<ColorRole>(&color);
        if (!role)
            return std::get<RGBColor>(color);
        if (auto const rgb = roleColor(*role, palette); contrastRatio(rgb, ground) >= MinimumTextContrast)
            return rgb;
        return legibleInkOn(palette, ground);
    }

    RGBColor resolveItemBackground(ItemColor const& color, ColorPalette const& palette) noexcept
    {
        if (auto const* const role = std::get_if<ColorRole>(&color))
            return roleColor(*role, palette);
        return std::get<RGBColor>(color);
    }
} // namespace StatusLineDefinitions

static std::optional<StatusLineDefinitions::ItemColor> tryParseColorAttribute(
    crispy::StringInterpolation const& interpolation, std::string_view key)
{
    if (auto const i = interpolation.attributes.find(key); i != interpolation.attributes.end())
        return StatusLineDefinitions::parseItemColor(i->second);

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

/// The raw text after "<Name>:" in a closed placeholder, e.g. "git status" for "{Command:git status}";
/// empty for a placeholder without a colon, an unterminated one, or one built without its source slice.
static std::string_view shorthandOf(crispy::StringInterpolation const& interpolation)
{
    auto text = interpolation.whole;
    if (!text.starts_with('{') || !text.ends_with('}'))
        return {};
    text = text.substr(1, text.size() - 2);
    auto const colon = text.find(':');
    if (colon == std::string_view::npos)
        return {};
    return text.substr(colon + 1);
}

/// Whether @p raw, the text of a `{Extension:<raw>}` shorthand, is an extension's name: not blank, and
/// holding no attribute, since a ',' or '=' means the user wrote attributes and forgot `Name=`.
[[nodiscard]] static bool isExtensionShorthand(std::string_view raw) noexcept
{
    return raw.find_first_not_of(" \t") != std::string_view::npos
           && raw.find_first_of(",={}") == std::string_view::npos;
}

/// Builds the item of type @p T that @p interpolation describes, given its already-parsed @p styles.
///
/// The alternatives carrying payload beyond `Styles` (Text, Command, Extension, Context and Tabs) are the
/// only ones that need saying here; every other placeholder is exactly its styles. Returns nullopt when a
/// required attribute is missing, which makes the placeholder unrecognized and so echoed verbatim by the
/// caller.
template <typename T>
static std::optional<StatusLineDefinitions::Item> makeItemOfType(
    StatusLineDefinitions::Styles const& styles, crispy::StringInterpolation const& interpolation)
{
    if constexpr (std::same_as<T, StatusLineDefinitions::Command>)
    {
        auto const program = tryParseStringAttribute(interpolation, "Program");
        auto const interval = tryParseStringAttribute(interpolation, "Interval");
        if (program)
        {
            auto item = StatusLineDefinitions::Command { styles, *program };
            if (interval)
                if (auto const seconds = core::toInteger<10, int>(*interval); seconds && *seconds > 0)
                    item.interval = std::chrono::seconds { *seconds };
            return item;
        }
        // Shorthand {Command:<raw>}: with neither attribute, everything after "Command:" is the command,
        // verbatim, commas and '=' included. A styled item needs the Program= form.
        if (!interval)
            if (auto const raw = shorthandOf(interpolation);
                raw.find_first_not_of(" \t") != std::string_view::npos)
                return StatusLineDefinitions::Command { StatusLineDefinitions::Styles {},
                                                        std::string { raw } };
        // Without a program there is nothing to run, so this is not a usable {Command}.
        return std::nullopt;
    }
    else if constexpr (std::same_as<T, StatusLineDefinitions::Extension>)
    {
        if (auto name = tryParseStringAttribute(interpolation, "Name"); name && !name->empty())
            return StatusLineDefinitions::Extension { styles, std::move(*name) };
        // Shorthand {Extension:<name>}: everything after "Extension:" is the name, unstyled, exactly as the
        // {Command:<raw>} shorthand is -- so "{Extension:Bold}" names an extension rather than a style.
        if (auto const raw = shorthandOf(interpolation); isExtensionShorthand(raw))
            return StatusLineDefinitions::Extension { StatusLineDefinitions::Styles {}, std::string { raw } };
        // Without a name there is nothing to show, so this is not a usable {Extension}.
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
        return item;
    }
    else if constexpr (std::same_as<T, StatusLineDefinitions::InputMode>)
    {
        auto item = StatusLineDefinitions::InputMode { styles };
        if (auto const mode = tryParseStringAttribute(interpolation, "Hide"))
            item.hiddenIn = StatusLineDefinitions::inputModeFrom(*mode);
        if (interpolation.flags.contains("Chip"))
            item.look = StatusLineDefinitions::InputModeLook::Chip;
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

/// Whether items of type @p T carry a `MaxWidth=`, the widest they may draw in columns.
template <typename T>
concept HasMaxWidth = requires(T t) { t.maxWidth; };

/// Reads @p interpolation's `MaxWidth=` into @p item; a missing, non-numeric or non-positive one keeps
/// the default, so a typo cannot collapse the item to nothing.
template <HasMaxWidth T>
static void readMaxWidth(T& item, crispy::StringInterpolation const& interpolation)
{
    if (auto const width = tryParseStringAttribute(interpolation, "MaxWidth"))
        if (auto const columns = core::toInteger<10, int>(*width); columns && *columns > 0)
            item.maxWidth = ColumnCount(*columns);
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
        if constexpr (HasMaxWidth<T>)
            if (result)
                readMaxWidth(std::get<T>(*result), interpolation);
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

        /// What text drawn over @p background sits on: that background, or the bar's when there is none.
        [[nodiscard]] RGBColor groundOf(
            std::optional<StatusLineDefinitions::ItemColor> const& background) const
        {
            if (background)
                return StatusLineDefinitions::resolveItemBackground(*background, vt.defaultColorPalette());
            return vt.indicatorStatusLineColors().background;
        }

        /// The SGR that selects @p color as the text colour over @p background. A role is resolved now,
        /// against the scheme (the default palette: the status line is host chrome, which a program's OSC 4
        /// does not recolour) and the ground it is drawn on. Empty when there is no colour.
        [[nodiscard]] std::string makeTextColor(
            std::optional<StatusLineDefinitions::ItemColor> const& color,
            std::optional<StatusLineDefinitions::ItemColor> const& background) const
        {
            if (!color)
                return {};

            auto const rgb = StatusLineDefinitions::resolveItemColor(
                *color, vt.defaultColorPalette(), groundOf(background));
            return std::format("\033[38:2:{}:{}:{}m", rgb.red, rgb.green, rgb.blue);
        }

        /// The SGR that selects @p color as the background, a role resolved against the scheme; empty when
        /// there is none.
        [[nodiscard]] std::string makeBackgroundColor(
            std::optional<StatusLineDefinitions::ItemColor> const& color) const
        {
            if (!color)
                return {};

            auto const rgb = StatusLineDefinitions::resolveItemBackground(*color, vt.defaultColorPalette());
            return std::format("\033[48:2:{}:{}:{}m", rgb.red, rgb.green, rgb.blue);
        }

        /// The SGR that fills @p item as a chip in its mode's pair, which wraps its adornments and text
        /// alike; empty when it is no chip, names a colour of its own (user colours win), or is drawn in an
        /// unfocused pane -- only the focused pane carries a coloured chip.
        [[nodiscard]] std::string chipColors(StatusLineDefinitions::InputMode const& item) const
        {
            if (item.look != StatusLineDefinitions::InputModeLook::Chip || item.foregroundColor
                || item.backgroundColor || !vt.focused())
                return {};

            auto const chip = StatusLineDefinitions::modeChipColors(
                vt.indicatorStatusLineColorsFor(vt.inputHandler().mode()), vt.defaultColorPalette());
            auto const fill = std::optional { StatusLineDefinitions::ItemColor { chip.background } };
            return makeTextColor(StatusLineDefinitions::ItemColor { chip.foreground }, fill)
                   + makeBackgroundColor(fill);
        }

        void applyStyles(StatusLineDefinitions::Styles const& styles) // {{{
        {
            if (styling == StatusLineStyling::Disabled)
                return;

            result += makeTextColor(styles.foregroundColor, styles.backgroundColor);
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
                                if constexpr (std::same_as<std::decay_t<decltype(item)>,
                                                           StatusLineDefinitions::InputMode>)
                                    result += chipColors(item);
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

        /// What @p describe says about the grid cell under the pointer -- the cell the view shows there, a
        /// cell of the page on display, which with the view scrolled back is in the history -- or nothing
        /// while the pointer is on no cell.
        /// @param describe Says what the item shows for a cell.
        /// @return The item's text; empty leaves the item out.
        template <typename Describe>
        [[nodiscard]] std::string describePointerCell(Describe const& describe) const
        {
            auto const cell = vt.currentMouseGridPosition();
            return cell ? describe(*cell) : std::string {};
        }

        // {{{
        std::string visit(StatusLineDefinitions::Title const&) { return vt.windowTitle(); }

        std::string visit(StatusLineDefinitions::CellSGR const&)
        {
            return describePointerCell(
                [&](CellLocation cell) { return std::format("{}", vt.displayedPage().cellFlagsAt(cell)); });
        }

        [[nodiscard]] std::string visit(StatusLineDefinitions::CellTextUtf32 const&) const
        {
            return describePointerCell([&](CellLocation cell) { return codepointsOf(cell); });
        }

        /// The codepoints of @p cell's text, as `U+XXXX`, separated by spaces.
        [[nodiscard]] std::string codepointsOf(CellLocation cell) const
        {
            auto const cellText = vt.displayedPage().cellTextAt(cell);
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
            return describePointerCell(
                [&](CellLocation cell) { return core::escape(vt.displayedPage().cellTextAt(cell)); });
        }

        std::string visit(StatusLineDefinitions::Clock const&)
        {
            return std::format("{:%H:%M}", vt.localTimeNow());
        }

        std::string visit(StatusLineDefinitions::HistoryLineCount const&)
        {
            // The viewport onto the page on display, as the numbers below are: nothing while it cannot
            // scroll.
            if (vt.viewport().scrollingDisabled())
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

        /// How far the view is scrolled back, out of the same bound HistoryLineCount divides by; nothing at
        /// the bottom, where every frame of ordinary use is, or while the view cannot scroll.
        [[nodiscard]] std::string visit(StatusLineDefinitions::Scroll const&) const
        {
            auto const& viewport = vt.viewport();
            auto const offset = viewport.scrollOffset();
            if (viewport.scrollingDisabled() || !offset.value)
                return {};
            return std::format("↑ {} / {}",
                               groupDigits(unbox<std::uint64_t>(offset)),
                               groupDigits(unbox<std::uint64_t>(viewport.scrollableLineCount())));
        }

        std::string visit(StatusLineDefinitions::Hyperlink const&)
        {
            return describePointerCell([&](CellLocation cell) {
                auto const hyperlink = vt.displayedPage().hyperlinkAt(cell);
                return hyperlink ? std::format("{}", hyperlink->uri) : std::string {};
            });
        }

        /// The mode's name, or nothing in the mode the item hides in.
        [[nodiscard]] std::string visit(StatusLineDefinitions::InputMode const& item) const
        {
            auto const mode = vt.inputHandler().mode();
            if (item.hiddenIn == mode)
                return {};
            return std::string(modeString(mode));
        }

        std::string visit(StatusLineDefinitions::ProtectedMode const&)
        {
            if (vt.allowInput())
                return {};

            return " (PROTECTED)";
        }

        std::string visit(StatusLineDefinitions::Progress const&)
        {
            // Read without locking, as every other visit() here does: buildStatusLineText runs on
            // whichever thread renders (the GUI's render thread by default) with _stateMutex held.
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
            core::unreachable();
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
            return vt.statusLineCommandOutput(item);
        }

        std::string visit(StatusLineDefinitions::Extension const& item)
        {
            // Measured unstyled: the extension's own SGR takes no columns.
            auto text = vt.statusLineExtensionText(item);
            if (styling == StatusLineStyling::Disabled)
                return withoutSgr(text);
            return text;
        }

        std::string visit(StatusLineDefinitions::Text const& item)
        {
            core::ignoreUnused(this);
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
            // Made safe to show, which is also defence in depth, and not theoretical: this string is handed
            // to writeToScreenInternal(), which parses it. The sequence decoder already rejects control
            // bytes, so one arriving here means a parser bug -- and without this, that bug would be an
            // injection able to clear the status screen from a field any program on the tty can write. A
            // bidi override would reverse the rest of the line.
            return (known ? std::string { rule->prefix } : std::string {})
                   + sanitizeCommandLine(value, SanitizePurpose::Display);
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

            return elideMiddle(
                segments, item.separator.value_or(" › "), item.maxWidth, vt.clusterWidthPolicy());
        }

        std::string visit(StatusLineDefinitions::Cwd const& item)
        {
            auto const directory = vt.resolveWorkingDirectory(CwdPurpose::Display);
            if (!directory)
                return {};

            // Sanitized before it is cut, as the pane header's text is: a byte that is not UTF-8 or is a
            // control becomes U+FFFD, so nothing the shell reports reaches writeToScreenInternal() as an
            // escape.
            auto const shown = sanitizeCommandLine(homeAbbreviatedPath(*directory, vt.homeDirectory()),
                                                   SanitizePurpose::Display);
            // A trailing separator names no further directory: without it, a long path would elide to "/…/",
            // which names nothing. The root keeps its one.
            auto path = std::string_view { shown };
            while (path.size() > 1 && path.ends_with('/'))
                path.remove_suffix(1);
            auto parts = std::vector<std::string> {};
            for (auto const part: std::views::split(path, '/'))
                parts.emplace_back(std::string_view { part });
            return elideMiddle(parts, "/", item.maxWidth, vt.clusterWidthPolicy());
        }

        /// The branch the terminal last read for the shell's directory: no I/O here, as on every frame.
        [[nodiscard]] std::string visit(StatusLineDefinitions::Git const&) const
        {
            return vt.gitBranch();
        }

        /// How the newest command that reported its end went; nothing while one runs, before any, or when
        /// a newer prompt ended it without a report.
        [[nodiscard]] std::string visit(StatusLineDefinitions::LastCommand const& item) const
        {
            using StatusLineDefinitions::ColorRole;

            auto const& blocks = vt.commandBlocks();
            if (auto const* const current = blocks.current();
                current && current->state == CommandBlockState::Running)
                return {};
            auto const* const last = blocks.lastFinished();
            if (!last || last->end != CommandBlockEnd::Reported)
                return {};

            // The status and the duration carry their own colours, as the Tabs strip's active tab does --
            // unless the item names a colour of its own, which then colours all of it.
            auto const intrinsic = styling == StatusLineStyling::Enabled && !item.foregroundColor;
            auto const run = [&](std::string text, ColorRole role) {
                if (!intrinsic)
                    return text;
                return SGRSAVE()
                       + makeTextColor(StatusLineDefinitions::ItemColor { role }, item.backgroundColor) + text
                       + SGRRESTORE();
            };

            auto parts = std::vector<std::string> {};
            if (auto const exitCode = last->exitCode)
                parts.push_back(*exitCode == 0 ? run("✓", ColorRole::Success)
                                               : run(std::format("✗ exit {}", *exitCode), ColorRole::Danger));
            // On one line: a multi-line command's breaks and tabs read as spaces, not as the replacement
            // character display() gives every control. Sanitized after the fold, so no byte of the command
            // line reaches the status screen as an escape.
            auto oneLine = last->commandLine.raw();
            std::ranges::replace_if(oneLine, [](char c) { return c == '\n' || c == '\r' || c == '\t'; }, ' ');
            if (auto command = elideEnd(sanitizeCommandLine(oneLine, SanitizePurpose::Display),
                                        item.maxWidth,
                                        vt.clusterWidthPolicy());
                !command.empty())
                parts.push_back(std::move(command));

            auto text = core::joinWith(parts, " ");
            if (last->duration)
                text += run(
                    std::format("{}{}", text.empty() ? "" : " · ", formatCommandDuration(*last->duration)),
                    ColorRole::Muted);
            return text;
        }

        /// How much the selection holds -- what `y` would copy; nothing before a selection is dragged.
        [[nodiscard]] std::string visit(StatusLineDefinitions::SelectionSize const&) const
        {
            auto const* const selection = vt.selector();
            if (!selection || selection->state() == Selection::State::Waiting)
                return {};

            auto const counted = [](std::size_t count, std::string_view noun) {
                return std::format("{} {}{}", groupDigits(count), noun, count == 1 ? "" : "s");
            };
            auto const extent = selection->extent();
            auto text = counted(unbox<std::size_t>(extent.lines), "line");
            if (extent.characters)
                text += " · " + counted(*extent.characters, "char");
            return text;
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
                    fragment +=
                        makeTextColor(tabs.activeColor,
                                      tabs.activeBackground ? tabs.activeBackground : tabs.backgroundColor);
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

bool showsModeChip(StatusLineDefinition const& definition) noexcept
{
    auto const isChip = [](StatusLineDefinitions::Item const& item) {
        auto const* const inputMode = std::get_if<StatusLineDefinitions::InputMode>(&item);
        return inputMode && inputMode->look == StatusLineDefinitions::InputModeLook::Chip;
    };
    return std::ranges::any_of(definition.left, isChip) || std::ranges::any_of(definition.middle, isChip)
           || std::ranges::any_of(definition.right, isChip);
}

std::string groupDigits(std::uint64_t value)
{
    auto const digits = std::to_string(value);
    auto result = std::string {};
    result.reserve(digits.size() + (digits.size() / 3));
    for (auto const [index, digit]: core::views::enumerate(digits))
    {
        // A space before each group of three counted from the right, but never before the first digit.
        if (index > 0 && (digits.size() - index) % 3 == 0)
            result += ' ';
        result += digit;
    }
    return result;
}

std::string serializeToVT(Terminal const& vt,
                          std::span<StatusLineDefinitions::Item const> items,
                          StatusLineStyling styling)
{
    auto serializer = VTSerializer { .vt = vt, .styling = styling };
    for (auto const& item: items)
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

        /// Appends "key=#RRGGBB" or "key=<role>" when @p color holds a value; a no-op otherwise.
        void addColor(std::string_view key, std::optional<StatusLineDefinitions::ItemColor> const& color)
        {
            // formatItemColor() is the exact inverse of parseItemColor(), which is what
            // tryParseColorAttribute() reads these back with -- so the two ends of the round trip are
            // one function apart rather than two spellings that have to be kept agreeing.
            if (color)
                add(key, StatusLineDefinitions::formatItemColor(*color));
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

    /// Whether @p styles carries no styling at all, so the item may be written in its bare form.
    [[nodiscard]] bool isUnstyled(StatusLineDefinitions::Styles const& styles) noexcept
    {
        return !styles.foregroundColor && !styles.backgroundColor && !styles.flags.any()
               && styles.textLeft.empty() && styles.textRight.empty();
    }

    /// Whether the shorthand {Command:<command>} reads back as an unstyled Command running @p command
    /// with the default interval. A raw that holds a Program= or Interval= attribute, or a brace,
    /// re-parses differently, so it is not written in the shorthand.
    [[nodiscard]] bool shorthandRoundTrips(std::string const& command)
    {
        auto const segment = parseStatusLineSegment("{Command:" + command + "}");
        if (segment.size() != 1 || !std::holds_alternative<StatusLineDefinitions::Command>(segment[0]))
            return false;
        auto const& item = std::get<StatusLineDefinitions::Command>(segment[0]);
        return item.command == command && item.interval == StatusLineDefinitions::DefaultCommandInterval
               && isUnstyled(item);
    }

    /// The payload @p item is written with in its `{<Name>:<payload>}` shorthand, or nullopt when it has
    /// none or the shorthand would not read back as @p item.
    template <typename T>
    [[nodiscard]] std::optional<std::string_view> shorthandPayloadOf(T const& item)
    {
        // A comma or brace cannot survive the Program= form, so an unstyled default-interval command that
        // needs one is written as the shorthand, which carries it verbatim. A styled or non-default-interval
        // command with a comma stays on the Program= form and is lossy, as it always was; the shorthand
        // cannot carry styles or an interval.
        if constexpr (std::same_as<T, StatusLineDefinitions::Command>)
        {
            if (isUnstyled(item) && item.interval == StatusLineDefinitions::DefaultCommandInterval
                && !isAttributeSafe(item.command) && shorthandRoundTrips(item.command))
                return item.command;
        }
        // The shorthand is what a user writes for an extension; kept wherever it reads back the same.
        else if constexpr (std::same_as<T, StatusLineDefinitions::Extension>)
        {
            if (isUnstyled(item) && isExtensionShorthand(item.name))
                return item.name;
        }
        return std::nullopt;
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

    /// Appends what an item of type @p T carries beyond its styles, after them: Context's verbosity and
    /// separator, Tabs' active colours and separator, InputMode's look and hidden mode, and a MaxWidth off
    /// its default.
    template <typename T>
    void appendOwnAttributes(AttributeWriter& attributes, T const& v)
    {
        if constexpr (std::same_as<T, StatusLineDefinitions::Context>)
        {
            // Only what is NOT the default: a config Contour writes must be one Contour reads
            // back, and one it wrote must not gain noise. The default is read off a
            // default-constructed item rather than restated, so it cannot drift from the header.
            if (auto const name = StatusLineDefinitions::contextVerbosityName(v.verbosity); !name.empty())
                attributes.add("Verbosity", std::string { name });
            if (v.separator)
                attributes.add("Separator", *v.separator);
        }

        if constexpr (std::same_as<T, StatusLineDefinitions::Tabs>)
        {
            attributes.addColor("ActiveColor", v.activeColor);
            attributes.addColor("ActiveBackground", v.activeBackground);
            if (v.separator)
                attributes.add("Separator", *v.separator);
        }

        // After the styles, Left and Right among them, as Context's own attributes are.
        if constexpr (std::same_as<T, StatusLineDefinitions::InputMode>)
        {
            if (v.look == StatusLineDefinitions::InputModeLook::Chip)
                attributes.add("Chip");
            if (v.hiddenIn)
                attributes.add("Hide", StatusLineDefinitions::inputModeName(*v.hiddenIn));
        }

        // Last, so that Context keeps writing Verbosity, Separator, MaxWidth.
        if constexpr (HasMaxWidth<T>)
            if (v.maxWidth != T {}.maxWidth)
                attributes.add("MaxWidth", std::to_string(unbox(v.maxWidth)));
    }
} // namespace

StatusLineItemKind StatusLineDefinitions::kindOf(Item const& item) noexcept
{
    auto const* const text = std::get_if<StatusLineDefinitions::Text>(&item);
    return text && isUnstyled(*text) ? StatusLineItemKind::Separator : StatusLineItemKind::Item;
}

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
                    // A bare literal is the form parseStatusLineSegment produces for the plain text
                    // between placeholders, and the only form that can hold a comma or a brace at
                    // all -- so text that cannot be an attribute is emitted this way even when that
                    // costs its styling, because the styled form would come back corrupted instead.
                    if (isUnstyled(v) || !isAttributeSafe(v.text))
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

                if (auto const shorthand = shorthandPayloadOf(v))
                {
                    result += '{';
                    result += StatusLineDefinitions::ItemTraits<T>::Name;
                    result += ':';
                    result += *shorthand;
                    result += '}';
                    return;
                }

                result += '{';
                result += StatusLineDefinitions::ItemTraits<T>::Name;
                auto attributes = AttributeWriter { .out = result };

                if constexpr (std::same_as<T, StatusLineDefinitions::Text>)
                    attributes.add("text", v.text);
                else if constexpr (std::same_as<T, StatusLineDefinitions::Command>)
                {
                    attributes.add("Program", v.command);
                    if (v.interval != StatusLineDefinitions::DefaultCommandInterval)
                        attributes.add("Interval", std::to_string(v.interval.count()));
                }
                else if constexpr (std::same_as<T, StatusLineDefinitions::Extension>)
                    attributes.add("Name", v.name);

                appendStyles(attributes, v);

                appendOwnAttributes(attributes, v);

                result += '}';
            },
            item);
    }

    return result;
}

} // namespace vtbackend
