// SPDX-License-Identifier: Apache-2.0
#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/ChromeTokenRows.hpp>
#include <contour/config/WindowControlLook.hpp>
#include <contour/config/WindowControlStyle.hpp>

#include <core/testing/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>
#include <string_view>
#include <vector>

using contour::config::configEnumValues;
using contour::config::HostPlatform;
using contour::config::resolveWindowControlStyle;
using contour::config::WindowControlKind;
using contour::config::WindowControlPresentation;
using contour::config::WindowControlSide;
using contour::config::WindowControlStyle;
using contour::config::windowControlTokens;

namespace
{

// The shared core::testing::FakeEnvironment is the whole point of core::Environment being an
// interface: the KDE detection can be driven through every case from a test binary running under any
// desktop at all, including none.
using core::testing::FakeEnvironment;

/// Every style a user can actually end up with, i.e. every enumerator except the one that resolves.
constexpr auto ConcreteStyles = std::array { WindowControlStyle::Windows,
                                             WindowControlStyle::MacOS,
                                             WindowControlStyle::Plasma,
                                             WindowControlStyle::Linux };

constexpr auto EveryHost =
    std::array { HostPlatform::Other, HostPlatform::Windows, HostPlatform::MacOS, HostPlatform::KdePlasma };

} // namespace

TEST_CASE("WindowControlStyle: every style has a usable token row", "[windowcontrols]")
{
    // windowControlTokens() indexes the token table by enumerator, so a style without a row would
    // read past the end. The static_assert in the header pins the sizes; this pins that each row is
    // filled in rather than default-constructed.
    for (auto const& info: configEnumValues<WindowControlStyle>())
    {
        auto const& tokens = windowControlTokens(info.value);

        // A control with no glyph is an invisible button: it still takes its width in the bar and
        // still closes the window when clicked, which is the worst of both.
        for (auto const& glyph: tokens.glyphs)
            CHECK(!glyph.empty());
        CHECK(!tokens.restoreGlyph.empty());

        // Every control appears exactly once. A row that repeated one would silently drop another --
        // and "the minimize button is gone" is not a diagnosis anyone would reach from a token table.
        auto order = tokens.order;
        std::ranges::sort(order);
        CHECK(order
              == std::array {
                  WindowControlKind::Close, WindowControlKind::Minimize, WindowControlKind::Maximize });

        // A percentage outside 0..100 would be a corner radius larger than the button it rounds.
        CHECK(tokens.hoverCornerPercent >= 0);
        CHECK(tokens.hoverCornerPercent <= 100);
    }
}

TEST_CASE("WindowControlStyle: a presentation states the colors it needs", "[windowcontrols]")
{
    for (auto const style: ConcreteStyles)
    {
        auto const& tokens = windowControlTokens(style);

        if (tokens.presentation == WindowControlPresentation::TrafficLight)
        {
            // A traffic light IS its color -- with none, all three dots draw as "unset" and the
            // control disappears into the bar.
            for (auto const color: tokens.dotColors)
                CHECK(color != 0);
            CHECK(tokens.inactiveDotColor != 0);
        }
        else
        {
            // The converse, so a Button row cannot quietly carry dot colors nothing reads: they
            // would look like the style's intent while having no effect at all.
            for (auto const color: tokens.dotColors)
                CHECK(color == 0);
            CHECK(tokens.inactiveDotColor == 0);
        }
    }
}

TEST_CASE("WindowControlStyle: Auto degrades to the historical appearance", "[windowcontrols]")
{
    // Auto's row is never read on a resolved value, but it exists so the lookup stays a plain index.
    // Its being the Windows row -- i.e. the appearance Contour has always shipped -- is what makes an
    // unresolved value harmless rather than an empty title bar. Compared whole rather than field by
    // field, so a field added later is covered without this test being touched.
    CHECK(windowControlTokens(WindowControlStyle::Auto) == windowControlTokens(WindowControlStyle::Windows));
}

