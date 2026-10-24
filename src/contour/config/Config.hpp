// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/Actions.hpp>
#include <contour/config/BlockActionEnums.hpp>
#include <contour/config/ConfigDocumentation.hpp>
#include <contour/config/ConfigEnum.hpp>
#include <contour/config/ConfigEnumReader.hpp>
#include <contour/config/FinishNotificationConfig.hpp>
#include <contour/config/GpuSelector.hpp>
#include <contour/config/HostUrl.hpp>
#include <contour/config/SingleQuoted.hpp>
#include <contour/config/TabBarMode.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/config/WindowControlStyle.hpp>
#include <contour/config/WindowShadow.hpp>

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/core/LinkDecoration.hpp>
#include <vtbackend/core/Primitives.hpp> // CursorDisplay
#include <vtbackend/core/TerminalContext.hpp>
#include <vtbackend/input/InputBinding.hpp>
#include <vtbackend/input/InputGenerator.hpp>
#include <vtbackend/input/MatchModes.hpp>
#include <vtbackend/links/LinkPatterns.hpp>
#include <vtbackend/screen/GutterSettings.hpp>
#include <vtbackend/screen/Settings.hpp>
#include <vtbackend/shell/MarkArbiter.hpp>
#include <vtbackend/shell/ScrollbarMarks.hpp>
#include <vtbackend/shell/StickyHeader.hpp>
#include <vtbackend/vt/ControlCode.hpp>
#include <vtbackend/vt/VTType.hpp>

#include <vtpty/ImageSize.hpp>
#include <vtpty/PageSize.hpp>
#include <vtpty/Process.hpp>
#include <vtpty/SshSession.hpp>

#include <vtrasterizer/BoxDrawingRenderer.hpp>
#include <vtrasterizer/Decorator.hpp>
#include <vtrasterizer/FontDescriptions.hpp>
#include <vtrasterizer/GlyphScaling.hpp>

#include <text_shaper/Font.hpp>
#include <text_shaper/MockFontLocator.hpp>

#include <crispy/ASCII.hpp>
#include <crispy/Size.hpp>
#include <crispy/StrongLRUHashtable.hpp>

#include <core/Assert.hpp>
#include <core/Environment.hpp>
#include <core/Flags.hpp>
#include <core/Utf8Path.hpp>
#include <core/Utils.hpp>
#include <core/log/LogStore.hpp>

#include <libunicode/convert.h>

#include <yaml-cpp/emitter.h>
#include <yaml-cpp/node/detail/iterator_fwd.h>
#include <yaml-cpp/ostream_wrapper.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <reflection-cpp/reflection.hpp>
#include <vthost/ClientSizePolicy.hpp>
#include <vtworkspace/LayoutTree.hpp> // layout tree structs (LayoutPane, LayoutTab, Layout)

namespace contour::config
{

enum class ScrollBarPosition : uint8_t
{
    Hidden,
    Left,
    Right
};

/// The unit Contour reports pixel sizes to applications in.
///
/// Applications learn the cell size by dividing a reported pixel extent by the grid
/// (`ws_ypixel / ws_row`, or a `CSI 14 t` reply over `CSI 18 t`) and size image canvases from it.
/// On a display with a content scale other than 1 the two answers differ by that scale, so this
/// decides how large a sixel an application draws into a given area.
enum class PixelReporting : uint8_t
{
    Logical, //!< Report logical pixels: what every other terminal reports.
    Device,  //!< Report device pixels: images land 1:1 on the display's own pixels (default).
};

// TabBarPosition and TabBarVisibility live in contour/TabBarMode.h, together with the table that
// describes them: an action carrying one of these modes is declared in Actions.h, which cannot
// include this header (this header includes Actions.h).

/// Selects the light/dark appearance of the Qt GUI chrome (title bar, tab strip, command
/// palette, settings pages, dialogs) independently of the OS.
///
/// This affects the GUI elements only; the terminal grid keeps following the OS light/dark
/// preference through its per-profile color scheme. @c System defers to the operating
/// system's color scheme (the historical behavior); @c Terminal follows the terminal's own
/// background (the default).
/// @note Exposed to QML/Settings as the lower-case strings produced by
///       @c std::formatter<GuiTheme>; the reader parses them case-insensitively.
enum class GuiTheme : uint8_t
{
    System,   //!< Follow the operating system's light/dark color scheme.
    Dark,     //!< Force a dark GUI appearance regardless of the OS.
    Light,    //!< Force a light GUI appearance regardless of the OS.
    Terminal, //!< Follow the luminance of the terminal's background (default).
};

enum class Permission : uint8_t
{
    Deny,
    Allow,
    Ask
};

/// How a permission is spelled in the configuration file.
/// @param value The permission.
/// @return `allow`, `deny` or `ask`.
[[nodiscard]] constexpr std::string_view permissionToken(Permission value) noexcept
{
    switch (value)
    {
        case Permission::Allow: return "allow";
        case Permission::Deny: return "deny";
        case Permission::Ask: return "ask";
    }
    return "ask";
}

enum class SelectionAction : uint8_t
{
    Nothing,
    CopyToSelectionClipboard,
    CopyToClipboard,
};

using ActionList = std::vector<actions::Action>;
using KeyInputMapping = vtbackend::InputBinding<vtbackend::Key, ActionList>;
using CharInputMapping = vtbackend::InputBinding<char32_t, ActionList>;
using MouseInputMapping = vtbackend::InputBinding<vtbackend::MouseButton, ActionList>;

struct InputMappings
{
    std::vector<KeyInputMapping> keyMappings;
    std::vector<CharInputMapping> charMappings;
    std::vector<MouseInputMapping> mouseMappings;
};

struct Config;

/// Enables a fallback row unconditionally. @see FallbackMapping::enabled.
[[nodiscard]] inline bool alwaysEnabled(Config const& /*config*/) noexcept
{
    return true;
}

/// What becomes of an input whose built-in binding matched, but whose actions all declined to run.
enum class DeclinedInput : uint8_t
{
    PassThrough = 0, ///< The input reaches the application, as if nothing were bound to it.
    Consume,         ///< The input is dropped, so a chord the application reads differently does no harm.
};

/// A built-in fallback binding, together with the global-config predicate that enables it.
///
/// The predicate is a COLUMN rather than an `if` at the consultation site: a default that only applies
/// when some option is on is then one more row here, and applyBuiltinFallback() stays the only place
/// that knows gating exists at all.
template <typename Input>
struct FallbackMapping
{
    vtbackend::InputBinding<Input, ActionList> mapping;
    /// Whether this row takes part in matching. Defaults to always.
    ///
    /// A plain function pointer, not std::function: it keeps the table a `static const` literal with
    /// no allocation, on a path walked for every input the application did not claim.
    bool (*enabled)(Config const&) noexcept = alwaysEnabled;
    /// What a declined press does: pass through to the application (the default), or be consumed.
    DeclinedInput declined = DeclinedInput::PassThrough;
};

using FallbackMouseMapping = FallbackMapping<vtbackend::MouseButton>;
using FallbackKeyMapping = FallbackMapping<vtbackend::Key>;
using FallbackCharMapping = FallbackMapping<char32_t>;

/// Bindings that apply when the user's `input_mapping:` does not claim the input itself.
///
/// A new DEFAULT cannot reach an existing user: loading an `input_mapping:` section REPLACES the built-in
/// table wholesale (see YAMLConfigReader::loadFromEntry for InputMappings), and the contour.yml Contour
/// generates on first run writes every default out into that section. So a mapping added to the defaults
/// after a user's config was written would be shadowed by their own file, forever. Consulting these
/// tables *after* theirs is what lets a new default still reach them — while an explicit binding of the
/// same input in their config continues to win, because that one is found first.
///
/// @return The fallback mappings, in match order.
[[nodiscard]] std::vector<FallbackMouseMapping> const& builtinFallbackMouseMappings();

/// @copydoc builtinFallbackMouseMappings
[[nodiscard]] std::vector<FallbackKeyMapping> const& builtinFallbackKeyMappings();

/// @copydoc builtinFallbackMouseMappings
/// Characters, stored folded upper-case as a parsed binding is: a Ctrl+Shift+letter chord arrives as one.
/// @see foldedBindingCodepoint
[[nodiscard]] std::vector<FallbackCharMapping> const& builtinFallbackCharMappings();

namespace helper
{
    inline bool testMatchMode(uint8_t actualModeFlags,
                              vtbackend::MatchModes expected,
                              vtbackend::MatchModes::Flag testFlag)
    {
        using MatchModes = vtbackend::MatchModes;
        switch (expected.status(testFlag))
        {
            case MatchModes::Status::Enabled:
                if (!(actualModeFlags & testFlag))
                    return false;
                break;
            case MatchModes::Status::Disabled:
                if ((actualModeFlags & testFlag))
                    return false;
                break;
            case MatchModes::Status::Any: break;
        }
        return true;
    }

    inline bool testMatchMode(uint8_t actualModeFlags, vtbackend::MatchModes expected)
    {
        using Flag = vtbackend::MatchModes::Flag;
        return testMatchMode(actualModeFlags, expected, Flag::AlternateScreen)
               && testMatchMode(actualModeFlags, expected, Flag::AppCursor)
               && testMatchMode(actualModeFlags, expected, Flag::AppKeypad)
               && testMatchMode(actualModeFlags, expected, Flag::Select)
               && testMatchMode(actualModeFlags, expected, Flag::Insert)
               && testMatchMode(actualModeFlags, expected, Flag::Search)
               && testMatchMode(actualModeFlags, expected, Flag::Trace);
    }

} // namespace helper

/// Folds @p ch into the case that character bindings are stored and matched in (uppercase).
///
/// WHICH case a letter reaches a binding in is decided by the route contour::sendKeyEvent() took,
/// not by the user: the CharMappings table and the Ctrl branch both report the UPPERCASE key label,
/// event->text() reports whatever the layout produced (lowercase for an unmodified press), and on
/// macOS the option-as-Alt branch picks the case from Shift XOR CapsLock. Since the chord carries
/// Shift separately, a letter's case says nothing the modifiers do not already say, so it is
/// normalized away on both sides -- when a binding is parsed, and when a key event is looked up.
///
/// US-ASCII only, which is not merely inherited from crispy::ascii::foldUpper but required here: every route
/// that can report a second case for one key is range-gated to ASCII (`0x20 <= key < 0x80`, and the
/// CharMappings table itself), so no codepoint above 0x7F can ever arrive in two different cases.
/// Folding those would only merge bindings that are distinct today.
///
/// @param ch The codepoint a binding is stored under, or one delivered by a key event.
/// @return The folded codepoint; @p ch unchanged when it is not an ASCII lowercase letter.
[[nodiscard]] constexpr char32_t foldedBindingCodepoint(char32_t ch) noexcept
{
    return crispy::ascii::foldUpper(ch);
}

/// Finds the binding matching @p input under the chord @p modifiers, or nullptr.
///
/// @p modifiers is a chord: lock keys cannot appear in it by construction, so bindings match
/// regardless of the CapsLock/NumLock state.
///
/// @p mappings is any range of bindings, not just a vector, so a filtered view over a gated table can
/// be matched without materialising it. The returned pointer aliases an element of @p mappings, so
/// that range must outlive the result — which it does for the static tables all callers pass.
///
/// @param clickCount The press's multi-click count for a click-count lookup; nullopt for an ordinary one.
template <typename Input, std::ranges::input_range Mappings>
    requires std::same_as<std::ranges::range_value_t<Mappings>, vtbackend::InputBinding<Input, ActionList>>
std::vector<actions::Action> const* apply(Mappings&& mappings,
                                          Input input,
                                          vtbackend::Modifiers modifiers,
                                          uint8_t actualModeFlags,
                                          std::optional<uint8_t> clickCount = std::nullopt)
{
    // Forwarded rather than taken by const&: std::views::filter is not const-iterable, so a filtered
    // view over the gated fallback table could not be walked through a const reference.
    //
    // The click count must match exactly: an ordinary press never reaches a `clicks:` row, and a
    // click-count lookup (TerminalSession's claimant) never reaches a plain one.
    for (vtbackend::InputBinding<Input, ActionList> const& mapping: std::forward<Mappings>(mappings))
    {
        if (mapping.modifiers == modifiers && mapping.input == input && mapping.clickCount == clickCount
            && helper::testMatchMode(actualModeFlags, mapping.modes))
        {
            return &mapping.binding;
        }
    }
    return nullptr;
}

/// Matches @p button against the built-in fallback rows that @p config enables.
///
/// The counterpart to apply() for @ref builtinFallbackMouseMappings: the caller states *what* was
/// pressed, never *which* defaults are currently switched on. A mouse lookup may also state the press's
/// click count, @p clickCount, which matches as it does for apply().
///
/// @return The bound actions, or nullptr when no enabled row matches.
[[nodiscard]] ActionList const* applyBuiltinFallback(Config const& config,
                                                     vtbackend::MouseButton button,
                                                     vtbackend::Modifiers modifiers,
                                                     uint8_t actualModeFlags,
                                                     std::optional<uint8_t> clickCount = std::nullopt);

/// @copydoc applyBuiltinFallback
/// An overload rather than a differently-named function, so every consultation site reads alike.
[[nodiscard]] ActionList const* applyBuiltinFallback(Config const& config,
                                                     vtbackend::Key key,
                                                     vtbackend::Modifiers modifiers,
                                                     uint8_t actualModeFlags);

/// @copydoc applyBuiltinFallback
/// @p codepoint is matched as given, so the caller folds it first (@ref foldedBindingCodepoint).
[[nodiscard]] ActionList const* applyBuiltinFallback(Config const& config,
                                                     char32_t codepoint,
                                                     vtbackend::Modifiers modifiers,
                                                     uint8_t actualModeFlags);

/// The enabled built-in row matching @p key, for a caller that also honours what a declined press does.
/// @return The row, or nullptr when no enabled row matches.
[[nodiscard]] FallbackKeyMapping const* builtinFallbackRow(Config const& config,
                                                           vtbackend::Key key,
                                                           vtbackend::Modifiers modifiers,
                                                           uint8_t actualModeFlags);

/// @copydoc builtinFallbackRow
/// @p codepoint is matched as given, so the caller folds it first (@ref foldedBindingCodepoint).
[[nodiscard]] FallbackCharMapping const* builtinFallbackRow(Config const& config,
                                                            char32_t codepoint,
                                                            vtbackend::Modifiers modifiers,
                                                            uint8_t actualModeFlags);

struct CursorConfig
{
    vtbackend::CursorShape cursorShape { vtbackend::CursorShape::Block };
    vtbackend::CursorDisplay cursorDisplay { vtbackend::CursorDisplay::Steady };
    std::chrono::milliseconds cursorBlinkInterval;
};

/// How deep the scrollback is, and how it gives rows back when it is full.
///
/// `hardLimit` above `maxHistoryLineCount` is what turns on block-atomic eviction: with headroom
/// between the two, what falls out of the buffer is a whole (prompt, output) command rather than
/// whatever happened to be at the limit. Defaulting it to the same value is today's behaviour, so a
/// configuration that names only `limit` is unaffected.
struct HistoryConfig
{
    /// The scrollback depth that always survives. Below the hard limit this is simply THE limit.
    vtbackend::MaxHistoryLineCount maxHistoryLineCount { vtbackend::LineCount(1000) };

    /// The ceiling, and the depth at which eviction stops caring about command boundaries. Anything
    /// at or below `maxHistoryLineCount` means "no ceiling of its own", which is why the default is
    /// zero rather than a second copy of the limit's default -- two literals that must agree are two
    /// chances to disagree. @see limits, which is where the two are reconciled.
    vtbackend::MaxHistoryLineCount hardLimit { vtbackend::LineCount(0) };

    vtbackend::LineCount historyScrollMultiplier { vtbackend::LineCount(3) };
    bool autoScrollOnUpdate { false };

    /// Both bounds, in the vocabulary vtbackend speaks, reconciled in this one place.
    ///
    /// A ceiling below the depth it bounds is raised rather than rejected: that lands on "no
    /// headroom", which is a working configuration rather than an error, and it is what an unset
    /// `hard_limit` therefore means too. A plain max because MaxHistoryLineCount is ORDERED -- the
    /// variant compares by alternative index first, so Infinite sits above every line count and this
    /// reads as "the deeper of the two" across all four combinations. That also carries an infinite
    /// `limit` into the capacity, which it must: a scrollback that never evicts has no ceiling it
    /// could reach.
    ///
    /// @return The reconciled bounds.
    [[nodiscard]] vtbackend::HistoryLimits limits() const noexcept
    {
        return vtbackend::HistoryLimits { .guaranteed = maxHistoryLineCount,
                                          .capacity = std::max(hardLimit, maxHistoryLineCount) };
    }
};

/// Output folding: collapsing a finished command's output down to the prompt line it was entered at.
///
/// The fold ranges come from the OSC 133 semantic marks a shell with shell integration emits, so a shell
/// that emits none simply has nothing to fold. Only FINISHED commands fold, which is why the prompt the
/// user is typing at never does.
///
/// Plain bools rather than enum classes because these are YAML schema fields, converted at the boundary
/// -- the documented carve-out in AGENT.md. The one enum below is not a bool in disguise: it selects
/// between two NAMED outcomes, which is exactly what that carve-out does not cover.
struct FoldingConfig
{
    bool enabled { true };                   ///< Whether folding is available at all.
    bool showMarkers { true };               ///< Draw fold markers in the gutter's block column.
    bool autoCollapseOnNewCommand { false }; ///< Collapse a command as soon as the next prompt starts.

    /// What a targeted Vi-mode jump does when its target sits inside a collapsed fold.
    vtbackend::FoldJumpBehavior onJumpIntoFold { vtbackend::FoldJumpBehavior::Expand };

    /// Whether the gutter's block column draws fold markers.
    ///
    /// Read by gutterSettingsFor() alone, which both the terminal and the window geometry take the
    /// gutter from, so the two cannot disagree about whether the markers -- and with them, possibly, the
    /// column they are drawn in -- are there.
    ///
    /// @return Whether markers are shown.
    [[nodiscard]] constexpr bool markersVisible() const noexcept { return enabled && showMarkers; }

    /// `enabled`, as the engine reads it.
    /// @see vtbackend::Settings::foldingAvailability
    /// @return Whether folding is available.
    [[nodiscard]] constexpr vtbackend::FoldingAvailability availability() const noexcept
    {
        return enabled ? vtbackend::FoldingAvailability::Enabled : vtbackend::FoldingAvailability::Disabled;
    }
};

/// The gutter left of the grid: what its block column shows, its line numbers and its timestamps.
///
/// GLOBAL for FoldingConfig's reason: it describes how the user reads output, not how one shell writes
/// it. Plain bools and an int because these are YAML schema fields, converted at the boundary by
/// gutterSettingsFor() -- the documented carve-out in AGENT.md. Whether the block column draws fold
/// controls stays where it always was, `folding.show_markers`.
struct GutterConfig
{
    /// Colour the block column by outcome, and draw ▸ and ●.
    bool exitStatus { true };

