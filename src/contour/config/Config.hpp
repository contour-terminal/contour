// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/Actions.hpp>
#include <contour/config/ConfigDocumentation.hpp>
#include <contour/config/TabBarMode.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/config/WindowControlStyle.hpp>
#include <contour/config/WindowShadow.hpp>

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/core/Primitives.hpp> // CursorDisplay
#include <vtbackend/core/TerminalContext.hpp>
#include <vtbackend/input/InputBinding.hpp>
#include <vtbackend/input/InputGenerator.hpp>
#include <vtbackend/input/MatchModes.hpp>
#include <vtbackend/screen/Settings.hpp>
#include <vtbackend/shell/MarkArbiter.hpp>
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
#include <crispy/Assert.hpp>
#include <crispy/Environment.hpp>
#include <crispy/Flags.hpp>
#include <crispy/LogStore.hpp>
#include <crispy/Size.hpp>
#include <crispy/StrongLRUHashtable.hpp>
#include <crispy/Utils.hpp>

#include <yaml-cpp/emitter.h>
#include <yaml-cpp/node/detail/iterator_fwd.h>
#include <yaml-cpp/ostream_wrapper.h>
#include <yaml-cpp/yaml.h>

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

#include <reflection-cpp/reflection.hpp>
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
/// system's color scheme (the historical behavior).
/// @note Exposed to QML/Settings as the lower-case strings produced by
///       @c std::formatter<GuiTheme>; the reader parses them case-insensitively.
enum class GuiTheme : uint8_t
{
    System, //!< Follow the operating system's light/dark color scheme (default).
    Dark,   //!< Force a dark GUI appearance regardless of the OS.
    Light,  //!< Force a light GUI appearance regardless of the OS.
};

enum class Permission : uint8_t
{
    Deny,
    Allow,
    Ask
};

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
};

using FallbackMouseMapping = FallbackMapping<vtbackend::MouseButton>;
using FallbackKeyMapping = FallbackMapping<vtbackend::Key>;

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
template <typename Input, std::ranges::input_range Mappings>
    requires std::same_as<std::ranges::range_value_t<Mappings>, vtbackend::InputBinding<Input, ActionList>>
