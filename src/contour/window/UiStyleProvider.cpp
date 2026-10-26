// SPDX-License-Identifier: Apache-2.0
#include <contour/config/Config.hpp>
#include <contour/geometry/WindowGeometry.hpp>
#include <contour/platform/ColorConversion.hpp>
#include <contour/platform/GuiTheme.hpp>
#include <contour/window/HostChipState.hpp>
#include <contour/window/PaneHeaderPolicy.hpp>
#include <contour/window/TabBadge.hpp>
#include <contour/window/TabWellLayout.hpp>
#include <contour/window/TitleBandLayout.hpp>
#include <contour/window/TitleBandSurface.hpp>
#include <contour/window/UiStyleProvider.hpp>

#include <vtbackend/vt/ProgressState.hpp>

#include <text_shaper/Font.hpp>

#include <core/log/LogStore.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QStringList>
#include <QtGui/QFontMetricsF>
#include <QtGui/QGuiApplication>
#include <QtGui/QPalette>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>

namespace contour::window
{

namespace
{
    /// The family a cell-counting chrome asks Qt for, given the configured @p family: the one the grid's
    /// font locator draws in. Qt knows the generic `monospace` only where fontconfig does, and elsewhere
    /// substitutes its own pick (Courier New on Windows) -- a cell that lines up with nothing below it.
    [[nodiscard]] QString chromeFamilyOf(std::string_view family)
    {
        return QString::fromUtf8(text::platformFontFamily(family));
    }

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

    /// @return @p glyphs as the string list QML reads.
    template <size_t N>
    [[nodiscard]] QStringList stringsOf(std::array<std::string_view, N> const& glyphs)
    {
        auto list = QStringList {};
        list.reserve(static_cast<qsizetype>(N));
        for (auto const glyph: glyphs)
            list.append(QString::fromUtf8(glyph));
        return list;
    }

    /// Whether the application palette paints a dark chrome: its Window colour's tone, as statusColor() and
    /// the colour roles read it.
    [[nodiscard]] bool paletteIsDark()
    {
        return platform::colorSchemeOfBackground(
                   platform::toRGBColor(QGuiApplication::palette().color(QPalette::Window)))
               == Qt::ColorScheme::Dark;
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
            .tabColor = QString::fromUtf8(tokens.tabColorGlyph),
            .chipOpen = QString::fromUtf8(tokens.chipOpenGlyph),
            .chipClose = QString::fromUtf8(tokens.chipCloseGlyph),
            .hostDot = QString::fromUtf8(tokens.hostDotGlyph),
            .paneActions = stringsOf(tokens.paneActionGlyphs),
            .splitLines = stringsOf(tokens.splitLineGlyphs),
            .restart = QString::fromUtf8(tokens.restartGlyph),
        };
    }

    /// Whether @p value, as QML passes it, indexes a table of @p count rows (an enum of @p count
    /// enumerators counted from 0).
    [[nodiscard]] constexpr bool inRange(int value, size_t count) noexcept
    {
        return value >= 0 && std::cmp_less(value, count);
    }

    /// @return @p name as the QString QML reads it.
    [[nodiscard]] QString nameOf(std::string_view name)
    {
        return QString::fromUtf8(name);
    }
} // namespace

ChromeInputs chromeInputsFor(config::ChromeStyle row, QFont const& font)
{
    return ChromeInputs { .row = row,
                          .font = font,
                          .accessibility = {},
                          .appearance = {},
                          .scheme = config::chromeSchemeColorsOf(vtbackend::ColorPalette {}),
                          .density = config::ChromeDensity::Comfortable };
}

UiStyleProvider::UiStyleProvider(ChromeInputs inputs, QObject* parent):
    QObject { parent },
    _inputs { std::move(inputs) },
    _tokens { config::uiStyleTokens(_inputs.row) },
    _fonts { chromeFontsFor(_tokens, _inputs.font) },
    _metrics { chromeMetricsFor(_tokens, _inputs.font) },
    _glyphs { chromeGlyphsFor(_tokens) },
    _groups { config::resolveChromeTokens(_inputs.row, _inputs.accessibility, _inputs.density) },
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
    _tint { _groups.tint, this },
    _colors { _groups.colors, _inputs.scheme, _groups.contrast.separatorPercent, *this },
    _paneRoles { this },
    _badgeKinds { this }
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
    return QString::fromLatin1(config::chromeStyleName(_inputs.row));
}

QColor UiStyleProvider::wash(QColor const& base) const
{
    return platform::atOpacity(base, _tokens.hoverWashPercent);
}

QColor UiStyleProvider::modalScrim(QColor const& base) const
{
    return platform::atOpacity(base, _tokens.modalScrimPercent);
}

QColor UiStyleProvider::modelessScrim(QColor const& base) const
{
    return platform::atOpacity(base, _tokens.modelessScrimPercent);
}

QColor UiStyleProvider::shadowTint(QColor const& base) const
{
    return platform::atOpacity(base, _tokens.shadowOpacityPercent);
}

