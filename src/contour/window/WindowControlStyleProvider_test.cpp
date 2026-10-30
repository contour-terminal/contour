// SPDX-License-Identifier: Apache-2.0
//
// Tests for the payload WindowControls.qml iterates. The QML reads nothing but `leadingButtons`,
// `trailingButtons`, `presentation` and the two hover fields, so what is asserted here is the whole contract
// between the window-control table and the title bar -- a row that arrived in the wrong order, or a color
// that arrived as black instead of as "unset", is a visibly wrong title bar with no other test in
// the tree to catch it.

#include <contour/config/DesktopButtonLayout.hpp>
#include <contour/config/WindowControlStyle.hpp>
#include <contour/window/UiStyleProvider.hpp>
#include <contour/window/WindowControlStyleProvider.hpp>

#include <QtCore/QStringList>
#include <QtCore/QVariantMap>
#include <QtGui/QGuiApplication>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <optional>
#include <string_view>
#include <utility>

#include <QtTest/QSignalSpy>

using contour::config::ChromeStyle;
using contour::config::StatusColorRole;
using contour::config::WindowButtonLayout;
using contour::config::windowButtonPlacement;
using contour::config::WindowControlStyle;
using contour::platform::ContrastTheme;
using enum contour::config::WindowControlKind;
using contour::window::WindowControlStyleProvider;

namespace
{

/// @return Whether a font family is installed, as on a machine with no Segoe font.
[[nodiscard]] bool noFonts(std::string_view /*family*/)
{
    return false;
}

/// @return A provider for @p style on @p row, placed by @p desktop, with @p isInstalled for its icon font.
[[nodiscard]] WindowControlStyleProvider providerFor(
    WindowControlStyle style,
    ChromeStyle row = ChromeStyle::ContourLinux,
    std::function<bool(std::string_view)> isInstalled = noFonts,
    std::optional<WindowButtonLayout> const& desktop = std::nullopt)
{
    return WindowControlStyleProvider {
        style, windowButtonPlacement(style, desktop), row, ContrastTheme::None, std::move(isInstalled)
    };
}

/// The `action` of each row of @p buttons, in draw order -- i.e. exactly what the Repeater walks.
[[nodiscard]] QStringList actionsOf(QVariantList const& buttons)
{
    auto actions = QStringList {};
    for (auto const& row: buttons)
        actions.push_back(row.toMap().value(QStringLiteral("action")).toString());
    return actions;
}

/// Both sides' rows, leading first.
[[nodiscard]] QVariantList allButtons(WindowControlStyleProvider const& provider)
{
    return provider.leadingButtons() + provider.trailingButtons();
}

/// The `action` of every button, leading side first.
[[nodiscard]] QStringList actionsOf(WindowControlStyleProvider const& provider)
{
    return actionsOf(allButtons(provider));
}

/// One button's row, by action.
[[nodiscard]] QVariantMap rowFor(WindowControlStyleProvider const& provider, QString const& action)
{
    for (auto const& row: allButtons(provider))
        if (auto const map = row.toMap(); map.value(QStringLiteral("action")).toString() == action)
            return map;
    return {};
}

} // namespace

TEST_CASE("WindowControlStyleProvider: each style draws its own order", "[windowcontrols]")
{
    // The three orders are the visible difference between the styles, and getting one wrong puts
    // the close button where the user's muscle memory has minimize.
    CHECK(actionsOf(providerFor(WindowControlStyle::Windows))
          == QStringList { "minimize", "maximize", "close" });
    CHECK(actionsOf(providerFor(WindowControlStyle::MacOS))
          == QStringList { "close", "minimize", "maximize" });
    CHECK(actionsOf(providerFor(WindowControlStyle::Plasma))
          == QStringList { "minimize", "maximize", "close" });
}

TEST_CASE("WindowControlStyleProvider: side and presentation reach QML by name", "[windowcontrols]")
{
    // These spellings ARE the contract with the QML, which compares against them literally, so they
    // are pinned rather than merely exercised. Names rather than the enumerators' underlying values
    // for the same reason each button's `action` is one: a bare 0/1 in the QML means whatever this
    // header's declaration order happens to say, and reordering the enum would silently invert it.
    auto const windows = providerFor(WindowControlStyle::Windows);
    CHECK(windows.leadingButtons().isEmpty());
    CHECK(windows.trailingButtons().size() == 3);
    CHECK(windows.presentation() == QStringLiteral("button"));

    auto const macos = providerFor(WindowControlStyle::MacOS);
    CHECK(macos.leadingButtons().size() == 3);
    CHECK(macos.trailingButtons().isEmpty());
    CHECK(macos.presentation() == QStringLiteral("trafficLight"));

    CHECK(providerFor(WindowControlStyle::Linux).presentation() == QStringLiteral("round"));
}

