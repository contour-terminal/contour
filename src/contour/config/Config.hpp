// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/AccessibilityPreferences.hpp>
#include <contour/config/ActionsFwd.hpp>
#include <contour/config/BlockActionEnums.hpp>
#include <contour/config/ChromeDensity.hpp>
#include <contour/config/ConfigDocumentationTags.hpp>
#include <contour/config/ConfigEnum.hpp>
#include <contour/config/ConfigFwd.hpp>
#include <contour/config/DefaultsEdition.hpp>
#include <contour/config/FinishNotificationConfig.hpp>
#include <contour/config/GpuSelector.hpp>
#include <contour/config/HostUrl.hpp>
#include <contour/config/OptionAsAlt.hpp>
#include <contour/config/PreeditStyle.hpp>
#include <contour/config/QuickTerminalConfig.hpp>
#include <contour/config/ScrollBarMode.hpp>
#include <contour/config/SecureInputMode.hpp>
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

#include <crispy/ASCII.hpp>
#include <crispy/Size.hpp>
#include <crispy/StrongLRUHashtable.hpp>

#include <core/Assert.hpp>
#include <core/Environment.hpp>
#include <core/Flags.hpp>
#include <core/Utf8Path.hpp>
#include <core/Utils.hpp>
#include <core/log/LogStore.hpp>

#include <algorithm>
#include <array>
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
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <vthost/ClientSizePolicy.hpp>
#include <vtworkspace/LayoutTree.hpp> // layout tree structs (LayoutPane, LayoutTab, Layout)

namespace contour::config
{

// TabBarPosition and TabBarVisibility live in contour/TabBarMode.h, together with the table that
// describes them: an action carrying one of these modes is declared in Actions.hpp, which cannot
// include this header (this header includes ActionsFwd.hpp).

using ActionList = actions::ActionList;
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

/// The macOS find chords (R-j): ⌘F opens the find bar, ⌘G and ⇧⌘G step to the next and previous match, ⌘E
/// uses the selection. Fallbacks, in every mode -- so ⌘G with no search reaches the session, which opens the
/// bar -- and an explicit binding of the same chord still wins. builtinFallbackCharMappings() holds them on
/// macOS only; declared everywhere so the table can be tested everywhere.
/// @return The four rows.
[[nodiscard]] std::array<FallbackCharMapping, 4> const& macFindChords();

/// How many tab positions ⌘1 … ⌘9 select.
inline constexpr int MacTabPositionCount = 9;

/// How many rows macTabChords() holds: ⌘T, ⌘W, ⇧⌘[ and ⇧⌘], and one per tab position.
inline constexpr std::size_t MacTabChordCount = 4 + static_cast<std::size_t>(MacTabPositionCount);

/// The macOS tab chords: ⌘T opens a tab, ⌘W closes it, ⇧⌘[ and ⇧⌘] step to the previous and next tab, and
/// ⌘1 … ⌘9 select the tab at that position. Qt's Control/Meta swap is off, so ⌘ arrives as Super. Fallbacks,
/// in every mode, so an explicit binding of the same chord still wins; builtinFallbackCharMappings() holds
/// them on macOS only, declared everywhere so the table can be tested everywhere.
/// @return The rows.
[[nodiscard]] std::array<FallbackCharMapping, MacTabChordCount> const& macTabChords();

/// The chords that act on the focused command block (R-j): Ctrl+Enter and Shift+F10 open its menu, Enter
/// folds it, Escape lets go of it. Every one requires MatchModes::BlockFocus, so without a focused block the
/// key reaches the application as it always did. In makeDefaultInputMappings() for a freshly written
/// configuration, and in builtinFallbackKeyMappings() for one that predates them.
/// @return The four rows.
[[nodiscard]] std::array<KeyInputMapping, 4> const& blockFocusChords();

/// The macOS chords that move the block focus (R-j): Super+Up and Super+Down step to the previous and next
/// command, Shift+Super+Up and Shift+Super+Down do so and select its output. In every mode but the
/// alternate screen, where an application keeps the chord. Fallbacks; builtinFallbackKeyMappings() holds
/// them on macOS only, declared everywhere so the table can be tested everywhere.
/// @return The four rows.
[[nodiscard]] std::array<FallbackKeyMapping, 4> const& macBlockChords();

/// The host a pane chord table is written for. A parameter rather than an `#ifdef` at the use site so that
/// both tables are testable on every machine.
enum class PaneChordHost : uint8_t
{
    Desktop = 0, ///< Linux and Windows: Ctrl/Alt chords.
    Mac,         ///< macOS: the Command chords iTerm2 and Terminal users already know.
    Count,       ///< Not a host: the number of them.
};

/// The host this binary runs on.
inline constexpr auto BuildPaneChordHost =
#ifdef __APPLE__
    PaneChordHost::Mac;
#else
    PaneChordHost::Desktop;
#endif

/// The key chords that move between panes (and zoom one) and move the active tab on @p host: Alt+Shift+Arrow
/// and Ctrl+Shift+PageUp/PageDown on the desktop, Command+Option+Arrow, Command+Shift+Enter and
/// Command+PageUp/PageDown on a Mac. Not Alt+Arrow on a Mac, where Option+Arrow is the
/// shell's word motion. In makeDefaultInputMappings() for a freshly written configuration, and in
/// builtinFallbackKeyMappings() for one that predates them.
/// @param host The host whose chords to return.
/// @return The rows, in match order.
[[nodiscard]] std::span<FallbackKeyMapping const> paneKeyChords(PaneChordHost host);

/// The character chords that split the active pane on @p host: Ctrl+Shift+E and Ctrl+Shift+O on the desktop,
/// Command+D (side by side) and Command+Shift+D (stacked) on a Mac.
/// @param host The host whose chords to return.
/// @return The rows, in match order.
/// @see paneKeyChords
[[nodiscard]] std::span<FallbackCharMapping const> paneCharChords(PaneChordHost host);

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
               && testMatchMode(actualModeFlags, expected, Flag::Trace)
               && testMatchMode(actualModeFlags, expected, Flag::BlockFocus);
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
actions::ActionList const* apply(Mappings&& mappings,
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
[[nodiscard]] actions::OutputFormat outputFormatFor(actions::OpenCommandOutput const& action,
                                                    CommandBlocksConfig const& commandBlocks) noexcept;

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

    /// How the bar behaves at the bottom of the scrollback (`scrollbar.mode`): the system's scroller style,
    /// or a mode that overrides it (@see resolveScrollBarMode). The profile seeds it from its edition
    /// (EditionDefaults::scrollBarMode); a ScrollBarConfig built on its own follows the system.
    ScrollBarModePreference mode { ScrollBarModePreference::System };
    bool hideScrollbarInAltScreen { true };

    /// Which marks the scrollbar draws as ticks (`scrollbar.marks`: a YAML list of `failures`, `commands`,
    /// `running`, `user_marks`, `search_matches`; `[]` draws none). The profile seeds it from its edition
    /// (EditionDefaults::scrollBarMarks); a ScrollBarConfig built on its own draws every kind.
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
                                                                 : vtbackend::EvictedPlaceholder::Hidden,
                                                 .presentation =
                                                     vtbackend::StickyHeaderPresentation::InGrid };
    }

    bool operator==(StickyHeaderConfig const&) const = default;
};