std::vector<actions::Action> const* apply(Mappings&& mappings,
                                          Input input,
                                          vtbackend::Modifiers modifiers,
                                          uint8_t actualModeFlags)
{
    // Forwarded rather than taken by const&: std::views::filter is not const-iterable, so a filtered
    // view over the gated fallback table could not be walked through a const reference.
    for (vtbackend::InputBinding<Input, ActionList> const& mapping: std::forward<Mappings>(mappings))
    {
        if (mapping.modifiers == modifiers && mapping.input == input
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
/// pressed, never *which* defaults are currently switched on.
///
/// @return The bound actions, or nullptr when no enabled row matches.
[[nodiscard]] ActionList const* applyBuiltinFallback(Config const& config,
                                                     vtbackend::MouseButton button,
                                                     vtbackend::Modifiers modifiers,
                                                     uint8_t actualModeFlags);

/// @copydoc applyBuiltinFallback
/// An overload rather than a differently-named function, so both consultation sites read alike.
[[nodiscard]] ActionList const* applyBuiltinFallback(Config const& config,
                                                     vtbackend::Key key,
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
    bool autoScrollOnUpdate { true };

    /// `autoScrollOnUpdate` in the vocabulary vtbackend speaks, spelled once.
    ///
    /// Two independent layers ask it -- the emulation settings the daemon and the GUI session factory
    /// share, and the re-apply on every config reload -- and the polarity of a mapping kept in two
    /// files is two chances to disagree. The same reason `limits()` and
    /// `FoldingConfig::markersVisible()` exist, and not a hypothetical one here: omitting the value
    /// from one of those two layers is exactly what used to hand every daemon-hosted session the
    /// engine default instead of the configured one.
    [[nodiscard]] constexpr vtbackend::AutoScrollOnUpdate autoScrollPolicy() const noexcept
    {
        return autoScrollOnUpdate ? vtbackend::AutoScrollOnUpdate::Yes : vtbackend::AutoScrollOnUpdate::No;
    }

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
    bool showMarkers { true };               ///< Reserve a gutter column and draw a marker in it.
    bool autoCollapseOnNewCommand { false }; ///< Collapse a command as soon as the next prompt starts.

    /// What a targeted Vi-mode jump does when its target sits inside a collapsed fold.
    vtbackend::FoldJumpBehavior onJumpIntoFold { vtbackend::FoldJumpBehavior::Expand };

    /// Whether the fold gutter is drawn, and therefore whether a column must be reserved for it.
    ///
    /// Spelled once because two independent layers ask it: the renderer, deciding whether to draw a
    /// marker, and the window geometry, deciding whether to reserve the column one would occupy. They
    /// must always agree -- a marker drawn into a column nobody reserved lands on top of the grid --
    /// and a third condition added to only one of them is a silent desync.
    ///
    /// @return Whether markers are shown.
    [[nodiscard]] constexpr bool markersVisible() const noexcept { return enabled && showMarkers; }
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

struct ScrollBarConfig
{
    ScrollBarPosition position { ScrollBarPosition::Hidden };
    bool hideScrollbarInAltScreen { true };
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
};

struct BackgroundConfig
{
    vtbackend::Opacity opacity { vtbackend::Opacity(0xFF) };
    bool blur { false };
};

struct HyperlinkDecorationConfig
{
    vtrasterizer::Decorator normal { vtrasterizer::Decorator::DottedUnderline };
    vtrasterizer::Decorator hover { vtrasterizer::Decorator::Underline };
};

/// A user-defined hint pattern for hint mode scanning.
struct HintPatternConfig
{
    std::string name;  ///< Pattern identifier (e.g. "uuid", "docker_id").
    std::string regex; ///< ECMAScript regex to match against visible text.
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

    /// GUI-set global overrides, keyed by the contour.yml top-level key, valued as the YAML scalar to
    /// write (e.g. "reflow_on_resize" -> "false"). Present here == overridden by the GUI; the on-load
    /// merge re-applies each through the same per-key loader contour.yml uses, so the value is typed
    /// correctly. Absent keys defer to contour.yml.
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
};

constexpr WindowMargins operator*(WindowMargins const& margin, double factor) noexcept
{
    return WindowMargins {
        .horizontal = HorizontalMargin { static_cast<unsigned>(*margin.horizontal * factor) },
    };
}

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
                return program | crispy::views::joinWith(std::string_view(" "));
            }(),
        .arguments = {},
        .workingDirectory = "",
        .env = {},
    } }; // namespace contour::config
    ConfigEntry<vtpty::SshHostConfig, documentation::SshHostConfig> ssh {};
    ConfigEntry<bool, documentation::EscapeSandbox> escapeSandbox { true };
    ConfigEntry<vtbackend::LineOffset, documentation::CopyLastMarkRangeOffset> copyLastMarkRangeOffset { 0 };
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
    ConfigEntry<BackgroundConfig, documentation::Background> background {};
    ConfigEntry<ColorConfig, documentation::Colors> colors { SimpleColorConfig {} };
    ConfigEntry<HyperlinkDecorationConfig, documentation::HyperlinkDecoration> hyperlinkDecoration {};
    ConfigEntry<std::vector<HintPatternConfig>, documentation::HintPatterns> hintPatterns {};
    ConfigEntry<vtbackend::LineCount, documentation::HintScrollbackLines> hintScrollbackLines {
        vtbackend::LineCount { 1000 }
    };

    ConfigEntry<std::string, documentation::WMClass> wmClass { CONTOUR_APP_ID };
    ConfigEntry<std::string, documentation::TabLabel> tabLabel { "{WindowTitle}" };
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
    ConfigEntry<int, documentation::PTYReadBufferSize> ptyReadBufferSize { 16384 };
    ConfigEntry<int, documentation::PTYBufferObjectSize> ptyBufferObjectSize { 1024 * 1024 };
    ConfigEntry<std::string, documentation::DefaultProfiles> defaultProfileName { "main" };
    ConfigEntry<unsigned, documentation::EarlyExitThreshold> earlyExitThreshold {
        documentation::DefaultEarlyExitThreshold
    };
    ConfigEntry<bool, documentation::SpawnNewProcess> spawnNewProcess { false };
    ConfigEntry<bool, documentation::ReflowOnResize> reflowOnResize { true };
    ConfigEntry<bool, documentation::TabSwitchOnHorizontalWheel> tabSwitchOnHorizontalWheel { true };
    ConfigEntry<bool, documentation::HyperlinkHoverTooltip> hyperlinkHoverTooltip { true };

    /// How long an OSC 9;4 progress indicator survives without an update. Zero disables expiry, which
    /// is the sequence's own behaviour: the indicator persists until the application clears it.
    ConfigEntry<std::chrono::milliseconds, documentation::ProgressTimeout> progressTimeout {
        std::chrono::milliseconds { 0 }
    };

    /// How long after showing a desktop notification the terminal assumes it was closed, where the
    /// desktop cannot say. Only the portal backend a sandboxed Contour must use is in that
    /// position; on the session bus a close is observed and this is never consulted. A
    /// notification's own OSC 99 `w=` wins where it states one, and zero reports nothing at all.
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
    ConfigEntry<contour::config::GuiTheme, documentation::Theme> theme { contour::config::GuiTheme::System };
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
    ConfigEntry<FoldingConfig, documentation::Folding> folding {};
    ConfigEntry<OscContextConfig, documentation::OscContext> oscContext {};

    ConfigEntry<std::unordered_map<std::string, TerminalProfile>, documentation::Profiles> profiles {
        { { "main", TerminalProfile {} } }
    };
    ConfigEntry<std::unordered_map<std::string, Layout>, documentation::Layouts> layouts {};
    ConfigEntry<std::string, documentation::DefaultLayout> defaultLayoutName { "" };
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
            crispy::unreachable();
        return found;
    }

    TerminalProfile& profile() noexcept
    {
        if (auto* prof = profile(defaultProfileName.value()); prof)
            return *prof;
        crispy::unreachable();
    }

    [[nodiscard]] TerminalProfile const& profile() const noexcept
    {
        if (auto const* prof = profile(defaultProfileName.value()); prof)
            return *prof;
        crispy::unreachable();
    }
};