TEST_CASE("WindowControlStyleProvider: each side lists the placement's buttons", "[windowcontrols]")
{
    using contour::config::fallbackButtonLayout;
    auto const leftHanded = WindowButtonLayout { .leading = { Close, Minimize, Maximize }, .trailing = {} };
    auto const linuxProvider =
        providerFor(WindowControlStyle::Linux, ChromeStyle::ContourLinux, noFonts, leftHanded);
    // Review Focus 1: the leading list is the desktop's own order, close first.
    CHECK(actionsOf(linuxProvider.leadingButtons()) == QStringList { "close", "minimize", "maximize" });
    CHECK(linuxProvider.trailingButtons().isEmpty());

    auto const split = WindowButtonLayout { .leading = { Close }, .trailing = { Minimize, Maximize } };
    auto const both = providerFor(WindowControlStyle::Linux, ChromeStyle::ContourLinux, noFonts, split);
    CHECK(actionsOf(both.leadingButtons()) == QStringList { "close" });
    CHECK(actionsOf(both.trailingButtons()) == QStringList { "minimize", "maximize" });

    auto const fallback = providerFor(WindowControlStyle::Linux);
    CHECK(fallback.leadingButtons().isEmpty());
    CHECK(actionsOf(fallback.trailingButtons()) == (QStringList { "minimize", "maximize", "close" }));
    CHECK(windowButtonPlacement(WindowControlStyle::Linux, std::nullopt) == fallbackButtonLayout());

    // The other styles ignore the desktop's layout.
    auto const macos = providerFor(WindowControlStyle::MacOS, ChromeStyle::ContourLinux, noFonts, split);
    CHECK(actionsOf(macos.leadingButtons()) == QStringList { "close", "minimize", "maximize" });
    CHECK(macos.trailingButtons().isEmpty());
    for (auto const style: { WindowControlStyle::Windows, WindowControlStyle::Plasma })
    {
        auto const provider = providerFor(style, ChromeStyle::ContourLinux, noFonts, split);
        CHECK(provider.leadingButtons().isEmpty());
        CHECK(actionsOf(provider.trailingButtons()) == QStringList { "minimize", "maximize", "close" });
    }

    // A desktop that wants no buttons gets none.
    auto const none =
        providerFor(WindowControlStyle::Linux, ChromeStyle::ContourLinux, noFonts, WindowButtonLayout {});
    CHECK(allButtons(none).isEmpty());
}

TEST_CASE("WindowControlStyleProvider: a window that picks its own controls gets them wherever the desktop "
          "puts them",
          "[windowcontrols]")
{
    // The Settings window draws close even where the desktop's layout names none (settingsWindowControls).
    auto const none =
        providerFor(WindowControlStyle::Linux, ChromeStyle::ContourLinux, noFonts, WindowButtonLayout {});
    CHECK(actionsOf(none.buttonsFor(QVariantList { static_cast<int>(Close) })) == QStringList { "close" });
    auto const split = WindowButtonLayout { .leading = { Minimize }, .trailing = { Maximize } };
    auto const provider = providerFor(WindowControlStyle::Linux, ChromeStyle::ContourLinux, noFonts, split);
    CHECK(actionsOf(provider.buttonsFor(QVariantList { static_cast<int>(Maximize), static_cast<int>(Close) }))
          == (QStringList { "maximize", "close" }));
    // Rows as the sides' rows are.
    CHECK(provider.buttonsFor(QVariantList { static_cast<int>(Maximize) }) == provider.trailingButtons());
    // A value that names no control is skipped.
    CHECK(provider.buttonsFor(QVariantList { -1, 3, QStringLiteral("close") }).isEmpty());
}

