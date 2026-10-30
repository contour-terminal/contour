// SPDX-License-Identifier: Apache-2.0
#include <contour/config/DesktopButtonLayout.hpp>

#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <string>
#include <string_view>

using contour::config::WindowButtonLayout;
using enum contour::config::WindowControlKind;

namespace
{
/// One parser input and the layout it must give.
struct GtkLayoutRow
{
    std::string_view input;
    WindowButtonLayout expected {};
};

/// One pair of KWin keys and the layout they must give.
struct KWinLayoutRow
{
    contour::config::KWinButtonKeys keys {};
    WindowButtonLayout expected {};
};
} // namespace

TEST_CASE("buttonLayout.gtk", "[buttonlayout]")
{
    using contour::config::parseGtkButtonLayout;
    auto const rows = std::array {
        // GNOME's default.
        GtkLayoutRow { .input = "appmenu:close", .expected = { .leading = {}, .trailing = { Close } } },
        GtkLayoutRow { .input = "appmenu:minimize,maximize,close",
                       .expected = { .leading = {}, .trailing = { Minimize, Maximize, Close } } },
        // macOS-style, at the leading edge (Review Focus 1).
        GtkLayoutRow { .input = "close,minimize,maximize:",
                       .expected = { .leading = { Close, Minimize, Maximize }, .trailing = {} } },
        GtkLayoutRow { .input = "close:minimize,maximize",
                       .expected = { .leading = { Close }, .trailing = { Minimize, Maximize } } },
        // No colon: everything is leading.
        GtkLayoutRow { .input = "minimize,close",
                       .expected = { .leading = { Minimize, Close }, .trailing = {} } },
        // No buttons at all (Review Focus 5).
        GtkLayoutRow { .input = ":", .expected = {} },
        GtkLayoutRow { .input = "", .expected = {} },
        // Names that are not window buttons, and names in the wrong case, are skipped.
        GtkLayoutRow { .input = "icon,menu:spacer,close,bogus,Close",
                       .expected = { .leading = {}, .trailing = { Close } } },
        // A kind is placed once.
        GtkLayoutRow { .input = "close:close,minimize",
                       .expected = { .leading = { Close }, .trailing = { Minimize } } },
        GtkLayoutRow { .input = " minimize , close :",
                       .expected = { .leading = { Minimize, Close }, .trailing = {} } },
        // GTK splits once: "b:close" is one unknown name.
        GtkLayoutRow { .input = "a:b:close", .expected = {} },
    };
    for (auto const& row: rows)
    {
        INFO("layout: \"" << row.input << "\"");
        CHECK(parseGtkButtonLayout(row.input) == row.expected);
    }
}

TEST_CASE("buttonLayout.kwin", "[buttonlayout]")
{
    using contour::config::KWinButtonKeys;
    using contour::config::parseKWinButtonLayout;
    auto const defaults = WindowButtonLayout { .leading = {}, .trailing = { Minimize, Maximize, Close } };
    auto const rows = std::array {
        KWinLayoutRow { .keys = { .left = "MS", .right = "HIAX" }, .expected = defaults },
        KWinLayoutRow { .keys = { .left = "XIA", .right = "" },
                        .expected = { .leading = { Close, Minimize, Maximize }, .trailing = {} } },
        // Both keys missing: KWin's defaults.
        KWinLayoutRow { .keys = {}, .expected = defaults },
        KWinLayoutRow { .keys = { .left = std::nullopt, .right = "X" },
                        .expected = { .leading = {}, .trailing = { Close } } },
        // A kind is placed once, the first time it is named.
        KWinLayoutRow { .keys = { .left = "X", .right = "IAX" },
                        .expected = { .leading = { Close }, .trailing = { Minimize, Maximize } } },
    };
    for (auto const& row: rows)
    {
        INFO("left: " << row.keys.left.value_or("(unset)")
                      << ", right: " << row.keys.right.value_or("(unset)"));
        CHECK(parseKWinButtonLayout(row.keys) == row.expected);
    }
}