struct YAMLConfigReader
{
    /// Callable that resolves a variable name to its value.
    using VariableReplacer = std::function<std::string(std::string_view)>;

    std::filesystem::path configFile;
    YAML::Node doc;
    logstore::Category const& logger;
    VariableReplacer variableReplacer;

    /// @param filename The document to parse.
    /// @param log      Where parse diagnostics go.
    /// @param env      The environment `${VAR}` expands against. It must outlive this reader: the
    ///                 default replacer holds a reference to it.
    /// @param replacer An expansion policy of the caller's own; the default one reads @p env.
    YAMLConfigReader(std::string const& filename,
                     logstore::Category const& log,
                     crispy::Environment const& env,
                     VariableReplacer replacer = {});

    /// Expands `${VAR}` tokens in @p input using the configured variable replacer.
    [[nodiscard]] std::string resolveVariables(std::string const& input) const;

    /// Expands `${VAR}` tokens then resolves `~` to the home directory.
    [[nodiscard]] std::filesystem::path resolvedPath(std::string const& input) const;

    /// Parses a single pane node. A node is a LEAF unless it carries a `split:` mapping.
    void parseLayoutPane(YAML::Node const& node, LayoutPane& where);

    template <typename T, documentation::StringLiteral ConfigDoc, documentation::StringLiteral WebDoc>
    void loadFromEntry(YAML::Node const& node,
                       std::string const& entry,
                       ConfigEntry<T, documentation::DocumentationEntry<ConfigDoc, WebDoc>>& where)
    {
        try
        {
            loadFromEntry(node, entry, where.value());
        }
        catch (std::exception const& e)
        {
            logger()("Failed, default value will be used");
        }
    }

    template <typename T,
              documentation::StringLiteral ConfigDoc,
              documentation::StringLiteral WebDoc,
              typename... Args>
    void loadFromEntry(std::string const& entry,
                       ConfigEntry<T, documentation::DocumentationEntry<ConfigDoc, WebDoc>>& where,
                       Args&&... args)
    {
        loadFromEntry(doc, entry, where.value(), std::forward<Args>(args)...);
    }