QColor UiStyleProvider::progressColor(int state) const
{
    // One accessor taking the state rather than three named ones: the state's role is one row of
    // config::ProgressStatusRoles, so QML says `progressColor(tabProgressState)` and neither side
    // repeats the table. Out-of-range is answered rather than asserted -- the value crosses from QML,
    // and an unknown state paints nothing rather than aborting the GUI.
    if (state <= static_cast<int>(vtbackend::ProgressState::Inactive)
        || !inRange(state, config::ProgressStatusRoles.size()))
        return { Qt::transparent };
    return statusInk(static_cast<int>(config::ProgressStatusRoles[static_cast<size_t>(state)]));
}

QColor UiStyleProvider::statusInk(int role) const
{
    if (!inRange(role, config::StatusColorRoleCount))
        return { Qt::transparent };
    return _colors.resolve(_groups.statusInk[static_cast<size_t>(role)]);
}

QVariantMap UiStyleProvider::badgeLook(int kind) const
{
    if (!inRange(kind, TabBadgeKindCount))
        return {};
    auto const& look = tabBadgeLook(static_cast<TabBadgeKind>(kind));
    return { { QStringLiteral("glyph"), nameOf(look.glyph) },
             { QStringLiteral("mark"), nameOf(badgeMarkName(look.mark)) },
             { QStringLiteral("ink"), statusInk(static_cast<int>(look.ink)) },
             { QStringLiteral("name"), nameOf(look.accessibleName) } };
}

QVariantMap UiStyleProvider::hostChipLook(int state) const
{
    if (!inRange(state, HostChipStateCount))
        return {};
    auto const& look = window::hostChipLook(static_cast<HostChipState>(state));
    return { { QStringLiteral("dot"), statusInk(static_cast<int>(look.dot)) },
             { QStringLiteral("status"), nameOf(look.status) },
             { QStringLiteral("accessibleState"), nameOf(look.accessibleState) } };
}

bool UiStyleProvider::isHostChipShown(QString const& hostLabel) const
{
    return hostChipPresence(hostLabel.toStdString()) == HostChipPresence::Shown;
}

QVariantMap UiStyleProvider::titleBand(
    int frame, int mode, int strip, int position, QString const& hostLabel, int hostChipState) const
{
    if (!inRange(frame, WindowFrameCount) || !inRange(mode, WindowModeCount)
        || !inRange(strip, TabStripGateCount) || !inRange(position, config::TabBarPositionCount)
        || !inRange(hostChipState, HostChipStateCount))
        return {};
    auto const band = resolveTitleBand(
        TitleBandInputs { .shape = _groups.layout.tabShape,
                          .frame = static_cast<WindowFrame>(frame),
                          .mode = static_cast<WindowMode>(mode),
                          .strip = static_cast<TabStripGate>(strip),
                          .position = static_cast<config::TabBarPosition>(position),
                          .notice = hostNoticeOf(hostChipPresence(hostLabel.toStdString()),
                                                 static_cast<HostChipState>(hostChipState)) });
    return { { QStringLiteral("band"), nameOf(barPresenceName(band.band)) },
             { QStringLiteral("bandEdge"), nameOf(barEdgeName(band.bandEdge)) },
             { QStringLiteral("centre"), nameOf(barCentreName(band.centre)) },
             { QStringLiteral("tabBar"), nameOf(barPresenceName(band.tabBar)) } };
}

QVariantMap UiStyleProvider::tabWellLayout(qreal available, int tabCount) const
{
    auto const layout = layoutTabWell(available,
                                      tabCount,
                                      TabWellMetrics { .minTab = _metrics.minTabWidth,
                                                       .maxTab = _metrics.maxTabWidth,
                                                       .iconOnlyTab = (2 * _space.s5()) + _size.tabIcon(),
                                                       .gap = _space.s1(),
                                                       .padding = _space.s1() });
    return { { QStringLiteral("tabWidth"), layout.tabWidth },
             { QStringLiteral("wellWidth"), layout.wellWidth },
             { QStringLiteral("label"), nameOf(tabLabelModeName(layout.label)) },
             { QStringLiteral("scrolls"), layout.overflow == TabWellOverflow::Scrolls } };
}

QFont UiStyleProvider::activeTabFont(QFont const& base) const
{
    auto font = base;
    switch (_groups.layout.activeTabWeight)
    {
        case config::ActiveTabWeight::Regular: break;
        case config::ActiveTabWeight::Bold: font.setBold(true); break;
    }
    return font;
}

qreal UiStyleProvider::snapToCells(qreal length) const
{
    auto const quantum = _metrics.widthQuantum;
    return quantum * std::ceil(length / quantum);
}

qreal UiStyleProvider::bandHeight() const noexcept
{
    switch (_groups.layout.tabShape)
    {
        case config::TabShape::Pill: return _size.titleBar();
        case config::TabShape::Flat: break;
    }
    return _metrics.chromeHeight;
}

