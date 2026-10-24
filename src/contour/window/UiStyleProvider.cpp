// SPDX-License-Identifier: Apache-2.0
#include <contour/config/Config.hpp>
#include <contour/geometry/WindowGeometry.hpp>
#include <contour/platform/ColorConversion.hpp>
#include <contour/platform/GuiTheme.hpp>
#include <contour/window/UiStyleProvider.hpp>

#include <vtbackend/vt/ProgressState.hpp>

#include <core/log/LogStore.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtGui/QFontMetricsF>
#include <QtGui/QGuiApplication>
#include <QtGui/QPalette>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace contour::window
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
            .windowControlInset = tokens.windowControlInsetUnits * horizontal,
            .windowControlGutter = tokens.windowControlGutterUnits * horizontal,
            // Horizontal for both extents of the dot: a traffic light is a circle, and quantizing its
            // width and height to different cells would make it an ellipse in a cell-counting style.
            .trafficLightDotSize = tokens.trafficLightDotUnits * horizontal,
            .trafficLightGap = tokens.trafficLightGapUnits * horizontal,
            .badgeWidth = tokens.badgeUnits * horizontal,
            .badgeHeight = tokens.badgeUnits * vertical,
            // Vertical for the offset, since it displaces the shadow downward. The margin has to
            // clear that offset on the axis it acts, so it takes the same unit.
            .shadowOffsetY = tokens.shadowOffsetUnits * vertical,
            .shadowMargin = tokens.shadowMarginUnits * vertical,
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

        // Scaled by a percentage of the chrome font rather than pinned to a size, so the caption and
        // the headline follow the user's font size. A pixel-sized font scales its pixel size instead.
        auto const scaled = [&font](int percent) {
            auto result = font;
            if (font.pointSizeF() > 0.0)
                result.setPointSizeF(font.pointSizeF() * percent / 100.0);
            else if (font.pixelSize() > 0)
                result.setPixelSize(std::max(1, font.pixelSize() * percent / 100));
            return result;
        };

        return ChromeFonts {
            .base = font,
            .close = sized(tokens.closePointSize),
            .newTab = sized(tokens.newTabPointSize),
            .menu = sized(tokens.menuPointSize),
            .windowControl = sized(tokens.windowControlPointSize),
            .badge = badge,
            .caption = scaled(config::ChromeTokens::CaptionFontPercent),
            .headline = scaled(config::ChromeTokens::HeadlineFontPercent),
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
            .trafficLight = QString::fromUtf8(tokens.trafficLightGlyph),
            .close = QString::fromUtf8(tokens.closeGlyph),
            .zoom = QString::fromUtf8(tokens.zoomGlyph),
            .newTab = QString::fromUtf8(tokens.newTabGlyph),
            .menu = QString::fromUtf8(tokens.menuGlyph),
            .submenu = QString::fromUtf8(tokens.submenuGlyph),
        };
    }
} // namespace

ChromeInputs chromeInputsFor(config::ChromeStyle row, QFont const& font)
{
    return ChromeInputs { .row = row,
                          .font = font,
                          .accessibility = {},
                          .appearance = {},
                          .scheme = config::chromeSchemeColorsOf(vtbackend::ColorPalette {}) };
}

UiStyleProvider::UiStyleProvider(ChromeInputs inputs, QObject* parent):
    QObject { parent },
    _inputs { std::move(inputs) },
    _row { _inputs.row },
    _tokens { config::uiStyleTokens(_row) },
    _fonts { chromeFontsFor(_tokens, _inputs.font) },
    _metrics { chromeMetricsFor(_tokens, _inputs.font) },
    _glyphs { chromeGlyphsFor(_tokens) },
    _groups { config::resolveChromeTokens(_row, _inputs.accessibility) },
    // The same pair chromeMetricsFor() resolves every other extent with, so a group token and a
    // legacy one land on the same grid.
    _units { .horizontal = _metrics.widthQuantum,
             .vertical = _tokens.unit == config::LengthUnit::Cell ? _metrics.cellHeight : 1.0 },
    _size { _groups.size, _units, this },
    _space { _groups.space, _units, this },
    _radii { _groups.radii, _units, this },
    _type { _groups.type, _inputs.font, this },
    _motion { _groups.motion, _units, this },
    _elevation { _groups.elevation, _units, this },
    _material { _groups.material, this },
    _contrast { _groups.contrast, this },
    _tui { _groups.tui, this },
    _layout { _groups.layout, this },
    _colors { _groups.colors, _inputs.scheme, *this }
{
    if (auto* application = QCoreApplication::instance())
        application->installEventFilter(this);
}