TEST_CASE("WindowControlStyleProvider: the Linux rows name their themed icons", "[windowcontrols]")
{
    auto const provider = providerFor(WindowControlStyle::Linux);
    auto const close = rowFor(provider, QStringLiteral("close"));
    CHECK(close.value(QStringLiteral("iconName")).toString() == QStringLiteral("window-close-symbolic"));
    CHECK(close.value(QStringLiteral("restoreIconName")).toString()
          == QStringLiteral("window-close-symbolic"));
    auto const maximize = rowFor(provider, QStringLiteral("maximize"));
    CHECK(maximize.value(QStringLiteral("iconName")).toString()
          == QStringLiteral("window-maximize-symbolic"));
    CHECK(maximize.value(QStringLiteral("restoreIconName")).toString()
          == QStringLiteral("window-restore-symbolic"));
    CHECK(rowFor(provider, QStringLiteral("minimize")).value(QStringLiteral("iconName")).toString()
          == QStringLiteral("window-minimize-symbolic"));
    // Each icon has its bundled copy, for a theme that lacks it.
    CHECK(maximize.value(QStringLiteral("bundledIcon")).toString()
          == QStringLiteral("qrc:/contour/icons/window-maximize-symbolic.svg"));
    CHECK(maximize.value(QStringLiteral("restoreBundledIcon")).toString()
          == QStringLiteral("qrc:/contour/icons/window-restore-symbolic.svg"));
    // A glyph style names no icon.
    CHECK(rowFor(providerFor(WindowControlStyle::Windows), QStringLiteral("close"))
              .value(QStringLiteral("iconName"))
              .toString()
              .isEmpty());
    // The Terminal row draws the Linux style's controls as dot cells too.
    CHECK(providerFor(WindowControlStyle::Linux, ChromeStyle::Terminal).presentation()
          == QStringLiteral("trafficLight"));
}

TEST_CASE("WindowControlStyleProvider: every row carries a glyph for both window states", "[windowcontrols]")
{
    for (auto const style: { WindowControlStyle::Windows,
                             WindowControlStyle::MacOS,
                             WindowControlStyle::Plasma,
                             WindowControlStyle::Linux })
    {
        auto const provider = providerFor(style);
        for (auto const& row: allButtons(provider))
        {
            auto const map = row.toMap();
            // The delegate reads restoreGlyph unconditionally, so a row that left it empty would
            // blank that control the moment the window was maximized.
            CHECK(!map.value(QStringLiteral("glyph")).toString().isEmpty());
            CHECK(!map.value(QStringLiteral("restoreGlyph")).toString().isEmpty());
        }

        // Only maximize has two faces; for everything else the two are the same string, which is
        // what lets the delegate read one field without asking which control it is drawing.
        auto const maximize = rowFor(provider, QStringLiteral("maximize"));
        CHECK(maximize.value(QStringLiteral("glyph")).toString()
              != maximize.value(QStringLiteral("restoreGlyph")).toString());

        auto const close = rowFor(provider, QStringLiteral("close"));
        CHECK(close.value(QStringLiteral("glyph")).toString()
              == close.value(QStringLiteral("restoreGlyph")).toString());
    }
}

TEST_CASE("WindowControlStyleProvider: an unset color is empty, not black", "[windowcontrols]")
{
    // The reason these cross as strings at all: QML's color value type has no validity to test, so
    // an unset QColor would arrive indistinguishable from black -- a black hover fill on every
    // button, and a black dot where the style meant "use the chrome's own wash".
    auto const windows = providerFor(WindowControlStyle::Windows);

    CHECK(rowFor(windows, QStringLiteral("close")).value(QStringLiteral("hoverFill")).toString()
          == QStringLiteral("#c42b1c"));
    // Only close is tinted; the others defer to the chrome's hover wash.
    CHECK(
        rowFor(windows, QStringLiteral("minimize")).value(QStringLiteral("hoverFill")).toString().isEmpty());
    CHECK(
        rowFor(windows, QStringLiteral("maximize")).value(QStringLiteral("hoverFill")).toString().isEmpty());
    // A button style draws no dots at all.
    for (auto const& row: allButtons(windows))
        CHECK(row.toMap().value(QStringLiteral("dotColor")).toString().isEmpty());
    CHECK(windows.inactiveDotColor().isEmpty());
}