QColor UiStyleProvider::statusColor(int role) const
{
    if (!inRange(role, config::StatusColors.size()))
        return { Qt::transparent };
    return statusColor(static_cast<config::StatusColorRole>(role), config::StatusInkUse::Text);
}

QColor UiStyleProvider::statusColor(config::StatusColorRole role, config::StatusInkUse use) const
{
    if (!inRange(static_cast<int>(role), config::StatusColors.size()))
        return { Qt::transparent };

    auto const& row = config::StatusColors[static_cast<size_t>(role)];
    switch (row.source)
    {
        case config::StatusColorSource::PaletteAccent: return accentColor();
        case config::StatusColorSource::Fixed: break;
    }
    auto const& hues = row.hues(use);
    return QColor::fromRgb(paletteIsDark() ? hues.dark : hues.light);
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

QColor UiStyleProvider::paneDimColor() const
{
    auto const& dim = _groups.dim;
    return platform::atOpacity(
        QColor(Qt::black),
        _groups.contrast.paneDimPercent.value_or(paletteIsDark() ? dim.darkPercent : dim.lightPercent));
}

QString UiStyleProvider::densityName() const
{
    return nameOf(config::chromeDensityName(_inputs.density));
}

qreal UiStyleProvider::overlayPaneHeader() const noexcept
{
    return config::ChromeTokenRows[static_cast<size_t>(_inputs.row)].size.paneHeader * _units.vertical;
}

QStringList UiStyleProvider::paneHeaderActionNames() const
{
    auto names = QStringList {};
    names.reserve(static_cast<qsizetype>(PaneHeaderActions.size()));
    for (auto const& row: PaneHeaderActions)
        names.append(nameOf(row.accessibleName));
    return names;
}

QVariantMap UiStyleProvider::paneHeader(
    int reveal, int pointer, int focus, int visiblePanes, qreal width, qreal height) const
{
    if (!inRange(reveal, HeaderRevealCount) || !inRange(pointer, PointerPresenceCount)
        || !inRange(focus, PaneFocusCount))
        return {};
    auto const header = resolvePaneHeader(PaneHeaderInputs { .density = _inputs.density,
                                                             .reveal = static_cast<HeaderReveal>(reveal),
                                                             .pointer = static_cast<PointerPresence>(pointer),
                                                             .focus = static_cast<PaneFocus>(focus),
                                                             .actionsReveal = _groups.layout.paneActions,
                                                             .visiblePanes = visiblePanes,
                                                             .width = width,
                                                             .height = height,
                                                             .headerHeight = _size.paneHeader(),
                                                             .minimumContentHeight = _metrics.cellHeight });
    return { { QStringLiteral("content"), nameOf(paneHeaderContentName(header.content)) },
             { QStringLiteral("actions"), nameOf(headerActionsName(header.actions)) },
             { QStringLiteral("placement"), nameOf(headerPlacementName(header.placement)) },
             { QStringLiteral("emphasis"), nameOf(headerEmphasisName(header.emphasis)) } };
}

QString UiStyleProvider::paneHeaderName(QString const& processName, QString const& title) const
{
    return QString::fromStdString(window::paneHeaderName(processName.toStdString(), title.toStdString()));
}

// colorOn() picks a pane's tone by toneOf(); the chrome's own scheme follows the terminal background by
// platform::colorSchemeOfBackground(). One threshold, so a pane and the chrome agree on what is dark.
static_assert(TitleBandDarkLuminance == platform::DarkBackgroundLuminance);

QColor UiStyleProvider::colorOn(int role, QColor const& background) const
{
    if (!inRange(role, config::ChromeColorRoleCount))
        return { Qt::transparent };
    auto const tone = toneOf(platform::toRGBColor(background));
    return _colors.resolve(_groups.colors[static_cast<size_t>(role)],
                           tone == ChromeTone::Dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
}

qreal UiStyleProvider::hairline(qreal devicePixelRatio) const
{
    return devicePixelRatio > 0 ? 1.0 / devicePixelRatio : 1.0;
}

qreal UiStyleProvider::unfocusedDim(qreal profileDim) const
{
    auto dim = profileDim;
    switch (_inputs.density)
    {
        case config::ChromeDensity::Minimal: {
            auto const& floor = _groups.dim;
            dim = std::max(dim, (paletteIsDark() ? floor.darkPercent : floor.lightPercent) / 100.0);
            break;
        }
        case config::ChromeDensity::Comfortable: break;
    }
    if (auto const cap = _groups.contrast.paneDimPercent)
        dim = std::min(dim, *cap / 100.0);
    return dim;
}

QColor UiStyleProvider::dotGlyphColor() const
{
    return platform::atOpacity(QColor(Qt::black), config::ChromeTokens::DotGlyphPercent);
}

QColor UiStyleProvider::errorColor() const
{
    return statusColor(static_cast<int>(config::StatusColorRole::Danger));
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
        font.setFamily(chromeFamilyOf(fonts.regular.familyName));
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
        font.setFamily(chromeFamilyOf(family));

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