    /// Draw ◆ beside rows marked with vi `mm`.
    bool userMarks { true };

    /// The block column's width in cells, MinBlockColumnWidth..MaxBlockColumnWidth. Two by default: its
    /// fold controls and marks are drawn across both, twice the size, and a click anywhere in them lands.
    int blockColumnWidth { 2 };

    /// Draw the hovered fold's line, head and end one stroke heavier, as well as at full strength.
    bool boldOnHover { true };

    /// Which line numbers, if any.
    vtbackend::LineNumberMode lineNumbers { vtbackend::LineNumberMode::Off };

    /// The columns the line numbers take, MinLineNumberWidth..MaxLineNumberWidth.
    int lineNumberWidth { 6 };

    /// Show the time each row was first reached.
    bool timestamps { false };

    /// A std::format chrono specification, without the braces, spelled in local time.
    std::string timestampFormat { vtbackend::DefaultTimestampFormat };

    bool operator==(GutterConfig const&) const = default;
};

/// OSC 3008 (UAPI.15) hierarchical context signalling: the nested stack of contexts a shell, `run0`,
/// `ssh` or a container runtime opens around whatever runs inside it.
///
/// GLOBAL rather than per-profile, for the reason FoldingConfig is: this describes how the terminal
/// READS a protocol the shell speaks, not how a pane looks. The two knobs that ARE presentation live
/// where presentation already lives -- the color scheme's `tint:` map, and the profile's indicator
/// template. There is deliberately no third place, and in particular no `status_line_breadcrumb` flag:
/// the status line has exactly one placeholder vocabulary, and a boolean elsewhere that force-injected
/// a segment would be invisible to its parser, its serializer and its settings editor.
///
/// The two limits are global on a safety argument as well: a depth cap a profile could lower is a cap
/// an attacker picks by getting the user to open a profile.
///
/// SECURITY: every field OSC 3008 carries is a DISPLAY HINT. Any program holding the tty can write
/// `type=elevate` or `user=root`, so nothing here gates behaviour -- and the ABSENCE of an elevate
/// context is not a statement that nothing is elevated.
///
/// Plain bool for `enabled` because it is a YAML schema field, converted at the boundary -- the
/// documented carve-out in AGENT.md. The two enums below are not bools in disguise: each selects
/// between three NAMED outcomes.
struct OscContextConfig
{
    bool enabled { true }; ///< Whether OSC 3008 is decoded at all.

    /// How deep the ancestry may grow before further pushes are refused.
    ///
    /// Real nesting is ssh -> container -> elevate -> shell -> command, i.e. five. Sixteen leaves room
    /// for the pathological-but-honest case without letting a hostile program cost unbounded memory.
    /// Per the specification the NEWER contexts are dropped on overflow, so a program deep in the
    /// ancestry cannot evict the elevate context above it.
    uint16_t maxDepth { 16 };

    /// How many context records are kept for scrolled-back lines that still point at them.
    /// Must be at least maxDepth, which the reader enforces.
    uint16_t maxRetained { 256 };

    /// Whether OSC 3008 may stand in for a shell integration that is not installed.
    vtbackend::ContextMarkPolicy deriveMarkers { vtbackend::ContextMarkPolicy::WhenAlone };

    /// Which context types may tint the page background.
    ///
    /// The COLORS come from the color scheme's `tint:` map, which is empty in every shipped scheme;
    /// this decides which of its entries are honoured at all.
    vtbackend::ContextTintScope tinting { vtbackend::ContextTintScope::Boundaries };
};

/// The pager OpenCommandOutput runs when its binding names no program: `less -R`, which keeps the
/// colours an `sgr` capture carries, wherever there is one; `more` on Windows, where there is not.
/// @return The default of `command_blocks.pager`.
[[nodiscard]] constexpr std::string_view defaultCommandOutputPager() noexcept
{
#ifdef _WIN32
    return "more";
#else
    return "less -R";
#endif
}

/// What the terminal remembers about each shell command block it saw run: command line, exit status,
/// duration, start time and working directory, kept even after the block's rows have scrolled away.
///
/// GLOBAL rather than per-profile, for the reason FoldingConfig is: it describes how the terminal reads
/// the protocol a shell speaks (OSC 133, OSC 3008), not how one pane looks.
struct CommandBlocksConfig
{
    /// How many blocks one session remembers; the oldest are forgotten first. Bounds what a flood of OSC
    /// 133 sequences -- a `cat` of a recorded session -- can cost. Applies to sessions opened afterwards.
    size_t maxRecords { 1000 };

    /// The program OpenCommandOutput opens a block's output with when its binding names none. Read as a
    /// profile's `shell:` is: a string naming a program runs as it is; otherwise its first word is the
    /// program and the rest its arguments, on Windows passed on as written. The output file is appended
    /// as its last argument.
    std::string pager { defaultCommandOutputPager() };

    /// Whether @ref pager renders SGR, so the file written for it keeps the output's colours. The same
    /// on every platform: `less -R` does, and so does Windows `more` running in the pane's ConPTY, which
    /// interprets the sequences itself.
    actions::OutputFormat pagerFormat { actions::OutputFormat::Sgr };
};

/// The format OpenCommandOutput writes a block's output in: the binding's own when it names one, else
/// the format of the program it runs -- `command_blocks.pager_format` for the configured pager, SGR for a
/// program the binding names.
/// @param action The action.
/// @param commandBlocks The configured pager and its format.
/// @return The format.
[[nodiscard]] constexpr actions::OutputFormat outputFormatFor(
    actions::OpenCommandOutput const& action, CommandBlocksConfig const& commandBlocks) noexcept
{
    if (action.format)
        return *action.format;
    return action.program.empty() ? commandBlocks.pagerFormat : actions::OutputFormat::Sgr;
}

/// The largest `command_blocks.max_records` the reader accepts; a larger value is clamped to it.
constexpr inline int64_t MaxCommandBlockRecords = 1'000'000;

struct ScrollBarConfig
{
    /// Which edge the scrollbar floats against, or Hidden for none (`scrollbar.position`).
    ///
    /// Right by default, so the command marks (@ref marks) are visible out of the box. The bar is an overlay
    /// (SessionChrome.qml): it takes no column from the page, shows only while there is history to scroll
    /// through, and stays off the alternate screen while @ref hideScrollbarInAltScreen is on.
    ScrollBarPosition position { ScrollBarPosition::Right };
    bool hideScrollbarInAltScreen { true };

    /// Which marks the scrollbar draws as ticks (`scrollbar.marks`: a YAML list of `failures`, `commands`,
    /// `user_marks`; `[]` draws none).
    ///
    /// Read by TerminalSession, which filters the terminal's marks with it. The terminal itself reports
    /// every mark and never consults this, so it is not mapped into vtbackend::Settings.
    vtbackend::ScrollbarMarkSources marks { vtbackend::AllScrollbarMarkSources };
};

/// The sticky command header: the command of the block being read, pinned over the top row once it has
/// scrolled out of view above its own output (profile key `sticky_header`).
///
/// A plain bool for `showEvicted` because it is a YAML schema field, converted at the boundary by
/// settings() -- the documented carve-out in AGENT.md. `mode` is not a bool in disguise: it names three
/// outcomes.
struct StickyHeaderConfig
{
    vtbackend::StickyHeaderMode mode { vtbackend::StickyHeaderMode::Scrolled }; ///< When the header shows.
    bool showEvicted { true }; ///< Whether the evicted-output placeholder may stand in for an evicted prompt.

    /// The terminal's view of this configuration; what sessionSettings() hands the terminal.
    /// @return `mode` as it is, and `showEvicted` as the placeholder's enum.
    [[nodiscard]] constexpr vtbackend::StickyHeaderSettings settings() const noexcept
    {
        return vtbackend::StickyHeaderSettings { .mode = mode,
                                                 .evictedPlaceholder =
                                                     showEvicted ? vtbackend::EvictedPlaceholder::Shown
                                                                 : vtbackend::EvictedPlaceholder::Hidden };
    }

    bool operator==(StickyHeaderConfig const&) const = default;
};

struct MouseConfig
{
    bool hideWhileTyping { true };
};

struct IndicatorConfig
{
    // The default indicator status line keeps the {Tabs} segment so the in-terminal tab list is still
    // shown even when the GUI tab strip is hidden or unavailable. (Tabs are also surfaced as first-class
    // GUI tabs in the window title bar; the two are complementary.)
    std::string left { " {InputMode:Bold,Color=#FFFF00}"
                       "{TraceMode:Bold,Color=#FFFF00,Left= │ }"
                       "{Tabs:ActiveColor=#FFFF00,Left= │ }"
                       "{ProtectedMode:Bold,Left= │ }"
                       // Draws NOTHING until a privilege or machine boundary is in force, so an
                       // ordinary session is unchanged by its presence.
                       "{Context:Left= │ }" };
    std::string middle { "« {Title} »" };
    std::string right { "{HistoryLineCount:Faint,Color=#c0c0c0} │ {Clock:Bold}" };
};

struct StatusLineConfig
{
    vtbackend::StatusDisplayType initialType { vtbackend::StatusDisplayType::Indicator };
    vtbackend::StatusDisplayPosition position { vtbackend::StatusDisplayPosition::Bottom };
    bool syncWindowTitleWithHostWritableStatusDisplay { false };
    IndicatorConfig indicator;
    /// The name of the extension (see ExtensionConfig) that takes the whole status line over; unset keeps the
    /// template-driven line, where `{Extension:<name>}` segments may still draw extensions.
    std::optional<std::string> extension;
};

/// An extension's configuration values, which the extension reads by key. Keys and values are text (the YAML
/// boundary); an extension parses its own numbers.
using ExtensionConfigMap = std::map<std::string, std::string>;

/// One entry of a profile's `extensions:` map: a WebAssembly status-line extension, by name.
struct ExtensionConfig
{
    std::string name;           ///< The key under `extensions:`; what `{Extension:<name>}` refers to.
    std::filesystem::path path; ///< The module on disk; empty names a bundled extension, found by @ref name.
    ExtensionConfigMap config;  ///< Values the extension reads through its host `config` call.
    Permission runCommand {
        Permission::Ask
    }; ///< Whether the extension may run the commands its manifest declares.

    bool operator==(ExtensionConfig const&) const = default;
};

struct BackgroundConfig
{
    vtbackend::Opacity opacity { vtbackend::Opacity(0xFF) };
    bool blur { false };
};

struct HyperlinkDecorationConfig
{
    vtbackend::LinkDecorationMode mode {
        vtbackend::LinkDecorationMode::Always
    }; ///< When links are decorated.
    vtrasterizer::Decorator normal { vtrasterizer::Decorator::DottedUnderline };
    vtrasterizer::Decorator hover { vtrasterizer::Decorator::Underline };
};

/// One screen group's switches, as YAML spells them (serialisation boundary: bool by design).
struct ScreenLinkDetectionConfig
{
    bool urls = true;      ///< Whether URLs and custom patterns are detected (not `file:` ones).
    bool filePaths = true; ///< Whether file paths and `file:` URLs are detected.

    bool operator==(ScreenLinkDetectionConfig const&) const = default;
};

/// The profile's link_detection: which links are found automatically in printed text, per screen.
struct LinkDetectionConfig
{
    /// The normal shell screen.
    ScreenLinkDetectionConfig primaryScreen { .urls = true, .filePaths = true };
    /// The alternate screen and every further page.
    ScreenLinkDetectionConfig otherScreens { .urls = true, .filePaths = false };

    bool operator==(LinkDetectionConfig const&) const = default;
};

/// How a detected file path opens (file_opener, §7.4).
struct FileOpenerConfig
{
    /// A command line with {path}, {line} and {column} (FileOpenerTemplate); empty: the system's default
    /// application. Validated when it is read: one with a problem is reported, and kept as written, so that
    /// a save writes it back; it opens files as an empty one does.
    std::string command;
    /// Where the program runs: detached (a program of its own), or a Contour pane, a split or a tab.
    actions::OutputPlacement placement { actions::OutputPlacement::Detached };
};

/// A user-defined hint pattern, or an override of a built-in one.
struct HintPatternConfig
{
    std::string name;  ///< Pattern identifier (e.g. "uuid"); a built-in's name overrides that pattern.
    std::string regex; ///< ECMAScript regex; empty in a name-only override of a built-in.
    /// Detected in output, decorated and opened with Ctrl+click; unset keeps the default. A YAML bool
    /// (serialisation boundary); `linkPatternSpecs()` converts it to `LinkActivation`.
    std::optional<bool> clickable {};
    std::optional<std::string> url {}; ///< A URL template over {0}-{9}; unset opens the match as printed.

    bool operator==(HintPatternConfig const&) const = default;
};

struct PermissionsConfig
{
    Permission captureBuffer { Permission::Ask };
    Permission changeFont { Permission::Ask };
    Permission displayHostWritableStatusLine { Permission::Ask };
};

struct InputModeConfig
{
    CursorConfig cursor;
};

struct DualColorConfig
{
    std::string colorSchemeLight = "default";
    std::string colorSchemeDark = "default";
    vtbackend::ColorPalette darkMode {};
    vtbackend::ColorPalette lightMode {};
};

struct SimpleColorConfig
{
    std::string colorScheme = "default";
    vtbackend::ColorPalette colors {};
};

using ColorConfig = std::variant<SimpleColorConfig, DualColorConfig>;

/// Where a named profile or color scheme was defined, so the GUI can decide whether it may edit it.
///
/// contour.yml-defined and built-in entities are shown but read-only in the settings page (editing
/// them would mean the GUI silently shadowing the hand-maintained file); only @ref SideFile entities,
/// which the GUI itself created, are editable.
enum class SettingsOrigin : uint8_t
{
    Builtin,    //!< A built-in default (e.g. the "default" color palette shipped with Contour).
    MainConfig, //!< Defined inline in the hand-maintained contour.yml.
    SideFile,   //!< Stored in a GUI-managed side file (profiles/<name>.yml, colorschemes/<name>.yml).
};

/// GUI-owned global overrides, persisted to the sibling `settings.yml` and merged over the
/// hand-maintained contour.yml at load (freshest wins, exactly like `layouts.yml`).
///
/// Kept deliberately small: it holds only what the settings page can currently edit at global scope,
/// and grows a field at a time as that surface widens. An unset optional means "defer to contour.yml".
struct GuiManagedSettings
{
    std::optional<std::string> defaultProfile; //!< Overrides contour.yml's `default_profile` when set.

    /// GUI-set global overrides, keyed by the setting's contour.yml path, valued as the YAML scalar to
    /// write. A top-level setting is keyed by its name ("reflow_on_resize" -> "false"); a setting inside
    /// a section by `section.key` ("renderer.gpu" -> "discrete"), which settings.yml stores nested under
    /// the section. Present here == overridden by the GUI; the on-load merge re-applies each through the
    /// same loader contour.yml uses, so the value is typed correctly. Absent keys defer to contour.yml.
    std::map<std::string, std::string> globalOverrides;
};

/// Selects which Qt RHI graphics API drives the terminal display.
///
/// @c Auto lets Qt resolve the platform-native backend (Direct3D 11 on Windows, Metal on macOS,
/// OpenGL on Linux); the remaining values force a specific backend, and @c Software forces a
/// software-emulated OpenGL rasterizer. A backend that the running platform cannot provide
/// (e.g. Metal on Windows) falls back to @c Auto with a warning.
enum class RenderingBackend : uint8_t
{
    Auto,
    OpenGL,
    Vulkan,
    Direct3D11,
    Direct3D12,
    Metal,
    Software,
};

struct RendererConfig
{
    RenderingBackend renderingBackend { RenderingBackend::Auto };
    GpuSelector gpu {}; ///< Which GPU renders (`renderer.gpu`); applied at the next start.
    crispy::LRUCapacity textureAtlasTileCount { 4000u };
    crispy::StrongHashtableSize textureAtlasHashtableSlots { 4096u };
    bool textureAtlasDirectMapping { false };
};

struct ImagesConfig
{
    bool sixelScrolling { true };
    int maxImageColorRegisters { 4096 };
    bool goodImageProtocol { false };
};

/// Whether the terminal hosts its sessions in a daemon process, and how hard it tries to.
enum class DaemonMode : uint8_t
{
    InProcess = 0, ///< Sessions live in the GUI process; no daemon is involved.
    Auto = 1,      ///< Use a daemon; when none can be reached, fall back to in-process with a notice.
    Required = 2,  ///< Use a daemon; when none can be reached, fail loudly.
};

/// The `daemon.mode` spellings, single-sourced: the reader, the writer and the diagnostic read this table.
constexpr auto DaemonModeNames = std::to_array<std::pair<std::string_view, DaemonMode>>({
    { "in_process", DaemonMode::InProcess },
    { "auto", DaemonMode::Auto },
    { "required", DaemonMode::Required },
});

/// @param mode The mode to name.
/// @return Its configuration spelling.
[[nodiscard]] constexpr std::string_view nameOf(DaemonMode mode) noexcept
{
    for (auto const& [name, value]: DaemonModeNames)
        if (value == mode)
            return name;
    return "in_process";
}

/// @param name A configuration spelling, as written by a user.
/// @return The mode it names, or nullopt if it names none.
[[nodiscard]] constexpr std::optional<DaemonMode> daemonModeFrom(std::string_view name) noexcept
{
    for (auto const& [candidate, value]: DaemonModeNames)
        if (candidate == name)
            return value;
    return std::nullopt;
}

/// The most rows `daemon.restore.scrollback_lines` keeps per pane.
inline constexpr std::size_t MaxRestoreScrollbackLines = 100'000;

/// The shortest `daemon.restore.interval`.
inline constexpr auto MinRestoreInterval = std::chrono::seconds { 5 };

/// `daemon.restore`: what a daemon keeps of its workspace across a restart.
struct DaemonRestoreConfig
{
    /// `scrollback`: whether a restored pane gets its history back. A Config boundary bool (AGENT.md
    /// exception); `contour daemon` converts it to vthost::ScrollbackPersistence.
    bool scrollback = true;
    /// `scrollback_lines`: the most rows kept per pane, clamped to [0, MaxRestoreScrollbackLines]. It bounds
    /// the snapshot only; the live scrollback stays `daemon.history_limit`.
    std::size_t scrollbackLines = 1000;
    std::vector<std::string> rerunPrograms {}; ///< `rerun_programs`: what a restored pane may re-run.
    std::chrono::seconds interval {
        30
    }; ///< `interval`: how often it is written, at least MinRestoreInterval.
};

/// The `daemon:` section: whether and how the terminal talks to a session-hosting daemon.
///
/// GLOBAL rather than per-profile: it decides where sessions live, not how one of them looks.
struct DaemonSettings
{
    /// Whether to host sessions in a daemon (`daemon.mode`).
    DaemonMode mode { DaemonMode::Auto };

    /// The daemon's socket path; empty means the `vthost::muxSocketPath` default.
    std::string socket {};

    /// Which attached client's size wins when several disagree (`daemon.size_policy`).
    vthost::ClientSizePolicy sizePolicy { vthost::ClientSizePolicy::Latest };

