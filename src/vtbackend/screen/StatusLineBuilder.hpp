// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/CellFlags.hpp>
#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/core/TerminalContext.hpp>
#include <vtbackend/screen/StatusLineLayout.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace vtbackend
{

namespace StatusLineDefinitions
{
    /// A colour a template may name by what it MEANS rather than by its value, resolved against the
    /// terminal's palette when the line is drawn -- so it follows the active scheme, and a live scheme
    /// change.
    enum class ColorRole : uint8_t
    {
        Accent = 0, ///< What draws the eye: the active item, a running job.
        Success,    ///< Something went well.
        Danger,     ///< Something failed.
        Warning,    ///< Something needs attention.
        Muted,      ///< Secondary detail.
    };

    /// One colour role as a template spells it, and the palette entry it resolves to.
    struct ColorRoleName
    {
        ColorRole role;           ///< The role.
        std::string_view name;    ///< As written in a template, lowercase.
        std::size_t paletteIndex; ///< The indexed palette colour the role resolves to.
    };

    /// The colour roles, one row per ColorRole in enumerator order: the parser, the serializer and the
    /// settings page all read this table, so a sixth role is one row here and one enumerator.
    inline constexpr auto ColorRoleNames = std::array {
        ColorRoleName { .role = ColorRole::Accent, .name = "accent", .paletteIndex = 4 },
        ColorRoleName { .role = ColorRole::Success, .name = "success", .paletteIndex = 2 },
        ColorRoleName { .role = ColorRole::Danger, .name = "danger", .paletteIndex = 1 },
        ColorRoleName { .role = ColorRole::Warning, .name = "warning", .paletteIndex = 3 },
        ColorRoleName { .role = ColorRole::Muted, .name = "muted", .paletteIndex = 8 },
    };

    static_assert(
        [] {
            for (auto const index: std::views::iota(std::size_t { 0 }, ColorRoleNames.size()))
                if (static_cast<std::size_t>(ColorRoleNames[index].role) != index)
                    return false;
            return true;
        }(),
        "ColorRoleNames' row i must describe ColorRole enumerator i.");

    /// A status-line item's colour: a fixed RGB value, or a scheme role resolved when drawn.
    using ItemColor = std::variant<RGBColor, ColorRole>;

    /// Reads a colour attribute's value.
    /// @param text A role name (ASCII case-insensitive), or anything parseColor() accepts.
    /// @return The colour, or nullopt when @p text is neither.
    [[nodiscard]] std::optional<ItemColor> parseItemColor(std::string_view text);

    /// Writes a colour attribute's value -- the inverse of parseItemColor().
    /// @param color The colour.
    /// @return The role's lowercase name, or formatColor()'s `#RRGGBB`.
    [[nodiscard]] std::string formatItemColor(ItemColor const& color);

    /// The colour @p role names in @p palette, before any concern for what it is drawn on.
    /// @param role The role.
    /// @param palette The scheme's palette.
    /// @return The palette entry the role's row names.
    [[nodiscard]] RGBColor roleColor(ColorRole role, ColorPalette const& palette) noexcept;

    /// The RGB value text in @p color draws with on @p ground.
    ///
    /// A hex colour is the user's and is drawn as written. A role is the scheme's colour for it while that
    /// reads on @p ground at @ref MinimumTextContrast, and otherwise the ground's legible ink
    /// (legibleInkOn()): blue accent text on the blue Normal-mode bar would vanish, where the ink keeps it
    /// readable.
    /// @param color The text colour.
    /// @param palette The scheme's palette -- the terminal's default one, which programs cannot change.
    /// @param ground What the text is drawn on: the item's own background, or the bar's.
    /// @return The colour to draw the text in.
    [[nodiscard]] RGBColor resolveItemColor(ItemColor const& color,
                                            ColorPalette const& palette,
                                            RGBColor ground) noexcept;

    /// The RGB value a background in @p color fills with: the hex colour, or the role's colour as is.
    /// @param color The background colour.
    /// @param palette The scheme's palette.
    /// @return The fill.
    [[nodiscard]] RGBColor resolveItemBackground(ItemColor const& color,
                                                 ColorPalette const& palette) noexcept;

    struct Styles
    {
        std::optional<ItemColor> foregroundColor;
        std::optional<ItemColor> backgroundColor;
        CellFlags flags;
        std::string textLeft;
        std::string textRight;
    };

    /// How often a {Command} item re-runs when its template names no Interval.
    inline constexpr auto DefaultCommandInterval = std::chrono::seconds { 10 };

    // clang-format off
    struct Title: Styles {};
    struct CellSGR: Styles {};
    struct CellTextUtf32: Styles {};
    struct CellTextUtf8: Styles {};
    struct Clock: Styles {};
    struct Command: Styles
    {
        std::string command;                                    ///< Shell command line, UTF-8.
        std::chrono::seconds interval = DefaultCommandInterval; ///< How often it re-runs.
    };
    /// The latest frame of the status-line extension the configuration calls @ref name, as text and SGR.
    struct Extension: Styles
    {
        /// What the configuration calls the extension. Defaulted, so that `Extension { styles }` builds,
        /// as the settings editor builds every item it does not special-case.
        std::string name {};
    };
    struct HistoryLineCount: Styles {};
    struct Hyperlink: Styles {};
    /// How an `{InputMode}` item draws.
    enum class InputModeLook : uint8_t
    {
        Text = 0, ///< As text on the line, which takes the mode's colours as a whole.
        Chip,     ///< As a chip filled with its mode's colours, on a line that keeps the idle ones.
    };

    /// The vi input mode, `NORMAL`, `VISUAL`, ...: optionally hidden in one mode, and optionally a chip.
    struct InputMode: Styles
    {
        /// The mode in which the item shows nothing, adornments and all; none when unset.
        std::optional<ViMode> hiddenIn {};
        /// Whether the item is drawn as text or as a chip.
        InputModeLook look = InputModeLook::Text;
    };
    struct Progress: Styles {};
    struct ProtectedMode: Styles {};
    struct SearchMode: Styles {};
    struct SearchPrompt: Styles {};
    struct Text: Styles { std::string text; };
    struct TraceMode: Styles {};
    struct VTType: Styles {};

    /// What a `{Context}` breadcrumb shows of the OSC 3008 ancestry.
    enum class ContextVerbosity : uint8_t
    {
        /// Only the contexts that name a privilege or machine boundary, and NOTHING at all when there
        /// is none -- the same collapse ProtectedMode and TraceMode use. That is what lets this ship
        /// in the default indicator line: on a stock systemd install the shim emits only
        /// session/shell/command, so the segment costs an ordinary session nothing until the user
        /// runs run0, ssh, or enters a container.
        Boundaries = 0,
        Full,   ///< The whole ancestry, root-first: "zeta > container:foobar > root > vim".
        Active, ///< The innermost context alone.
    };

    /// The attribute spelling of @p verbosity, or an empty view for the default.
    ///
    /// Paired with @ref contextVerbosityFrom rather than left as two if-chains in the parser and the
    /// serializer: those face in opposite directions, and a spelling that disagreed between them was a
    /// silent round-trip failure that lost the whole placeholder. Same shape as contextTypeName /
    /// contextTypeFrom.
    [[nodiscard]] constexpr std::string_view contextVerbosityName(ContextVerbosity verbosity) noexcept
    {
        switch (verbosity)
        {
            case ContextVerbosity::Full: return "Full";
            case ContextVerbosity::Active: return "Active";
            case ContextVerbosity::Boundaries: break;
        }
        return {};
    }

    /// The verbosity @p name spells, or @ref ContextVerbosity::Boundaries when it spells none.
    ///
    /// An unrecognised spelling keeps the DEFAULT rather than making the whole placeholder unknown: a
    /// typo in one attribute must not silently delete the segment.
    [[nodiscard]] constexpr ContextVerbosity contextVerbosityFrom(std::string_view name) noexcept
    {
        if (name == contextVerbosityName(ContextVerbosity::Full))
            return ContextVerbosity::Full;
        if (name == contextVerbosityName(ContextVerbosity::Active))
            return ContextVerbosity::Active;
        return ContextVerbosity::Boundaries;
    }

    struct Context: Styles
    {
        ContextVerbosity verbosity = ContextVerbosity::Boundaries;

        /// Defaulted explicitly, unlike Tabs::separator: the settings editor builds every item it does
        /// not special-case as `T { styles }`, and a member without an initializer makes that a
        /// -Wmissing-field-initializers error under -Werror.
        std::optional<std::string> separator {};

        /// Widest the breadcrumb may draw, in COLUMNS -- not bytes, because a container name is UTF-8
        /// and may be double-width. Beyond it the MIDDLE is elided and both ends kept: the first
        /// segment answers *where* and the last answers *who*, and the ones between are the least
        /// costly to lose.
        ColumnCount maxWidth { 32 };
    };

    /// One vi input mode as `{InputMode:Hide=<name>}` spells it.
    struct InputModeName
    {
        ViMode mode;           ///< The mode.
        std::string_view name; ///< As written in a template.
    };

    /// The input modes a template may name, one row per ViMode in enumerator order: the parser, the
    /// serializer and the settings page all read this table.
    inline constexpr auto InputModeNames = std::array {
        InputModeName { .mode = ViMode::Normal, .name = "Normal" },
        InputModeName { .mode = ViMode::Insert, .name = "Insert" },
        InputModeName { .mode = ViMode::Visual, .name = "Visual" },
        InputModeName { .mode = ViMode::VisualLine, .name = "VisualLine" },
        InputModeName { .mode = ViMode::VisualBlock, .name = "VisualBlock" },
        InputModeName { .mode = ViMode::Hint, .name = "Hint" },
    };

    static_assert(InputModeNames.size() == ViModeCount, "Every ViMode needs a row in InputModeNames.");
    static_assert(
        [] {
            for (auto const index: std::views::iota(std::size_t { 0 }, InputModeNames.size()))
                if (static_cast<std::size_t>(InputModeNames[index].mode) != index)
                    return false;
            return true;
        }(),
        "InputModeNames' row i must describe ViMode enumerator i.");

    /// The input mode @p name spells.
    /// @param name A mode's name, ASCII case-insensitive.
    /// @return The mode, or nullopt for an unknown name -- so a typo never hides an item.
    [[nodiscard]] std::optional<ViMode> inputModeFrom(std::string_view name) noexcept;

    /// How a template spells @p mode -- the inverse of inputModeFrom().
    /// @param mode The mode.
    /// @return Its row's name in @ref InputModeNames.
    [[nodiscard]] std::string_view inputModeName(ViMode mode) noexcept;

    /// The colours an `{InputMode:Chip}` draws in: filled with @p modePair's background, its text in the
    /// pair's foreground while that reads at @ref MinimumTextContrast, and otherwise in the fill's legible
    /// ink (legibleInkOn()).
    /// @param modePair The indicator status line's pair for the mode shown.
    /// @param palette The scheme's palette, whose ink is tried.
    /// @return The chip's foreground and background.
    [[nodiscard]] RGBColorPair modeChipColors(RGBColorPair modePair, ColorPalette const& palette) noexcept;

    /// The working directory the shell reports, home-abbreviated and middle-elided.
    struct Cwd: Styles
    {
        /// Widest the path may draw, in COLUMNS. Beyond it the MIDDLE components are elided, keeping the
        /// first and the last: `/…/d59`.
        ColumnCount maxWidth { 40 };
    };

    /// The branch, ref or short commit of the repository the shell is in, read from its `.git/HEAD`.
    struct Git: Styles {};

    /// How the last command went, from the shell's command blocks: `✓ make · 4ms`, `✗ exit 8 ctest · 12s`.
    struct LastCommand: Styles
    {
        /// Widest the COMMAND TEXT alone may draw, in columns; a longer one is cut at its end with `…`.
        ColumnCount maxWidth { 32 };
    };

    /// How much the selection holds, as `y` would copy it: `12 lines · 418 chars`.
    struct SelectionSize: Styles {};

    /// How far the view is scrolled back, out of how far it can be: `↑ 1 240 / 10 000`; empty at the bottom.
    struct Scroll: Styles {};

    struct Tabs: Styles
    {
        std::optional<ItemColor> activeColor;
        std::optional<ItemColor> activeBackground;
        std::optional<std::string> separator;
    };

    using Item = std::variant<
        CellSGR,
        CellTextUtf32,
        CellTextUtf8,
        Clock,
        Command,
        Extension,
        HistoryLineCount,
        Hyperlink,
        InputMode,
        Progress,
        ProtectedMode,
        SearchMode,
        SearchPrompt,
        Text,
        Title,
        TraceMode,
        VTType,
        Tabs,
        Context,
        Cwd,
        Git,
        LastCommand,
        SelectionSize,
        Scroll
    >;
    // clang-format on

    /// One cell flag as it appears in a status-line template.
    struct CellFlagName
    {
        std::string_view name;  ///< As written in a template, e.g. "DoubleUnderline".
        std::string_view label; ///< Human-readable, for the settings UI.
        CellFlag flag;
    };

    /// The cell flags a status-line item may carry, named as templates spell them.
    ///
    /// The template spelling is deliberately *not* the enumerator spelling (`DoubleUnderline` vs.
    /// `CellFlag::DoublyUnderlined`), so `CellFlagList` cannot stand in for this table. It is read by
    /// the template parser, the template serializer and the settings page's flag catalog alike, so a
    /// flag cannot be understood by one and dropped by another -- which is exactly what happened
    /// while the serializer knew thirteen flags and the settings bridge knew four, silently
    /// discarding `Underline` and `Inverse` from any profile it saved.
    inline constexpr auto CellFlagNames = std::array {
        CellFlagName { "Bold", "Bold", CellFlag::Bold },
        CellFlagName { "Faint", "Faint", CellFlag::Faint },
        CellFlagName { "Italic", "Italic", CellFlag::Italic },
        CellFlagName { "Underline", "Underline", CellFlag::Underline },
        CellFlagName { "DoubleUnderline", "Double underline", CellFlag::DoublyUnderlined },
        CellFlagName { "CurlyUnderline", "Curly underline", CellFlag::CurlyUnderlined },
        CellFlagName { "DottedUnderline", "Dotted underline", CellFlag::DottedUnderline },
        CellFlagName { "DashedUnderline", "Dashed underline", CellFlag::DashedUnderline },
        CellFlagName { "Overline", "Overline", CellFlag::Overline },
        CellFlagName { "CrossedOut", "Crossed out", CellFlag::CrossedOut },
        CellFlagName { "Blinking", "Blinking", CellFlag::Blinking },
        CellFlagName { "RapidBlinking", "Rapid blinking", CellFlag::RapidBlinking },
        CellFlagName { "Inverse", "Inverse", CellFlag::Inverse },
    };

    /// Per-alternative metadata for an @ref Item: the placeholder name a template spells it with, a
    /// human-readable label, and a short stand-in rendering for a preview (the real value needs a
    /// live Terminal, which a settings page does not have).
    ///
    /// This is the single source of truth for the placeholder vocabulary. @ref
    /// parseStatusLineSegment, @ref serializeStatusLineSegment and the settings page's placeholder
    /// catalog all read it, so **adding a placeholder is one specialization here plus one entry in
    /// @ref Item** -- it was previously six hand-maintained lists, and the two in the GUI had
    /// already drifted (its picker offered no `Command`, while its add-handler had a dead branch for
    /// one).
    ///
    /// Declared but not defined for an unhandled type, so a new @ref Item alternative fails to
    /// compile rather than silently disappearing from parsing and serialization.
    template <typename T>
    struct ItemTraits;

    // clang-format off
    template <> struct ItemTraits<Title>
    { static constexpr std::string_view Name = "Title", Label = "Window title", Sample = "~/src"; static constexpr auto Priority = StatusLinePriority::Normal; };

    template <> struct ItemTraits<Clock>
    { static constexpr std::string_view Name = "Clock", Label = "Clock", Sample = "12:04"; static constexpr auto Priority = StatusLinePriority::Low; };

    template <> struct ItemTraits<InputMode>
    { static constexpr std::string_view Name = "InputMode", Label = "Input mode", Sample = "NORMAL"; static constexpr auto Priority = StatusLinePriority::Essential; };

    template <> struct ItemTraits<TraceMode>
    { static constexpr std::string_view Name = "TraceMode", Label = "Trace mode", Sample = "TRACING"; static constexpr auto Priority = StatusLinePriority::Essential; };

    template <> struct ItemTraits<ProtectedMode>
    { static constexpr std::string_view Name = "ProtectedMode", Label = "Protected mode", Sample = "PROTECTED"; static constexpr auto Priority = StatusLinePriority::Essential; };

    template <> struct ItemTraits<Progress>
    { static constexpr std::string_view Name = "Progress", Label = "Progress indicator", Sample = "45%"; static constexpr auto Priority = StatusLinePriority::High; };

    template <> struct ItemTraits<SearchMode>
    { static constexpr std::string_view Name = "SearchMode", Label = "Search mode", Sample = "SEARCH"; static constexpr auto Priority = StatusLinePriority::Essential; };

    template <> struct ItemTraits<SearchPrompt>
    { static constexpr std::string_view Name = "SearchPrompt", Label = "Search prompt", Sample = "Search: needle"; static constexpr auto Priority = StatusLinePriority::Essential; };

    template <> struct ItemTraits<HistoryLineCount>
    { static constexpr std::string_view Name = "HistoryLineCount", Label = "Scrollback position", Sample = "0/1000"; static constexpr auto Priority = StatusLinePriority::Normal; };

    template <> struct ItemTraits<Hyperlink>
    { static constexpr std::string_view Name = "Hyperlink", Label = "Hovered hyperlink", Sample = "https://contour-terminal.org/"; static constexpr auto Priority = StatusLinePriority::Normal; };

    template <> struct ItemTraits<VTType>
    { static constexpr std::string_view Name = "VTType", Label = "Terminal type", Sample = "VT525"; static constexpr auto Priority = StatusLinePriority::Normal; };

    template <> struct ItemTraits<Tabs>
    { static constexpr std::string_view Name = "Tabs", Label = "Tab strip", Sample = "1 2 3"; static constexpr auto Priority = StatusLinePriority::Normal; };

    template <> struct ItemTraits<Context>
    { static constexpr std::string_view Name = "Context", Label = "Context breadcrumb", Sample = "container:build \u203a root"; static constexpr auto Priority = StatusLinePriority::Essential; };

    template <> struct ItemTraits<Cwd>
    { static constexpr std::string_view Name = "Cwd", Label = "Working directory", Sample = "~/repo/contour"; static constexpr auto Priority = StatusLinePriority::High; };

    template <> struct ItemTraits<Git>
    { static constexpr std::string_view Name = "Git", Label = "Git branch", Sample = "main"; static constexpr auto Priority = StatusLinePriority::Normal; };

    template <> struct ItemTraits<LastCommand>
    { static constexpr std::string_view Name = "LastCommand", Label = "Last command", Sample = "\u2713 make \u00b7 4ms"; static constexpr auto Priority = StatusLinePriority::High; };

    template <> struct ItemTraits<SelectionSize>
    { static constexpr std::string_view Name = "Selection", Label = "Selection size", Sample = "12 lines \u00b7 418 chars"; static constexpr auto Priority = StatusLinePriority::High; };

    template <> struct ItemTraits<Scroll>
    { static constexpr std::string_view Name = "Scroll", Label = "Scroll position", Sample = "\u2191 1 240 / 10 000"; static constexpr auto Priority = StatusLinePriority::High; };

    template <> struct ItemTraits<Text>
    { static constexpr std::string_view Name = "Text", Label = "Static text", Sample = "text"; static constexpr auto Priority = StatusLinePriority::Normal; };

    template <> struct ItemTraits<Command>
    { static constexpr std::string_view Name = "Command", Label = "Shell command output", Sample = "(output)"; static constexpr auto Priority = StatusLinePriority::Normal; };

    template <> struct ItemTraits<Extension>
    { static constexpr std::string_view Name = "Extension", Label = "Extension", Sample = "(extension)"; static constexpr auto Priority = StatusLinePriority::Normal; };

    template <> struct ItemTraits<CellSGR>
    { static constexpr std::string_view Name = "CellSGR", Label = "Cell SGR at cursor", Sample = "1;38:2::255:0:0"; static constexpr auto Priority = StatusLinePriority::Low; };

    template <> struct ItemTraits<CellTextUtf8>
    { static constexpr std::string_view Name = "CellTextUTF8", Label = "Cell text at cursor (UTF-8)", Sample = "E2 96 88"; static constexpr auto Priority = StatusLinePriority::Low; };

    template <> struct ItemTraits<CellTextUtf32>
    { static constexpr std::string_view Name = "CellTextUTF32", Label = "Cell text at cursor (UTF-32)", Sample = "U+2588"; static constexpr auto Priority = StatusLinePriority::Low; };
    // clang-format on

    /// Invokes @p f with a `std::type_identity<T>` for each @ref Item alternative, in variant order.
    ///
    /// Lets a caller loop over the whole placeholder vocabulary via @ref ItemTraits instead of
    /// writing the list out again. Use it wherever a name has to be matched or enumerated.
    ///
    /// @p f is taken by value because it is invoked once per alternative: a forwarding reference
    /// would invite moving from the same callable repeatedly.
    template <typename F>
    constexpr void forEachItemType(F f)
    {
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            (f(std::type_identity<std::variant_alternative_t<I, Item>> {}), ...);
        }(std::make_index_sequence<std::variant_size_v<Item>> {});
    }

    /// Whether @ref ItemTraits gives items of type @p T a priority, which collapsing a narrow line reads.
    template <typename T>
    concept HasPriority = requires {
        { ItemTraits<T>::Priority } -> std::convertible_to<StatusLinePriority>;
    };

    static_assert(
        [] {
            auto covered = true;
            forEachItemType(
                [&](auto tag) { covered = covered && HasPriority<typename decltype(tag)::type>; });
            return covered;
        }(),
        "Every Item alternative needs an ItemTraits<T>::Priority.");

    /// Whether @p item says something of its own or is a bare literal between items, which collapsing a
    /// narrow line drops only with the items beside it.
    /// @param item The item.
    /// @return Separator for unstyled Text -- the literal between placeholders -- and Item for anything else.
    [[nodiscard]] StatusLineItemKind kindOf(Item const& item) noexcept;

    /// How much @p item matters when the line is too narrow for all of it.
    /// @param item The item.
    /// @return Its type's @ref ItemTraits priority.
    [[nodiscard]] inline StatusLinePriority priorityOf(Item const& item) noexcept
    {
        return std::visit(
            [](auto const& alternative) { return ItemTraits<std::decay_t<decltype(alternative)>>::Priority; },
            item);
    }
} // namespace StatusLineDefinitions

