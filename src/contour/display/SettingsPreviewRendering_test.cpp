// SPDX-License-Identifier: Apache-2.0
//
// Display-gated pixel test for the Settings window's preview hero: a REAL TerminalDisplay bound to a REAL
// preview session (the one SettingsPreview makes, over the canned transcript) is driven through the RHI
// loop, and what the case measures is the grabbed frame. The chrome snapshots stub the terminal, so a hero
// that rendered blank, or in colours the scheme never gave it, would only be found here.
//
// Like every [display] case this SKIPs unless CONTOUR_TEST_DISPLAY=1 opts into a real windowing system
// (scripts/run-display-tests.sh runs them under a headless weston).

#include <contour/config/ConfigFwd.hpp>
#include <contour/display/TerminalDisplay.hpp>
#include <contour/session/PreviewTranscript.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/test/TestEnvironment.hpp>

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/screen/Terminal.hpp>

#include <core/Environment.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QRect>
#include <QtGui/QColor>
#include <QtGui/QImage>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

using namespace std::string_view_literals;

namespace
{

/// The bundled colour schemes the case loops: a dark one and two light ones.
constexpr auto SchemeNames = std::array { "contour-dark"sv, "solarized-light"sv, "gruvbox-light"sv };

/// The transcript's rows: the prompt and command, the changed file, the prompt again.
constexpr auto CommandRow = 0;
constexpr auto OutputRow = 1;

/// The columns of `git status --short` after the prompt "~/repo/contour ❯ " (17 cells), and of the `M`.
constexpr auto CommandFirstColumn = 17;
constexpr auto CommandLastColumn = 34;
constexpr auto ChangedMarkColumn = 1;

/// How much of the contrast a colour pair has survives in the pixels, where an edge is anti-aliased.
constexpr auto PixelContrastShare = 0.95;

/// What the pixels of a region are made of: the colour most of them have (the ground the ink sits on), and
/// the colour farthest from it (the ink at its strongest, where a stem fills a whole pixel).
struct RegionColors
{
    vtbackend::RGBColor ground {}; ///< The most common colour.
    vtbackend::RGBColor ink {};    ///< The colour farthest from the ground.
    int inkPixels = 0;             ///< How many pixels are not the ground's.
};

[[nodiscard]] vtbackend::RGBColor rgbOf(QRgb pixel) noexcept
{
    return vtbackend::RGBColor { static_cast<uint8_t>(qRed(pixel)),
                                 static_cast<uint8_t>(qGreen(pixel)),
                                 static_cast<uint8_t>(qBlue(pixel)) };
}

[[nodiscard]] int distanceBetween(vtbackend::RGBColor a, vtbackend::RGBColor b) noexcept
{
    return std::abs(int(a.red) - int(b.red)) + std::abs(int(a.green) - int(b.green))
           + std::abs(int(a.blue) - int(b.blue));
}

[[nodiscard]] RegionColors regionColors(QImage const& frame, QRect const& region)
{
    auto const area = region.intersected(frame.rect());
    auto counts = std::map<QRgb, int> {};
    for (auto const y: std::views::iota(area.top(), area.bottom() + 1))
        for (auto const x: std::views::iota(area.left(), area.right() + 1))
            ++counts[frame.pixel(x, y)];
    REQUIRE_FALSE(counts.empty());

    auto const common = std::ranges::max_element(counts, {}, &std::pair<QRgb const, int>::second);
    auto result =
        RegionColors { .ground = rgbOf(common->first), .ink = rgbOf(common->first), .inkPixels = 0 };
    auto farthest = 0;
    for (auto const& [pixel, count]: counts)
    {
        if (pixel == common->first)
            continue;
        result.inkPixels += count;
        if (auto const distance = distanceBetween(rgbOf(pixel), result.ground); distance > farthest)
        {
            farthest = distance;
            result.ink = rgbOf(pixel);
        }
    }
    return result;
}

[[nodiscard]] std::string hexOf(vtbackend::RGBColor color)
{
    return std::format("#{:02x}{:02x}{:02x}", color.red, color.green, color.blue);
}

} // namespace