    /// The scrollback the daemon keeps per session; unset means the hosting profile's `history.limit`.
    std::optional<vtbackend::MaxHistoryLineCount> historyLimit {};

    /// What the daemon keeps of its workspace across a restart (`daemon.restore`).
    DaemonRestoreConfig restore {};
};

struct HorizontalMarginTag
{
};
struct VerticalMarginTag
{
};

using HorizontalMargin = boxed::boxed<unsigned, HorizontalMarginTag>;
using VerticalMargin = boxed::boxed<unsigned, VerticalMarginTag>;

struct WindowMargins
{
    HorizontalMargin horizontal { 0 };
    VerticalMargin vertical { 0 };

    /// Member-wise equality: both axes alike.
    [[nodiscard]] constexpr bool operator==(WindowMargins const&) const noexcept = default;
};

template <typename... T>
struct ConfigEntry
{
};

template <typename T, documentation::StringLiteral configDoc, documentation::StringLiteral webDoc>
struct ConfigEntry<T, documentation::DocumentationEntry<configDoc, webDoc>>
{
    // NOLINTNEXTLINE(readability-identifier-naming): standard iterator/container trait spelling.
    using value_type = T;

    std::string documentation = configDoc.value;
    constexpr ConfigEntry(): _value {} {}

    constexpr explicit ConfigEntry(T in): _value { std::move(in) } {}

    template <typename F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, T>)
    constexpr explicit ConfigEntry(F&& in): _value { std::forward<F>(in) }
    {
    }

    [[nodiscard]] constexpr T const& value() const { return _value; }
    [[nodiscard]] constexpr T& value() { return _value; }

    constexpr ConfigEntry& operator=(T const& value)
    {
        _value = value;
        return *this;
    }

    constexpr ConfigEntry& operator=(T&& value) noexcept
    {
        _value = std::move(value);
        return *this;
    }

    constexpr ConfigEntry(ConfigEntry const&) = default;
    constexpr ConfigEntry& operator=(ConfigEntry const&) = default;
    constexpr ConfigEntry(ConfigEntry&&) noexcept = default;
    constexpr ConfigEntry& operator=(ConfigEntry&&) noexcept = default;
    ~ConfigEntry() = default;

  private:
    T _value;
};

template <typename T>
concept ConfigEntryConcept = requires(T t) {
    t.makeDocumentation();
    t._value;
};

struct Bell
{
    std::string sound = "default";
    bool alert = true;
    float volume = 1.0f;
};

#ifdef __APPLE__
inline auto defaultFamilyName = "Monaco";
#else
inline auto defaultFamilyName = "monospace";
#endif

/// The built-in font configuration, used when the config file does not override it.
///
/// A function-local static rather than a namespace-scope object: its construction allocates, so a
/// throw here is catchable by the caller instead of terminating during static initialization.
/// @return The default font descriptions (constructed on first use).
[[nodiscard]] inline vtrasterizer::FontDescriptions const& defaultFont()
{
    static auto const value = vtrasterizer::FontDescriptions {
        .dpiScale = 1.0,
        .dpi = { 0, 0 },
        .size = { 12 },
        .regular = text::FontDescription { .familyName = { defaultFamilyName },
                                           .weight = text::FontWeight::Normal,
                                           .slant = text::FontSlant::Normal,
                                           .spacing = text::FontSpacing::Mono,
                                           .strictSpacing = false,
                                           .features = {} },
        .bold = text::FontDescription { .familyName = { defaultFamilyName },
                                        .weight = text::FontWeight::Bold,
                                        .slant = text::FontSlant::Normal,
                                        .spacing = text::FontSpacing::Mono,
                                        .strictSpacing = false,
                                        .features = {} },
        .italic = text::FontDescription { .familyName = { defaultFamilyName },
                                          .weight = text::FontWeight::Normal,
                                          .slant = text::FontSlant::Italic,
                                          .spacing = text::FontSpacing::Mono,
                                          .strictSpacing = false,
                                          .features = {} },
        .boldItalic = text::FontDescription { .familyName = { defaultFamilyName },
                                              .weight = text::FontWeight::Bold,
                                              .slant = text::FontSlant::Italic,
                                              .spacing = text::FontSpacing::Mono,
                                              .strictSpacing = false,
                                              .features = {} },
        .emoji = text::FontDescription { .familyName = { "emoji" } },
        .renderMode = text::RenderMode::Gray,
        .textShapingEngine = vtrasterizer::TextShapingEngine::OpenShaper,
        .fontLocator = vtrasterizer::FontLocatorEngine::Native,
        .builtinBoxDrawing = true,
        .maxFallbackCount = vtrasterizer::DefaultMaxFallbackCount,
        .textOutline = {},
    };
    return value;
}

/// The layout tree model (structs + realize/serialize helpers) lives in vtworkspace::LayoutTree so the
/// Qt-free daemon shares it; these aliases keep the config-side spellings working.
using LayoutPane = vtworkspace::LayoutPane;
using LayoutTab = vtworkspace::LayoutTab;
using Layout = vtworkspace::Layout;

struct TerminalProfile
{
    ConfigEntry<vtpty::Process::ExecInfo, documentation::Shell> shell { {
        .program =
            []() {
                auto const program = vtpty::Process::loginShell(true);
                return program | core::views::joinWith(std::string_view(" "));
            }(),
        .arguments = {},
        .workingDirectory = "",
        .env = {},
        .removedEnvironment = {},
    } }; // namespace contour::config
    ConfigEntry<vtpty::SshHostConfig, documentation::SshHostConfig> ssh {};
    ConfigEntry<bool, documentation::EscapeSandbox> escapeSandbox { true };
    ConfigEntry<vtbackend::LineOffset, documentation::CopyLastMarkRangeOffset> copyLastMarkRangeOffset { 0 };
    ConfigEntry<bool, documentation::CopyCommandPreserveMultiline> copyCommandPreserveMultiline { false };
    // show_title_bar now selects the WINDOW DECORATION: true = native server-side frame (+ the OS's
    // own min/max/close controls; our tab strip then omits its custom ones), false = frameless with
    // full client-side decoration (our tab strip draws the controls). Defaults to false so the
    // out-of-box look is the custom CSD tab strip, matching the prior Linux/Windows appearance.
    ConfigEntry<bool, documentation::ShowTitleBar> showTitleBar { false };
    // The drop shadow a FRAMELESS window publishes for itself. Only meaningful alongside
    // show_title_bar: false — with the native frame the window manager draws its own, and a second
    // one would stack on it. Large is Breeze's default, so the out-of-box look matches Plasma.
    ConfigEntry<ShadowSize, documentation::WindowShadow> windowShadow { ShadowSize::Large };
    // Blend amount (0.0 = off .. 1.0 = fully background-colored) applied to a pane while it is not
    // the focused one: an inactive pane of a split, or any pane of an unfocused window. Composited
    // in QML (TerminalPane.qml); the renderer is untouched.
    ConfigEntry<double, documentation::DimUnfocused> dimUnfocused { 0.0 };
    ConfigEntry<bool, documentation::ShowIndicatorOnResize> sizeIndicatorOnResize { true };
    ConfigEntry<bool, documentation::Fullscreen> fullscreen { false };
    ConfigEntry<bool, documentation::Maximized> maximized { false };
    ConfigEntry<bool, documentation::InsertAfterYank> insertAfterYank { false };
    ConfigEntry<Bell, documentation::Bell> bell { { .sound = "default", .alert = true, .volume = 1.0f } };
    ConfigEntry<FinishNotificationConfig, documentation::NotifyOnCommandFinish> notifyOnCommandFinish {};
    ConfigEntry<vtbackend::VTType, documentation::TerminalId> terminalId { vtbackend::VTType::VT525 };
    ConfigEntry<PixelReporting, documentation::PixelReporting> pixelReporting { PixelReporting::Device };
    ConfigEntry<std::map<vtbackend::DECMode, bool>, documentation::FrozenDecMode> frozenModes {};
    ConfigEntry<std::chrono::milliseconds, documentation::SmoothLineScrolling> smoothLineScrolling { 100 };
    ConfigEntry<bool, documentation::SmoothScrolling> smoothScrolling { true };
    ConfigEntry<bool, documentation::MomentumScrolling> momentumScrolling { true };
    ConfigEntry<vtbackend::PageSize, documentation::TerminalSize> terminalSize { {
        .lines = vtbackend::LineCount(25),
        .columns = vtbackend::ColumnCount(80),
    } };
    ConfigEntry<WindowMargins, documentation::Margins> margins { { .horizontal = HorizontalMargin { 0u },
                                                                   .vertical = VerticalMargin { 0u } } };
    ConfigEntry<HistoryConfig, documentation::History> history {};
    ConfigEntry<ScrollBarConfig, documentation::Scrollbar> scrollbar {};
    ConfigEntry<StickyHeaderConfig, documentation::StickyHeader> stickyHeader {};
    ConfigEntry<MouseConfig, documentation::Mouse> mouse { true };
    ConfigEntry<PermissionsConfig, documentation::Permissions> permissions {};
    ConfigEntry<bool, documentation::InputMethodEditorSupport> inputMethodEditor { true };
    ConfigEntry<bool, documentation::HighlightDoubleClickerWord> highlightDoubleClickedWord { true };
    ConfigEntry<vtrasterizer::FontDescriptions, documentation::Fonts> fonts { defaultFont() };
    ConfigEntry<bool, documentation::DrawBoldTextWithBrightColors> drawBoldTextWithBrightColors { false };
    ConfigEntry<vtbackend::SearchCaseSensitivity, documentation::SearchCaseSensitivity>
        searchCaseSensitivity { vtbackend::SearchCaseSensitivity::Smart };
    ConfigEntry<vtbackend::BlinkStyle, documentation::BlinkStyle> blinkStyle {
        vtbackend::BlinkStyle::Smooth
    };
    ConfigEntry<vtbackend::ScreenTransitionStyle, documentation::ScreenTransitionStyle>
        screenTransitionStyle { vtbackend::ScreenTransitionStyle::Fade };
    ConfigEntry<std::chrono::milliseconds, documentation::ScreenTransitionDuration> screenTransitionDuration {
        std::chrono::milliseconds { 250 }
    };
    ConfigEntry<std::chrono::milliseconds, documentation::CursorMotionAnimationDuration>
        cursorMotionAnimationDuration { std::chrono::milliseconds { 80 } };
    ConfigEntry<InputModeConfig, documentation::ModeInsert> modeInsert { CursorConfig {
        .cursorShape = vtbackend::CursorShape::Bar,
        .cursorDisplay = vtbackend::CursorDisplay::Steady,
        .cursorBlinkInterval = std::chrono::milliseconds { 500 } } };
    ConfigEntry<InputModeConfig, documentation::ModeNormal> modeNormal {
        CursorConfig { .cursorShape = vtbackend::CursorShape::Block,
                       .cursorDisplay = vtbackend::CursorDisplay::Steady,
                       .cursorBlinkInterval = std::chrono::milliseconds { 500 } },
    };
    ConfigEntry<InputModeConfig, documentation::ModeVisual> modeVisual {
        CursorConfig { .cursorShape = vtbackend::CursorShape::Block,
                       .cursorDisplay = vtbackend::CursorDisplay::Steady,
                       .cursorBlinkInterval = std::chrono::milliseconds { 500 } },
    };
    ConfigEntry<std::chrono::milliseconds, documentation::HighlightTimeout> highlightTimeout { 100 };
    ConfigEntry<vtbackend::LineCount, documentation::ModalCursorScrollOff> modalCursorScrollOff {
        vtbackend::LineCount { 8 }
    };
    ConfigEntry<StatusLineConfig, documentation::StatusLine> statusLine {};
    ConfigEntry<std::vector<ExtensionConfig>, documentation::Extensions> extensions {};
    ConfigEntry<BackgroundConfig, documentation::Background> background {};
    ConfigEntry<ColorConfig, documentation::Colors> colors { SimpleColorConfig {} };
    ConfigEntry<HyperlinkDecorationConfig, documentation::HyperlinkDecoration> hyperlinkDecoration {};
    ConfigEntry<LinkDetectionConfig, documentation::LinkDetection> linkDetection {};
    ConfigEntry<FileOpenerConfig, documentation::FileOpener> fileOpener {};
    ConfigEntry<std::vector<HintPatternConfig>, documentation::HintPatterns> hintPatterns {};
    ConfigEntry<vtbackend::LineCount, documentation::HintScrollbackLines> hintScrollbackLines {
        vtbackend::LineCount { 1000 }
    };

    ConfigEntry<std::string, documentation::WMClass> wmClass { CONTOUR_APP_ID };
    ConfigEntry<std::string, documentation::TabLabel> tabLabel { "{WindowTitle}" };
    ConfigEntry<std::optional<vtbackend::RGBColor>, documentation::TabColor> tabColor {};
    ConfigEntry<bool, documentation::OptionKeyAsAlt> optionKeyAsAlt { false };
};

