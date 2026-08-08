// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ConfigEnum.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string_view>

namespace crispy
{
class Environment;
}

namespace contour::config
{

/// How the window's own minimize/maximize/close controls are drawn, and which side they sit on.
///
/// Only relevant while the window is frameless (`show_title_bar: false`), which is when the tab bar
/// IS the title bar and has to draw everything the native decoration otherwise would. With the
/// native frame kept, the OS draws these and Contour draws none.
///
/// This selects a *form*, not a color: every style takes its colors from the OS palette and follows
/// the dark/light @c theme setting, apart from the few hues that mean something on their own (a red
/// close affordance, the traffic lights' red/amber/green).
///
/// It composes with @c UiStyle rather than duplicating it: this enum says which side, which order
/// and which shape, while @c UiStyleTokens says how long each of those is in the active chrome's own
/// unit. That is what lets every style here work in a cell-quantized chrome as well as a pixel one.
enum class WindowControlStyle : uint8_t
{
    Auto = 0, //!< Match the host: macOS -> MacOS, Windows -> Windows, a KDE session -> Plasma.
    Windows,  //!< Trailing edge, flush rectangular buttons, red close hover (historical behavior).
    MacOS,    //!< Leading edge, traffic lights, close/minimize/zoom order.
    Plasma,   //!< Trailing edge, circular hover fills -- KDE Plasma's Breeze decoration.
};

/// Which end of the title bar the window controls sit at.
///
/// Named by edge rather than by hand, because "leading" is what survives a right-to-left layout;
/// the QML resolves it against the layout direction, not against a fixed left and right.
enum class WindowControlSide : uint8_t
{
    Trailing = 0, //!< After the tabs and the drag region -- the right in a left-to-right layout.
    Leading,      //!< Before the tabs -- the left in a left-to-right layout.
};

/// The shape a window control is drawn as.
///
/// A style needs a new enumerator here only when it wants a shape neither of these describes, and
/// that -- unlike a new color, order or side -- is the one change that also needs QML. It is the
/// same split @c UiStyle draws between its token table and its Quick Controls module.
enum class WindowControlPresentation : uint8_t
{
    Button = 0,   //!< A rectangular button showing a glyph, with a hover fill behind it.
    TrafficLight, //!< A small colored dot that reveals its glyph while the group is hovered.
};

/// What one window control does. Also the index into @c WindowControlTokens::dotColors.
enum class WindowControlKind : uint8_t
{
    Close = 0,
    Minimize,
    Maximize, //!< Maximize, or restore when the window is already maximized.
};

/// How many controls a title bar draws. Fixed at three: minimize, maximize and close is what every
/// desktop this supports puts in its title bar, and a style that wanted fewer would be saying
/// something about the window's capabilities rather than about its appearance.
inline constexpr size_t WindowControlCount = 3;

/// What one @c WindowControlStyle is, as data.
///
/// Deliberately holds no extents. How wide a button is, how large a dot is and how far either sits
/// from the window's edge are lengths, and lengths belong to @c UiStyleTokens, which states them in
/// the unit the active chrome counts in. Keeping them apart is what stops the two tables from
/// multiplying into a style-per-combination.
struct WindowControlTokens
{
    /// Which end of the title bar the group sits at.
    WindowControlSide side;

    /// The shape every control in the group is drawn as. Whether a field below applies at all is
    /// this value's to say -- @c dotColors are a @c TrafficLight's, @c closeHoverColor a @c Button's.
    WindowControlPresentation presentation;

    /// The controls in the order they are drawn, from the leading edge of the group to its trailing
    /// edge. This is a real difference between platforms and not a mirroring of one order: macOS
    /// puts close FIRST at the left, where Windows puts it LAST at the right.
    std::array<WindowControlKind, WindowControlCount> order;

    /// Corner radius of a @c Button presentation's hover fill, as a percentage of half its shorter
    /// side. 0 leaves the fill square, 100 rounds it fully.
    int hoverCornerPercent;

    /// Fill the close button takes on hover, as 0xRRGGBB, or 0 to use the chrome's ordinary hover
    /// wash like every other control. Stated rather than derived from the palette because it says
    /// what the button DOES -- a theme-picked hue could render "close the window" in a reassuring
    /// green. @c Button presentation only.
    uint32_t closeHoverColor;

    /// The color of each dot, as 0xRRGGBB, indexed by @c WindowControlKind.
    /// @c TrafficLight presentation only; a @c Button style leaves these 0.
    std::array<uint32_t, WindowControlCount> dotColors;