TEST_CASE("display: the Settings preview shows its transcript in the scheme's colours",
          "[contour][gui][display][settingspreview]")
{
    if (!contour::test::displayTestsEnabled(core::LiveEnvironment {}))
    {
        WARN("Skipping display test: set CONTOUR_TEST_DISPLAY=1 with a real display");
        return;
    }

    for (auto const scheme: SchemeNames)
    {
        INFO("scheme " << std::string { scheme });
        auto app = contour::test::TestApp {};
        auto config = app.app().config();
        auto colors = contour::config::SimpleColorConfig { .colorScheme = std::string { scheme } };
        REQUIRE(vtbackend::defaultColorPalettes(scheme, colors.colors));
        config.profiles.value().at("main").colors.value() = colors;

        // What SettingsPreview::apply does with the controller's preview configuration.
        auto session = app.manager().createPreviewSession(config, "main");
        REQUIRE(session != nullptr);
        auto const palette = session->terminal().colorPalette();

        auto window = QQuickWindow {};
        window.resize(800, 600);
        // The hero's own ground is the scheme's background, which the page paints beneath the terminal.
        window.setColor(QColor(
            palette.defaultBackground.red, palette.defaultBackground.green, palette.defaultBackground.blue));
        auto display = std::make_unique<contour::display::TerminalDisplay>();
        display->setParentItem(window.contentItem());
        display->setSize(QSizeF(800, 600));
        display->setSession(session.get());
        window.show();
        QCoreApplication::processEvents();
        auto frame = window.grabWindow();
        QCoreApplication::processEvents();
        frame = window.grabWindow();
        REQUIRE_FALSE(frame.isNull());

        auto const metrics = display->gridMetrics();
        auto const cellWidth = unbox<int>(metrics.cellSize.width);
        auto const cellHeight = unbox<int>(metrics.cellSize.height);
        auto const cells = [&](int row, int from, int to) {
            return QRect { metrics.pageMargin.left + (from * cellWidth),
                           metrics.pageMargin.top + (row * cellHeight),
                           (to - from + 1) * cellWidth,
                           cellHeight };
        };

        // The transcript is on the screen: the command's cells hold ink on the scheme's ground, readable.
        auto const command = regionColors(frame, cells(CommandRow, CommandFirstColumn, CommandLastColumn));
        INFO("ground " << hexOf(command.ground) << " ink " << hexOf(command.ink));
        CHECK(distanceBetween(command.ground, palette.defaultBackground) <= 18);
        CHECK(command.inkPixels > 0);
        // The preview shows a scheme as it is, so it reads as well as the scheme's own pair does
        // (solarized-light's is 4.1:1, under the text ratio, and is the scheme's to answer for) and never
        // under a mark's ratio.
        auto const measured = vtbackend::contrastRatio(command.ink, command.ground);
        auto const schemeOwn = vtbackend::contrastRatio(palette.defaultForeground, palette.defaultBackground);
        CHECK(measured >= std::min(vtbackend::MinimumTextContrast, schemeOwn) * PixelContrastShare);
        CHECK(measured >= vtbackend::MinimumMarkContrast);

        // The changed file's mark is the scheme's red, the colour being judged: it is present in its cell.
        auto const mark = regionColors(frame, cells(OutputRow, ChangedMarkColumn, ChangedMarkColumn));
        auto const red = palette.normalColor(1);
        auto const hasRed = std::ranges::any_of(std::views::iota(0, cellWidth * cellHeight), [&](int index) {
            auto const area = cells(OutputRow, ChangedMarkColumn, ChangedMarkColumn);
            auto const pixel =
                rgbOf(frame.pixel(area.left() + (index % cellWidth), area.top() + (index / cellWidth)));
            return distanceBetween(pixel, red) <= 24;
        });
        INFO("mark ground " << hexOf(mark.ground) << " red " << hexOf(red));
        CHECK(hasRed);

        display.reset();
        session->terminate();
        QCoreApplication::processEvents();
    }
}