/// The built-in key/mouse bindings, used when the config file does not override them.
///
/// A function-local static rather than a namespace-scope object: its construction allocates, so a
/// throw here is catchable by the caller instead of terminating during static initialization.
/// @return The default input mappings (constructed on first use).
[[nodiscard]] inline InputMappings const& defaultInputMappings()
{
    static auto const value = InputMappings {
        .keyMappings {
            KeyInputMapping { .modes { vtbackend::MatchModes {} },
                              .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Alt } },
                              .input = vtbackend::Key::Enter,
                              .binding = { { actions::ToggleFullscreen {} } } },
            KeyInputMapping { .modes = []() -> vtbackend::MatchModes {
                                 auto mods = vtbackend::MatchModes();
                                 mods.enable(vtbackend::MatchModes::Select);
                                 mods.enable(vtbackend::MatchModes::Insert);
                                 return mods;
                             }(),
                              .modifiers { vtbackend::Modifiers {} },
                              .input = vtbackend::Key::Escape,
                              .binding = { { actions::CancelSelection {} } } },
            KeyInputMapping { .modes { vtbackend::MatchModes {} },
                              .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift } },
                              .input = vtbackend::Key::DownArrow,
                              .binding = { { actions::ScrollOneDown {} } } },
            KeyInputMapping { .modes { vtbackend::MatchModes {} },
                              .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift } },
                              .input = vtbackend::Key::End,
                              .binding = { { actions::ScrollToBottom {} } } },
            KeyInputMapping { .modes { vtbackend::MatchModes {} },
                              .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift } },
                              .input = vtbackend::Key::Home,
                              .binding = { { actions::ScrollToTop {} } } },
            KeyInputMapping { .modes { vtbackend::MatchModes {} },
                              .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift } },
                              .input = vtbackend::Key::PageDown,
                              .binding = { { actions::ScrollPageDown {} } } },
            KeyInputMapping { .modes { vtbackend::MatchModes {} },
                              .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift } },
                              .input = vtbackend::Key::PageUp,
                              .binding = { { actions::ScrollPageUp {} } } },
            KeyInputMapping { .modes { vtbackend::MatchModes {} },
                              .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift } },
                              .input = vtbackend::Key::UpArrow,
                              .binding = { { actions::ScrollOneUp {} } } },
            KeyInputMapping { .modes = []() -> vtbackend::MatchModes {
                                 auto mods = vtbackend::MatchModes();
                                 mods.enable(vtbackend::MatchModes::Search);
                                 return mods;
                             }(),
                              .modifiers { vtbackend::Modifiers {} },
                              .input = vtbackend::Key::F3,
                              .binding = { { actions::FocusNextSearchMatch {} } } },
            KeyInputMapping { .modes = []() -> vtbackend::MatchModes {
                                 auto mods = vtbackend::MatchModes();
                                 mods.enable(vtbackend::MatchModes::Search);
                                 return mods;
                             }(),
                              .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift } },
                              .input = vtbackend::Key::F3,
                              .binding = { { actions::FocusPreviousSearchMatch {} } } },
            KeyInputMapping { .modes { vtbackend::MatchModes {} },
                              .modifiers { vtbackend::Modifier::Shift },
                              .input = vtbackend::Key::LeftArrow,
                              .binding = { { actions::SwitchToTabLeft {} } } },
            KeyInputMapping { .modes { vtbackend::MatchModes {} },
                              .modifiers { vtbackend::Modifier::Shift },
                              .input = vtbackend::Key::RightArrow,
                              .binding = { { actions::SwitchToTabRight {} } } },
            // The browser chords for the same thing. They are ALSO in builtinFallbackKeyMappings(), which is
            // what carries them to anyone whose contour.yml predates them; listing them here is what puts
            // them in a freshly generated one, where they can be seen and rebound.
            KeyInputMapping { .modes { vtbackend::MatchModes {} },
                              .modifiers { vtbackend::Modifier::Control },
                              .input = vtbackend::Key::PageUp,
                              .binding = { { actions::SwitchToTabLeft {} } } },
            KeyInputMapping { .modes { vtbackend::MatchModes {} },
                              .modifiers { vtbackend::Modifier::Control },
                              .input = vtbackend::Key::PageDown,
                              .binding = { { actions::SwitchToTabRight {} } } },
            KeyInputMapping { .modes { vtbackend::MatchModes {} },
                              .modifiers { vtbackend::Modifier::Control },
                              .input = vtbackend::Key::Tab,
                              .binding = { { actions::SwitchToTabRight {} } } },
            KeyInputMapping { .modes { vtbackend::MatchModes {} },
                              .modifiers { vtbackend::Modifier::Control, vtbackend::Modifier::Shift },
                              .input = vtbackend::Key::Tab,
                              .binding = { { actions::SwitchToTabLeft {} } } },
            KeyInputMapping { .modes { vtbackend::MatchModes {} },
                              .modifiers { vtbackend::Modifier::Alt, vtbackend::Modifier::Shift },
                              .input = vtbackend::Key::LeftArrow,
                              .binding = { { actions::FocusPaneLeft {} } } },
            KeyInputMapping { .modes { vtbackend::MatchModes {} },
                              .modifiers { vtbackend::Modifier::Alt, vtbackend::Modifier::Shift },
                              .input = vtbackend::Key::RightArrow,
                              .binding = { { actions::FocusPaneRight {} } } },
            KeyInputMapping { .modes { vtbackend::MatchModes {} },
                              .modifiers { vtbackend::Modifier::Alt, vtbackend::Modifier::Shift },
                              .input = vtbackend::Key::UpArrow,
                              .binding = { { actions::FocusPaneUp {} } } },
            KeyInputMapping { .modes { vtbackend::MatchModes {} },
                              .modifiers { vtbackend::Modifier::Alt, vtbackend::Modifier::Shift },
                              .input = vtbackend::Key::DownArrow,
                              .binding = { { actions::FocusPaneDown {} } } },
            // Ctrl+Alt+Arrow: swap the active pane with its neighbor (the two terminals trade slots).
            KeyInputMapping { .modes { vtbackend::MatchModes {} },
                              .modifiers { vtbackend::Modifier::Control, vtbackend::Modifier::Alt },
                              .input = vtbackend::Key::LeftArrow,
                              .binding = { { actions::SwapPaneLeft {} } } },
            KeyInputMapping { .modes { vtbackend::MatchModes {} },
                              .modifiers { vtbackend::Modifier::Control, vtbackend::Modifier::Alt },
                              .input = vtbackend::Key::RightArrow,
                              .binding = { { actions::SwapPaneRight {} } } },
            KeyInputMapping { .modes { vtbackend::MatchModes {} },
                              .modifiers { vtbackend::Modifier::Control, vtbackend::Modifier::Alt },
                              .input = vtbackend::Key::UpArrow,
                              .binding = { { actions::SwapPaneUp {} } } },
            KeyInputMapping { .modes { vtbackend::MatchModes {} },
                              .modifiers { vtbackend::Modifier::Control, vtbackend::Modifier::Alt },
                              .input = vtbackend::Key::DownArrow,
                              .binding = { { actions::SwapPaneDown {} } } },
            // Ctrl+Shift+Alt+Arrow: grow/shrink the active pane by moving the divider in that direction.
            KeyInputMapping {
                .modes { vtbackend::MatchModes {} },
                .modifiers {
                    vtbackend::Modifier::Control, vtbackend::Modifier::Shift, vtbackend::Modifier::Alt },
                .input = vtbackend::Key::LeftArrow,
                .binding = { { actions::ResizePane { .direction = actions::Direction::Left } } } },
            KeyInputMapping {
                .modes { vtbackend::MatchModes {} },
                .modifiers {
                    vtbackend::Modifier::Control, vtbackend::Modifier::Shift, vtbackend::Modifier::Alt },
                .input = vtbackend::Key::RightArrow,
                .binding = { { actions::ResizePane { .direction = actions::Direction::Right } } } },
            KeyInputMapping {
                .modes { vtbackend::MatchModes {} },
                .modifiers {
                    vtbackend::Modifier::Control, vtbackend::Modifier::Shift, vtbackend::Modifier::Alt },
                .input = vtbackend::Key::UpArrow,
                .binding = { { actions::ResizePane { .direction = actions::Direction::Up } } } },
            KeyInputMapping {
                .modes { vtbackend::MatchModes {} },
                .modifiers {
                    vtbackend::Modifier::Control, vtbackend::Modifier::Shift, vtbackend::Modifier::Alt },
                .input = vtbackend::Key::DownArrow,
                .binding = { { actions::ResizePane { .direction = actions::Direction::Down } } } },
            //     KeyInputMapping { .modes { vtbackend::MatchModes {} },
            //                       .modifiers { vtbackend::Modifiers {  vtbackend::Modifier {
            //                       vtbackend::Modifier::Shift }
            //                                    | vtbackend::Modifiers { vtbackend::Modifier::Control }
            //                                    }
            //                                    },
            //                       .input { vtbackend::Key::Plus },
            //                       .binding = { { actions::IncreaseFontSize {} } } },
        },
        .charMappings {
            CharInputMapping { .modes { vtbackend::MatchModes {} },
                               .modifiers { vtbackend::Modifiers {
                                   vtbackend::Modifiers { vtbackend::Modifier::Alt }
                                   | vtbackend::Modifiers { vtbackend::Modifier::Control } } },
                               .input = ',',
                               .binding = { { actions::ToggleInputMethodHandling {} } } },
            CharInputMapping { .modes { vtbackend::MatchModes {} },
                               .modifiers { vtbackend::Modifiers {
                                   vtbackend::Modifiers { vtbackend::Modifier::Shift }
                                   | vtbackend::Modifiers { vtbackend::Modifier::Control } } },
                               .input = '-',
                               .binding = { { actions::DecreaseFontSize {} } } },
            CharInputMapping { .modes { vtbackend::MatchModes {} },
                               .modifiers { vtbackend::Modifiers {
                                   vtbackend::Modifiers { vtbackend::Modifier::Shift }
                                   | vtbackend::Modifiers { vtbackend::Modifier::Control } } },
                               .input = '_',
                               .binding = { { actions::DecreaseFontSize {} } } },
            CharInputMapping { .modes { vtbackend::MatchModes {} },
                               .modifiers { vtbackend::Modifiers {
                                   vtbackend::Modifiers { vtbackend::Modifier::Shift }
                                   | vtbackend::Modifiers { vtbackend::Modifier::Control } } },
                               .input = 'N',
                               .binding = { actions::NewTerminal {} } },
            CharInputMapping { .modes { vtbackend::MatchModes {} },
                               .modifiers { vtbackend::Modifiers {
                                   vtbackend::Modifiers { vtbackend::Modifier::Alt }
                                   | vtbackend::Modifiers { vtbackend::Modifier::Control } } },
                               .input = 'V',
                               .binding = { { actions::PasteClipboard { .strip = true } } } },
            CharInputMapping { .modes { vtbackend::MatchModes {} },
                               .modifiers { vtbackend::Modifiers {
                                   vtbackend::Modifiers { vtbackend::Modifier::Shift }
                                   | vtbackend::Modifiers { vtbackend::Modifier::Control } } },
                               .input = 'V',
                               .binding = { { actions::PasteClipboard { .strip = false } } } },
            CharInputMapping { .modes { vtbackend::MatchModes {} },
                               .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Alt }
                                            | vtbackend::Modifiers { vtbackend::Modifier::Control } },
                               .input = 'S',
                               .binding = { { actions::ScreenshotVT {} } } },
            CharInputMapping { .modes { vtbackend::MatchModes {} },
                               .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control } },
                               .input = '0',
                               .binding = { { actions::ResetFontSize {} } } },
            CharInputMapping { .modes { vtbackend::MatchModes {} },
                               .modifiers { vtbackend::Modifier::Control, vtbackend::Modifier::Shift },
                               .input = 'T',
                               .binding = { { actions::CreateNewTab {} } } },
            // Reads the selection aloud. Does nothing where no speech engine is installed, and the context
            // menu hides its row there, so the binding is harmless on a machine without a voice.
            CharInputMapping { .modes { vtbackend::MatchModes {} },
                               .modifiers { vtbackend::Modifier::Control, vtbackend::Modifier::Shift },
                               .input = 'S',
                               .binding = { { actions::SpeakSelection {} } } },
            CharInputMapping { .modes { vtbackend::MatchModes {} },
                               .modifiers { vtbackend::Modifier::Control, vtbackend::Modifier::Shift },
                               .input = 'E',
                               .binding = { { actions::SplitVertical {} } } },
            CharInputMapping { .modes { vtbackend::MatchModes {} },
                               .modifiers { vtbackend::Modifier::Control, vtbackend::Modifier::Shift },
                               .input = 'O',
                               .binding = { { actions::SplitHorizontal {} } } },
            CharInputMapping { .modes { vtbackend::MatchModes {} },
                               .modifiers { vtbackend::Modifier::Control, vtbackend::Modifier::Shift },
                               .input = 'X',
                               .binding = { { actions::ClosePane {} } } },
            // Ctrl+Shift+P: the command palette. A CharInputMapping (not a KeyInputMapping) because
            // Qt::Key_P is not in helper.cpp's KeyMappings table: any Ctrl+printable is routed through
            // sendCharEvent, so the chord arrives as the character 'P' — exactly like Ctrl+Shift+T above.
            CharInputMapping { .modes { vtbackend::MatchModes {} },
                               .modifiers { vtbackend::Modifier::Control, vtbackend::Modifier::Shift },
                               .input = 'P',
                               .binding = { { actions::OpenCommandPalette {} } } },
            // Ctrl+Shift+Backslash: flip the active pane's split orientation (horizontal <-> vertical).
            CharInputMapping { .modes { vtbackend::MatchModes {} },
                               .modifiers { vtbackend::Modifier::Control, vtbackend::Modifier::Shift },
                               .input = '\\',
                               .binding = { { actions::ToggleSplitOrientation {} } } },
            // Ctrl+Shift+Z: zoom the active pane to fill the tab, and back. Deliberately NOT Alt+F (which
            // issue #91 floated): Alt+F is readline/emacs forward-word, and a default binding here would
            // swallow it before it ever reached the shell.
            CharInputMapping { .modes { vtbackend::MatchModes {} },
                               .modifiers { vtbackend::Modifier::Control, vtbackend::Modifier::Shift },
                               .input = 'Z',
                               .binding = { { actions::TogglePaneZoom {} } } },
            CharInputMapping { .modes { vtbackend::MatchModes {} },
                               .modifiers { vtbackend::Modifier::Alt },
                               .input = '1',
                               .binding = { { actions::SwitchToTab { .position = 1 } } } },
            CharInputMapping { .modes { vtbackend::MatchModes {} },
                               .modifiers { vtbackend::Modifier::Alt },
                               .input = '2',
                               .binding = { { actions::SwitchToTab { .position = 2 } } } },
            CharInputMapping { .modes { vtbackend::MatchModes {} },
                               .modifiers { vtbackend::Modifier::Alt },
                               .input = '3',
                               .binding = { { actions::SwitchToTab { .position = 3 } } } },
            CharInputMapping { .modes { vtbackend::MatchModes {} },
                               .modifiers { vtbackend::Modifier::Alt },
                               .input = '4',
                               .binding = { { actions::SwitchToTab { .position = 4 } } } },
            CharInputMapping { .modes { vtbackend::MatchModes {} },
                               .modifiers { vtbackend::Modifier::Alt },
                               .input = '5',
                               .binding = { { actions::SwitchToTab { .position = 5 } } } },
            CharInputMapping { .modes { vtbackend::MatchModes {} },
                               .modifiers { vtbackend::Modifier::Alt },
                               .input = '6',
                               .binding = { { actions::SwitchToTab { .position = 6 } } } },
            CharInputMapping { .modes { vtbackend::MatchModes {} },
                               .modifiers { vtbackend::Modifier::Alt },
                               .input = '7',
                               .binding = { { actions::SwitchToTab { .position = 7 } } } },
            CharInputMapping { .modes { vtbackend::MatchModes {} },
                               .modifiers { vtbackend::Modifier::Alt },
                               .input = '8',
                               .binding = { { actions::SwitchToTab { .position = 8 } } } },
            CharInputMapping { .modes { vtbackend::MatchModes {} },
                               .modifiers { vtbackend::Modifier::Alt },
                               .input = '9',
                               .binding = { { actions::SwitchToTab { .position = 9 } } } },
            CharInputMapping { .modes { vtbackend::MatchModes {} },
                               .modifiers { vtbackend::Modifier::Alt },
                               .input = '0',
                               .binding = { { actions::SwitchToTab { .position = 10 } } } },
            CharInputMapping { .modes = []() -> vtbackend::MatchModes {
                                  auto mods = vtbackend::MatchModes();
                                  mods.enable(vtbackend::MatchModes::Select);
                                  mods.enable(vtbackend::MatchModes::Insert);
                                  return mods;
                              }(),
                               .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control } },
                               .input = 'C',
                               .binding = { { actions::CopySelection {} } } },
            CharInputMapping { .modes = []() -> vtbackend::MatchModes {
                                  auto mods = vtbackend::MatchModes();
                                  mods.enable(vtbackend::MatchModes::Select);
                                  mods.enable(vtbackend::MatchModes::Insert);
                                  return mods;
                              }(),
                               .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control } },
                               .input = 'C',
                               .binding = { { actions::CancelSelection {} } } },
            CharInputMapping { .modes = []() -> vtbackend::MatchModes {
                                  auto mods = vtbackend::MatchModes();
                                  mods.enable(vtbackend::MatchModes::Select);
                                  mods.enable(vtbackend::MatchModes::Insert);
                                  return mods;
                              }(),
                               .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control } },
                               .input = 'V',
                               .binding = { { actions::PasteClipboard { .strip = false } } } },
            CharInputMapping { .modes = []() -> vtbackend::MatchModes {
                                  auto mods = vtbackend::MatchModes();
                                  mods.enable(vtbackend::MatchModes::Select);
                                  mods.enable(vtbackend::MatchModes::Insert);
                                  return mods;
                              }(),
                               .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control } },
                               .input = 'V',
                               .binding = { { actions::CancelSelection {} } } },
            CharInputMapping { .modes = []() -> vtbackend::MatchModes {
                                  auto mods = vtbackend::MatchModes();
                                  mods.enable(vtbackend::MatchModes::Insert);
                                  return mods;
                              }(),
                               .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift }
                                            | vtbackend::Modifiers { vtbackend::Modifier::Control } },
                               .input = ',',
                               .binding = { { actions::OpenConfiguration {} } } },
            CharInputMapping { .modes = []() -> vtbackend::MatchModes {
                                  auto mods = vtbackend::MatchModes();
                                  mods.enable(vtbackend::MatchModes::Insert);
                                  return mods;
                              }(),
                               .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift }
                                            | vtbackend::Modifiers { vtbackend::Modifier::Control } },
                               .input = ' ', // SPACE
                               .binding = { { actions::ViNormalMode {} } } },
            CharInputMapping { .modes { vtbackend::MatchModes {} },
                               .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift }
                                            | vtbackend::Modifiers { vtbackend::Modifier::Control } },
                               .input = ',',
                               .binding = { { actions::OpenConfiguration {} } } },
            CharInputMapping { .modes { vtbackend::MatchModes {} },
                               .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift }
                                            | vtbackend::Modifiers { vtbackend::Modifier::Control } },
                               .input = 'Q',
                               .binding = { { actions::Quit {} } } },
            CharInputMapping { .modes = []() -> vtbackend::MatchModes {
                                  auto mods = vtbackend::MatchModes();
                                  mods.disable(vtbackend::MatchModes::AlternateScreen);
                                  return mods;
                              }(),
                               .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Alt }
                                            | vtbackend::Modifiers { vtbackend::Modifier::Control } },
                               .input = 'K',
                               .binding = { { actions::ScrollMarkUp {} } } },
            CharInputMapping { .modes = []() -> vtbackend::MatchModes {
                                  auto mods = vtbackend::MatchModes();
                                  mods.disable(vtbackend::MatchModes::AlternateScreen);
                                  return mods;
                              }(),
                               .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Alt }
                                            | vtbackend::Modifiers { vtbackend::Modifier::Control } },
                               .input = 'J',
                               .binding = { { actions::ScrollMarkDown {} } } },
            CharInputMapping { .modes { vtbackend::MatchModes {} },
                               .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Alt }
                                            | vtbackend::Modifiers { vtbackend::Modifier::Control } },
                               .input = 'O',
                               .binding = { { actions::OpenFileManager {} } } },
            CharInputMapping { .modes { vtbackend::MatchModes {} },
                               .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Alt }
                                            | vtbackend::Modifiers { vtbackend::Modifier::Control } },
                               .input = '.',
                               .binding = { { actions::ToggleStatusLine {} } } },
            CharInputMapping { .modes { vtbackend::MatchModes {} },
                               .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift }
                                            | vtbackend::Modifiers { vtbackend::Modifier::Control } },
                               .input = 'F',
                               .binding = { { actions::SearchReverse {} } } },
            CharInputMapping { .modes { vtbackend::MatchModes {} },
                               .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift }
                                            | vtbackend::Modifiers { vtbackend::Modifier::Control } },
                               .input = 'H',
                               .binding = { { actions::NoSearchHighlight {} } } },
            CharInputMapping { .modes { vtbackend::MatchModes {} },
                               .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift }
                                            | vtbackend::Modifiers { vtbackend::Modifier::Control } },
                               .input = 'U',
                               .binding = { { actions::HintMode {
                                   .patterns = "url", .hintAction = vtbackend::HintAction::Open } } } },
        },
        .mouseMappings {
            MouseInputMapping { .modes { vtbackend::MatchModes {} },
                                .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control } },
                                .input = vtbackend::MouseButton::Left,
                                .binding = { { actions::FollowHyperlink {} } } },
            MouseInputMapping { .modes { vtbackend::MatchModes {} },
                                .modifiers { vtbackend::Modifiers { vtbackend::Modifier::None } },
                                .input = vtbackend::MouseButton::Middle,
                                .binding = { { actions::PasteSelection {} } } },
            MouseInputMapping { .modes { vtbackend::MatchModes {} },
                                .modifiers { vtbackend::Modifiers { vtbackend::Modifier::None } },
                                .input = vtbackend::MouseButton::WheelDown,
                                .binding = { { actions::ScrollDown {} } } },
            MouseInputMapping { .modes { vtbackend::MatchModes {} },
                                .modifiers { vtbackend::Modifiers { vtbackend::Modifier::None } },
                                .input = vtbackend::MouseButton::WheelUp,
                                .binding = { { actions::ScrollUp {} } } },
            MouseInputMapping { .modes { vtbackend::MatchModes {} },
                                .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Alt } },
                                .input = vtbackend::MouseButton::WheelDown,
                                .binding = { { actions::DecreaseOpacity {} } } },
            MouseInputMapping { .modes { vtbackend::MatchModes {} },
                                .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Alt } },
                                .input = vtbackend::MouseButton::WheelUp,
                                .binding = { { actions::IncreaseOpacity {} } } },
            MouseInputMapping { .modes { vtbackend::MatchModes {} },
                                .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control } },
                                .input = vtbackend::MouseButton::WheelDown,
                                .binding = { { actions::DecreaseFontSize {} } } },
            MouseInputMapping { .modes { vtbackend::MatchModes {} },
                                .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control } },
                                .input = vtbackend::MouseButton::WheelUp,
                                .binding = { { actions::IncreaseFontSize {} } } },
            MouseInputMapping { .modes { vtbackend::MatchModes {} },
                                .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift } },
                                .input = vtbackend::MouseButton::WheelDown,
                                .binding = { { actions::ScrollPageDown {} } } },
            MouseInputMapping { .modes { vtbackend::MatchModes {} },
                                .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift } },
                                .input = vtbackend::MouseButton::WheelUp,
                                .binding = { { actions::ScrollPageUp {} } } },
        }
    };
    return value;
}

struct Config
{
    std::filesystem::path configFile {};
    ConfigEntry<std::string, documentation::PlatformPlugin> platformPlugin { "auto" };
    ConfigEntry<RendererConfig, documentation::Renderer> renderer {};
    ConfigEntry<std::string, documentation::WordDelimiters> wordDelimiters {
        " /\\()\"'-.,:;<>~!@#$%^&*+=[]{{}}~?|│"
    };
    ConfigEntry<std::string, documentation::ExtendedWordDelimiters> extendedWordDelimiters {
        " /\\()\"'-.,:;<>~!@#$%^&*+=[]{{}}~?|│"
    };
    ConfigEntry<int, documentation::CommandPaletteRecentCount> commandPaletteRecentCount { 5 };
    ConfigEntry<int, documentation::PTYReadBufferSize> ptyReadBufferSize { static_cast<int>(
        vtbackend::DefaultPtyReadBufferSize) };
    ConfigEntry<int, documentation::PTYBufferObjectSize> ptyBufferObjectSize { 1024 * 1024 };
    ConfigEntry<std::string, documentation::DefaultProfiles> defaultProfileName { "main" };
    ConfigEntry<unsigned, documentation::EarlyExitThreshold> earlyExitThreshold {
        documentation::DefaultEarlyExitThreshold
    };
    ConfigEntry<bool, documentation::SpawnNewProcess> spawnNewProcess { false };
    ConfigEntry<bool, documentation::ReflowOnResize> reflowOnResize { true };
    ConfigEntry<bool, documentation::SnapToRows> snapToRows { false };
#ifdef _WIN32
    ConfigEntry<bool, documentation::RememberWindowPosition> rememberWindowPosition { true };
#else
    ConfigEntry<bool, documentation::RememberWindowPosition> rememberWindowPosition { false };
#endif
    ConfigEntry<bool, documentation::TabSwitchOnHorizontalWheel> tabSwitchOnHorizontalWheel { true };
    ConfigEntry<bool, documentation::HyperlinkHoverTooltip> hyperlinkHoverTooltip { true };

