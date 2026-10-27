// SPDX-License-Identifier: Apache-2.0
//
// Tests for the payload WindowControls.qml iterates. The QML reads nothing but `buttons`, `side`,
// `presentation` and the two hover fields, so what is asserted here is the whole contract between
// the window-control table and the title bar -- a row that arrived in the wrong order, or a color
// that arrived as black instead of as "unset", is a visibly wrong title bar with no other test in
// the tree to catch it.

#include <contour/config/WindowControlStyle.hpp>
#include <contour/window/UiStyleProvider.hpp>
#include <contour/window/WindowControlStyleProvider.hpp>

#include <QtCore/QStringList>
#include <QtCore/QVariantMap>
#include <QtGui/QGuiApplication>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <string_view>
#include <utility>

#include <QtTest/QSignalSpy>

using contour::config::ChromeStyle;
using contour::config::StatusColorRole;
using contour::config::WindowControlStyle;
using contour::window::WindowControlStyleProvider;

namespace
{

/// @return Whether a font family is installed, as on a machine with no Segoe font.
[[nodiscard]] bool noFonts(std::string_view /*family*/)
{
    return false;
}

/// @return A provider for @p style on @p row, with @p isInstalled for its icon font.
[[nodiscard]] WindowControlStyleProvider providerFor(
    WindowControlStyle style,
    ChromeStyle row = ChromeStyle::ContourLinux,
    std::function<bool(std::string_view)> isInstalled = noFonts)
{
    return WindowControlStyleProvider { style, row, std::move(isInstalled) };
}

/// The `action` of each button, in draw order -- i.e. exactly what the Repeater walks.
[[nodiscard]] QStringList actionsOf(WindowControlStyleProvider const& provider)
{
    auto actions = QStringList {};
    for (auto const& row: provider.buttons())
        actions.push_back(row.toMap().value(QStringLiteral("action")).toString());
    return actions;
}

/// One button's row, by action.
[[nodiscard]] QVariantMap rowFor(WindowControlStyleProvider const& provider, QString const& action)
{
    for (auto const& row: provider.buttons())
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
    CHECK(windows.side() == QStringLiteral("trailing"));
    CHECK(windows.presentation() == QStringLiteral("button"));

    auto const macos = providerFor(WindowControlStyle::MacOS);
    CHECK(macos.side() == QStringLiteral("leading"));
    CHECK(macos.presentation() == QStringLiteral("trafficLight"));
}

TEST_CASE("WindowControlStyleProvider: every row carries a glyph for both window states", "[windowcontrols]")
{
    for (auto const style:
         { WindowControlStyle::Windows, WindowControlStyle::MacOS, WindowControlStyle::Plasma })
    {
        auto const provider = providerFor(style);
        for (auto const& row: provider.buttons())
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
    for (auto const& row: windows.buttons())
        CHECK(row.toMap().value(QStringLiteral("dotColor")).toString().isEmpty());
    CHECK(windows.inactiveDotColor().isEmpty());
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
    for (auto const& row: macos.buttons())
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

    provider.setStyle(WindowControlStyle::MacOS);
    CHECK(changes.count() == 1);
    CHECK(provider.side() == QStringLiteral("leading"));
    CHECK(actionsOf(provider) == QStringList { "close", "minimize", "maximize" });

    // Re-applying the same style is what a reload does when the user changed something ELSE, and
    // every reload re-running every window-control binding would be churn for nothing.
    provider.setStyle(WindowControlStyle::MacOS);
    CHECK(changes.count() == 1);

    provider.setStyle(WindowControlStyle::Windows);
    CHECK(changes.count() == 2);
    CHECK(provider.side() == QStringLiteral("trailing"));
}

TEST_CASE("WindowControlStyleProvider: the Terminal row draws scheme-inked dots", "[windowcontrols]")
{
    auto const provider = providerFor(WindowControlStyle::Windows, ChromeStyle::Terminal);
    CHECK(provider.presentation() == QStringLiteral("trafficLight"));
    // Windows' place and order, trailing: minimise, maximise, close.
    CHECK(provider.side() == QStringLiteral("trailing"));
    CHECK(actionsOf(provider) == QStringList { "minimize", "maximize", "close" });

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