TEST_CASE("buttonLayout.ini", "[buttonlayout]")
{
    using contour::config::iniValue;
    using contour::config::kwinButtonKeysOf;
    constexpr auto Text = std::string_view { "# a comment\r\n"
                                             "[General]\r\n"
                                             "ButtonsOnLeft=wrong\r\n"
                                             "[org.kde.kdecoration2]\r\n"
                                             "; ButtonsOnRight=commented\r\n"
                                             "  ButtonsOnLeft  =  MS  \r\n"
                                             "ButtonsOnRight[$i]=X\r\n" };
    CHECK(iniValue(Text, "org.kde.kdecoration2", "ButtonsOnLeft") == "MS");
    CHECK(iniValue(Text, "General", "ButtonsOnLeft") == "wrong");
    CHECK(iniValue(Text, "org.kde.kdecoration2", "ButtonsOnRight") == "X");
    CHECK(iniValue(Text, "org.kde.kdecoration2", "Missing") == std::nullopt);
    CHECK(iniValue(Text, "Absent", "ButtonsOnLeft") == std::nullopt);
    CHECK(iniValue("", "General", "Key") == std::nullopt);
    // An empty value is a value.
    CHECK(iniValue("[G]\nKey=\n", "G", "Key") == "");

    // An excerpt of a Plasma 6 kwinrc.
    constexpr auto Kwinrc = std::string_view { "[Desktops]\n"
                                               "Id_1=0a8f1c0e-4e8b-4f6e-9d0c-3e7c2b1a9f00\n"
                                               "Number=1\n"
                                               "Rows=1\n"
                                               "\n"
                                               "[Tiling]\n"
                                               "padding=4\n"
                                               "\n"
                                               "[org.kde.kdecoration2]\n"
                                               "ButtonsOnLeft=XIA\n"
                                               "ButtonsOnRight=FB\n"
                                               "library=org.kde.breeze\n"
                                               "theme=Breeze\n" };
    CHECK(kwinButtonKeysOf(Kwinrc) == contour::config::KWinButtonKeys { .left = "XIA", .right = "FB" });
    CHECK(kwinButtonKeysOf("") == contour::config::KWinButtonKeys {});
}

TEST_CASE("buttonLayout.files", "[buttonlayout]")
{
    using contour::config::DesktopConfigFiles;
    using contour::config::fileButtonLayoutSources;
    using contour::config::HostPlatform;
    using contour::config::KWinButtonKeys;
    auto fileSystem = core::platform::testing::InMemoryFileSystem {};
    auto const home = std::filesystem::path { "/cfg" };
    auto const sourcesOn = [&](HostPlatform desktop) {
        return fileButtonLayoutSources(
            DesktopConfigFiles { .fileSystem = fileSystem, .configHome = home, .desktop = desktop });
    };

    SECTION("KDE with a kwinrc reads its keys")
    {
        fileSystem.addFile(home / "kwinrc", "[org.kde.kdecoration2]\nButtonsOnLeft=X\n");
        auto const sources = sourcesOn(HostPlatform::KdePlasma);
        CHECK(sources.kwin == KWinButtonKeys { .left = "X", .right = std::nullopt });
        CHECK(!sources.gnomeButtonLayout);
        CHECK(!sources.gtkDecorationLayout);
    }
    SECTION("KDE without a kwinrc takes KWin's defaults")
    {
        CHECK(sourcesOn(HostPlatform::KdePlasma).kwin == KWinButtonKeys {});
    }
    SECTION("another desktop ignores a kwinrc")
    {
        fileSystem.addFile(home / "kwinrc", "[org.kde.kdecoration2]\nButtonsOnLeft=X\n");
        CHECK(!sourcesOn(HostPlatform::Other).kwin);
    }
    SECTION("GTK 4's settings.ini wins over GTK 3's")
    {
        fileSystem.addFile(home / "gtk-4.0/settings.ini", "[Settings]\ngtk-decoration-layout=close:\n");
        fileSystem.addFile(home / "gtk-3.0/settings.ini", "[Settings]\ngtk-decoration-layout=:close\n");
        CHECK(sourcesOn(HostPlatform::Other).gtkDecorationLayout == "close:");
    }
    SECTION("GTK 3's settings.ini alone is read")
    {
        fileSystem.addFile(home / "gtk-3.0/settings.ini", "[Settings]\ngtk-decoration-layout=:close\n");
        CHECK(sourcesOn(HostPlatform::Other).gtkDecorationLayout == ":close");
    }
    SECTION("a GTK 4 file without the key falls through to GTK 3's")
    {
        fileSystem.addFile(home / "gtk-4.0/settings.ini", "[Settings]\ngtk-theme-name=Adwaita\n");
        fileSystem.addFile(home / "gtk-3.0/settings.ini", "[Settings]\ngtk-decoration-layout=:close\n");
        CHECK(sourcesOn(HostPlatform::Other).gtkDecorationLayout == ":close");
    }
    SECTION("no config home reads nothing")
    {
        fileSystem.addFile(home / "kwinrc", "[org.kde.kdecoration2]\nButtonsOnLeft=X\n");
        fileSystem.addFile(home / "gtk-3.0/settings.ini", "[Settings]\ngtk-decoration-layout=:close\n");
        auto const sources = fileButtonLayoutSources(DesktopConfigFiles {
            .fileSystem = fileSystem, .configHome = std::nullopt, .desktop = HostPlatform::KdePlasma });
        CHECK(sources == contour::config::ButtonLayoutSources {});
    }
}