    /// How long an OSC 9;4 progress indicator survives without an update. Zero disables expiry, which
    /// is the sequence's own behaviour: the indicator persists until the application clears it.
    ConfigEntry<std::chrono::milliseconds, documentation::ProgressTimeout> progressTimeout {
        std::chrono::milliseconds { 0 }
    };

    /// How long after showing a desktop notification that asked to hear of its close (OSC 99 `c=1`)
    /// the terminal assumes it was closed, where the desktop cannot say. Only the portal backend a
    /// sandboxed Contour must use is in that position; on the session bus a close is observed and
    /// this is never consulted. A notification's own OSC 99 `w=` wins where it states one, and zero
    /// reports nothing at all. A notification that asked for no close report is never assumed closed.
    ConfigEntry<std::chrono::milliseconds, documentation::NotificationCloseTimeout> notificationCloseTimeout {
        std::chrono::milliseconds { 10000 }
    };
    ConfigEntry<bool, documentation::AccessibilityAnnouncements> accessibilityAnnouncements { true };
    ConfigEntry<bool, documentation::AccessibilityCaretReporting> accessibilityCaretReporting { true };
    // The tab bar belongs to the WINDOW, not to the profile a pane happens to run: a window shows one
    // tab bar while its tabs may each run a different profile, so asking a profile where the tab bar
    // sits had no answer when they disagreed.
    ConfigEntry<TabBarPosition, documentation::TabBarPosition> tabBarPosition { TabBarPosition::Top };
    ConfigEntry<TabBarVisibility, documentation::TabBarVisibility> tabBarVisibility {
        TabBarVisibility::Always
    };
    ConfigEntry<bool, documentation::GuiConfigLocked> guiConfigLocked { false };
    ConfigEntry<contour::config::GuiTheme, documentation::Theme> theme {
        contour::config::GuiTheme::Terminal
    };
    // Like the tab bar above, the chrome style belongs to the APPLICATION rather than to a profile: a
    // window paints one tab strip and one set of menus while its panes may each run a different
    // profile, so asking a profile which style the chrome wears has no answer when they disagree.
    ConfigEntry<UiStyle, documentation::UiStyle> uiStyle { UiStyle::Native };
    // Like uiStyle, this belongs to the APPLICATION: a window draws one title bar, so which corner
    // its controls sit in cannot be a per-profile answer. Auto rather than a concrete default so the
    // shipped configuration follows whatever host it is opened on.
    ConfigEntry<WindowControlStyle, documentation::WindowControlStyle> windowControlStyle {
        WindowControlStyle::Auto
    };
    // Empty / 0 mean "inherit the default profile's regular font", which is what makes the
    // terminal-style chrome match the grid below it. Resolved in UiStyleProvider, not here, because
    // the default profile is not known until the whole config has been read.
    ConfigEntry<std::string, documentation::UiFontFamily> uiFontFamily { "" };
    ConfigEntry<double, documentation::UiFontSize> uiFontSize { 0.0 };

    /// How a glyph is enlarged for scaled text (kitty text sizing protocol, OSC 66).
    /// @see vtrasterizer::GlyphScalingMethod for what each method costs.
    /// Whether DEC mode 2027 starts out set. @see vtbackend::ClusterWidthPolicy.
    ConfigEntry<bool, documentation::GraphemeClustering> graphemeClustering { true };

    ConfigEntry<vtrasterizer::GlyphScalingMethod, documentation::TextScalingMethod> textScalingMethod {
        vtrasterizer::GlyphScalingMethod::Rerasterize
    };
    ConfigEntry<vtbackend::Modifiers, documentation::BypassMouseProtocolModifiers>
        bypassMouseProtocolModifiers { vtbackend::Modifier::Shift };
    ConfigEntry<vtbackend::Modifiers, documentation::MouseBlockSelectionModifiers>
        mouseBlockSelectionModifiers { vtbackend::Modifier::Control };
    ConfigEntry<contour::config::SelectionAction, documentation::OnMouseSelection> onMouseSelection {
        contour::config::SelectionAction::CopyToSelectionClipboard
    };
    ConfigEntry<bool, documentation::Live> live { false };
    ConfigEntry<std::set<std::string>, documentation::ExperimentalFeatures> experimentalFeatures {};
    ConfigEntry<ImagesConfig, documentation::Images> images {};
    ConfigEntry<DaemonSettings, documentation::Daemon> daemon {};
    ConfigEntry<FoldingConfig, documentation::Folding> folding {};
    ConfigEntry<GutterConfig, documentation::Gutter> gutter {};
    ConfigEntry<OscContextConfig, documentation::OscContext> oscContext {};
    ConfigEntry<CommandBlocksConfig, documentation::CommandBlocks> commandBlocks {};

    ConfigEntry<std::unordered_map<std::string, TerminalProfile>, documentation::Profiles> profiles {
        { { "main", TerminalProfile {} } }
    };
    ConfigEntry<std::unordered_map<std::string, Layout>, documentation::Layouts> layouts {};
    ConfigEntry<std::string, documentation::DefaultLayout> defaultLayoutName { "" };
    /// The saved remote hosts, by name. Ordered, so every listing is sorted by name without a sort at
    /// each consumer.
    ConfigEntry<std::map<std::string, HostEntry>, documentation::Hosts> hosts {};
    ConfigEntry<std::unordered_map<std::string, vtbackend::ColorPalette>, documentation::ColorSchemes>
        colorschemes { { { "default", vtbackend::ColorPalette {} } } };

    /// Runtime-only provenance of each entry in @ref profiles / @ref colorschemes, populated by the
    /// loader (never serialized). The GUI consults these to gate editability: a name absent from the
    /// map, or mapped to anything other than SettingsOrigin::SideFile, is read-only in the settings page.
    /// @{
    std::unordered_map<std::string, SettingsOrigin> profileOrigins {};
    std::unordered_map<std::string, SettingsOrigin> colorSchemeOrigins {};
    /// @}

    /// GUI-owned global overrides loaded from the sibling `settings.yml` (see GuiManagedSettings).
    /// Runtime-only mirror of that file; the resolved values have already been applied to the fields
    /// above (e.g. @ref defaultProfileName), this simply records what the GUI had persisted.
    GuiManagedSettings guiManagedSettings {};

    ConfigEntry<InputMappings, documentation::InputMappings> inputMappings { defaultInputMappings() };
    ConfigEntry<vtrasterizer::BoxDrawingRenderer::GitDrawingsStyle, documentation::GitDrawings>
        gitDrawings {};
    ConfigEntry<vtrasterizer::BoxDrawingRenderer::ArcStyle, documentation::BoxArcStyle> boxArcStyle {
        vtrasterizer::BoxDrawingRenderer::ArcStyle::Round
    };
    ConfigEntry<vtrasterizer::BoxDrawingRenderer::BrailleStyle, documentation::BrailleStyle> brailleStyle {
        vtrasterizer::BoxDrawingRenderer::BrailleStyle::Circle
    };

    /// Looks up a profile by name, returning nullptr if none exists — the FALLIBLE lookup for
    /// runtime input (a keybinding or {ChangeProfile} naming a profile the user may have removed).
    /// @param name Profile name to find.
    /// @return The profile, or nullptr if @p name is not configured.
    [[nodiscard]] TerminalProfile* findProfile(std::string const& name) noexcept
    {
        auto i = profiles.value().find(name);
        return i != profiles.value().end() ? &i->second : nullptr;
    }
    /// @copydoc findProfile
    [[nodiscard]] TerminalProfile const* findProfile(std::string const& name) const noexcept
    {
        auto i = profiles.value().find(name);
        return i != profiles.value().end() ? &i->second : nullptr;
    }

    /// Looks up a profile by name whose existence is a PRECONDITION (asserts on a miss). Use only
    /// where the name provably names a configured profile (e.g. the default profile); for runtime
    /// input use findProfile(), which reports a miss instead of aborting.
    /// @param name Profile name that must exist.
    /// @return The profile (never nullptr in a correct program).
    TerminalProfile* profile(std::string const& name) noexcept
    {
        assert(!name.empty());
        auto* const found = findProfile(name);
        assert(found != nullptr && "Profile not found.");
        return found;
    }

    /// @copydoc profile
    [[nodiscard]] TerminalProfile const* profile(std::string const& name) const
    {
        assert(!name.empty());
        auto const* const found = findProfile(name);
        assert(found != nullptr && "Profile not found.");
        if (found == nullptr)
            core::unreachable();
        return found;
    }

    TerminalProfile& profile() noexcept
    {
        if (auto* prof = profile(defaultProfileName.value()); prof)
            return *prof;
        core::unreachable();
    }

    [[nodiscard]] TerminalProfile const& profile() const noexcept
    {
        if (auto const* prof = profile(defaultProfileName.value()); prof)
            return *prof;
        core::unreachable();
    }
};

struct YAMLConfigReader
{
    /// Callable that resolves a variable name to its value.
    using VariableReplacer = std::function<std::string(std::string_view)>;

    std::filesystem::path configFile;
    YAML::Node doc;
    core::log::Category const& logger;
    VariableReplacer variableReplacer;

    /// @param filename The document to parse.
    /// @param log      Where parse diagnostics go.
    /// @param env      The environment `${VAR}` expands against. It must outlive this reader: the
    ///                 default replacer holds a reference to it.
    /// @param replacer An expansion policy of the caller's own; the default one reads @p env.
    YAMLConfigReader(std::filesystem::path const& filename,
                     core::log::Category const& log,
                     core::Environment const& env,
                     VariableReplacer replacer = {});

    /// Reads a document the caller has parsed already, so that a file it has to parse anyway, to tell
    /// a broken one from an empty one, is not read and parsed a second time.
    /// @param filename Where @p document came from.
    /// @param document The parsed document.
    /// @param log      Where diagnostics go.
    /// @param env      The environment `${VAR}` expands against. It must outlive this reader: the
    ///                 default replacer holds a reference to it.
    /// @param replacer An expansion policy of the caller's own; the default one reads @p env.
    YAMLConfigReader(std::filesystem::path filename,
                     YAML::Node const& document,
                     core::log::Category const& log,
                     core::Environment const& env,
                     VariableReplacer replacer = {});

    /// Expands `${VAR}` tokens in @p input using the configured variable replacer.
    [[nodiscard]] std::string resolveVariables(std::string const& input) const;

    /// Expands `${VAR}` tokens then resolves `~` to the home directory.
    [[nodiscard]] std::filesystem::path resolvedPath(std::string const& input) const;

    /// The directory a relative path in the document being read is relative to: the one holding the file
    /// that declares it -- the side file inside readSideFile(), else the configuration file.
    /// @return That directory, made absolute.
    [[nodiscard]] std::filesystem::path declaringDirectory() const;

    /// Parses a single pane node. A node is a LEAF unless it carries a `split:` mapping.
    void parseLayoutPane(YAML::Node const& node, LayoutPane& where);

    /// Loads @p entry of @p node into @p where, so that a value of the wrong shape costs that entry alone.
    ///
    /// yaml-cpp throws when a value has another shape than its reader expects: a scalar where a map
    /// belongs, a list where one word belongs. Uncaught, that throw would end the whole load, and every
    /// entry read after it would keep its default without a word. Instead @p where is put back as it
    /// was, the entry is reported by its whole key path, and loading goes on. A reader subscripts its
    /// own value under its own guard, so a value of the wrong shape is blamed on its own key.
    /// @param node  The mapping to read from.
    /// @param entry The key within @p node.
    /// @param where Receives the value; unchanged when the entry cannot be read.
    /// @param args  Passed on to the reader of @p where.
    template <typename T, typename... Args>
    void loadGuarded(YAML::Node const& node, std::string const& entry, T& where, Args&&... args)
    {
        readGuarded(
            node, entry, where, [&]() { loadFromEntry(node, entry, where, std::forward<Args>(args)...); });
    }

    /// Runs @p read, which reads @p entry of @p node into @p where, under loadGuarded()'s guard: for an
    /// entry whose reader is no loadFromEntry() overload of its own, such as one map of a colour scheme.
    /// @param node  The mapping @p entry is read from.
    /// @param entry The key within @p node.
    /// @param where What @p read may change; put back as it was when @p read throws.
    /// @param read  Reads the entry.
    template <typename T, typename Reader>
    void readGuarded(YAML::Node const& node, std::string const& entry, T& where, Reader const& read)
    {
        auto fallback = where;
        _keyPath.push_back(KeyStep { .container = node, .entry = entry });
        auto const leave = core::Finally { [this]() { _keyPath.pop_back(); } };
        try
        {
            read();
        }
        catch (std::exception const& e)
        {
            where = std::move(fallback);
            errorLog()("Ignoring the malformed config entry {}: {}", keyPathTo(node, entry), e.what());
        }
    }

    /// Runs @p read with every key path it reports rooted at side file @p file -- `<file>: <key>` --
    /// rather than at the document this reader began with: a side file's entry is found in that file,
    /// not where the configuration names it, and settings.yml spells its keys as contour.yml does.
    /// @param file     The side file @p document was parsed from.
    /// @param document The side file's root.
    /// @param read     Reads from @p document.
    template <typename Reader>
    void readSideFile(std::filesystem::path const& file, YAML::Node const& document, Reader const& read)
    {
        auto outerPath = std::exchange(_keyPath,
                                       std::vector { KeyStep { .container = document,
                                                               .entry = core::pathToUtf8(file),
                                                               .kind = KeyStepKind::File } });
        auto const restore = core::Finally { [&]() { _keyPath = std::move(outerPath); } };
        read();
    }

    /// Reads and parses side file @p file -- settings.yml, a profile's file or a colour scheme's -- once,
    /// for readSideFile() to read from.
    /// @param file The side file.
    /// @return Its document, a map or empty, or why there is none: `<file>: <reason>`, a missing file and
    ///         a document of another shape included.
    [[nodiscard]] static std::expected<YAML::Node, std::string> parseSideFile(
        std::filesystem::path const& file);

    /// @p entry of @p node, spelled out from the document's root as loadGuarded() descended to it:
    /// `profiles.main.cursor`.
    /// @param node  The mapping @p entry is read from.
    /// @param entry The key within @p node.
    /// @return The key path.
    [[nodiscard]] std::string keyPathTo(YAML::Node const& node, std::string_view entry) const;

    template <typename T, documentation::StringLiteral ConfigDoc, documentation::StringLiteral WebDoc>
    void loadFromEntry(YAML::Node const& node,
                       std::string const& entry,
                       ConfigEntry<T, documentation::DocumentationEntry<ConfigDoc, WebDoc>>& where)
    {
        loadGuarded(node, entry, where.value());
    }

    template <typename T,
              documentation::StringLiteral ConfigDoc,
              documentation::StringLiteral WebDoc,
              typename... Args>
    void loadFromEntry(std::string const& entry,
                       ConfigEntry<T, documentation::DocumentationEntry<ConfigDoc, WebDoc>>& where,
                       Args&&... args)
    {
        loadGuarded(doc, entry, where.value(), std::forward<Args>(args)...);
    }

    template <typename T>
        requires(std::is_scalar_v<T> && !ConfigEnumConcept<T>)
    void loadFromEntry(YAML::Node const& node, std::string const& entry, T& where)
    {
        auto const child = node[entry];
        if (child)
            where = child.as<T>();
        logger()("Loading entry: {}, value {}", entry, where);
    }

    /// Reads an enum that has a configuration table, through that table.
    ///
    /// The settings page builds its combo box from the very same rows (@see configEnumValues), so a
    /// value this reader accepts is by construction one the settings page can offer and the writer
    /// can spell back. An unknown word, a list or a map is reported and leaves @p where as it was.
    /// @param node  The mapping to read from.
    /// @param entry The key within @p node.
    /// @param where Receives the value.
    template <ConfigEnumConcept Enum>
    void loadFromEntry(YAML::Node const& node, std::string const& entry, Enum& where)
    {
        (void) detail::loadConfigEnum(node, entry, keyPathTo(node, entry), where, logger);
    }

    template <typename V, typename T>
    void loadFromEntry(YAML::Node const& node, std::string const& entry, boxed::boxed<V, T>& where)
    {
        auto const child = node[entry];
        if (child)
            where = boxed::boxed<V, T>(child.as<V>());
        logger()("Loading entry: {}, value {}", entry, where.template as<V>());
    }

    void loadFromEntry(YAML::Node const& node,
                       std::string const& entry,
                       std::unordered_map<std::string, TerminalProfile>& where,
                       std::string const& defaultProfileName)
    {
        if (auto const child = node[entry]; child && child.IsMap())
        {
            logger()("Loading default profile: {}", defaultProfileName);
            // Guarded one profile at a time, so a profile that is not a map costs no other profile.
            loadGuarded(child, defaultProfileName, where[defaultProfileName]);
            for (auto entry: child)
            {
                auto const name = entry.first.as<std::string>();
                if (name == defaultProfileName)
                    continue;
                logger()("Loading map with entry: {}", name);
                where[name] = where[defaultProfileName]; // inherit from default
                loadGuarded(child, name, where[name]);
            }
        }
    }

    void loadFromEntry(YAML::Node const& node,
                       std::string const& entry,
                       std::unordered_map<std::string, Layout>& where);

    void loadFromEntry(YAML::Node const& node, std::string const& entry, Layout& where);

    /// Reads `hosts:` -- a map from a host's name to its `url`, `token_file`, `tls_ca`, `remote_command`
    /// and `profile`. A malformed entry costs that entry alone, reported, never the map.
    /// @param node The mapping that holds @p entry.
    /// @param entry The key to read (`hosts`).
    /// @param where Receives the entries; a later duplicate replaces an earlier one.
    void loadFromEntry(YAML::Node const& node,
                       std::string const& entry,
                       std::map<std::string, HostEntry>& where) const;

    /// Parses one entry of `hosts:`. Never throws: an entry that cannot be used is reported and skipped.
    /// @param hostNode The entry's value.
    /// @param name The entry's key, for the report.
    /// @return The host, or no value when it is skipped.
    [[nodiscard]] std::optional<HostEntry> parseHostNode(YAML::Node const& hostNode,
                                                         std::string const& name) const;

    /// Parses one layout's node (its `tabs:` sequence) into @p where. Never throws: malformed
    /// scalars are logged and skipped, so one broken layout cannot unwind the whole config load
    /// (which would silently drop every entry loaded after it, e.g. the user's input_mapping).
    void parseLayoutNode(YAML::Node const& layoutNode, Layout& where);