using StatusLineSegment = std::vector<StatusLineDefinitions::Item>;

struct StatusLineDefinition
{
    StatusLineSegment left;
    StatusLineSegment middle;
    StatusLineSegment right;
};

/// Whether any segment of @p definition holds an item of type @p T.
/// @tparam T The @ref StatusLineDefinitions::Item alternative to look for.
/// @param definition The status line.
/// @return Whether its left, middle or right segment names a @p T.
template <typename T>
[[nodiscard]] bool containsItem(StatusLineDefinition const& definition) noexcept
{
    auto const holdsOne = [](StatusLineSegment const& segment) {
        return std::ranges::any_of(
            segment, [](StatusLineDefinitions::Item const& item) { return std::holds_alternative<T>(item); });
    };
    return holdsOne(definition.left) || holdsOne(definition.middle) || holdsOne(definition.right);
}

/// Whether @p definition draws its input mode as a chip, in which case the rest of the line keeps the idle
/// colours in every mode.
/// @param definition The status line.
/// @return Whether any of its segments holds an `{InputMode:Chip}`.
[[nodiscard]] bool showsModeChip(StatusLineDefinition const& definition) noexcept;

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

/// Serializes a parsed status line segment back to the config-file template format -- the inverse of
/// @ref parseStatusLineSegment, and round-trip stable through it.
///
/// Example: "{InputMode:Bold,Color=#FFFF00}{Tabs:ActiveColor=#FFFF00,Left= │ }"
///
/// @note A template has no escaping (see the note in crispy::parseInterpolatedString), so a
///       *styled* Text item whose content holds ',', '{' or '}' cannot be expressed: the ',' would
///       read back as an attribute separator. Such content is emitted as an unstyled literal
///       instead, which round-trips exactly, rather than as a styled item that would come back
///       corrupted. Callers offering a text field should reject those characters up front so the
///       user is told, rather than quietly losing the styling. Unstyled text is always safe.
[[nodiscard]] std::string serializeStatusLineSegment(StatusLineSegment const& segment);

/// @p value with its thousands joined by a space, as the status line prints a count: `1240` reads `1 240`.
/// @param value The number.
/// @return Its decimal digits, grouped in threes from the right.
[[nodiscard]] std::string groupDigits(std::uint64_t value);

class Terminal;
/// The VT text that draws @p items on the status screen: SGR and all with @p styling enabled, or the text
/// alone to measure it.
/// @param vt The terminal whose state the items show.
/// @param items A segment, or any run of its items.
/// @param styling Whether the items' styles are written.
/// @return The text.
[[nodiscard]] std::string serializeToVT(Terminal const& vt,
                                        std::span<StatusLineDefinitions::Item const> items,
                                        StatusLineStyling styling);

} // namespace vtbackend