TEST_CASE("buttonLayout.chain", "[buttonlayout]")
{
    using contour::config::ButtonLayoutSources;
    using contour::config::fallbackButtonLayout;
    using contour::config::HostPlatform;
    using contour::config::KWinButtonKeys;
    using contour::config::resolveDesktopButtonLayout;
    auto const gnome = std::optional<std::string> { "close,minimize,maximize:" };
    auto const kwin = std::optional<KWinButtonKeys> { KWinButtonKeys { .left = "", .right = "X" } };
    auto const gtk = std::optional<std::string> { "minimize:" };
    auto const gnomeLayout = WindowButtonLayout { .leading = { Close, Minimize, Maximize }, .trailing = {} };
    auto const kwinLayout = WindowButtonLayout { .leading = {}, .trailing = { Close } };
    auto const gtkLayout = WindowButtonLayout { .leading = { Minimize }, .trailing = {} };

    // GNOME's key beats KWin's and GTK's away from KDE.
    CHECK(resolveDesktopButtonLayout(
              ButtonLayoutSources { .gnomeButtonLayout = gnome, .kwin = kwin, .gtkDecorationLayout = gtk },
              HostPlatform::Other)
          == gnomeLayout);
    // On KDE, KWin's keys beat a GNOME key the portal's GTK backend answers with its defaults (F12).
    CHECK(resolveDesktopButtonLayout(
              ButtonLayoutSources { .gnomeButtonLayout = gnome, .kwin = kwin, .gtkDecorationLayout = gtk },
              HostPlatform::KdePlasma)
          == kwinLayout);
    // KWin beats GTK.
    CHECK(resolveDesktopButtonLayout(
              ButtonLayoutSources { .gnomeButtonLayout = {}, .kwin = kwin, .gtkDecorationLayout = gtk },
              HostPlatform::Other)
          == kwinLayout);
    // GTK alone is used.
    CHECK(resolveDesktopButtonLayout(
              ButtonLayoutSources { .gnomeButtonLayout = {}, .kwin = {}, .gtkDecorationLayout = gtk },
              HostPlatform::KdePlasma)
          == gtkLayout);
    // Nothing: minimize, maximize and close on the right (Review Focus 2, ruling D3).
    CHECK(fallbackButtonLayout()
          == WindowButtonLayout { .leading = {}, .trailing = { Minimize, Maximize, Close } });
    for (auto const desktop: { HostPlatform::Other, HostPlatform::KdePlasma })
        CHECK(resolveDesktopButtonLayout(ButtonLayoutSources {}, desktop) == fallbackButtonLayout());
}

TEST_CASE("buttonLayout.placement", "[buttonlayout]")
{
    using contour::config::fallbackButtonLayout;
    using contour::config::windowButtonPlacement;
    using contour::config::WindowControlStyle;
    auto const leading = WindowButtonLayout { .leading = { Close, Minimize, Maximize }, .trailing = {} };
    auto const trailing = WindowButtonLayout { .leading = {}, .trailing = { Minimize, Maximize, Close } };

    // Linux takes the desktop's layout, and the fallback where the desktop states none.
    CHECK(windowButtonPlacement(WindowControlStyle::Linux, leading) == leading);
    CHECK(windowButtonPlacement(WindowControlStyle::Linux, std::nullopt) == fallbackButtonLayout());
    // Every other style keeps its own order on its own side, whatever the desktop says.
    for (auto const& desktop: { std::optional<WindowButtonLayout> {}, std::optional { leading } })
    {
        CHECK(windowButtonPlacement(WindowControlStyle::MacOS, desktop) == leading);
        CHECK(windowButtonPlacement(WindowControlStyle::Windows, desktop) == trailing);
        CHECK(windowButtonPlacement(WindowControlStyle::Plasma, desktop) == trailing);
    }
    CHECK(windowButtonPlacement(WindowControlStyle::Windows, std::optional { trailing }) == trailing);
}