    // Used for color scheme loading
    template <typename T>
    void loadFromEntry(YAML::Node const& node,
                       std::string const& entry,
                       std::unordered_map<std::string, T>& where)
    {
        if (auto const child = node[entry]; child && child.IsMap())
        {
            for (auto entry: child)
            {
                auto const name = entry.first.as<std::string>();
                logger()("Loading map with entry: {}", name);
                loadGuarded(child, name, where[name]);
            }
        }
    }

    void loadFromEntry(YAML::Node const& node, std::string const& entry, std::chrono::milliseconds& where)
    {
        if (auto const child = node[entry]; child)
            where = std::chrono::milliseconds(child.as<int>());
        logger()("Loading entry: {}, value {}", entry, where.count());
    }

    template <typename Input>
    void appendOrCreateBinding(std::vector<vtbackend::InputBinding<Input, ActionList>>& bindings,
                               vtbackend::MatchModes modes,
                               vtbackend::Modifiers modifier,
                               Input input,
                               actions::Action action,
                               std::optional<uint8_t> clickCount = std::nullopt)
    {
        for (auto& binding: bindings)
        {
            if (match(binding, modes, modifier, input, clickCount))
            {
                binding.binding.emplace_back(std::move(action));
                return;
            }
        }

        bindings.emplace_back(vtbackend::InputBinding<Input, ActionList> {
            modes, modifier, input, ActionList { std::move(action) }, clickCount });
    }

    // clang-format off
    void loadFromEntry(YAML::Node const& node, std::string const& entry, std::filesystem::path& where) const;
    void loadFromEntry(YAML::Node const& node, std::string const& entry, RenderingBackend& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, GpuSelector& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, crispy::StrongHashtableSize& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::MaxHistoryLineCount& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, crispy::LRUCapacity& where);
    void loadFromEntry(YAML::Node const& node,
                       std::string const& entry,
                       vtrasterizer::GlyphScalingMethod& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::CursorDisplay& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::BlinkStyle& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::ScreenTransitionStyle& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::Modifiers& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::CursorShape& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, contour::config::SelectionAction& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, InputMappings& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::ImageSize& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, std::set<std::string>& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, std::string& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::StatusDisplayPosition& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, ScrollBarPosition& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, PixelReporting& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, GuiTheme& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtrasterizer::FontDescriptions& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, text::RenderMode& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtrasterizer::TextOutlineConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtrasterizer::FontLocatorEngine& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtrasterizer::TextShapingEngine& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, ColorConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, text::FontDescription& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, std::vector<text::FontFeature>& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, text::FontWeight& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, text::FontSlant& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, text::FontSize& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::LineCount& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::VTType& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtpty::PageSize& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, WindowMargins& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::LineOffset& where);
    void loadFromEntry(YAML::Node const& node,
                       std::string const& entry,
                       vtpty::Process::ExecInfo& where) const;
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtpty::SshHostConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, Bell& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, FinishNotificationConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, std::map<vtbackend::DECMode, bool>& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtrasterizer::Decorator& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::Opacity& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::StatusDisplayType& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, Permission& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, std::shared_ptr<vtbackend::BackgroundImage>& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::CellRGBColor& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::CursorColor& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::RGBColor& where);
    /// Reads an optional tab color; null clears the profile's color.
    /// @param node Mapping containing the entry.
    /// @param entry Configuration key.
    /// @param where Receives the color, or no value for null.
    void loadFromEntry(YAML::Node const& node,
                       std::string const& entry,
                       std::optional<vtbackend::RGBColor>& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::RGBColorPair& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::CellRGBColorAndAlphaPair& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::ColorPalette::Palette& colors);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::ColorPalette& where);
    void loadFromEntry(YAML::Node const& node, vtbackend::ColorPalette& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, TerminalProfile& where);

    /// Reads a mode's `cursor:` map -- its shape, blinking and blinking interval.
    /// @param node  The mapping to read from.
    /// @param entry The key within @p node.
    /// @param where Receives what the map states.
    void loadFromEntry(YAML::Node const& node, std::string const& entry, CursorConfig& where);

    /// Reads a Vi mode's map (`normal_mode:`, `visual_mode:`), its `cursor:` guarded on its own.
    /// @param node  The mapping to read from.
    /// @param entry The key within @p node.
    /// @param where Receives what the map states.
    void loadFromEntry(YAML::Node const& node, std::string const& entry, InputModeConfig& where);

    /// Loads @p entry of @p node into @p where if the key is there, and leaves @p where as it is if not:
    /// for a value whose absence means "derive it", such as a colour scheme's fold column.
    ///
    /// The key is a view, so the tables that spell a colour scheme's keys -- block_status, sticky_header,
    /// tint -- hand theirs over as they are.
    /// @param node The mapping to read.
    /// @param entry The key.
    /// @param where Set to the value read; untouched without the key.
    template <typename T>
    void loadOptional(YAML::Node const& node, std::string_view entry, std::optional<T>& where)
    {
        auto const key = std::string { entry };
        if (!node[key])
            return;
        auto value = T {};
        loadFromEntry(node, key, value);
        where = value;
    }

    /// Parses a profile body map (a profile's `shell`, `font`, `colors`, ... keys) directly into
    /// @p where. This is the shared core of profile parsing: the `loadFromEntry(node, entry, profile)`
    /// overload above resolves `node[entry]` to the body and delegates here, and the GUI side-file
    /// loader (whose `profiles/<name>.yml` document root IS the body) calls it directly.
    /// @param profileNode The profile body map; a null node is a no-op (@p where keeps its base values).
    /// @param where The profile to populate; pre-seed it with an inheritance base before calling.
    /// @param profileName The profile's name, as problems found in its body name it.
    void loadProfileBody(YAML::Node const& child, TerminalProfile& where, std::string_view profileName);

    void loadFromEntry(YAML::Node const& node, std::string const& entry, RendererConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, ImagesConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, DaemonSettings& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, DaemonRestoreConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, HistoryConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, FoldingConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, GutterConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, OscContextConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, CommandBlocksConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, ScrollBarConfig& where);

    /// Loads @p entry as a YAML list of scrollbar mark sources, replacing @p where whole: `[]` is a
    /// deliberate "none". Left unchanged when absent or not a list; a list item that names no source is
    /// reported and skipped.
    /// @param node The mapping that holds @p entry.
    /// @param entry The key to read (`marks`).
    /// @param where The sources to fill.
    void loadFromEntry(YAML::Node const& node,
                       std::string const& entry,
                       vtbackend::ScrollbarMarkSources& where);

    void loadFromEntry(YAML::Node const& node, std::string const& entry, StickyHeaderConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, MouseConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, StatusLineConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, BackgroundConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, HyperlinkDecorationConfig& where);
    /// Reads link_detection: per screen group, which of urls and file_paths are detected; a key left out
    /// keeps the value already in @p where.
    void loadFromEntry(YAML::Node const& node, std::string const& entry, LinkDetectionConfig& where);
    /// Reads file_opener as a command line, or as a map of `command` and `placement`, and validates the command
    /// (FileOpenerTemplate::parse()): one that cannot be used is reported and cleared, so files open with the
    /// system's default application.
    void loadFromEntry(YAML::Node const& node, std::string const& entry, FileOpenerConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, PermissionsConfig& where);
    void loadFromEntry(YAML::Node const& node,
                       std::string const& entry,
                       std::vector<HintPatternConfig>& where) const;
    /// Reads `extensions:` -- a map from an extension's name to its `path`, `config` and `run_command`.
    ///
    /// A relative `path` is relative to the directory of the file that declares it. A malformed entry costs
    /// that entry, not the map, and so does a `path` that is empty or names nothing: the entry never falls
    /// back to the bundled extension of its name. An unknown `run_command` is reported and the entry asks.
    /// A name declared twice keeps the first, a `config` key set twice the later.
    /// @param node The mapping that holds @p entry.
    /// @param entry The key to read (`extensions`).
    /// @param where Receives the entries in file order; replaced when the key is present.
    void loadFromEntry(YAML::Node const& node,
                       std::string const& entry,
                       std::vector<ExtensionConfig>& where) const;

    /// Reads one entry of `extensions:`.
    /// @param name The entry's key.
    /// @param settings The entry's value: a map, or null for a bundled extension with the defaults.
    /// @param keyPath Where `extensions` is declared, for the diagnostics.
    /// @return The extension, or no value when the entry is skipped, with the reason reported.
    [[nodiscard]] std::optional<ExtensionConfig> readExtension(std::string name,
                                                               YAML::Node const& settings,
                                                               std::string_view keyPath) const;
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtrasterizer::BoxDrawingRenderer::ArcStyle& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtrasterizer::BoxDrawingRenderer::GitDrawingsStyle& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtrasterizer::BoxDrawingRenderer::BrailleStyle& where);


    void defaultSettings(vtpty::Process::ExecInfo& shell);
    // clang-format on

    void load(Config& c);

    /// Loads the `layouts:` entry of this reader's own document into @p where.
    /// Used to merge a sibling layouts.yml on top of the already-loaded Config.
    void loadLayoutsInto(std::unordered_map<std::string, Layout>& where)
    {
        loadFromEntry(doc, "layouts", where);
    }

    std::optional<actions::Action> parseAction(YAML::Node const& node);
    std::optional<vtbackend::Modifiers> parseModifierKey(std::string const& key);
    std::optional<vtbackend::Modifiers> parseModifier(YAML::Node const& nodeYAML);
    static std::optional<vtbackend::MatchModes> parseMatchModes(YAML::Node const& nodeYAML);
    std::optional<vtbackend::Key> parseKey(std::string const& name);
    std::optional<std::variant<vtbackend::Key, char32_t>> parseKeyOrChar(std::string const& name);

    bool tryAddKey(InputMappings& inputMappings,
                   vtbackend::MatchModes modes,
                   vtbackend::Modifiers modifier,
                   YAML::Node const& node,
                   actions::Action action);
    std::optional<vtbackend::MouseButton> parseMouseButton(YAML::Node const& node);
    bool tryAddMouse(std::vector<MouseInputMapping>& bindings,
                     vtbackend::MatchModes modes,
                     vtbackend::Modifiers modifier,
                     YAML::Node const& node,
                     actions::Action action,
                     std::optional<uint8_t> clickCount);

  private:
    /// What one step of the key path names, which says how the next step is joined to it.
    enum class KeyStepKind : uint8_t
    {
        Key = 0, ///< A key of its mapping; a dot joins the key read within it.
        File,    ///< A side file, whose own key path follows it after a colon: `<file>: <map>`.
    };

    /// One key loadGuarded() is reading, and the mapping it is read from.
    struct KeyStep
    {
        YAML::Node container;                ///< The mapping the key is read from.
        std::string entry;                   ///< The key, or the side file's path.
        KeyStepKind kind = KeyStepKind::Key; ///< What @ref entry names.
    };

    /// The keys from the document's root down to the one loadGuarded() is reading.
    std::vector<KeyStep> _keyPath;
};

/// How a configuration file spells a shell's initial working directory.
/// @param workingDirectory The directory; empty for the home directory.
/// @return "~" quoted for the home directory, else the directory in UTF-8, as the file is written.
[[nodiscard]] std::string spelledWorkingDirectory(std::filesystem::path const& workingDirectory);

struct Writer
{
    virtual ~Writer() = default;

    struct Offset
    {
        static inline int Levels = 0;
        Offset() { Levels++; }
        ~Offset() { --Levels; }
    };

    template <typename F>
    void scoped(F lambda)
    {
        auto const _ = Offset {};
        lambda();
    }

    virtual inline std::string replaceCommentPlaceholder(std::string const& docString) = 0;

    template <typename... T>
    [[nodiscard]] std::string format(std::string_view doc, T... args)
    {
        return std::vformat(replaceCommentPlaceholder(std::string(doc)), std::make_format_args(args...));
    }

    [[nodiscard]] std::string format(KeyInputMapping const& v)
    {
        return formatKeyBinding(v, std::format("{}", v.input));
    }

    [[nodiscard]] std::string format(CharInputMapping const& v)
    {
        // As UTF-8, which is how the reader takes a one-character key back.
        return formatKeyBinding(v, unicode::convert_to<char>(v.input));
    }

    /// One `key:` binding row.
    /// @param v The binding.
    /// @param key How the binding's key is spelled, unquoted.
    /// @return The row, ending in a newline.
    template <typename Binding>
    [[nodiscard]] std::string formatKeyBinding(Binding const& v, std::string_view key)
    {
        return format("{:<30},{:<30},{:<30}\n",
                      format("- {{ mods: [{}]", format(v.modifiers)),
                      format(" key: {}", SingleQuoted { key }),
                      formatActionAndModes(v));
    }

    /// The tail of a binding row: its action, then its `mode:` when it names one, closing the map.
    template <typename Binding>
    [[nodiscard]] std::string formatActionAndModes(Binding const& v)
    {
        if (!v.modes.any())
            return format(" action: {} }}", v.binding[0]);
        return format(" action: {}, mode: {} }}", v.binding[0], SingleQuoted { std::format("{}", v.modes) });
    }

    [[nodiscard]] std::string format(MouseInputMapping v)
    {
        auto button = v.clickCount ? format(" mouse: {}, clicks: {}", v.input, *v.clickCount)
                                   : format(" mouse: {}", v.input);
        return format("{:<30},{:<30},{:<30}\n",
                      format("- {{ mods: [{}]", format(v.modifiers)),
                      button,
                      formatActionAndModes(v));
    }

    [[nodiscard]] std::string static format(std::vector<text::FontFeature> const& v)
    {

        auto result = std::string { "[" };
        result.append(v | std::views::transform([](auto f) { return std::format("{}", f); })
                      | core::views::joinWith(std::string_view(", ")));
        result.append("]");
        return result;
    }

    [[nodiscard]] std::string static format(vtbackend::Modifiers const& flags)
    {
        std::string result;
        for (auto i = 0u; i < sizeof(vtbackend::Modifier) * 8; ++i)
        {
            auto const flag = static_cast<vtbackend::Modifier>(1 << i);
            if (!flags.test(flag))
                continue;

            // We assume that only valid enum values resulting into non-empty strings.
            auto const element = std::format("{}", flag);
            if (element.empty())
                continue;

            if (!result.empty())
                result += ',';

            result += element;
        }
        return result;
    }

    [[nodiscard]] std::string format(std::string_view doc, vtrasterizer::FontDescriptions const& v)
    {
        return format(doc,
                      v.size.pt,
                      v.fontLocator,
                      v.textShapingEngine,
                      v.builtinBoxDrawing,
                      v.maxFallbackCount,
                      v.renderMode,
                      // strict_spacing is stored per face, but the reader fans one value out to
                      // all four, so the regular face carries it for the whole block.
                      v.regular.strictSpacing,
                      v.regular.familyName,
                      v.regular.weight,
                      v.regular.slant,
                      format(v.regular.features),
                      v.emoji.familyName);
    }

    template <typename T, typename R>
    [[nodiscard]] std::string format(std::string_view doc, std::chrono::duration<T, R> v)
    {
        return format(doc, v.count());
    }

    [[nodiscard]] std::string format(std::string_view doc, vtpty::Process::ExecInfo const& v)
    {
        auto args = std::string { "[" };
        args.append(v.arguments | core::views::joinWith(std::string_view(", ")));
        args.append("]");
        return format(doc, v.program, args, spelledWorkingDirectory(v.workingDirectory));
    }

    [[nodiscard]] std::string format(std::string_view doc, std::map<vtbackend::DECMode, bool> const& mods)
    {
        if (mods.empty())
            return format(doc, std::string_view { "0" });

        auto result = std::string { "[" };
        for (auto const& [mode, value]: mods)
        {
            if (!result.empty())
                result += ',';
            result += std::format("{}", toString(mode));
        }
        result += "]";
        return format(doc, result);
    }

    [[nodiscard]] std::string format(std::string_view doc, vtpty::SshHostConfig const& v)
    {
        return format(doc, v.hostname);
    }

    /// Serializes hint patterns: outputs the doc template (pure comment) followed by any user entries.
    [[nodiscard]] std::string format(std::string_view doc, std::vector<HintPatternConfig> const& patterns)
    {
        // doc is already processed by process() (comment placeholders replaced, indentation applied).
        auto result = std::string { doc };
        if (!patterns.empty())
        {
            // Derive indentation from the first non-empty line in doc.
            auto indent = std::string {};
            for (auto const ch: doc)
            {
                if (ch == '\n')
                {
                    indent.clear();
                    continue;
                }
                if (ch == ' ' || ch == '\t')
                    indent.push_back(ch);
                else
                    break;
            }
            result += indent + "hint_patterns:\n";
            for (auto const& p: patterns)
            {
                result += std::format("{}    - name: {}\n", indent, SingleQuoted { p.name });
                if (!p.regex.empty())
                    result += std::format("{}      regex: {}\n", indent, SingleQuoted { p.regex });
                if (p.clickable)
                    result += std::format("{}      clickable: {}\n", indent, *p.clickable);
                if (p.url)
                    result += std::format("{}      url: {}\n", indent, SingleQuoted { *p.url });
            }
        }
        return result;
    }

    /// Serializes extensions: the doc template (pure comment) followed by the entries, if any.
    /// @param doc The documentation, already indented.
    /// @param extensions The profile's extensions, written in order.
    /// @return @p doc, then an `extensions:` map when @p extensions holds an entry.
    [[nodiscard]] std::string format(std::string_view doc, std::vector<ExtensionConfig> const& extensions)
    {
        auto result = format(doc);
        if (extensions.empty())
            return result;

        // The doc is indented to where the key goes: derive that from its last line.
        auto const indent = [&] {
            auto const lastLine = doc.substr(doc.rfind('\n', doc.size() > 1 ? doc.size() - 2 : 0) + 1);
            return std::string(std::min(lastLine.find_first_not_of(' '), lastLine.size()), ' ');
        }();
        result += indent + "extensions:\n";
        for (auto const& extension: extensions)
        {
            result += std::format("{}    {}:\n", indent, SingleQuoted { extension.name });
            if (!extension.path.empty())
                result += std::format(
                    "{}        path: {}\n", indent, SingleQuoted { core::pathToUtf8(extension.path) });
            if (!extension.config.empty())
            {
                result += std::format("{}        config:\n", indent);
                for (auto const& [key, value]: extension.config)
                    result += std::format(
                        "{}            {}: {}\n", indent, SingleQuoted { key }, SingleQuoted { value });
            }
            result +=
                std::format("{}        run_command: {}\n", indent, permissionToken(extension.runCommand));
        }
        return result;
    }

    [[nodiscard]] std::string static format(vtbackend::CellRGBColor const& v)
    {
        if (auto const* const color = std::get_if<vtbackend::RGBColor>(&v))
            return format(*color);

        return std::format("{}", v);
    }

    [[nodiscard]] std::string static format(vtbackend::RGBColor const& v)
    {
        return std::format("{}", SingleQuoted { std::format("{}", v) });
    }

