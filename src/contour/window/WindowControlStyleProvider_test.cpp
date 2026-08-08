// SPDX-License-Identifier: Apache-2.0
//
// Tests for the payload WindowControls.qml iterates. The QML reads nothing but `buttons`, `side`,
// `presentation` and the two hover fields, so what is asserted here is the whole contract between
// the window-control table and the title bar -- a row that arrived in the wrong order, or a color
// that arrived as black instead of as "unset", is a visibly wrong title bar with no other test in
// the tree to catch it.

#include <contour/config/WindowControlStyle.hpp>
#include <contour/window/WindowControlStyleProvider.hpp>

#include <QtCore/QStringList>
#include <QtCore/QVariantMap>

#include <catch2/catch_test_macros.hpp>

#include <QtTest/QSignalSpy>

using contour::config::WindowControlStyle;
using contour::window::WindowControlStyleProvider;

namespace
{

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
    CHECK(actionsOf(WindowControlStyleProvider { WindowControlStyle::Windows })
          == QStringList { "minimize", "maximize", "close" });
    CHECK(actionsOf(WindowControlStyleProvider { WindowControlStyle::MacOS })
          == QStringList { "close", "minimize", "maximize" });
    CHECK(actionsOf(WindowControlStyleProvider { WindowControlStyle::Plasma })
          == QStringList { "minimize", "maximize", "close" });
}

TEST_CASE("WindowControlStyleProvider: side and presentation reach QML by name", "[windowcontrols]")
{
    // These spellings ARE the contract with the QML, which compares against them literally, so they
    // are pinned rather than merely exercised. Names rather than the enumerators' underlying values
    // for the same reason each button's `action` is one: a bare 0/1 in the QML means whatever this
    // header's declaration order happens to say, and reordering the enum would silently invert it.
    auto const windows = WindowControlStyleProvider { WindowControlStyle::Windows };
    CHECK(windows.side() == QStringLiteral("trailing"));
    CHECK(windows.presentation() == QStringLiteral("button"));

    auto const macos = WindowControlStyleProvider { WindowControlStyle::MacOS };
    CHECK(macos.side() == QStringLiteral("leading"));
    CHECK(macos.presentation() == QStringLiteral("trafficLight"));
}

TEST_CASE("WindowControlStyleProvider: every row carries a glyph for both window states", "[windowcontrols]")
{
    for (auto const style:
         { WindowControlStyle::Windows, WindowControlStyle::MacOS, WindowControlStyle::Plasma })
    {
        auto const provider = WindowControlStyleProvider { style };
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
    auto const windows = WindowControlStyleProvider { WindowControlStyle::Windows };

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
    auto const macos = WindowControlStyleProvider { WindowControlStyle::MacOS };

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
    CHECK(WindowControlStyleProvider { WindowControlStyle::Windows }.hoverCornerPercent() == 0);
    CHECK(WindowControlStyleProvider { WindowControlStyle::Plasma }.hoverCornerPercent() == 100);
}

TEST_CASE("WindowControlStyleProvider: a live change notifies exactly once", "[windowcontrols]")
{
    // The whole reason this is a second provider rather than more properties on chromeStyle: the
    // settings page changes it without a restart, which only works if the bindings are told.
    auto provider = WindowControlStyleProvider { WindowControlStyle::Windows };
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