    template <typename T>
        requires std::is_scalar_v<T>
    void loadFromEntry(YAML::Node const& node, std::string const& entry, T& where)
    {
        auto const child = node[entry];
        if (child)
            where = child.as<T>();
        logger()("Loading entry: {}, value {}", entry, where);
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
            loadFromEntry(child, defaultProfileName, where[defaultProfileName]);
            for (auto entry: child)
            {
                auto const name = entry.first.as<std::string>();
                if (name == defaultProfileName)
                    continue;
                logger()("Loading map with entry: {}", name);
                where[name] = where[defaultProfileName]; // inherit from default
                loadFromEntry(child, entry.first.as<std::string>(), where[name]);
            }
        }
    }

    void loadFromEntry(YAML::Node const& node,
                       std::string const& entry,
                       std::unordered_map<std::string, Layout>& where);

    void loadFromEntry(YAML::Node const& node, std::string const& entry, Layout& where);

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
                loadFromEntry(child, entry.first.as<std::string>(), where[name]);
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
                               actions::Action action)
    {
        for (auto& binding: bindings)
        {
            if (match(binding, modes, modifier, input))
            {
                binding.binding.emplace_back(std::move(action));
                return;
            }
        }

        bindings.emplace_back(vtbackend::InputBinding<Input, ActionList> {
            modes, modifier, input, ActionList { std::move(action) } });
    }

    // clang-format off
    void loadFromEntry(YAML::Node const& node, std::string const& entry, std::filesystem::path& where) const;
    void loadFromEntry(YAML::Node const& node, std::string const& entry, RenderingBackend& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, crispy::StrongHashtableSize& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::MaxHistoryLineCount& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, crispy::LRUCapacity& where);
    void loadFromEntry(YAML::Node const& node,
                       std::string const& entry,
                       vtrasterizer::GlyphScalingMethod& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::CursorDisplay& where);
    void loadFromEntry(YAML::Node const& node,
                       std::string const& entry,
                       vtbackend::SearchCaseSensitivity& where);
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
    void loadFromEntry(YAML::Node const& node, std::string const& entry, TabBarPosition& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, ShadowSize& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, TabBarVisibility& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, UiStyle& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, WindowControlStyle& where);
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
    void loadFromEntry(YAML::Node const& node, std::string const& entry, std::map<vtbackend::DECMode, bool>& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtrasterizer::Decorator& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::Opacity& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::StatusDisplayType& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, Permission& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, std::shared_ptr<vtbackend::BackgroundImage>& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::CellRGBColor& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::CursorColor& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::RGBColor& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::RGBColorPair& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::CellRGBColorAndAlphaPair& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::ColorPalette::Palette& colors);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::ColorPalette& where);
    void loadFromEntry(YAML::Node const& node, vtbackend::ColorPalette& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, TerminalProfile& where);

    /// Parses a profile body map (a profile's `shell`, `font`, `colors`, ... keys) directly into
    /// @p where. This is the shared core of profile parsing: the `loadFromEntry(node, entry, profile)`
    /// overload above resolves `node[entry]` to the body and delegates here, and the GUI side-file
    /// loader (whose `profiles/<name>.yml` document root IS the body) calls it directly.
    /// @param profileNode The profile body map; a null node is a no-op (@p where keeps its base values).
    /// @param where The profile to populate; pre-seed it with an inheritance base before calling.
    void loadProfileBody(YAML::Node const& child, TerminalProfile& where);

    void loadFromEntry(YAML::Node const& node, std::string const& entry, RendererConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, ImagesConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, HistoryConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, FoldingConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, OscContextConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::ContextMarkPolicy& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::ContextTintScope& where);
    void loadFromEntry(YAML::Node const& node,
                       std::string const& entry,
                       vtbackend::FoldJumpBehavior& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, ScrollBarConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, MouseConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, StatusLineConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, BackgroundConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, HyperlinkDecorationConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, PermissionsConfig& where);
    void loadFromEntry(YAML::Node const& node,
                       std::string const& entry,
                       std::vector<HintPatternConfig>& where);
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
                     actions::Action action);
};

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

    [[nodiscard]] std::string format(KeyInputMapping v)
    {
        auto actionAndModes = format(" action: {} }}", v.binding[0]);
        if (v.modes.any())
        {
            actionAndModes = format(" action: {}, mode: '{}' }}", v.binding[0], v.modes);
        }
        return format("{:<30},{:<30},{:<30}\n",
                      format("- {{ mods: [{}]", format(v.modifiers)),
                      format(" key: '{}'", v.input),
                      actionAndModes);
    }

    [[nodiscard]] std::string format(CharInputMapping v)
    {
        auto actionAndModes = format(" action: {} }}", v.binding[0]);
        if (v.modes.any())
        {
            actionAndModes = format(" action: {}, mode: '{}' }}", v.binding[0], v.modes);
        }
        return format("{:<30},{:<30},{:<30}\n",
                      format("- {{ mods: [{}]", format(v.modifiers)),
                      format(" key: '{}'", static_cast<char>(v.input)),
                      actionAndModes);
    }

    [[nodiscard]] std::string format(MouseInputMapping v)
    {
        auto actionAndModes = format(" action: {} }}", v.binding[0]);
        return format("{:<30},{:<30},{:<30}\n",
                      format("- {{ mods: [{}]", format(v.modifiers)),
                      format(" mouse: {}", v.input),
                      actionAndModes);
    }

    [[nodiscard]] std::string static format(std::vector<text::FontFeature> const& v)
    {

        auto result = std::string { "[" };
        result.append(v | std::views::transform([](auto f) { return std::format("{}", f); })
                      | crispy::views::joinWith(std::string_view(", ")));
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
        args.append(v.arguments | crispy::views::joinWith(std::string_view(", ")));
        args.append("]");
        return format(doc, v.program, args, [&]() -> std::string {
            auto fromConfig = v.workingDirectory.string();
            if (fromConfig.empty()
                || fromConfig == crispy::homeResolvedPath("~", vtpty::Process::homeDirectory()))
                return std::string { "\"~\"" };
            return fromConfig;
        }());
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
                // Escape single quotes for YAML single-quoted scalars by doubling them.
                auto escapedRegex = p.regex;
                for (auto pos = escapedRegex.find('\''); pos != std::string::npos;
                     pos = escapedRegex.find('\'', pos + 2))
                    escapedRegex.insert(pos, 1, '\'');

                result += std::format(
                    "{}    - name: {}\n{}      regex: '{}'\n", indent, p.name, indent, escapedRegex);
            }
        }
        return result;
    }

    [[nodiscard]] std::string static format(vtbackend::CellRGBColor const& v)
    {
        if (std::holds_alternative<vtbackend::RGBColor>(v))
            return std::format("'{}'", v);

        return std::format("{}", v);
    }

    [[nodiscard]] std::string static format(vtbackend::RGBColor const& v) { return std::format("'{}'", v); }

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

    [[nodiscard]] std::string format(std::string_view doc, RendererConfig const& v)
    {
        return format(doc,
                      v.renderingBackend,
                      v.textureAtlasDirectMapping,
                      v.textureAtlasHashtableSlots,
                      v.textureAtlasTileCount);
    }

    [[nodiscard]] std::string format(std::string_view doc, ImagesConfig const& v)
    {
        return format(doc, v.sixelScrolling, v.maxImageColorRegisters, v.goodImageProtocol);
    }

    [[nodiscard]] std::string format(std::string_view doc, WindowMargins const& v)
    {
        return format(doc, v.horizontal, v.vertical);
    }

    [[nodiscard]] std::string format(std::string_view doc, FoldingConfig const& v)
    {
        return format(doc, v.enabled, v.showMarkers, v.autoCollapseOnNewCommand, v.onJumpIntoFold);
    }

    [[nodiscard]] std::string format(std::string_view doc, OscContextConfig const& v)
    {
        return format(doc, v.enabled, v.maxDepth, v.maxRetained, v.deriveMarkers, v.tinting);
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
        return format(doc, v.position, v.hideScrollbarInAltScreen);
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
                      v.indicator.right);
    }

    [[nodiscard]] std::string format(std::string_view doc, BackgroundConfig const& v)
    {
        return format(doc, v.opacity, v.blur);
    }

    [[nodiscard]] std::string format(std::string_view doc, HyperlinkDecorationConfig const& v)
    {
        return format(doc, v.normal, v.hover);
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

    template <typename... T>
    std::string process(std::string_view doc, [[maybe_unused]] std::string_view name, T const&... val)
    {
        return format(addOffset(replaceCommentPlaceholder(std::string { doc }),
                                static_cast<size_t>(Offset::Levels) * OneOffset),
                      val...);
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
        return process(doc, std::string_view { "" }, val...);
    }

    template <typename... T>
    std::string process(std::string_view doc, std::string_view name, T const&... val)
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

/// Loads a configuration and resolves one profile's @ref emulationSettings from it.
///
/// The whole load-resolve-refuse sequence, in one place, because two verbs need exactly it and
/// nothing else: `contour daemon`, which hosts terminals no display ever presents, and
/// `contour client`, which states the same settings in its handshake so a WARM daemon emulates the
/// sessions it creates the way this client's configuration says. Spelling it twice is how the two
/// would come to disagree.
///
/// @param configPath Configuration file to read; empty selects the default location.
/// @param profileName Profile to resolve; empty selects the configuration's default.
/// @return The settings, or a human-readable reason they could not be resolved. An unknown profile
///         is a failure rather than a fall back to the default one: hosting sessions under a
///         profile the user did not name is a misconfiguration they would discover only much later,
///         through the wrong scrollback depth or the wrong reported terminal.
[[nodiscard]] std::expected<vtbackend::Settings, std::string> resolveEmulationSettings(
    std::string const& configPath, std::string const& profileName);

/// Loads ONLY the `layouts:` map contained in the single file at @p path (no sibling-merge, no
/// inline-config layouts). A missing file yields an empty map (nothing saved yet is not an
/// error); a file that fails to parse yields the parse error instead, so SaveLayout can REFUSE to
/// rewrite — and thereby destroy — layouts it could not read back.
/// Used by SaveLayout to read back layouts.yml's own prior contents before appending the new one,
/// so the file is never overwritten with the merged (inline + file) in-memory view.
std::expected<std::unordered_map<std::string, Layout>, std::string> loadLayoutsFile(
    std::filesystem::path const& path);

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

/// Splits a command line into tokens the way a shell would: whitespace separates tokens; single quotes
/// ('...') quote a run literally; double quotes ("...") quote a run allowing \" and \\ escapes; a
/// backslash outside quotes escapes the next character. Used so a layout `command:` may be written as a
/// full command line ("emacs -nw") rather than requiring a separate `arguments:` list. Returns an empty
/// vector for an empty/whitespace-only input.
[[nodiscard]] std::vector<std::string> shellSplit(std::string_view commandLine);

/// The inverse of shellSplit for a single token: returns @p token single-quoted when it contains
/// whitespace or a shell-significant character (or is empty), so that shellSplit() reconstructs it as
/// one token; embedded single quotes use the '\'' idiom. Used when serializing a layout's `command`
/// program so a program path containing spaces survives the save/reload round-trip.
[[nodiscard]] std::string shellQuote(std::string_view token);

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
        string_view name;
        switch (value)
        {
            case contour::config::Permission::Allow: name = "allow"; break;
            case contour::config::Permission::Deny: name = "deny"; break;
            case contour::config::Permission::Ask: name = "ask"; break;
        }
        return formatter<string_view>::format(name, ctx);
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
            return formatter<string_view>::format(loc->string(), ctx);
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
        result.append(value | crispy::views::joinWith(std::string_view(", ")));
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

// Both tab bar modes render as the configuration token their table row carries, so what is written
// back is by construction what the reader accepts -- see contour/ConfigEnum.h.
template <>
struct std::formatter<contour::config::TabBarPosition>: formatter<std::string_view>
{
    auto format(contour::config::TabBarPosition value, auto& ctx) const
    {
        return formatter<std::string_view>::format(contour::config::configEnumToken(value), ctx);
    }
};

// vtbackend owns the enum, but the TOKEN is a configuration fact, so the formatter lives here with
// the others and delegates to the same table the reader uses. Spelling it in vtbackend instead would
// put the written spelling out of reach of the table that defines the accepted one.
template <>
struct std::formatter<vtbackend::SearchCaseSensitivity>: formatter<std::string_view>
{
    auto format(vtbackend::SearchCaseSensitivity value, auto& ctx) const
    {
        return formatter<std::string_view>::format(contour::config::configEnumToken(value), ctx);
    }
};

template <>
struct std::formatter<contour::config::ShadowSize>: formatter<std::string_view>
{
    auto format(contour::config::ShadowSize value, auto& ctx) const
    {
        return formatter<std::string_view>::format(contour::config::configEnumToken(value), ctx);
    }
};

template <>
struct std::formatter<contour::config::TabBarVisibility>: formatter<std::string_view>
{
    auto format(contour::config::TabBarVisibility value, auto& ctx) const
    {
        return formatter<std::string_view>::format(contour::config::configEnumToken(value), ctx);
    }
};

template <>
struct std::formatter<contour::config::UiStyle>: formatter<std::string_view>
{
    auto format(contour::config::UiStyle value, auto& ctx) const
    {
        return formatter<std::string_view>::format(contour::config::configEnumToken(value), ctx);
    }
};

template <>
struct std::formatter<contour::config::WindowControlStyle>: formatter<std::string_view>
{
    auto format(contour::config::WindowControlStyle value, auto& ctx) const
    {
        return formatter<std::string_view>::format(contour::config::configEnumToken(value), ctx);
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