struct MouseConfig
{
    bool hideWhileTyping { true };
};

/// The three segments of an indicator status line template, as constants an edition's default table
/// can hold.
struct IndicatorTemplate
{
    std::string_view left;   ///< The left-aligned segment.
    std::string_view middle; ///< The centred segment.
    std::string_view right;  ///< The right-aligned segment.
};

/// The indicator status line every release before the 2026 redesign shipped (DefaultsEdition::Classic).
///
/// It keeps the {Tabs} segment so the in-terminal tab list is still shown even when the GUI tab strip is
/// hidden or unavailable. (Tabs are also surfaced as first-class GUI tabs in the window title bar; the
/// two are complementary.)
inline constexpr auto ClassicIndicatorTemplate = IndicatorTemplate {
    .left = " {InputMode:Bold,Color=#FFFF00}"
            "{TraceMode:Bold,Color=#FFFF00,Left= │ }"
            "{Tabs:ActiveColor=#FFFF00,Left= │ }"
            "{ProtectedMode:Bold,Left= │ }"
            // Draws NOTHING until a privilege or machine boundary is in force, so an
            // ordinary session is unchanged by its presence.
            "{Context:Left= │ }",
    .middle = "« {Title} »",
    .right = "{HistoryLineCount:Faint,Color=#c0c0c0} │ {Clock:Bold}",
};

struct IndicatorConfig
{
    std::string left { ClassicIndicatorTemplate.left };
    std::string middle { ClassicIndicatorTemplate.middle };
    std::string right { ClassicIndicatorTemplate.right };

    /// The indicator configuration @p segments spells.
    /// @param segments The template to copy.
    /// @return An indicator configuration holding a copy of each segment.
    [[nodiscard]] static IndicatorConfig from(IndicatorTemplate const& segments)
    {
        return IndicatorConfig { .left = std::string { segments.left },
                                 .middle = std::string { segments.middle },
                                 .right = std::string { segments.right } };
    }
};