    /// What every dot fades to while the window is not the active one, as 0xRRGGBB. macOS greys all
    /// three out together, which is a large part of how a stack of its windows reads at a glance.
    uint32_t inactiveDotColor;

    /// The glyph each control is drawn with, indexed by @c WindowControlKind -- like @c dotColors,
    /// and for the same reason: a control is then reached by indexing rather than by a switch that
    /// every new control would have to be added to as well as to every row.
    std::array<std::string_view, WindowControlCount> glyphs;

    /// Replaces the maximize glyph while the window is already maximized. The one control with two
    /// faces, so the one glyph that is not in the array above.
    std::string_view restoreGlyph;

    /// Defaulted so a style derived from another (Breeze from Windows) can be checked against it as
    /// a whole rather than field by field, which is what keeps such a test honest when a field is
    /// added.
    [[nodiscard]] bool operator==(WindowControlTokens const&) const = default;
};

/// The host a build is running on, to the extent @c WindowControlStyle::Auto cares.
///
/// A separate enum from @c WindowControlStyle rather than the same one reused: what the host IS and
/// which controls the user WANTS are different questions, and conflating them would leave no way to
/// say "a macOS build showing Windows controls", which is exactly what an explicit setting is for.
enum class HostPlatform : uint8_t
{
    Other = 0, //!< Anything with no window-control convention of its own that this knows about.
    Windows,
    MacOS,
    KdePlasma,
};

namespace detail
{
    // inline, so every translation unit that includes this header shares one table rather than
    // getting a private copy the returned span would then point into.
    //
    // The appearance every Contour window has had until now: flush full-height buttons at the
    // trailing edge, square hover wash, and the close button turning Windows' own red.
    inline constexpr auto WindowsTokens = WindowControlTokens {
        .side = WindowControlSide::Trailing,
        .presentation = WindowControlPresentation::Button,
        .order = { WindowControlKind::Minimize, WindowControlKind::Maximize, WindowControlKind::Close },
        .hoverCornerPercent = 0,
        .closeHoverColor = 0xC42B1C,
        .dotColors = { 0, 0, 0 },
        .inactiveDotColor = 0,
        .glyphs = { "✕", "—", "▢" },
        .restoreGlyph = "❐",
    };

    // Traffic lights at the leading edge, in macOS's own order and colors. The glyphs are the ones
    // macOS reveals inside a dot on hover: a cross to close, a minus to minimize, and two opposed
    // arrows for zoom -- which is what the green button does there, and what toggleMaximized() does
    // here.
    inline constexpr auto MacOsTokens = WindowControlTokens {
        .side = WindowControlSide::Leading,
        .presentation = WindowControlPresentation::TrafficLight,
        .order = { WindowControlKind::Close, WindowControlKind::Minimize, WindowControlKind::Maximize },
        // No hover fill: a traffic light's own color IS the affordance, and a wash behind it would
        // only muddy the dot.
        .hoverCornerPercent = 0,
        .closeHoverColor = 0,
        .dotColors = {
            0xFF5F57, // Close
            0xFEBC2E, // Minimize
            0x28C840, // Maximize (zoom)
        },
        .inactiveDotColor = 0xB9B9B9,
        .glyphs = { "✕", "—", "⤢" },
        // Zoom is a toggle on macOS and shows the same glyph either way, but the two arrows are
        // turned inward to say the window is coming back.
        .restoreGlyph = "⤡",
    };

    // Breeze differs from Windows in two values and agrees on the other seven, so it says only the
    // two rather than restating the row: a glyph or an order that drifted between them would be a
    // silent inconsistency no reader would spot in two adjacent literal blocks.
    inline constexpr auto PlasmaTokens = [] {
        auto tokens = WindowsTokens;
        tokens.hoverCornerPercent = 100;   // a circular hover fill rather than a full-height rectangle
        tokens.closeHoverColor = 0xC9312B; // Breeze's own red
        return tokens;
    }();

    // Rows are indexed by WindowControlStyle, so their order is the enumerator order --
    // windowControlTokens() relies on that, and the static_assert below pins it.
    //
    // Auto's row is the Windows row itself, not a copy of it: Auto is never read on a resolved value
    // (resolveWindowControlStyle() never returns it), and this keeps the lookup a plain index with
    // no arithmetic and no bounds check while making an unresolved value degrade to the appearance
    // Contour has always had rather than to an empty title bar.
    inline constexpr auto WindowControlTokenTable =
        std::array { WindowsTokens, WindowsTokens, MacOsTokens, PlasmaTokens };