    /// Formats an optional color as a YAML scalar.
    /// @param v Color to quote, or no value for null.
    /// @return A quoted '#RRGGBB' value or the unquoted null literal.
    [[nodiscard]] std::string static format(std::optional<vtbackend::RGBColor> const& v)
    {
        return v ? std::format("{}", SingleQuoted { vtbackend::formatColor(*v) }) : "null";
    }

    /// Substitutes an optional color into a documentation template.
    /// @param doc Format string containing the color placeholder.
    /// @param v Optional color to serialize.
    /// @return The template with the serialized color substituted.
    [[nodiscard]] std::string format(std::string_view doc, std::optional<vtbackend::RGBColor> const& v)
    {
        return format(doc, format(v));
    }

    [[nodiscard]] std::string format(std::string_view doc, vtbackend::ImageSize v)
    {
        return format(doc, unbox(v.width), unbox(v.height));
    }

    [[nodiscard]] std::string format(std::string_view doc, vtbackend::PageSize v)
    {
        return format(doc, unbox(v.columns), unbox(v.lines));
    }

    [[nodiscard]] std::string format(std::string_view doc, ColorConfig const& v)
    {
        if (auto const* simple = get_if<SimpleColorConfig>(&v))
            return format(doc, simple->colorScheme);
        else if (auto const* dual = get_if<DualColorConfig>(&v))
        {
            auto const formattedValue = format("\n"
                                               "    light: {}\n"
                                               "    dark: {}\n",
                                               dual->colorSchemeLight,
                                               dual->colorSchemeDark);
            return format(doc, formattedValue);
        }

        return format(doc, "BAD");
    }

    [[nodiscard]] std::string format(std::string_view doc, Bell const& v)
    {
        return format(doc, v.sound, v.volume, v.alert);
    }

    [[nodiscard]] std::string format(std::string_view doc, FinishNotificationConfig const& v)
    {
        return format(doc, v.when, v.minDuration.count(), v.outcome, v.action, v.clearOn);
    }

    [[nodiscard]] std::string format(std::string_view doc, RendererConfig const& v)
    {
        return format(doc,
                      v.renderingBackend,
                      v.gpu,
                      v.textureAtlasDirectMapping,
                      v.textureAtlasHashtableSlots,
                      v.textureAtlasTileCount);
    }

    [[nodiscard]] std::string format(std::string_view doc, ImagesConfig const& v)
    {
        return format(doc, v.sixelScrolling, v.maxImageColorRegisters, v.goodImageProtocol);
    }

    [[nodiscard]] std::string format(std::string_view doc, DaemonSettings const& v)
    {
        auto const historyLimit = [&]() -> std::string {
            if (!v.historyLimit)
                return "~";
            if (std::holds_alternative<vtbackend::Infinite>(*v.historyLimit))
                return "-1";
            return std::format("{}", unbox(std::get<vtbackend::LineCount>(*v.historyLimit)));
        }();
        auto rerunPrograms = std::string { "[" };
        for (auto const& program: v.restore.rerunPrograms)
            rerunPrograms +=
                std::format("{}{}", rerunPrograms.size() > 1 ? ", " : "", SingleQuoted { program });
        rerunPrograms += "]";
        return format(doc,
                      nameOf(v.mode),
                      SingleQuoted { v.socket },
                      vthost::nameOf(v.sizePolicy),
                      historyLimit,
                      v.restore.scrollback,
                      v.restore.scrollbackLines,
                      rerunPrograms,
                      v.restore.interval.count());
    }

    [[nodiscard]] std::string format(std::string_view doc, WindowMargins const& v)
    {
        return format(doc, v.horizontal, v.vertical);
    }

    [[nodiscard]] std::string format(std::string_view doc, FoldingConfig const& v)
    {
        return format(doc, v.enabled, v.showMarkers, v.autoCollapseOnNewCommand, v.onJumpIntoFold);
    }

    [[nodiscard]] std::string format(std::string_view doc, GutterConfig const& v)
    {
        // Single-quoted: a plain YAML scalar cannot begin with `%`.
        return format(doc,
                      v.exitStatus,
                      v.userMarks,
                      v.blockColumnWidth,
                      v.boldOnHover,
                      v.lineNumbers,
                      v.lineNumberWidth,
                      v.timestamps,
                      SingleQuoted { v.timestampFormat });
    }

    [[nodiscard]] std::string format(std::string_view doc, OscContextConfig const& v)
    {
        return format(doc, v.enabled, v.maxDepth, v.maxRetained, v.deriveMarkers, v.tinting);
    }

    [[nodiscard]] std::string format(std::string_view doc, CommandBlocksConfig const& v)
    {
        return format(doc, v.maxRecords, SingleQuoted { v.pager }, v.pagerFormat);
    }

    [[nodiscard]] std::string format(std::string_view doc, HistoryConfig const& v)
    {
        auto const spell = [](vtbackend::MaxHistoryLineCount limit) {
            if (std::holds_alternative<vtbackend::Infinite>(limit))
                return -1;
            return unbox(std::get<vtbackend::LineCount>(limit));
        };
        return format(doc,
                      spell(v.maxHistoryLineCount),
                      // The resolved ceiling rather than the raw field: an unset hard_limit is zero,
                      // and writing that back would read as a scrollback of none.
                      spell(v.limits().capacity),
                      v.autoScrollOnUpdate,
                      v.historyScrollMultiplier);
    }

    [[nodiscard]] std::string format(std::string_view doc, ScrollBarConfig const& v)
    {
        return format(doc, v.position, v.hideScrollbarInAltScreen, format(v.marks));
    }

    /// `scrollbar.marks` as the YAML flow list the reader accepts, spelled from the same token table.
    /// @param sources The configured sources.
    /// @return E.g. `[failures, commands, user_marks]`, or `[]`.
    [[nodiscard]] static std::string format(vtbackend::ScrollbarMarkSources sources)
    {
        using Info = ConfigEnumInfo<vtbackend::ScrollbarMarkSource>;
        auto const tokens = std::ranges::to<std::vector>(
            configEnumValues<vtbackend::ScrollbarMarkSource>()
            | std::views::filter([sources](Info const& info) { return sources.test(info.value); })
            | std::views::transform(&Info::token));
        return std::format("[{}]", tokens | core::views::joinWith(std::string_view(", ")));
    }

    [[nodiscard]] std::string format(std::string_view doc, StickyHeaderConfig const& v)
    {
        return format(doc, v.mode, v.showEvicted);
    }

    [[nodiscard]] std::string format(std::string_view doc, MouseConfig const& v)
    {
        return format(doc, v.hideWhileTyping);
    }

    [[nodiscard]] std::string format(std::string_view doc, StatusLineConfig const& v)
    {
        return format(doc,
                      v.initialType,
                      v.position,
                      v.syncWindowTitleWithHostWritableStatusDisplay,
                      v.indicator.left,
                      v.indicator.middle,
                      v.indicator.right,
                      v.extension ? std::format("{}", SingleQuoted { *v.extension })
                                  : std::string { "null" });
    }

    [[nodiscard]] std::string format(std::string_view doc, BackgroundConfig const& v)
    {
        return format(doc, v.opacity, v.blur);
    }

    [[nodiscard]] std::string format(std::string_view doc, HyperlinkDecorationConfig const& v)
    {
        return format(doc, v.mode, v.normal, v.hover);
    }

    [[nodiscard]] std::string format(std::string_view doc, LinkDetectionConfig const& v)
    {
        return format(doc,
                      v.primaryScreen.urls,
                      v.primaryScreen.filePaths,
                      v.otherScreens.urls,
                      v.otherScreens.filePaths);
    }

    [[nodiscard]] std::string format(std::string_view doc, FileOpenerConfig const& v)
    {
        return format(doc, SingleQuoted { v.command }, v.placement);
    }

    [[nodiscard]] std::string format(std::string_view doc, PermissionsConfig const& v)
    {
        return format(doc, v.captureBuffer, v.changeFont, v.displayHostWritableStatusLine);
    }

    [[nodiscard]] std::string format(std::string_view doc, InputModeConfig v)
    {
        auto const shape = [v]() -> std::string_view {
            switch (v.cursor.cursorShape)
            {
                case vtbackend::CursorShape::Block: return "block";
                case vtbackend::CursorShape::Rectangle: return "rectangle";
                case vtbackend::CursorShape::Underscore: return "underscore";
                case vtbackend::CursorShape::Bar: return "bar";
            };
            return "unknown";
        }();
        auto const blinking = v.cursor.cursorDisplay == vtbackend::CursorDisplay::Blink;
        auto const blinkingInterval = v.cursor.cursorBlinkInterval.count();
        return format(doc, shape, blinking, blinkingInterval);
    }

    [[nodiscard]] std::string format(std::string_view doc,
                                     vtrasterizer::BoxDrawingRenderer::ArcStyle const& v)
    {
        using ArcStyle = vtrasterizer::BoxDrawingRenderer::ArcStyle;
        switch (v)
        {
            case ArcStyle::Elliptic: return format(doc, "elliptic");
            case ArcStyle::Round: return format(doc, "round");
            default: assert(false); return format(doc, "round");
        }
    }

    [[nodiscard]] std::string format(std::string_view doc,
                                     vtrasterizer::BoxDrawingRenderer::BrailleStyle const& v)
    {
        using BrailleStyle = vtrasterizer::BoxDrawingRenderer::BrailleStyle;
        switch (v)
        {
            case BrailleStyle::Font: return format(doc, "font");
            case BrailleStyle::Solid: return format(doc, "solid");
            case BrailleStyle::Circle: return format(doc, "circle");
            case BrailleStyle::CircleEmpty: return format(doc, "circle_empty");
            case BrailleStyle::Square: return format(doc, "square");
            case BrailleStyle::SquareEmpty: return format(doc, "square_empty");
            case BrailleStyle::AASquare: return format(doc, "aa_square");
            case BrailleStyle::AASquareEmpty: return format(doc, "aa_square_empty");
            default: assert(false); return format(doc, "circle");
        }
    }

    [[nodiscard]] std::string format(std::string_view doc,
                                     vtrasterizer::BoxDrawingRenderer::GitDrawingsStyle const& v)
    {
        using ArcStyle = vtrasterizer::BoxDrawingRenderer::ArcStyle;
        using DrawingStyle = vtrasterizer::BoxDrawingRenderer::GitDrawingsStyle;
        auto const arc = [&]() -> std::string_view {
            switch (v.arcStyle)
            {
                case ArcStyle::Elliptic: return "elliptic";
                case ArcStyle::Round: return "round";
                default: assert(false); return "round";
            }
        }();
        auto const branch = [&]() -> std::string_view {
            switch (v.branchStyle)
            {
                case DrawingStyle::BranchStyle::None: return "none";
                case DrawingStyle::BranchStyle::Thin: return "thin";
                case DrawingStyle::BranchStyle::Double: return "double";
                case DrawingStyle::BranchStyle::Thick: return "thick";
                default: assert(false); return "thin";
            }
        }();
        auto const mergeCommit = [&]() -> std::string_view {
            switch (v.mergeCommitStyle)
            {
                case DrawingStyle::MergeCommitStyle::Solid: return "solid";
                case DrawingStyle::MergeCommitStyle::Bullet: return "bullet";
                default: assert(false); return "solid";
            }
        }();
        return format(doc, branch, arc, mergeCommit);
    }
};

template <typename T>
std::string createString(Config const& c);

template <typename T>
std::string documentationGlobalConfig(Config const& c);

template <typename T>
std::string documentationProfileConfig(Config const& c);

struct YAMLConfigWriter: Writer
{

    constexpr static std::string_view FormatTemplate = "{}";
    std::string replaceCommentPlaceholder(std::string const& docString) override
    {
        return std::regex_replace(docString, std::regex { "\\{comment\\}" }, "#");
    }

    static constexpr int OneOffset = 4;
    using Writer::format;
    std::string static addOffset(std::string_view doc, size_t off)
    {
        auto offset = std::string(off, ' ');
        return std::regex_replace(std::string { doc }, std::regex(".+\n"), offset + "$&");
    }

    template <typename... T>
    std::string process(std::string_view doc, T const&... val)
    {
        return format(addOffset(replaceCommentPlaceholder(std::string { doc }),
                                static_cast<size_t>(Offset::Levels) * OneOffset),
                      val...);
    }

    /// Writes the configuration entry a reflection walk reached under @p name, which is dropped: @p doc
    /// already spells the entry's key.
    ///
    /// A function of its own, not an overload of process(): one taking the name first also takes a row
    /// whose first value is a std::string_view -- a tint's context type -- and drops that value instead.
    /// @param doc The entry's documentation, with a placeholder per value.
    /// @param name The entry's member name.
    /// @param val The entry's values.
    /// @return @p doc, indented and filled in.
    template <typename... T>
    std::string processEntry(std::string_view doc, [[maybe_unused]] std::string_view name, T const&... val)
    {
        return process(doc, val...);
    }

    template <typename T, documentation::StringLiteral ConfigDoc, documentation::StringLiteral WebDoc>
    constexpr std::string_view whichDoc(
        contour::config::
            ConfigEntry<T, contour::config::documentation::DocumentationEntry<ConfigDoc, WebDoc>> const&)
    {
        return ConfigDoc.value;
    }

    template <documentation::StringLiteral ConfigDoc, documentation::StringLiteral WebDoc>
    constexpr std::string_view whichDoc(
        contour::config::documentation::DocumentationEntry<ConfigDoc, WebDoc> const&)
    {
        return ConfigDoc.value;
    }
};

struct DocumentationWriter: Writer
{
    constexpr static std::string_view FormatTemplate = "{}";
    std::string replaceCommentPlaceholder(std::string const& docString) override
    {
        return std::regex_replace(docString, std::regex { "\\{comment\\}" }, "");
    }

    static constexpr int OneOffset = 0;
    std::string static addOffset(std::string_view doc, [[maybe_unused]] size_t off)
    {
        return std::string { doc };
    }

    using Writer::format;
    template <typename... T>
    std::string process(std::string_view doc, T const&... val)
    {
        return processEntry(doc, std::string_view { "" }, val...);
    }

    /// Writes the configuration entry a reflection walk reached, headed by @p name in snake_case.
    /// @see YAMLConfigWriter::processEntry for why this is not an overload of process().
    /// @param doc The entry's documentation, with a placeholder per value.
    /// @param name The entry's member name.
    /// @param val The entry's values.
    /// @return The heading, then @p doc filled in.
    template <typename... T>
    std::string processEntry(std::string_view doc, std::string_view name, T const&... val)
    {
        return format(
            "### `{}`\n"
            "{}\n",
            [](std::string_view name) -> std::string {
                // camelCase into snake_case
                auto result = std::string { name };
                for (auto i = 0u; i < result.size(); ++i)
                {
                    if (std::isupper(result[i]))
                    {
                        result.insert(i, 1, '_');
                        result[i + 1] = static_cast<char>(std::tolower(result[i + 1]));
                    }
                }
                return result;
            }(name),
            format(replaceCommentPlaceholder(std::string { doc }), val...));
    }

    template <typename T, documentation::StringLiteral ConfigDoc, documentation::StringLiteral WebDoc>
    constexpr std::string_view whichDoc(
        contour::config::
            ConfigEntry<T, contour::config::documentation::DocumentationEntry<ConfigDoc, WebDoc>> const&)
    {
        return WebDoc.value;
    }

    template <documentation::StringLiteral ConfigDoc, documentation::StringLiteral WebDoc>
    constexpr std::string_view whichDoc(
        contour::config::documentation::DocumentationEntry<ConfigDoc, WebDoc> const&)
    {
        return WebDoc.value;
    }
};

// Will ignore documentation
struct PlainWriter: Writer
{
    constexpr static std::string_view FormatTemplate = "{}";
    std::string replaceCommentPlaceholder(std::string const& docString) override
    {
        return std::regex_replace(docString, std::regex { "\\{comment\\}" }, "");
    }

    static constexpr int OneOffset = 0;
    std::string static addOffset(std::string_view doc, [[maybe_unused]] size_t off)
    {
        return std::string { doc };
    }

    using Writer::format;
    template <typename... T>
    std::string process([[maybe_unused]] std::string_view doc, T... val)
    {
        return format("{}", format(val...));
    }
};

std::filesystem::path configHome();
std::filesystem::path configHome(std::string const& programName);

std::optional<std::string> readConfigFile(std::string const& filename);

void loadConfigFromFile(Config& config, std::filesystem::path const& fileName);
Config loadConfigFromFile(std::filesystem::path const& fileName);
Config loadConfig();

/// Loads @p fileName into a configuration of its own, so a file that fails to load leaves every other
/// configuration as it was.
/// @param fileName The configuration file.
/// @return The configuration, or a human-readable reason it could not be loaded.
[[nodiscard]] std::expected<Config, std::string> tryLoadConfigFromFile(std::filesystem::path const& fileName);

/// Loads the configuration file a command line names, or the default one.
/// @param configPath The file, UTF-8 as the command line is read on every platform; empty for the
///                   default one (@see loadConfig).
/// @return The configuration loaded.
[[nodiscard]] Config loadConfigNamed(std::string_view configPath);

/// @ref loadConfigNamed, reporting a configuration that cannot be loaded instead of throwing.
/// @param configPath The file, UTF-8; empty for the default one.
/// @return The configuration loaded, or why it could not be.
[[nodiscard]] std::expected<Config, std::string> tryLoadConfigNamed(std::string_view configPath);
void compareEntries(Config& config, auto const& output);

/// The VT-EMULATION half of a profile's terminal settings — everything that decides what
/// the terminal *is* (history depth, initial page size, reported identity, reflow,
/// grapheme clustering, image limits, word delimiters, frozen modes) as opposed to how it
/// is *presented* (cursor blink, transitions, status-line layout, colors).
///
/// Split out because two very different callers need exactly this much and no more: the
/// GUI's session factory, which layers its presentation fields on top, and
/// `contour daemon`, which owns terminals nobody presents locally. One table for the
/// shared half is what stops the two from emulating differently — see
/// `vthost::DefaultSessionHistoryLineCount` for what that costs when they do.
///
/// @param config The loaded configuration (global entries).
/// @param profile The resolved profile.
/// @return Settings with the emulation fields applied over the vtbackend defaults.
[[nodiscard]] vtbackend::Settings emulationSettings(Config const& config, TerminalProfile const& profile);

/// What the terminal's gutter shows under @p config: the `gutter` section, with the fold switch taken from
/// `folding`.
///
/// Whatever configures a terminal's gutter and whatever reserves the gutter's width must both take it
/// from here, or the strip reserved and the strip drawn can disagree.
/// @see FoldingConfig::markersVisible
/// @param config The loaded configuration.
/// @return The gutter settings vtbackend draws with.
[[nodiscard]] vtbackend::GutterSettings gutterSettingsFor(Config const& config);

/// One profile's @ref emulationSettings, from a configuration already loaded in memory -- what
/// `contour client` needs, whose new sessions follow the configuration as a reload left it.
///
/// @param config The loaded configuration.
/// @param profileName Profile to resolve; empty selects the configuration's default.
/// @return The settings, or a human-readable reason they could not be resolved; an unknown profile is
///         refused rather than replaced by the default.
[[nodiscard]] std::expected<vtbackend::Settings, std::string> emulationSettingsFor(
    Config const& config, std::string const& profileName);