/// One segment of the indicator template, in the configuration and in an edition's defaults.
struct IndicatorSegment
{
    std::string IndicatorConfig::* spelled;        ///< The segment as a configuration holds it.
    std::string_view IndicatorTemplate::* shipped; ///< The segment as an edition ships it.
};

/// The indicator template's segments, so that a rule over them is one loop.
/// @return The segments, in the order left, middle, right; valid for the program's lifetime.
[[nodiscard]] std::span<IndicatorSegment const> indicatorSegments() noexcept;

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

    /// The keys of globalOverrides whose value is the YAML flow text of a mapping or sequence
    /// (`quick_terminal.hotkey`), not a scalar that happens to look like one: `command_blocks.pager:
    /// "{ less -R; }"` is a string. Decided where the value comes from -- the node a load read, the row a
    /// Settings edit wrote -- and never by looking at the text, which cannot tell the two apart.
    std::set<std::string> collectionOverrides {};
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

/// One configuration setting: its value, and the tag that names its documentation.
///
/// @p Doc is a tag type declared in ConfigDocumentationTags.hpp. Only its name is carried here: the strings
/// it names are defined in ConfigDocumentation.hpp, which only the writers that emit them include, so a file
/// that reads a setting does not parse every setting's documentation.
template <typename T, typename Doc>
struct ConfigEntry
{
    // NOLINTNEXTLINE(readability-identifier-naming): standard iterator/container trait spelling.
    using value_type = T;

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

/// The seconds within which a shell that exits counts as an early exit (`early_exit_threshold`).
inline constexpr unsigned DefaultEarlyExitThreshold = 5u;

struct Bell
{
    std::string sound = "default";
    bool alert = true;
    float volume = 1.0f;
};

/// Every default that differs between two editions (@see DefaultsEdition), one row per edition.
///
/// A default that is the same in every edition stays at its own member. One that changes is a column
/// here, so a third edition is a third row rather than a third branch at each member, and the members
/// that read a column cannot disagree about which edition they are in.
struct EditionDefaults
{
    DefaultsEdition edition;                            ///< The edition this row describes.
    std::string_view fontFamily;                        ///< Every face of the profile's font.
    double fontSize;                                    ///< The font size in points.
    vtbackend::PageSize terminalSize;                   ///< The initial page size.
    WindowMargins margins;                              ///< The padding around the grid.
    IndicatorTemplate statusLine;                       ///< The indicator status line's template.
    vtbackend::LinkDecorationMode linkDecoration;       ///< When detected links are decorated.
    double dimUnfocused;                                ///< How far an unfocused pane is dimmed.
    std::string_view bellSound;                         ///< Bell::sound; "off" is silent.
    vtbackend::LineCount scrollback;                    ///< The scrollback depth that always survives.
    vtbackend::CursorShape insertCursorShape;           ///< The cursor's shape in insert mode.
    std::chrono::milliseconds screenTransitionDuration; ///< How long a screen switch animates.
    /// The color scheme a dark system uses. When it equals @ref lightColorScheme the profile names one
    /// scheme (`colors: <name>`) rather than a dark/light pair.
    std::string_view darkColorScheme;
    std::string_view lightColorScheme;           ///< The color scheme a light system uses.
    TabBarVisibility tabBarVisibility;           ///< When the window's tab strip is shown.
    vtbackend::Modifier followHyperlinkModifier; ///< The modifier a click opens a link with.
    GuiTheme theme;                              ///< Which palette the window chrome follows.
    ChromeDensity chromeDensity;                 ///< How much room the window chrome takes.
    /// Whether hovering a link shows its target without the follow modifier held
    /// (`hyperlink_hover_tooltip`). A `bool` because the YAML key it seeds is one: Config's documented
    /// carve-out; the session converts it at the boundary.
    bool hyperlinkHoverTooltip;
    ScrollBarModePreference scrollBarMode;          ///< The profile's `scrollbar.mode`.
    vtbackend::PreeditStyle preeditStyle;           ///< How an input method's preedit is drawn.
    SecureInputMode secureInput;                    ///< `secure_input`: the lock and Secure Keyboard Entry.
    vtbackend::ScrollbarMarkSources scrollBarMarks; ///< The profile's `scrollbar.marks`.
};

/// The defaults @p edition ships.
/// @param edition The edition to look up.
/// @return Its row of the default table.
[[nodiscard]] EditionDefaults const& editionDefaults(DefaultsEdition edition) noexcept;

/// The indicator status line @p spelled means once the defaults switch from @p writtenFor to @p now.
///
/// A generated contour.yml, and every profile Settings writes, spells the template out, so a file written
/// under one edition holds that edition's default as if the user had chosen it, and "Use New" would not
/// reach the most visible default it promises. A segment equal to @p writtenFor's default is therefore
/// read as that default and becomes @p now's; any other segment is the user's own and stays.
/// @param spelled The template as written.
/// @param writtenFor The edition it was written under.
/// @param now The edition in effect.
/// @return The template in effect.
[[nodiscard]] IndicatorConfig indicatorForEdition(IndicatorConfig spelled,
                                                  DefaultsEdition writtenFor,
                                                  DefaultsEdition now);

/// The built-in font configuration of @p edition, used when the config file does not override it.
///
/// A function-local static rather than a namespace-scope object: its construction allocates, so a
/// throw here is catchable by the caller instead of terminating during static initialization.
/// @param edition The edition whose font to describe.
/// @return The default font descriptions (constructed on first use).
[[nodiscard]] vtrasterizer::FontDescriptions const& defaultFont(DefaultsEdition edition);

/// The color configuration @p edition ships, with each scheme's palette resolved from the built-in
/// schemes as the loader resolves a scheme name.
/// @param edition The edition whose colors to describe.
/// @return A single scheme, or a dark/light pair when the edition names two.
[[nodiscard]] ColorConfig defaultColors(DefaultsEdition edition);

/// The color configuration a profile effectively has once its `colors:` entry is read.
///
/// An edition that ships a dark/light pair (@ref defaultColors) pairs a profile that still names the stock
/// `default` scheme, untouched, so a configuration written before the pair existed follows the OS's dark
/// mode like a new one does. Any other named scheme, a customized `default` scheme, a map and an edition
/// that ships one scheme all stay as written.
/// @param edition The edition of the file the profile is in.
/// @param loaded The profile's colors as the file states them.
/// @return The edition's pair for the untouched default scheme under an edition that has one, else @p loaded.
[[nodiscard]] ColorConfig pairedColors(DefaultsEdition edition, ColorConfig loaded);

/// The layout tree model (structs + realize/serialize helpers) lives in vtworkspace::LayoutTree so the
/// Qt-free daemon shares it; these aliases keep the config-side spellings working.
using LayoutPane = vtworkspace::LayoutPane;
using LayoutTab = vtworkspace::LayoutTab;
using Layout = vtworkspace::Layout;

struct TerminalProfile
{
    /// The edition whose defaults the members below start from. First, so that every later default
    /// can read it and `TerminalProfile { .defaultsEdition = e }` builds a profile of edition @c e.
    /// Fixed at construction: changing it afterwards changes no default already taken.
    DefaultsEdition defaultsEdition { DefaultsEdition::Native2026 };
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
    ConfigEntry<double, documentation::DimUnfocused> dimUnfocused {
        editionDefaults(defaultsEdition).dimUnfocused
    };
    ConfigEntry<bool, documentation::ShowIndicatorOnResize> sizeIndicatorOnResize { true };
    ConfigEntry<bool, documentation::Fullscreen> fullscreen { false };
    ConfigEntry<bool, documentation::Maximized> maximized { false };
    ConfigEntry<bool, documentation::InsertAfterYank> insertAfterYank { false };
    ConfigEntry<Bell, documentation::Bell> bell { Bell {
        .sound = std::string { editionDefaults(defaultsEdition).bellSound },
        .alert = true,
        .volume = 1.0f } };
    ConfigEntry<FinishNotificationConfig, documentation::NotifyOnCommandFinish> notifyOnCommandFinish {};
    ConfigEntry<vtbackend::VTType, documentation::TerminalId> terminalId { vtbackend::VTType::VT525 };
    ConfigEntry<PixelReporting, documentation::PixelReporting> pixelReporting { PixelReporting::Device };
    ConfigEntry<std::map<vtbackend::DECMode, bool>, documentation::FrozenDecMode> frozenModes {};
    ConfigEntry<std::chrono::milliseconds, documentation::SmoothLineScrolling> smoothLineScrolling { 100 };
    ConfigEntry<bool, documentation::SmoothScrolling> smoothScrolling { true };
    ConfigEntry<bool, documentation::MomentumScrolling> momentumScrolling { true };
    ConfigEntry<vtbackend::PageSize, documentation::TerminalSize> terminalSize {
        editionDefaults(defaultsEdition).terminalSize
    };
    ConfigEntry<WindowMargins, documentation::Margins> margins { editionDefaults(defaultsEdition).margins };
    ConfigEntry<HistoryConfig, documentation::History> history { HistoryConfig {
        .maxHistoryLineCount = editionDefaults(defaultsEdition).scrollback } };
    ConfigEntry<ScrollBarConfig, documentation::Scrollbar> scrollbar { ScrollBarConfig {
        .mode = editionDefaults(defaultsEdition).scrollBarMode,
        .marks = editionDefaults(defaultsEdition).scrollBarMarks } };
    ConfigEntry<StickyHeaderConfig, documentation::StickyHeader> stickyHeader {};
    ConfigEntry<MouseConfig, documentation::Mouse> mouse { true };
    ConfigEntry<PermissionsConfig, documentation::Permissions> permissions {};
    ConfigEntry<bool, documentation::InputMethodEditorSupport> inputMethodEditor { true };
    ConfigEntry<vtbackend::PreeditStyle, documentation::InputMethodEditorStyle> inputMethodEditorStyle {
        editionDefaults(defaultsEdition).preeditStyle
    };
    ConfigEntry<bool, documentation::HighlightDoubleClickerWord> highlightDoubleClickedWord { true };
    ConfigEntry<vtrasterizer::FontDescriptions, documentation::Fonts> fonts { defaultFont(defaultsEdition) };
    ConfigEntry<bool, documentation::DrawBoldTextWithBrightColors> drawBoldTextWithBrightColors { false };
    ConfigEntry<vtbackend::SearchCaseSensitivity, documentation::SearchCaseSensitivity>
        searchCaseSensitivity { vtbackend::SearchCaseSensitivity::Smart };
    ConfigEntry<vtbackend::BlinkStyle, documentation::BlinkStyle> blinkStyle {
        vtbackend::BlinkStyle::Smooth
    };
    ConfigEntry<vtbackend::ScreenTransitionStyle, documentation::ScreenTransitionStyle>
        screenTransitionStyle { vtbackend::ScreenTransitionStyle::Fade };
    ConfigEntry<std::chrono::milliseconds, documentation::ScreenTransitionDuration> screenTransitionDuration {
        editionDefaults(defaultsEdition).screenTransitionDuration
    };
    ConfigEntry<std::chrono::milliseconds, documentation::CursorMotionAnimationDuration>
        cursorMotionAnimationDuration { std::chrono::milliseconds { 80 } };
    ConfigEntry<InputModeConfig, documentation::ModeInsert> modeInsert { CursorConfig {
        .cursorShape = editionDefaults(defaultsEdition).insertCursorShape,
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
    ConfigEntry<StatusLineConfig, documentation::StatusLine> statusLine { StatusLineConfig {
        .indicator = IndicatorConfig::from(editionDefaults(defaultsEdition).statusLine),
        .extension = std::nullopt } };
    ConfigEntry<std::vector<ExtensionConfig>, documentation::Extensions> extensions {};
    ConfigEntry<BackgroundConfig, documentation::Background> background {};
    ConfigEntry<ColorConfig, documentation::Colors> colors { defaultColors(defaultsEdition) };
    ConfigEntry<HyperlinkDecorationConfig, documentation::HyperlinkDecoration> hyperlinkDecoration {
        HyperlinkDecorationConfig { .mode = editionDefaults(defaultsEdition).linkDecoration }
    };
    ConfigEntry<LinkDetectionConfig, documentation::LinkDetection> linkDetection {};
    ConfigEntry<FileOpenerConfig, documentation::FileOpener> fileOpener {};
    ConfigEntry<std::vector<HintPatternConfig>, documentation::HintPatterns> hintPatterns {};
    ConfigEntry<vtbackend::LineCount, documentation::HintScrollbackLines> hintScrollbackLines {
        vtbackend::LineCount { 1000 }
    };

    ConfigEntry<std::string, documentation::WMClass> wmClass { CONTOUR_APP_ID };
    ConfigEntry<std::string, documentation::TabLabel> tabLabel { "{WindowTitle}" };
    ConfigEntry<std::optional<vtbackend::RGBColor>, documentation::TabColor> tabColor {};
    ConfigEntry<OptionAsAlt, documentation::OptionKeyAsAlt> optionAsAlt { OptionAsAlt::Off };
};

/// The built-in key/mouse bindings of @p edition, built afresh.
/// @param edition The edition whose bindings to build; only the FollowHyperlink modifier differs.
/// @param paneHost The host whose pane chords the bindings carry; the build host's for a real configuration.
/// @return The bindings.
[[nodiscard]] InputMappings makeDefaultInputMappings(DefaultsEdition edition,
                                                     PaneChordHost paneHost = BuildPaneChordHost);

/// The built-in key/mouse bindings of @p edition, used when the config file does not override them.
///
/// A function-local static rather than a namespace-scope object: its construction allocates, so a
/// throw here is catchable by the caller instead of terminating during static initialization.
/// @param edition The edition whose bindings to return.
/// @return The default input mappings (constructed on first use).
[[nodiscard]] InputMappings const& defaultInputMappings(DefaultsEdition edition);

/// The built-in key/mouse bindings a configuration without an edition of its own gets: the newest.
/// @return The default input mappings of DefaultsEdition::Native2026.
[[nodiscard]] InputMappings const& defaultInputMappings();

struct Config
{
    /// The edition whose defaults the members below start from. First, so that every later default
    /// can read it and `Config { .defaultsEdition = e }` builds a configuration of edition @c e; a
    /// plain `Config {}` is the newest edition. It is not read by the typed loader:
    /// loadConfigFromFile(path) scans `defaults_edition` before it constructs the Config.
    /// Fixed at construction: changing it afterwards changes no default already taken.
    DefaultsEdition defaultsEdition { DefaultsEdition::Native2026 };
    /// What `contour.yml` and `settings.yml` each stated as their edition, as the scan found them. Set by
    /// loadConfigFromFile(path) beside @ref defaultsEdition, and empty for a Config built in code. Runtime
    /// only: the GUI reads it to decide whether to offer the newest defaults (see NewDefaultsOffer).
    DefaultsEditionSources defaultsEditionSources {};
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
    ConfigEntry<unsigned, documentation::EarlyExitThreshold> earlyExitThreshold { DefaultEarlyExitThreshold };
    ConfigEntry<bool, documentation::SpawnNewProcess> spawnNewProcess { false };
    ConfigEntry<bool, documentation::ReflowOnResize> reflowOnResize { true };
    ConfigEntry<bool, documentation::SnapToRows> snapToRows { false };
    ConfigEntry<bool, documentation::ConnectPickerOnStart> connectPickerOnStart { false };
#ifdef _WIN32
    ConfigEntry<bool, documentation::RememberWindowPosition> rememberWindowPosition { true };
#else
    ConfigEntry<bool, documentation::RememberWindowPosition> rememberWindowPosition { false };
#endif
    ConfigEntry<bool, documentation::TabSwitchOnHorizontalWheel> tabSwitchOnHorizontalWheel { true };
    ConfigEntry<bool, documentation::HyperlinkHoverTooltip> hyperlinkHoverTooltip {
        editionDefaults(defaultsEdition).hyperlinkHoverTooltip
    };

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
        editionDefaults(defaultsEdition).tabBarVisibility
    };
    ConfigEntry<bool, documentation::GuiConfigLocked> guiConfigLocked { false };
    ConfigEntry<contour::config::GuiTheme, documentation::Theme> theme {
        editionDefaults(defaultsEdition).theme
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
    // Application-wide like windowControlStyle: a window has one chrome, so the density cannot be a
    // per-profile answer. Comfortable in both editions, so a column keeps a third edition a row.
    ConfigEntry<ChromeDensity, documentation::ChromeDensity> chromeDensity {
        editionDefaults(defaultsEdition).chromeDensity
    };
    // Application-wide, not a profile's: Secure Keyboard Entry is one switch for the whole process, so
    // which profile asks for it cannot be the answer. Off in Classic, which never showed the lock.
    ConfigEntry<SecureInputMode, documentation::SecureInput> secureInput {
        editionDefaults(defaultsEdition).secureInput
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
    ConfigEntry<QuickTerminalConfig, documentation::QuickTerminal> quickTerminal {};
    ConfigEntry<FoldingConfig, documentation::Folding> folding {};
    ConfigEntry<GutterConfig, documentation::Gutter> gutter {};
    ConfigEntry<OscContextConfig, documentation::OscContext> oscContext {};
    ConfigEntry<CommandBlocksConfig, documentation::CommandBlocks> commandBlocks {};

    ConfigEntry<std::unordered_map<std::string, TerminalProfile>, documentation::Profiles> profiles {
        { { "main", TerminalProfile { .defaultsEdition = defaultsEdition } } }
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
    /// The same for each entry of @ref hosts: MainConfig for `hosts:` of contour.yml (or a file it includes),
    /// SideFile for a `hosts/<name>.yml` Settings wrote.
    std::unordered_map<std::string, SettingsOrigin> hostOrigins {};
    /// @}

    /// GUI-owned global overrides loaded from the sibling `settings.yml` (see GuiManagedSettings).
    /// Runtime-only mirror of that file; the resolved values have already been applied to the fields
    /// above (e.g. @ref defaultProfileName), this simply records what the GUI had persisted.
    GuiManagedSettings guiManagedSettings {};
    /// Whether that `settings.yml` could be read at all. Runtime only, like @ref guiManagedSettings.
    GuiSettingsFileHealth guiSettingsFileHealth = GuiSettingsFileHealth::Usable;
    /// The bytes of each GUI side file the loader merged -- `profiles/<name>.yml`, `hosts/<name>.yml` and
    /// `settings.yml` -- keyed by its path relative to the configuration directory, with `/` separators;
    /// nullopt for a `settings.yml` there was none of. Runtime only. Settings seeds its drafts from this
    /// configuration, so these, not what the files hold by the time a draft opens, are what it last saw of
    /// them.
    std::map<std::string, std::optional<std::string>> loadedSideFiles {};

    /// Built afresh rather than copied from defaultInputMappings(): a copy here would instantiate the Action
    /// variant's copy constructor in every file that includes this header, where a prvalue is moved in.
    ConfigEntry<InputMappings, documentation::InputMappings> inputMappings { makeDefaultInputMappings(
        defaultsEdition) };
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

std::filesystem::path configHome();
std::filesystem::path configHome(std::string const& programName);

std::optional<std::string> readConfigFile(std::string const& filename);

/// The key a configuration states its DefaultsEdition under, in contour.yml or settings.yml.
inline constexpr std::string_view DefaultsEditionKey = "defaults_edition";

/// What the configuration at @p configFile and the `settings.yml` beside it each state as their
/// `defaults_edition`.
/// @param configFile The contour.yml to scan.
/// @return Each document's statement; nothing for a document that is absent, unreadable or states none.
[[nodiscard]] DefaultsEditionSources scanDefaultsEditionSources(std::filesystem::path const& configFile);

/// The edition of defaults the configuration at @p configFile is written against.
///
/// Read before the configuration is, because the edition decides the defaults it is read onto: the
/// `defaults_edition` of @p configFile, then that of the `settings.yml` beside it, which wins. Neither
/// stating one is a file from before the key existed, and so DefaultsEdition::Classic.
/// @param configFile The contour.yml to scan.
/// @return The edition that applies, `effectiveEdition(scanDefaultsEditionSources(configFile))`.
[[nodiscard]] DefaultsEdition scanDefaultsEdition(std::filesystem::path const& configFile);

/// Loads @p fileName into @p config, whose defaults (and so its edition) are already fixed.
/// @param config The configuration to load into.
/// @param fileName The contour.yml to load; created with the default configuration if absent.
void loadConfigFromFile(Config& config, std::filesystem::path const& fileName);

/// Loads @p fileName onto the defaults of the edition it states (@see scanDefaultsEdition).
/// @param fileName The contour.yml to load; created with the default configuration if absent.
/// @return The loaded configuration.
Config loadConfigFromFile(std::filesystem::path const& fileName);
Config loadConfig();

/// Loads @p fileName into a configuration of its own, so a file that fails to load leaves every other
/// configuration as it was.
/// @param fileName The configuration file.
/// @return The configuration, or a human-readable reason it could not be loaded.
[[nodiscard]] std::expected<Config, std::string> tryLoadConfigFromFile(std::filesystem::path const& fileName);

/// How a configuration file spells a shell's initial working directory.
/// @param workingDirectory The directory; empty for the home directory.
/// @return "~" quoted for the home directory, else the directory in UTF-8, as the file is written.
[[nodiscard]] std::string spelledWorkingDirectory(std::filesystem::path const& workingDirectory);

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
/// `folding`, and the block column drawn in @p form.
///
/// Whatever configures a terminal's gutter and whatever reserves the gutter's width must both take it
/// from here, or the strip reserved and the strip drawn can disagree.
/// @see FoldingConfig::markersVisible
/// @param config The loaded configuration.
/// @param form How the block column is drawn: the chrome row's choice, not a configuration key.
/// @return The gutter settings vtbackend draws with.
[[nodiscard]] vtbackend::GutterSettings gutterSettingsFor(Config const& config,
                                                          vtbackend::BlockColumnForm form);

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
/// @param blockColumnForm How the gutter draws the block column: the presenting chrome row's form. A
///        daemon-hosted session, which no local chrome draws, takes BlockColumnForm::Glyphs.
/// @param initialPageSize Overrides `profile.terminal_size` when set (a new tab/split inherits the
///        live window's running grid rather than starting at the profile's configured size).
/// @return The resolved settings.
[[nodiscard]] vtbackend::Settings sessionSettings(
    Config const& config,
    TerminalProfile const& profile,
    vtbackend::ColorPreference colorPreference,
    vtbackend::BlockColumnForm blockColumnForm,
    std::optional<vtbackend::PageSize> initialPageSize = std::nullopt);

/// The animation durations Reduce Motion zeroes: the cursor glide and the screen transition. A duration
/// that should follow Reduce Motion too is a new entry in the table behind this, and nothing else changes.
/// @return The settings members, valid for the program's lifetime.
[[nodiscard]] std::span<std::chrono::milliseconds vtbackend::Settings::* const> motionSettings() noexcept;

/// The settings a terminal runs with under the OS motion preference, leaving the configured ones intact.
/// @param settings The settings as configured, e.g. from @ref sessionSettings.
/// @param motion The OS motion preference.
/// @return @p settings with every @ref motionSettings duration at zero under
///         MotionPreference::Reduced, or @p settings unchanged under MotionPreference::Full.
[[nodiscard]] vtbackend::Settings motionAdjusted(vtbackend::Settings settings, MotionPreference motion);

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

/// Parses @p contents as a bare-body `colorschemes/<name>.yml` palette, as loadColorSchemeFile() parses the
/// file it reads. Used where the bytes come through a GuiConfigStore rather than from the disk directly.
/// @param path     The file the contents are of, for messages.
/// @param contents The file's bytes.
/// @return The parsed palette, or nullopt if the contents do not parse.
[[nodiscard]] std::optional<vtbackend::ColorPalette> parseColorSchemeYaml(std::filesystem::path const& path,
                                                                          std::string contents);

/// Loads ONLY the GUI-owned `settings.yml` at @p path (no merge). A missing file yields default
/// (all-unset) settings — nothing saved yet is not an error; a file that fails to parse yields the
/// parse error, so a caller about to rewrite it can refuse rather than destroy it.
/// @param path The `settings.yml` path (sibling of contour.yml).
/// @return The parsed overrides, or a human-readable parse error.
[[nodiscard]] std::expected<GuiManagedSettings, std::string> loadGuiSettingsFile(
    std::filesystem::path const& path);

/// The name a StatusDisplayPosition is formatted as.
/// @param value The position.
/// @return Its name.
[[nodiscard]] std::string_view nameOf(vtbackend::StatusDisplayPosition value) noexcept;

/// The name a StatusDisplayType is formatted as.
/// @param value The type.
/// @return Its name.
[[nodiscard]] std::string_view nameOf(vtbackend::StatusDisplayType value) noexcept;

/// The name a ScrollBarPosition is formatted as.
/// @param value The position.
/// @return Its name.
[[nodiscard]] std::string_view nameOf(ScrollBarPosition value) noexcept;

/// The text a background image is formatted as.
/// @param value The image.
/// @return The image's file path, or "Image" for one that is not a file.
[[nodiscard]] std::string formatBackgroundImage(vtbackend::BackgroundImage const& value);

std::string defaultConfigString();
std::error_code createDefaultConfig(std::filesystem::path const& path);
std::string defaultConfigFilePath();

std::string documentationGlobalConfig();
std::string documentationProfileConfig();

} // namespace contour::config

// {{{ fmt custom formatter support

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
        return formatter<string_view>::format(contour::config::nameOf(value), ctx);
    }
};

template <>
struct std::formatter<vtbackend::BackgroundImage>: formatter<std::string_view>
{
    auto format(vtbackend::BackgroundImage const& value, auto& ctx) const
    {
        return formatter<string_view>::format(contour::config::formatBackgroundImage(value), ctx);
    }
};

template <>
struct std::formatter<vtbackend::StatusDisplayType>: formatter<std::string_view>
{
    auto format(vtbackend::StatusDisplayType value, auto& ctx) const
    {
        return formatter<string_view>::format(contour::config::nameOf(value), ctx);
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
struct std::formatter<contour::config::ScrollBarPosition>: formatter<std::string_view>
{
    auto format(contour::config::ScrollBarPosition value, auto& ctx) const
    {
        return formatter<std::string_view>::format(contour::config::nameOf(value), ctx);
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
struct std::formatter<contour::config::WindowMargins>: public std::formatter<std::string>
{
    using WindowMargins = contour::config::WindowMargins;
    auto format(WindowMargins margins, auto& ctx) const
    {
        return formatter<std::string>::format(std::format("{}x+{}y", margins.horizontal, margins.vertical),
                                              ctx);
    }
};

template <typename T, typename Doc>
struct std::formatter<contour::config::ConfigEntry<T, Doc>>
{
    auto format(contour::config::ConfigEntry<T, Doc> const& c, auto& ctx) const
    {
        return std::format_to(ctx.out(), "{}", c.value());
    }
};
// }}}
