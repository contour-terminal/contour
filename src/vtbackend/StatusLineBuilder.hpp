// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/CellFlags.hpp>
#include <vtbackend/Color.hpp>

#include <array>
#include <cstddef>
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
    { static constexpr std::string_view Name = "Title", Label = "Window title", Sample = "~/src"; };

    template <> struct ItemTraits<Clock>
    { static constexpr std::string_view Name = "Clock", Label = "Clock", Sample = "12:04"; };

    template <> struct ItemTraits<InputMode>
    { static constexpr std::string_view Name = "InputMode", Label = "Input mode", Sample = "NORMAL"; };

    template <> struct ItemTraits<TraceMode>
    { static constexpr std::string_view Name = "TraceMode", Label = "Trace mode", Sample = "TRACING"; };

    template <> struct ItemTraits<ProtectedMode>
    { static constexpr std::string_view Name = "ProtectedMode", Label = "Protected mode", Sample = "PROTECTED"; };

    template <> struct ItemTraits<SearchMode>
    { static constexpr std::string_view Name = "SearchMode", Label = "Search mode", Sample = "SEARCH"; };

    template <> struct ItemTraits<SearchPrompt>
    { static constexpr std::string_view Name = "SearchPrompt", Label = "Search prompt", Sample = "/needle"; };

    template <> struct ItemTraits<HistoryLineCount>
    { static constexpr std::string_view Name = "HistoryLineCount", Label = "Scrollback position", Sample = "0/1000"; };

    template <> struct ItemTraits<Hyperlink>
    { static constexpr std::string_view Name = "Hyperlink", Label = "Hovered hyperlink", Sample = "https://contour-terminal.org/"; };

    template <> struct ItemTraits<VTType>
    { static constexpr std::string_view Name = "VTType", Label = "Terminal type", Sample = "VT525"; };

    template <> struct ItemTraits<Tabs>
    { static constexpr std::string_view Name = "Tabs", Label = "Tab strip", Sample = "1 2 3"; };

    template <> struct ItemTraits<Text>
    { static constexpr std::string_view Name = "Text", Label = "Static text", Sample = "text"; };

    template <> struct ItemTraits<Command>
    { static constexpr std::string_view Name = "Command", Label = "Shell command output", Sample = "(output)"; };

    template <> struct ItemTraits<CellSGR>
    { static constexpr std::string_view Name = "CellSGR", Label = "Cell SGR at cursor", Sample = "1;38:2::255:0:0"; };

    template <> struct ItemTraits<CellTextUtf8>
    { static constexpr std::string_view Name = "CellTextUTF8", Label = "Cell text at cursor (UTF-8)", Sample = "E2 96 88"; };

    template <> struct ItemTraits<CellTextUtf32>
    { static constexpr std::string_view Name = "CellTextUTF32", Label = "Cell text at cursor (UTF-32)", Sample = "U+2588"; };
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

class Terminal;
std::string serializeToVT(Terminal const& vt, StatusLineSegment const& segment, StatusLineStyling styling);

} // namespace vtbackend