TEST_CASE("WindowControlStyleProvider: a contrast theme drops the red close hover", "[windowcontrols]")
{
    // Under a contrast theme the OS palette owns every colour, so the close button washes like its
    // neighbours instead of insisting on #C42B1C.
    auto const contrast = WindowControlStyleProvider { WindowControlStyle::Windows,
                                                       windowButtonPlacement(WindowControlStyle::Windows, {}),
                                                       ChromeStyle::Fluent,
                                                       ContrastTheme::Active,
                                                       noFonts };
    for (auto const& row: allButtons(contrast))
        CHECK(row.toMap().value(QStringLiteral("hoverFill")).toString().isEmpty());

    // Live: the theme comes and goes while the window is open.
    auto provider = providerFor(WindowControlStyle::Windows, ChromeStyle::Fluent);
    auto changes = QSignalSpy { &provider, &WindowControlStyleProvider::changed };
    provider.setContrastTheme(ContrastTheme::Active);
    CHECK(changes.count() == 1);
    CHECK(rowFor(provider, QStringLiteral("close")).value(QStringLiteral("hoverFill")).toString().isEmpty());
    provider.setContrastTheme(ContrastTheme::Active);
    CHECK(changes.count() == 1);
    provider.setContrastTheme(ContrastTheme::None);
    CHECK(changes.count() == 2);
    CHECK(rowFor(provider, QStringLiteral("close")).value(QStringLiteral("hoverFill")).toString()
          == QStringLiteral("#c42b1c"));
}

TEST_CASE("WindowControlStyleProvider: a traffic light states every dot's color", "[windowcontrols]")
{
    auto const macos = providerFor(WindowControlStyle::MacOS);

    CHECK(rowFor(macos, QStringLiteral("close")).value(QStringLiteral("dotColor")).toString()
          == QStringLiteral("#ff5f57"));
    CHECK(rowFor(macos, QStringLiteral("minimize")).value(QStringLiteral("dotColor")).toString()
          == QStringLiteral("#febc2e"));
    CHECK(rowFor(macos, QStringLiteral("maximize")).value(QStringLiteral("dotColor")).toString()
          == QStringLiteral("#28c840"));
    CHECK(!macos.inactiveDotColor().isEmpty());

    // No hover fill anywhere: a dot's own color IS the affordance, and a wash behind it would only
    // muddy it. This is the assertion that catches the dotColors/closeHoverColor columns being
    // filled in for the wrong presentation.
    for (auto const& row: allButtons(macos))
        CHECK(row.toMap().value(QStringLiteral("hoverFill")).toString().isEmpty());
}

TEST_CASE("WindowControlStyleProvider: Breeze rounds its hover fill", "[windowcontrols]")
{
    // The axis that proves shape and side are independent: Plasma sits where Windows does and
    // differs only in how its hover fill is drawn.
    CHECK(providerFor(WindowControlStyle::Windows).hoverCornerPercent() == 0);
    CHECK(providerFor(WindowControlStyle::Plasma).hoverCornerPercent() == 100);
}

TEST_CASE("WindowControlStyleProvider: a live change notifies exactly once", "[windowcontrols]")
{
    // The whole reason this is a second provider rather than more properties on chromeStyle: the
    // settings page changes it without a restart, which only works if the bindings are told.
    auto provider = providerFor(WindowControlStyle::Windows);
    auto changes = QSignalSpy { &provider, &WindowControlStyleProvider::changed };

    auto const macosPlacement = windowButtonPlacement(WindowControlStyle::MacOS, std::nullopt);
    provider.update(WindowControlStyle::MacOS, macosPlacement);
    CHECK(changes.count() == 1);
    CHECK(provider.trailingButtons().isEmpty());
    CHECK(actionsOf(provider.leadingButtons()) == QStringList { "close", "minimize", "maximize" });

    // Re-applying the same pair is what a reload does when the user changed something ELSE, and
    // every reload re-running every window-control binding would be churn for nothing.
    provider.update(WindowControlStyle::MacOS, macosPlacement);
    CHECK(changes.count() == 1);

    provider.update(WindowControlStyle::Windows,
                    windowButtonPlacement(WindowControlStyle::Windows, std::nullopt));
    CHECK(changes.count() == 2);
    CHECK(provider.leadingButtons().isEmpty());

    // The desktop moving its buttons is a change too, with the style unchanged.
    auto linuxProvider = providerFor(WindowControlStyle::Linux);
    auto linuxChanges = QSignalSpy { &linuxProvider, &WindowControlStyleProvider::changed };
    auto const leftHanded = WindowButtonLayout { .leading = { Close, Minimize, Maximize }, .trailing = {} };
    linuxProvider.update(WindowControlStyle::Linux, leftHanded);
    CHECK(linuxChanges.count() == 1);
    CHECK(actionsOf(linuxProvider.leadingButtons()) == QStringList { "close", "minimize", "maximize" });
    linuxProvider.update(WindowControlStyle::Linux, leftHanded);
    CHECK(linuxChanges.count() == 1);
}