TEST_CASE("WindowControlStyle: Auto resolves against the host", "[windowcontrols]")
{
    CHECK(resolveWindowControlStyle(WindowControlStyle::Auto, HostPlatform::MacOS)
          == WindowControlStyle::MacOS);
    CHECK(resolveWindowControlStyle(WindowControlStyle::Auto, HostPlatform::Windows)
          == WindowControlStyle::Windows);
    // Every other desktop, KDE's included, takes the Linux header bar's round buttons, placed where the
    // desktop's own button layout puts them (R-c); Breeze's look stays an explicit `plasma`.
    CHECK(resolveWindowControlStyle(WindowControlStyle::Auto, HostPlatform::KdePlasma)
          == WindowControlStyle::Linux);
    CHECK(resolveWindowControlStyle(WindowControlStyle::Auto, HostPlatform::Other)
          == WindowControlStyle::Linux);
}

TEST_CASE("WindowControlStyle: the Linux row draws round buttons with themed icons", "[windowcontrols]")
{
    auto const& linuxRow = windowControlTokens(WindowControlStyle::Linux);
    CHECK(linuxRow.presentation == WindowControlPresentation::Round);
    CHECK(linuxRow.side == WindowControlSide::Trailing);
    CHECK(
        linuxRow.order
        == std::array { WindowControlKind::Minimize, WindowControlKind::Maximize, WindowControlKind::Close });
    CHECK(linuxRow.hoverCornerPercent == 100);
    // The round fill is the affordance; no style hue tints close.
    CHECK(linuxRow.closeHoverColor == 0);
    CHECK(linuxRow.iconNames
          == std::array<std::string_view, contour::config::WindowControlCount> {
              "window-close-symbolic", "window-minimize-symbolic", "window-maximize-symbolic" });
    CHECK(linuxRow.restoreIconName == "window-restore-symbolic");
    // The glyph styles name no icon.
    for (auto const style:
         { WindowControlStyle::Windows, WindowControlStyle::MacOS, WindowControlStyle::Plasma })
    {
        auto const& tokens = windowControlTokens(style);
        CHECK(std::ranges::all_of(tokens.iconNames, &std::string_view::empty));
        CHECK(tokens.restoreIconName.empty());
    }
}

TEST_CASE("WindowControlStyle: an explicit style survives every host", "[windowcontrols]")
{
    // The entire reason the setting exists rather than being inferred: a user who names a style gets
    // it, on a host that would have chosen otherwise.
    for (auto const style: ConcreteStyles)
        for (auto const host: EveryHost)
            CHECK(resolveWindowControlStyle(style, host) == style);
    // Plasma in particular stays Plasma on a KDE session, where Auto now answers Linux.
    CHECK(resolveWindowControlStyle(WindowControlStyle::Plasma, HostPlatform::KdePlasma)
          == WindowControlStyle::Plasma);
}

TEST_CASE("WindowControlStyle: resolving never answers Auto", "[windowcontrols]")
{
    // What lets every caller treat the result as a concrete style with no second check -- including
    // WindowControlStyleProvider, which indexes the token table with it.
    for (auto const& info: configEnumValues<WindowControlStyle>())
        for (auto const host: EveryHost)
            CHECK(resolveWindowControlStyle(info.value, host) != WindowControlStyle::Auto);
}