    inline constexpr auto WindowControlStyleTable = std::array {
        ConfigEnumInfo<WindowControlStyle> { WindowControlStyle::Auto, "auto", "Match the system" },
        ConfigEnumInfo<WindowControlStyle> { WindowControlStyle::Windows, "windows", "Windows" },
        ConfigEnumInfo<WindowControlStyle> { WindowControlStyle::MacOS, "macos", "macOS" },
        ConfigEnumInfo<WindowControlStyle> { WindowControlStyle::Plasma, "plasma", "KDE Plasma" },
    };

    static_assert(WindowControlTokenTable.size() == WindowControlStyleTable.size(),
                  "Every WindowControlStyle needs exactly one token row; windowControlTokens() "
                  "indexes by enumerator.");

    // The size check above says nothing about ORDER: reordering the enumerators, or inserting one in
    // the middle, keeps both tables the same size and compiles cleanly while every style silently
    // hands back another style's row -- a macOS window drawing Breeze buttons, with no diagnostic.
    // So walk the index and check that row i really is enumerator i, the same way ModifierNames.hpp
    // pins its own table against reordering.
    static_assert(std::ranges::all_of(std::views::iota(size_t { 0 }, WindowControlStyleTable.size()),
                                      [](size_t index) {
                                          return WindowControlStyleTable[index].value
                                                 == static_cast<WindowControlStyle>(index);
                                      }),
                  "WindowControlStyleTable must be in enumerator order, one row per enumerator, "
                  "starting at zero: windowControlTokens() indexes the parallel token table by "
                  "static_cast<size_t>(style) and does no bounds check.");
} // namespace detail

template <>
constexpr std::span<ConfigEnumInfo<WindowControlStyle> const> configEnumValues() noexcept
{
    return detail::WindowControlStyleTable;
}

/// What @p style is, as data.
///
/// Indexes the table by enumerator, which the static_assert above keeps sound; no bounds check is
/// needed and none is done, so this stays usable from a noexcept context.
///
/// @param style A style, conventionally the result of @c resolveWindowControlStyle. Passing
///              @c Auto is not an error but answers with the @c Windows row -- see the note on that
///              row for why.
[[nodiscard]] constexpr WindowControlTokens const& windowControlTokens(WindowControlStyle style) noexcept
{
    return detail::WindowControlTokenTable[static_cast<size_t>(style)];
}

/// The concrete style @p configured selects on @p host.
///
/// Split from the host detection so the policy -- which host wants which controls -- is a pure
/// function that a test can drive through every combination on whatever machine it happens to run
/// on. A host this knows nothing about resolves to @c Windows, which is the appearance Contour has
/// always shipped, so `auto` changes what an existing user sees on exactly one platform.
///
/// @param configured What the user asked for.
/// @param host       The host to resolve @c Auto against.
/// @return A concrete style; never @c Auto.
[[nodiscard]] constexpr WindowControlStyle resolveWindowControlStyle(WindowControlStyle configured,
                                                                     HostPlatform host) noexcept
{
    if (configured != WindowControlStyle::Auto)
        return configured;

    switch (host)
    {
        case HostPlatform::MacOS: return WindowControlStyle::MacOS;
        case HostPlatform::KdePlasma: return WindowControlStyle::Plasma;
        case HostPlatform::Windows:
        case HostPlatform::Other: break;
    }
    return WindowControlStyle::Windows;
}

/// The desktop environment @p env names.
///
/// A runtime question, unlike "is this a macOS build": one Linux binary runs under every desktop
/// there is, and which one is running is something only the environment can say. Takes the
/// environment as a collaborator rather than calling getenv() so a test can answer for it.
///
/// @param env The environment to read @c XDG_CURRENT_DESKTOP and @c KDE_FULL_SESSION from.
/// @return @c KdePlasma for a KDE session, @c Other for anything else.
[[nodiscard]] HostPlatform detectDesktopPlatform(crispy::Environment const& env);

/// The host this build is running on.
///
/// Compile-time on Windows and macOS, where the platform is the build; @c detectDesktopPlatform
/// everywhere else, where it is the session.
///
/// @param env The environment, consulted only where the platform is a runtime question.
[[nodiscard]] HostPlatform detectHostPlatform(crispy::Environment const& env);

} // namespace contour::config