UiStyleProvider::UiStyleProvider(config::ChromeStyle row, QFont const& chromeFont, QObject* parent):
    UiStyleProvider { chromeInputsFor(row, chromeFont), parent }
{
}

bool UiStyleProvider::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == QCoreApplication::instance() && event->type() == QEvent::ApplicationPaletteChange)
    {
        ++_paletteRevision;
        emit paletteRevisionChanged();
    }
    return QObject::eventFilter(watched, event);
}

QString UiStyleProvider::chromeStyleName() const
{
    return QString::fromLatin1(config::chromeStyleName(_row));
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

QColor UiStyleProvider::shadowTint(QColor const& base) const
{
    return atOpacity(base, _tokens.shadowOpacityPercent);
}

QColor UiStyleProvider::progressColor(int state) const
{
    // One accessor taking the state rather than three named ones: the mapping is an index into the
    // style's own row, so QML says `progressColor(tabProgressState)` and neither side repeats the
    // state->appearance table. Out-of-range is answered rather than asserted -- the value crosses
    // from QML, and an unknown state paints nothing rather than aborting the GUI.
    auto const& colors = _tokens.progressColors;
    if (state < 0 || std::cmp_greater_equal(state, colors.size()))
        return { Qt::transparent };
    return QColor::fromRgb(colors[static_cast<size_t>(state)]);
}

QColor UiStyleProvider::statusColor(int role) const
{
    if (role < 0 || std::cmp_greater_equal(role, config::StatusColors.size()))
        return { Qt::transparent };

    auto const& row = config::StatusColors[static_cast<size_t>(role)];
    auto const palette = QGuiApplication::palette();
    switch (row.source)
    {
        case config::StatusColorSource::PaletteAccent: return accentColor();
        case config::StatusColorSource::Fixed: break;
    }

    auto const dark = platform::colorSchemeOfBackground(platform::toRGBColor(palette.color(QPalette::Window)))
                      == Qt::ColorScheme::Dark;
    return QColor::fromRgb(dark ? row.dark : row.light);
}

QColor UiStyleProvider::accentColor() const
{
    if (auto const& accent = _inputs.appearance.accent)
        return platform::toQColor(*accent);
    return QGuiApplication::palette().color(QPalette::Highlight);
}

QColor UiStyleProvider::onColor(QColor const& background) const
{
    auto const rgb = platform::toRGBColor(background);
    auto const black = vtbackend::RGBColor(0, 0, 0);
    auto const white = vtbackend::RGBColor(255, 255, 255);
    return vtbackend::contrastRatio(rgb, black) >= vtbackend::contrastRatio(rgb, white) ? QColor(Qt::black)
                                                                                        : QColor(Qt::white);
}

QColor UiStyleProvider::onStatusColor(int role) const
{
    return onColor(statusColor(role));
}

QColor UiStyleProvider::paneDimTextColor() const
{
    return onColor(QColor(Qt::black));
}

QColor UiStyleProvider::paneDimColor() const
{
    return atOpacity(QColor(Qt::black), config::ChromeTokens::PaneDimPercent);
}

QColor UiStyleProvider::dotGlyphColor() const
{
    return atOpacity(QColor(Qt::black), config::ChromeTokens::DotGlyphPercent);
}

QColor UiStyleProvider::errorColor() const
{
    return progressColor(static_cast<int>(vtbackend::ProgressState::Error));
}

QFont resolveChromeFont(config::Config const& config, config::ChromeStyle row, std::string const& profileName)
{
    auto const& tokens = config::uiStyleTokens(row);
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

int UiStyleProvider::resizeBorderThickness() const noexcept
{
    return geometry::ResizeBorderThickness;
}

} // namespace contour::window