TEST_CASE("WindowControlStyle: the desktop is read from the environment", "[windowcontrols]")
{
    using contour::config::detectDesktopPlatform;

    SECTION("XDG_CURRENT_DESKTOP names KDE")
    {
        auto const env = FakeEnvironment { { { "XDG_CURRENT_DESKTOP", "KDE" } } };
        CHECK(detectDesktopPlatform(env) == HostPlatform::KdePlasma);
    }

    SECTION("XDG_CURRENT_DESKTOP is a colon-separated list KDE appears in")
    {
        // The form a distribution's own session file produces; matching only the whole string would
        // miss every desktop that prefixes its vendor name.
        auto const env = FakeEnvironment { { { "XDG_CURRENT_DESKTOP", "KDE:Plasma" } } };
        CHECK(detectDesktopPlatform(env) == HostPlatform::KdePlasma);
    }

    SECTION("the match ignores case")
    {
        auto const env = FakeEnvironment { { { "XDG_CURRENT_DESKTOP", "kde" } } };
        CHECK(detectDesktopPlatform(env) == HostPlatform::KdePlasma);
    }

    SECTION("KDE_FULL_SESSION alone is enough")
    {
        // KDE's older marker, and the one that survives an su/sudo shell where the XDG variable is
        // not propagated.
        auto const env = FakeEnvironment { { { "KDE_FULL_SESSION", "true" } } };
        CHECK(detectDesktopPlatform(env) == HostPlatform::KdePlasma);
    }

    SECTION("an empty KDE_FULL_SESSION is not a KDE session")
    {
        auto const env = FakeEnvironment { { { "KDE_FULL_SESSION", "" } } };
        CHECK(detectDesktopPlatform(env) == HostPlatform::Other);
    }

    SECTION("another desktop is not KDE")
    {
        auto const env = FakeEnvironment { { { "XDG_CURRENT_DESKTOP", "ubuntu:GNOME" } } };
        CHECK(detectDesktopPlatform(env) == HostPlatform::Other);
    }

    SECTION("a leaked KDE_FULL_SESSION does not overrule a desktop that named itself")
    {
        // The reason KDE_FULL_SESSION is a fallback and not a second chance: Plasma exports it into
        // every child process, so a GNOME session started from a Plasma terminal -- or reached
        // through `su -` -- still carries it. Reading it as evidence would draw Breeze's circular
        // hover fills and Breeze red on a desktop that explicitly said it was something else.
        auto const env = FakeEnvironment { {
            { "XDG_CURRENT_DESKTOP", "ubuntu:GNOME" },
            { "KDE_FULL_SESSION", "true" },
        } };
        CHECK(detectDesktopPlatform(env) == HostPlatform::Other);
    }

    SECTION("no desktop at all")
    {
        // A bare TTY, a container, a CI runner. It must answer rather than guess.
        auto const env = FakeEnvironment { {} };
        CHECK(detectDesktopPlatform(env) == HostPlatform::Other);
    }
}

TEST_CASE("WindowControlStyle: the two platform conventions actually differ", "[windowcontrols]")
{
    // The bug report this whole setting comes from: macOS windows drew Windows-shaped controls in
    // the top-RIGHT corner. Both halves of that are asserted here, because a table edit that undid
    // either one would leave every other test in this file passing.
    auto const& macos = windowControlTokens(WindowControlStyle::MacOS);
    auto const& windows = windowControlTokens(WindowControlStyle::Windows);

    CHECK(macos.side == WindowControlSide::Leading);
    CHECK(windows.side == WindowControlSide::Trailing);

    CHECK(macos.presentation == WindowControlPresentation::TrafficLight);
    CHECK(windows.presentation == WindowControlPresentation::Button);

    // Not a mirroring of the same order: macOS puts close FIRST at the left, Windows puts it LAST at
    // the right, so reversing one does not produce the other.
    CHECK(macos.order.front() == WindowControlKind::Close);
    CHECK(windows.order.back() == WindowControlKind::Close);

    // Plasma is the case that proves side and shape are independent axes: it sits where Windows
    // does, but rounds its hover fill the way Breeze draws it.
    auto const& plasma = windowControlTokens(WindowControlStyle::Plasma);
    CHECK(plasma.side == WindowControlSide::Trailing);
    CHECK(plasma.hoverCornerPercent > windows.hoverCornerPercent);

    // And it differs from Windows in NOTHING else: revert its stated deltas -- the round fill, Breeze red, no
    // Segoe icon font, the highlight wash -- and the whole row must come back. This is what the derivation
    // buys -- stated as a test so that a later edit adding another difference has to say so here too.
    auto reverted = plasma;
    reverted.hoverCornerPercent = windows.hoverCornerPercent;
    reverted.closeHoverColor = windows.closeHoverColor;
    reverted.iconFamilies = windows.iconFamilies;
    reverted.wash = windows.wash;
    CHECK(reverted == windows);
}

namespace
{
/// A font database that has exactly the families named.
struct InstalledFonts
{
    std::vector<std::string_view> families {};

    [[nodiscard]] bool operator()(std::string_view family) const
    {
        return std::ranges::find(families, family) != families.end();
    }
};
} // namespace

