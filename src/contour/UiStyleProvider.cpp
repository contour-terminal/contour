// SPDX-License-Identifier: Apache-2.0
#include <contour/config/Config.h>
#include <contour/UiStyleProvider.h>

#include <crispy/logstore.h>

#include <QtGui/QFontMetricsF>
#include <QtGui/QGuiApplication>

#include <cmath>
#include <string>

namespace contour
{

namespace
{
    /// The width of one character cell of @p metrics, in logical pixels.
    ///
    /// The advance of a single glyph rather than QFontMetricsF::averageCharWidth(): for the monospace
    /// font a cell-counting style expects, the advance IS the cell, whereas averageCharWidth() reports
    /// the font's OS/2 xAvgCharWidth, which some fonts leave at a value unrelated to their advance. A
    /// proportional font merely makes this an over-estimate, which widens tabs rather than breaking
    /// them.
    [[nodiscard]] qreal cellWidthOf(QFontMetricsF const& metrics)
    {
        return metrics.horizontalAdvance(QStringLiteral("M"));
    }

    /// The height of one character cell of @p metrics, in logical pixels.
    ///
    /// lineSpacing() rather than height(), so a font that asks for leading gets it -- the same
    /// distinction vtrasterizer makes when it takes the shaper's lineHeight for the terminal grid.
    /// Rounded up so a one-row chrome never clips its own text on a fractional metric.
    [[nodiscard]] qreal cellHeightOf(QFontMetricsF const& metrics)
    {
        return std::ceil(metrics.lineSpacing());
    }

    /// Resolves @p tokens against @p font's metrics into logical pixels.
    [[nodiscard]] ChromeMetrics chromeMetricsFor(config::UiStyleTokens const& tokens, QFont const& font)
    {
        auto const metrics = QFontMetricsF(font);
        auto const cellWidth = cellWidthOf(metrics);
        auto const cellHeight = cellHeightOf(metrics);

        // The whole style difference reduces to this pair: what one unit of horizontal and vertical
        // length means. Pixel counting leaves both at 1, so every token passes through as the literal
        // the QML used to carry; Cell counting turns the same tokens into a grid. Nothing downstream
        // needs to know which style is active.
        auto const horizontal = tokens.unit == config::LengthUnit::Cell ? cellWidth : 1.0;
        auto const vertical = tokens.unit == config::LengthUnit::Cell ? cellHeight : 1.0;

        return ChromeMetrics {
            .cellWidth = cellWidth,
            .cellHeight = cellHeight,
            .widthQuantum = horizontal,
            .chromeHeight = tokens.chromeHeightUnits * vertical,
            .tabHeight = tokens.tabHeightUnits * vertical,
            .labelPadding = tokens.labelPaddingUnits * horizontal,
            .labelGap = tokens.labelGapUnits * horizontal,
            .trailingPadding = tokens.trailingPaddingUnits * horizontal,
            .minTabWidth = tokens.minTabUnits * horizontal,
            .maxTabWidth = tokens.maxTabUnits * horizontal,
            .tabSlack = tokens.tabSlackUnits * horizontal,
            .controlWidth = tokens.controlUnits * horizontal,
            .controlHeight = tokens.controlUnits * vertical,
            .stripButtonWidth = tokens.stripButtonUnits * horizontal,
            .windowControlWidth = tokens.windowControlUnits * horizontal,
            .badgeWidth = tokens.badgeUnits * horizontal,
            .badgeHeight = tokens.badgeUnits * vertical,
        };
    }

    /// Builds the per-control fonts @p tokens asks for, based on @p font.
    [[nodiscard]] ChromeFonts chromeFontsFor(config::UiStyleTokens const& tokens, QFont const& font)
    {
        // A style pins a control's point size only where it wants one different from the chrome
        // font's (the native chrome's larger "+" and smaller "▾"); 0 means "whatever the chrome font
        // is", which is what keeps a cell-counting style's control on the grid.
        auto const sized = [&font](int pointSize) {
            if (pointSize == 0)
                return font;
            auto scaled = font;
            scaled.setPointSize(pointSize);
            return scaled;
        };

        auto badge = font;
        if (tokens.badgePixelSize != 0)
            badge.setPixelSize(tokens.badgePixelSize);
        badge.setBold(true);

        return ChromeFonts {
            .base = font,
            .close = sized(tokens.closePointSize),
            .newTab = sized(tokens.newTabPointSize),
            .menu = sized(tokens.menuPointSize),
            .windowControl = sized(tokens.windowControlPointSize),
            .badge = badge,
        };
    }