/// Resolves the host `contour client --host` names: a saved one of `hosts:`, or an ad-hoc URL.
///
/// @param configPath Configuration file to read; empty selects the default location. Not read for a URL.
/// @param nameOrUrl A name under `hosts:`, or a URL (anything containing `://`). An ad-hoc `ssh://` URL
///        needs no configuration; an ad-hoc `tls://` URL is refused, as a TLS daemon needs a token
///        and only a saved host can name its `token_file`.
/// @return The host, or a human-readable reason it could not be resolved: a malformed URL, or an
///         unknown name with the saved ones listed.
[[nodiscard]] std::expected<HostEntry, std::string> resolveHost(std::string const& configPath,
                                                                std::string_view nameOrUrl);

/// Picks the color palette @p colorConfig yields for @p preference: the matching half of a dual
/// (dark/light) config, or the single palette of a simple one.
/// @return The palette, or nullptr if @p colorConfig holds neither known alternative.
[[nodiscard]] vtbackend::ColorPalette const* preferredColorPalette(ColorConfig const& colorConfig,
                                                                   vtbackend::ColorPreference preference);

/// Everything a HOSTED session needs beyond @ref emulationSettings: the shell to run, whether it
/// escapes its sandbox, and the presentation fields (cursor, status line, scrolling, colors, ...)
/// that decide how the terminal looks rather than what it is.
///
/// Split out from @ref emulationSettings for the reason its own doc comment gives: `contour
/// daemon` hosts terminals no LOCAL display presents, but every one of them is presented by
/// whichever client attaches — `contour client` included — so the daemon needs this whole picture,
/// not just the emulation half. Before this type existed, `ContourApp::daemonAction()` resolved
/// only `emulationSettings()` and hardcoded the shell and sandbox policy, silently dropping a
/// profile's `shell:`, presentation and color configuration for every session it hosts.
struct ResolvedSessionConfig
{
    /// The VT-emulation settings, presentation fields and color palette applied — everything
    /// `vtbackend::Terminal` needs to both emulate AND look the way the profile says.
    vtbackend::Settings settings;
    /// The shell (program, arguments, working directory, environment) each new session runs.
    /// Carries `CONTOUR_PROFILE` in its environment, matching what a local session sets.
    vtpty::Process::ExecInfo shell;
    /// Whether a spawned shell escapes its sandbox (Flatpak), from `profile.escape_sandbox`.
    bool escapeSandbox = true;
    /// The profile's configured startup layout (`default_layout` looked up in `layouts:`),
    /// realized by `vthost::SessionHost` before any client attaches. Empty `tabs` means no
    /// layout is configured, or the named one does not exist — the caller falls back to a
    /// single default tab, exactly like a local session's `TerminalSessionManager::findLayout`
    /// miss does.
    vtworkspace::Layout startupLayout {};
    /// The configuration file the profile was read from: what a daemon keeps its own side files beside.
    std::filesystem::path configFile {};
    /// The profile's status-line extensions (`extensions:`), resolved on the daemon's side.
    std::vector<ExtensionConfig> extensions {};
    /// `status_line.extension`: the one of @ref extensions that takes the indicator status line over.
    std::optional<std::string> statusLineExtension {};
};

/// Resolves @p profile's presentation half: @ref emulationSettings plus the fields that decide how
/// a session looks rather than what it is (cursor, status line, scrolling, colors, initial page
/// size, focus) — everything `ResolvedSessionConfig::settings` carries, computed standalone so a
/// caller that only needs `vtbackend::Settings` (a local `TerminalSession`, which gets its shell
/// from `SessionFactory` and its startup layout from `TerminalSessionManager::findLayout`) is not
/// charged for the shell/sandbox/layout resolution @ref resolvedSessionConfig also does.
///
/// @param config The loaded configuration (global entries).
/// @param profile The resolved profile.
/// @param colorPreference Which half of a dual (dark/light) color config to prefer; irrelevant for
///        a simple (single) color config.
/// @param initialPageSize Overrides `profile.terminal_size` when set (a new tab/split inherits the
///        live window's running grid rather than starting at the profile's configured size).
/// @return The resolved settings.
[[nodiscard]] vtbackend::Settings sessionSettings(
    Config const& config,
    TerminalProfile const& profile,
    vtbackend::ColorPreference colorPreference,
    std::optional<vtbackend::PageSize> initialPageSize = std::nullopt);

/// Why the reader skips a hint_patterns entry: one needs a name, and a regex unless it names a built-in.
/// @param pattern The entry as read.
/// @return What is wrong with it, or nullopt for an entry the reader keeps.
[[nodiscard]] std::optional<std::string> skippedHintPatternReason(HintPatternConfig const& pattern);

/// The profile's link_detection as the engine's policy.
/// @param config The profile's link_detection.
/// @return The same switches, as the engine spells them.
[[nodiscard]] vtbackend::LinkDetectionPolicy linkDetectionPolicyOf(
    LinkDetectionConfig const& config) noexcept;

/// The profile's hint_patterns as link pattern entries.
/// @param profile The profile.
/// @return One entry per hint_patterns entry, in order.
[[nodiscard]] std::vector<vtbackend::LinkPatternSpec> linkPatternSpecs(TerminalProfile const& profile);

/// The profile's link pattern table, compiled: the shared built-in table when the profile has no
/// hint_patterns.
/// @param profile The profile.
/// @return The table; never null.
[[nodiscard]] std::shared_ptr<vtbackend::LinkPatterns const> linkPatternsFor(TerminalProfile const& profile);

/// Whether @p actions include an action of type @p A: FollowHyperlink makes a binding the follow gesture,
/// ToggleAllKeyMaps one that still runs while key mappings are off.
/// @tparam A The action type looked for, one of actions::Action's alternatives.
/// @param actions A binding's actions.
/// @return True when one of them is an @p A.
template <typename A>
[[nodiscard]] bool containsAction(ActionList const& actions)
{
    return std::ranges::any_of(
        actions, [](actions::Action const& action) { return std::holds_alternative<A>(action); });
}

/// The modifiers of the left-button mouse binding that runs FollowHyperlink.
///
/// Only the first such binding counts, and its match modes are ignored: links are revealed for one chord, and
/// a second binding (Ctrl+Click and Alt+Click both following) reveals nothing for the later one. The modes,
/// which pick a binding by terminal state such as the alternate screen, are ignored because the reveal is
/// decided before any click and cannot know which binding that click would find.
/// @param config The loaded configuration.
/// @return Them; nullopt when no binding follows links.
[[nodiscard]] std::optional<vtbackend::Modifiers> followHyperlinkModifiers(Config const& config);

/// Resolves @p profile's hosted-session configuration: @ref sessionSettings plus the shell,
/// sandbox policy and startup layout a daemon-hosted session ALSO needs. The one place
/// `ContourApp::daemonAction()` and `resolveSessionConfig()`'s daemon callers go through, so no
/// hosted path can disagree with another about what a profile means.
///
/// A local `TerminalSession` calls @ref sessionSettings directly instead: its shell comes from
/// `SessionFactory` (profile SSH/local, plus a layout pane's command override) and its startup
/// layout from `TerminalSessionManager::findLayout`, so resolving them here too would be
/// immediately discarded work.
///
/// @param config The loaded configuration (global entries).
/// @param profile The resolved profile.
/// @param profileName The name @p profile was resolved under — `TerminalProfile` does not carry
///        its own name, and the shell environment's `CONTOUR_PROFILE` needs it.
/// @param colorPreference Which half of a dual (dark/light) color config to prefer; irrelevant for
///        a simple (single) color config.
/// @param initialPageSize Overrides `profile.terminal_size` when set (a new tab/split inherits the
///        live window's running grid rather than starting at the profile's configured size).
/// @return The resolved configuration.
[[nodiscard]] ResolvedSessionConfig resolvedSessionConfig(
    Config const& config,
    TerminalProfile const& profile,
    std::string const& profileName,
    vtbackend::ColorPreference colorPreference,
    std::optional<vtbackend::PageSize> initialPageSize = std::nullopt);

/// Resolves @p profileName's @ref resolvedSessionConfig in an already loaded @p config, under the color
/// configuration's light half (a daemon presents nothing itself).
/// @param config The loaded configuration.
/// @param profileName Profile to resolve; empty selects the configuration's default.
/// @return The resolved configuration, or why the profile does not exist.
[[nodiscard]] std::expected<ResolvedSessionConfig, std::string> resolvedSessionConfigFor(
    Config const& config, std::string const& profileName);

/// Loads a configuration and resolves one profile's @ref resolvedSessionConfig from it — the
/// @ref emulationSettingsFor counterpart for callers that also need the shell, sandbox policy and
/// presentation fields, chiefly `ContourApp::daemonAction()`.
///
/// @param configPath Configuration file to read; empty selects the default location.
/// @param profileName Profile to resolve; empty selects the configuration's default.
/// @return The resolved configuration, or a human-readable reason it could not be resolved.
[[nodiscard]] std::expected<ResolvedSessionConfig, std::string> resolveSessionConfig(
    std::string const& configPath, std::string const& profileName);

/// Loads ONLY the `layouts:` map contained in the single file at @p path (no sibling-merge, no
/// inline-config layouts). A missing file yields an empty map (nothing saved yet is not an
/// error); a file that fails to parse yields the parse error instead, so SaveLayout can REFUSE to
/// rewrite — and thereby destroy — layouts it could not read back.
/// Used by SaveLayout to read back layouts.yml's own prior contents before appending the new one,
/// so the file is never overwritten with the merged (inline + file) in-memory view.
std::expected<std::unordered_map<std::string, Layout>, std::string> loadLayoutsFile(
    std::filesystem::path const& path);

/// The file a GUI side file named for @p name is kept in, beside the others of its kind: `<name>.yml`.
/// @param name A profile's or a color scheme's name, in UTF-8.
/// @return The file name, without a directory.
[[nodiscard]] std::filesystem::path sideFileName(std::string_view name);

/// Serializes a single profile's body as the exact text written to a `profiles/<name>.yml` GUI side
/// file: a bare top-level map of the profile's fields (no `profiles:`/name wrapper), so the same
/// reader that parses an inline profile parses it back. Produced by the reflection-driven config
/// writer, so every profile field serializes here identically to how it appears in contour.yml and is
/// covered without hand-listing.
/// @param profile The profile to serialize.
/// @return The complete YAML document text for the side file.
[[nodiscard]] std::string emitProfileYaml(TerminalProfile const& profile);

/// Serializes a single color palette as the exact text written to a `colorschemes/<name>.yml` GUI
/// side file: a bare top-level palette body, matching the existing lazy read path for that file.
/// @param palette The palette to serialize.
/// @return The complete YAML document text for the side file.
[[nodiscard]] std::string emitColorSchemeYaml(vtbackend::ColorPalette const& palette);

/// Serializes the GUI-owned global overrides as the text written to the sibling `settings.yml`.
/// @param settings The overrides to persist (unset fields are omitted, i.e. "defer to contour.yml").
/// @return The complete YAML document text.
[[nodiscard]] std::string emitGuiSettingsYaml(GuiManagedSettings const& settings);

/// Loads a single bare-body `colorschemes/<name>.yml` palette file at @p path (the same file the lazy
/// scheme resolver reads). Used by the GUI scheme editor to load an existing scheme's colors for
/// editing, independently of whether any profile currently references it.
/// @param path The color scheme file to read.
/// @return The parsed palette, or nullopt if the file is missing or unreadable.
[[nodiscard]] std::optional<vtbackend::ColorPalette> loadColorSchemeFile(std::filesystem::path const& path);

/// Loads ONLY the GUI-owned `settings.yml` at @p path (no merge). A missing file yields default
/// (all-unset) settings — nothing saved yet is not an error; a file that fails to parse yields the
/// parse error, so a caller about to rewrite it can refuse rather than destroy it.
/// @param path The `settings.yml` path (sibling of contour.yml).
/// @return The parsed overrides, or a human-readable parse error.
[[nodiscard]] std::expected<GuiManagedSettings, std::string> loadGuiSettingsFile(
    std::filesystem::path const& path);

std::string defaultConfigString();
std::error_code createDefaultConfig(std::filesystem::path const& path);
std::string defaultConfigFilePath();

std::string documentationGlobalConfig();
std::string documentationProfileConfig();

} // namespace contour::config

// {{{ fmt custom formatter support

template <>
struct std::formatter<contour::config::Permission>: formatter<std::string_view>
{
    auto format(contour::config::Permission value, auto& ctx) const
    {
        return formatter<string_view>::format(contour::config::permissionToken(value), ctx);
    }
};

template <>
struct std::formatter<vtbackend::Opacity>: formatter<float>
{
    auto format(vtbackend::Opacity value, auto& ctx) const
    {
        return formatter<float>::format(static_cast<float>(value) / std::numeric_limits<uint8_t>::max(), ctx);
    }
};

template <>
struct std::formatter<crispy::StrongHashtableSize>: formatter<unsigned>
{
    auto format(crispy::StrongHashtableSize value, auto& ctx) const
    {
        return formatter<unsigned>::format(value.value, ctx);
    }
};

template <>
struct std::formatter<vtbackend::StatusDisplayPosition>: formatter<std::string_view>
{
    auto format(vtbackend::StatusDisplayPosition value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case vtbackend::StatusDisplayPosition::Bottom: name = "Bottom"; break;
            case vtbackend::StatusDisplayPosition::Top: name = "Top"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<vtbackend::BackgroundImage>: formatter<std::string_view>
{
    auto format(vtbackend::BackgroundImage value, auto& ctx) const
    {
        if (auto* loc = std::get_if<std::filesystem::path>(&value.location))
            return formatter<string_view>::format(core::pathToUtf8(*loc), ctx);
        return formatter<string_view>::format("Image", ctx);
    }
};

template <>
struct std::formatter<vtbackend::StatusDisplayType>: formatter<std::string_view>
{
    auto format(vtbackend::StatusDisplayType value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case vtbackend::StatusDisplayType::None: name = "none"; break;
            case vtbackend::StatusDisplayType::Indicator: name = "indicator"; break;
            case vtbackend::StatusDisplayType::HostWritable: name = "host writable"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<crispy::LRUCapacity>: formatter<unsigned>
{
    auto format(crispy::LRUCapacity value, auto& ctx) const
    {
        return formatter<unsigned>::format(value.value, ctx);
    }
};

template <>
struct std::formatter<std::set<std::basic_string<char>>>: formatter<std::string_view>
{
    auto format(std::set<std::basic_string<char>> const& value, auto& ctx) const
    {
        auto result = std::string {};
        result.append(value | core::views::joinWith(std::string_view(", ")));
        return formatter<std::string_view>::format(result, ctx);
    }
};

template <>
struct std::formatter<contour::config::SelectionAction>: formatter<std::string_view>
{
    auto format(contour::config::SelectionAction value, auto& ctx) const
    {
        std::string_view name;
        switch (value)
        {
            case contour::config::SelectionAction::CopyToClipboard: name = "CopyToClipboard"; break;
            case contour::config::SelectionAction::CopyToSelectionClipboard:
                name = "CopyToSelectionClipboard";
                break;
            case contour::config::SelectionAction::Nothing: name = "Waiting"; break;
        }
        return formatter<std::string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<contour::config::ScrollBarPosition>: formatter<std::string_view>
{
    auto format(contour::config::ScrollBarPosition value, auto& ctx) const
    {
        std::string_view name;
        switch (value)
        {
            case contour::config::ScrollBarPosition::Hidden: name = "Hidden"; break;
            case contour::config::ScrollBarPosition::Left: name = "Left"; break;
            case contour::config::ScrollBarPosition::Right: name = "Right"; break;
        }
        return formatter<std::string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<contour::config::PixelReporting>: formatter<std::string_view>
{
    auto format(contour::config::PixelReporting value, auto& ctx) const
    {
        std::string_view name;
        switch (value)
        {
            case contour::config::PixelReporting::Logical: name = "Logical"; break;
            case contour::config::PixelReporting::Device: name = "Device"; break;
        }
        return formatter<std::string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<vtrasterizer::GlyphScalingMethod>: formatter<std::string_view>
{
    auto format(vtrasterizer::GlyphScalingMethod value, auto& ctx) const
    {
        // nameOf() is the single source of truth for these names -- the config reader parses the
        // same strings, so a new method cannot be formatted one way and parsed another.
        return formatter<std::string_view>::format(vtrasterizer::nameOf(value), ctx);
    }
};

template <>
struct std::formatter<contour::config::GuiTheme>: formatter<std::string_view>
{
    auto format(contour::config::GuiTheme value, auto& ctx) const
    {
        std::string_view name;
        switch (value)
        {
            case contour::config::GuiTheme::System: name = "system"; break;
            case contour::config::GuiTheme::Dark: name = "dark"; break;
            case contour::config::GuiTheme::Light: name = "light"; break;
            case contour::config::GuiTheme::Terminal: name = "terminal"; break;
        }
        return formatter<std::string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<contour::config::RenderingBackend>: formatter<std::string_view>
{
    auto format(contour::config::RenderingBackend const& val, auto& ctx) const
    {
        std::string_view name;
        switch (val)
        {
            case contour::config::RenderingBackend::Auto: name = "auto"; break;
            case contour::config::RenderingBackend::OpenGL: name = "OpenGL"; break;
            case contour::config::RenderingBackend::Vulkan: name = "vulkan"; break;
            case contour::config::RenderingBackend::Direct3D11: name = "direct3d11"; break;
            case contour::config::RenderingBackend::Direct3D12: name = "direct3d12"; break;
            case contour::config::RenderingBackend::Metal: name = "metal"; break;
            case contour::config::RenderingBackend::Software: name = "software"; break;
        }
        return formatter<std::string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<contour::config::WindowMargins>: public std::formatter<std::string>
{
    using WindowMargins = contour::config::WindowMargins;
    auto format(WindowMargins margins, auto& ctx) const
    {
        return formatter<std::string>::format(std::format("{}x+{}y", margins.horizontal, margins.vertical),
                                              ctx);
    }
};

template <typename T,
          contour::config::documentation::StringLiteral ConfigDoc,
          contour::config::documentation::StringLiteral WebDoc>
struct std::formatter<
    contour::config::ConfigEntry<T, contour::config::documentation::DocumentationEntry<ConfigDoc, WebDoc>>>
{
    auto format(contour::config::ConfigEntry<
                    T,
                    contour::config::documentation::DocumentationEntry<ConfigDoc, WebDoc>> const& c,
                auto& ctx) const
    {
        return std::format_to(ctx.out(), "{}", c.value());
    }
};
// }}}

/// Formats a daemon mode as its configuration spelling, so diagnostics quote what the user would write.
template <>
struct std::formatter<contour::config::DaemonMode>: std::formatter<std::string_view>
{
    auto format(contour::config::DaemonMode mode, auto& ctx) const
    {
        return std::formatter<std::string_view>::format(contour::config::nameOf(mode), ctx);
    }
};