TEST_CASE("the Windows row draws Segoe Fluent glyphs where installed", "[windowcontrols]")
{
    using contour::config::ControlGlyphs;
    using contour::config::controlGlyphsFor;
    auto const& windows = windowControlTokens(WindowControlStyle::Windows);
    auto const iconGlyphs = std::array<std::string_view, 3> { "\uE8BB", "\uE921", "\uE922" };

    CHECK(controlGlyphsFor(windows, InstalledFonts { .families = { "Segoe MDL2 Assets" } })
          == ControlGlyphs { .family = "Segoe MDL2 Assets", .glyphs = iconGlyphs, .restore = "\uE923" });
    CHECK(controlGlyphsFor(windows,
                           InstalledFonts { .families = { "Segoe MDL2 Assets", "Segoe Fluent Icons" } })
              .family
          == "Segoe Fluent Icons");
    CHECK(controlGlyphsFor(windows, InstalledFonts {})
          == ControlGlyphs { .family = {}, .glyphs = { "✕", "—", "▢" }, .restore = "❐" });
}

TEST_CASE("Breeze never borrows the Segoe glyphs", "[windowcontrols]")
{
    auto const everything = [](std::string_view) {
        return true;
    };
    auto const glyphs =
        contour::config::controlGlyphsFor(windowControlTokens(WindowControlStyle::Plasma), everything);
    CHECK(glyphs.family.empty());
    CHECK(glyphs.glyphs == windowControlTokens(WindowControlStyle::Plasma).glyphs);
    // A traffic light names no icon font either.
    CHECK(contour::config::controlGlyphsFor(windowControlTokens(WindowControlStyle::MacOS), everything)
              .family.empty());
}

TEST_CASE("the Terminal row draws dots on every host", "[windowcontrols]")
{
    using contour::config::ChromeStyle;
    using contour::config::presentationFor;
    for (auto const style: ConcreteStyles)
    {
        CAPTURE(static_cast<int>(style));
        CHECK(presentationFor(windowControlTokens(style), ChromeStyle::Terminal)
              == WindowControlPresentation::TrafficLight);
        // Elsewhere each style keeps its own shape.
        CHECK(presentationFor(windowControlTokens(style), ChromeStyle::Fluent)
              == windowControlTokens(style).presentation);
    }
    CHECK(presentationFor(windowControlTokens(WindowControlStyle::Windows), ChromeStyle::Fluent)
          == WindowControlPresentation::Button);

    // Each dot's hue says what it does: close the danger, minimise the warning, maximise the success.
    using contour::config::DotRoles;
    using contour::config::StatusColorRole;
    CHECK(DotRoles[static_cast<size_t>(WindowControlKind::Close)] == StatusColorRole::Danger);
    CHECK(DotRoles[static_cast<size_t>(WindowControlKind::Minimize)] == StatusColorRole::Warning);
    CHECK(DotRoles[static_cast<size_t>(WindowControlKind::Maximize)] == StatusColorRole::Success);
}

TEST_CASE("every row that draws round buttons carries their size and icon and radius", "[windowcontrols]")
{
    // The Settings dropdown offers every style on every row, so a row that can draw a style's shape must also
    // carry that shape's metrics: a zero there is an invisible, unclickable button.
    using contour::config::ChromeStyle;
    using contour::config::ChromeTokenRows;
    using contour::config::presentationFor;
    for (auto const rowIndex: std::views::iota(std::size_t { 0 }, contour::config::detail::ChromeStyleCount))
    {
        auto const row = static_cast<ChromeStyle>(rowIndex);
        auto const& tokens = ChromeTokenRows[rowIndex];
        for (auto const style: ConcreteStyles)
        {
            CAPTURE(rowIndex, static_cast<int>(style));
            if (presentationFor(windowControlTokens(style), row) != WindowControlPresentation::Round)
                continue;
            CHECK(tokens.size.windowButton > 0);
            CHECK(tokens.size.windowButtonGlyph > 0);
            CHECK(tokens.size.windowButtonGlyph <= tokens.size.windowButton);
            CHECK(tokens.radii.windowButton > 0);
        }
    }
}

TEST_CASE("the Windows row washes with the kit fill", "[windowcontrols]")
{
    using contour::config::ControlWash;
    CHECK(windowControlTokens(WindowControlStyle::Windows).wash == ControlWash::KitFill);
    CHECK(windowControlTokens(WindowControlStyle::Plasma).wash == ControlWash::Highlight);
    CHECK(windowControlTokens(WindowControlStyle::MacOS).wash == ControlWash::Highlight);
}