    /// @p base at @p percent opacity.
    ///
    /// Every translucent overlay the chrome paints -- the hover wash, the two popup scrims -- is a
    /// palette color taken down to an alpha the token row states, so the one arithmetic step lives
    /// here rather than once per caller.
    [[nodiscard]] QColor atOpacity(QColor const& base, int percent)
    {
        auto faded = base;
        faded.setAlphaF(static_cast<float>(percent) / 100.0F);
        return faded;
    }

    /// Converts @p tokens' glyphs to the strings QML draws.
    [[nodiscard]] ChromeGlyphs chromeGlyphsFor(config::UiStyleTokens const& tokens)
    {
        // QString::fromUtf8 takes a QByteArrayView, which a std::string_view converts to directly --
        // no data()/size() pair and no hand-written qsizetype cast.
        return ChromeGlyphs {
            .tabSeparator = QString::fromUtf8(tokens.tabSeparator),
            .close = QString::fromUtf8(tokens.closeGlyph),
            .zoom = QString::fromUtf8(tokens.zoomGlyph),
            .newTab = QString::fromUtf8(tokens.newTabGlyph),
            .menu = QString::fromUtf8(tokens.menuGlyph),
            .submenu = QString::fromUtf8(tokens.submenuGlyph),
        };
    }
} // namespace

UiStyleProvider::UiStyleProvider(config::UiStyle style, QFont const& chromeFont, QObject* parent):
    QObject { parent },
    _tokens { config::uiStyleTokens(style) },
    _fonts { chromeFontsFor(_tokens, chromeFont) },
    _metrics { chromeMetricsFor(_tokens, chromeFont) },
    _glyphs { chromeGlyphsFor(_tokens) }
{
}

QColor UiStyleProvider::wash(QColor const& base) const
{
    return atOpacity(base, _tokens.hoverWashPercent);
}

QColor UiStyleProvider::modalScrim(QColor const& base) const
{
    return atOpacity(base, _tokens.modalScrimPercent);
}

QColor UiStyleProvider::modelessScrim(QColor const& base) const
{
    return atOpacity(base, _tokens.modelessScrimPercent);
}

QFont resolveChromeFont(config::Config const& config, std::string const& profileName)
{
    auto const& tokens = config::uiStyleTokens(config.uiStyle.value());
    auto font = QGuiApplication::font();

    // A style that wants the platform's own font is done here -- and the ui_font_* keys, which exist
    // to make a cell-counting chrome match the grid, are deliberately not applied to it rather than
    // quietly restyling a chrome nobody asked to restyle.
    if (tokens.fontSource == config::ChromeFontSource::PlatformUi)
        return font;

    // Fall back to the RUNNING profile's regular font, so `ui_style: terminal` alone already matches
    // the terminal grid. The running one rather than `default_profile`: `contour profile=big` puts a
    // 20pt grid on screen, and a chrome quantized to the 12pt default's cell would line up with
    // nothing below it -- which is the entire premise of the style.
    if (auto const* profile = config.findProfile(profileName))
    {
        auto const& fonts = profile->fonts.value();
        font.setFamily(QString::fromStdString(fonts.regular.familyName));
        font.setPointSizeF(static_cast<qreal>(fonts.size.pt));
    }
    else
    {
        // Reachable from a partial configuration (a `default_profile` naming a profile that is not
        // there) and from a mistyped `-p`. The chrome still resolves -- to the proportional platform
        // font -- so without this the user gets non-monospace TUI chrome and no clue why.
        errorLog()("ui_style: no profile named '{}'; the terminal chrome falls back to the platform "
                   "UI font and will not match the terminal grid.",
                   profileName);
    }

    if (auto const& family = config.uiFontFamily.value(); !family.empty())
        font.setFamily(QString::fromStdString(family));

    if (auto const size = config.uiFontSize.value(); size > 0.0)
        font.setPointSizeF(size);

    // Ask for a fixed-pitch match: the style is built on a character cell, so if the requested family
    // is missing, a monospace substitute keeps the chrome on the grid where the proportional default
    // would not.
    font.setStyleHint(QFont::Monospace);
    return font;
}

} // namespace contour