TEST_CASE("WindowControlStyleProvider: the Terminal row draws scheme-inked dots", "[windowcontrols]")
{
    auto const provider = providerFor(WindowControlStyle::Windows, ChromeStyle::Terminal);
    CHECK(provider.presentation() == QStringLiteral("trafficLight"));
    // Windows' place and order, trailing: minimise, maximise, close.
    CHECK(provider.leadingButtons().isEmpty());
    CHECK(actionsOf(provider.trailingButtons()) == QStringList { "minimize", "maximize", "close" });

    auto const close = rowFor(provider, QStringLiteral("close"));
    CHECK(close.value(QStringLiteral("hasDot")).toBool());
    CHECK(close.value(QStringLiteral("dotRole")).toInt() == static_cast<int>(StatusColorRole::Danger));
    CHECK(rowFor(provider, QStringLiteral("minimize")).value(QStringLiteral("dotRole")).toInt()
          == static_cast<int>(StatusColorRole::Warning));
    CHECK(rowFor(provider, QStringLiteral("maximize")).value(QStringLiteral("dotRole")).toInt()
          == static_cast<int>(StatusColorRole::Success));
}

TEST_CASE("WindowControlStyleProvider: the Fluent row draws Segoe glyphs with the kit fill",
          "[windowcontrols]")
{
    auto const segoeFluent = [](std::string_view family) {
        return family == "Segoe Fluent Icons";
    };
    auto const provider = providerFor(WindowControlStyle::Windows, ChromeStyle::Fluent, segoeFluent);
    CHECK(provider.presentation() == QStringLiteral("button"));

    auto const close = rowFor(provider, QStringLiteral("close"));
    CHECK(close.value(QStringLiteral("font")).toString() == QStringLiteral("Segoe Fluent Icons"));
    CHECK(close.value(QStringLiteral("glyph")).toString() == QString::fromUtf8("\uE8BB"));
    CHECK(rowFor(provider, QStringLiteral("maximize")).value(QStringLiteral("restoreGlyph")).toString()
          == QString::fromUtf8("\uE923"));
    CHECK(close.value(QStringLiteral("wash")).toString() == QStringLiteral("kitFill"));
    CHECK_FALSE(close.value(QStringLiteral("hasDot")).toBool());

    // Without the font, the Unicode glyphs in the chrome font.
    auto const bare =
        rowFor(providerFor(WindowControlStyle::Windows, ChromeStyle::Fluent), QStringLiteral("close"));
    CHECK(bare.value(QStringLiteral("font")).toString().isEmpty());
    CHECK(bare.value(QStringLiteral("glyph")).toString() == QString::fromUtf8("✕"));

    CHECK(rowFor(providerFor(WindowControlStyle::Plasma), QStringLiteral("close"))
              .value(QStringLiteral("wash"))
              .toString()
          == QStringLiteral("highlight"));
}

#ifdef _WIN32
TEST_CASE("WindowControlStyleProvider: a Windows host draws its caption in a Segoe icon font",
          "[native][win32][caption]")
{
    if (QGuiApplication::platformName() != QStringLiteral("windows"))
        SKIP("Requires the native Windows platform.");
    // The real font database: Windows 11 ships Segoe Fluent Icons, Windows 10 (and a server SKU without it)
    // Segoe MDL2 Assets.
    auto const provider = providerFor(
        WindowControlStyle::Windows, ChromeStyle::Fluent, contour::window::fontDatabaseAvailability());
    auto const font = rowFor(provider, QStringLiteral("close")).value(QStringLiteral("font")).toString();
    INFO("icon font: " << font.toStdString());
    CHECK((font == QStringLiteral("Segoe Fluent Icons") || font == QStringLiteral("Segoe MDL2 Assets")));
}
#endif
