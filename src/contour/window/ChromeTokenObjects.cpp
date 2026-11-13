// SPDX-License-Identifier: Apache-2.0
#include <contour/config/HighlightPair.hpp>
#include <contour/platform/ColorConversion.hpp>
#include <contour/platform/GuiTheme.hpp>
#include <contour/window/ChromeTokenObjects.hpp>
#include <contour/window/UiStyleProvider.hpp>

#include <QtGui/QGuiApplication>
#include <QtGui/QPalette>

#include <algorithm>
#include <cmath>
#include <span>
#include <utility>

namespace contour::window
{

namespace
{
    /// @p font at @p pixelSize, or @p font itself where the token is 0 ("the chrome font's own size").
    [[nodiscard]] QFont sizedFont(QFont const& font, double pixelSize)
    {
        if (pixelSize == 0.0)
            return font;
        auto sized = font;
        sized.setPixelSize(static_cast<int>(std::lround(pixelSize)));
        return sized;
    }

    /// @p font at @p weight on the CSS 100-900 scale, which QFont::Weight shares.
    [[nodiscard]] QFont weightedFont(QFont font, int weight)
    {
        font.setWeight(static_cast<QFont::Weight>(std::clamp(weight, 1, 1000)));
        return font;
    }

    /// The row of a kit table a 1-based @p level names.
    /// @return The row, or null for a level outside the table.
    template <typename T>
    [[nodiscard]] T const* stepOf(std::span<T const> table, int level) noexcept
    {
        if (level < 1 || std::cmp_greater(level, table.size()))
            return nullptr;
        return &table[static_cast<size_t>(level - 1)];
    }
} // namespace

ChromeSizeTokens::ChromeSizeTokens(config::SizeTokens tokens, ChromeUnits units, QObject* parent):
    QObject { parent }, _tokens { tokens }, _units { units }
{
}

ChromeSpaceTokens::ChromeSpaceTokens(config::SpaceTokens tokens, ChromeUnits units, QObject* parent):
    QObject { parent }, _tokens { tokens }, _units { units }
{
}

ChromeRadiusTokens::ChromeRadiusTokens(config::RadiusTokens tokens, ChromeUnits units, QObject* parent):
    QObject { parent }, _tokens { tokens }, _units { units }
{
}

ChromeTypeTokens::ChromeTypeTokens(config::TypeTokens tokens, QFont const& font, QObject* parent):
    QObject { parent },
    _caption { sizedFont(font, tokens.caption) },
    _footnote { sizedFont(font, tokens.footnote) },
    _body { sizedFont(font, tokens.body) },
    _tab { sizedFont(font, tokens.tab) },
    _title { sizedFont(font, tokens.title) },
    _paneHeader { weightedFont(sizedFont(font, tokens.paneHeader), tokens.paneHeaderWeight) },
    _paneHeaderWeight { tokens.paneHeaderWeight },
    _paneHeaderAction { sizedFont(font, tokens.paneHeaderAction) },
    _paletteInput { sizedFont(font, tokens.paletteInput) },
    _shortcut { sizedFont(font, tokens.shortcut) },
    _bodyStrong { weightedFont(_body, QFont::Bold) },
    _bodyMedium { weightedFont(_body, QFont::Medium) },
    _footnoteStrong { weightedFont(_footnote, QFont::Bold) },
    _footnoteSemibold { weightedFont(_footnote, QFont::DemiBold) }
{
}

ChromeMotionTokens::ChromeMotionTokens(config::MotionTokens tokens, ChromeUnits units, QObject* parent):
    QObject { parent }, _tokens { tokens }, _units { units }
{
}

ChromeElevationLevel::ChromeElevationLevel(config::ElevationLevel level, ChromeUnits units, QObject* parent):
    QObject { parent }, _level { level }, _units { units }
{
}

ChromeElevationTokens::ChromeElevationTokens(config::ElevationTokens const& tokens,
                                             ChromeUnits units,
                                             QObject* parent):
    QObject { parent },
    _e0 { tokens.levels[0], units, this },
    _e1 { tokens.levels[1], units, this },
    _e2 { tokens.levels[2], units, this },
    _e3 { tokens.levels[3], units, this }
{
}

ChromeMaterialTokens::ChromeMaterialTokens(config::MaterialTokens tokens, QObject* parent):
    QObject { parent }, _tokens { tokens }
{
}

ChromeContrastTokens::ChromeContrastTokens(config::ContrastTokens tokens, QObject* parent):
    QObject { parent }, _tokens { tokens }
{
}

ChromeTuiTokens::ChromeTuiTokens(config::TuiTokens tokens, QObject* parent):
    QObject { parent }, _tokens { tokens }
{
}

ChromeTintTokens::ChromeTintTokens(config::TintTokens tokens, QObject* parent):
    QObject { parent }, _tokens { tokens }
{
}

ChromeLayoutTokens::ChromeLayoutTokens(config::LayoutTokens tokens, QObject* parent):
    QObject { parent }, _tokens { tokens }
{
}

ChromeSettingsTokens::ChromeSettingsTokens(config::SettingsTokens tokens, ChromeUnits units, QObject* parent):
    QObject { parent }, _tokens { tokens }, _units { units }
{
}

ChromeSettingsTraits::ChromeSettingsTraits(config::SettingsLayout layout, QObject* parent):
    QObject { parent }, _traits { config::settingsLayoutTraits(layout) }
{
}

ChromeColorTokens::ChromeColorTokens(Sources sources,
                                     config::ChromeSchemeColors scheme,
                                     int separatorPercent,
                                     UiStyleProvider& provider,
                                     ChromeGround ground):
    QObject { &provider },
    _sources { sources },
    _scheme { scheme },
    _separatorPercent { separatorPercent },
    _ground { ground },
    _provider { provider }
{
    connect(&provider, &UiStyleProvider::paletteRevisionChanged, this, &ChromeColorTokens::changed);
}

namespace
{
    /// The OS's selected-row pair as the chrome paints it: its own where the text reads at 4.5:1, else
    /// its hue under white or black text (config::legibleHighlight).
    config::HighlightPair osHighlight()
    {
        auto const palette = QGuiApplication::palette();
        return config::legibleHighlight(
            { .fill = platform::toRGBColor(palette.color(QPalette::Highlight)),
              .text = platform::toRGBColor(palette.color(QPalette::HighlightedText)) });
    }
} // namespace

QColor ChromeColorTokens::color(config::ChromeColorRole role) const
{
    return resolve(_sources[static_cast<size_t>(role)]);
}

QColor ChromeColorTokens::resolve(config::ChromeColorSource const& source) const
{
    // On a bar of the user's colour the kit's inks take that colour's tone, and the two palette sources
    // that name the window become the bar and the ink that reads on it: the palette's own window text was
    // chosen for the palette's window, not for this ground.
    if (auto const bar = _ground == ChromeGround::Bar ? _provider.barGround() : std::nullopt)
    {
        switch (source.kind)
        {
            case config::ColorSourceKind::PaletteWindow:
                return platform::atOpacity(*bar, source.alphaPercent);
            case config::ColorSourceKind::PaletteWindowText:
                return platform::atOpacity(_provider.onColor(*bar), source.alphaPercent);
            default: return resolve(source, platform::colorSchemeOfBackground(platform::toRGBColor(*bar)));
        }
    }
    auto const window = platform::toRGBColor(QGuiApplication::palette().color(QPalette::Window));
    return resolve(source, platform::colorSchemeOfBackground(window));
}

QColor ChromeColorTokens::resolve(config::ChromeColorSource const& source, Qt::ColorScheme chrome) const
{
    auto const dark = chrome == Qt::ColorScheme::Dark;
    auto const faded = [&source](QColor const& base) {
        return platform::atOpacity(base, source.alphaPercent);
    };
    // A kit wash or label is black on a light chrome and white on a dark one, at the table's alpha
    // scaled by the source's own. A level outside its table paints nothing.
    auto const kitInk = [&](std::span<config::AlphaPair const> table) -> QColor {
        auto const* const pair = stepOf(table, source.level);
        if (pair == nullptr)
            return { Qt::transparent };
        return platform::atOpacity(QColor(dark ? Qt::white : Qt::black),
                                   (dark ? pair->dark : pair->light) * source.alphaPercent / 100);
    };

    switch (source.kind)
    {
        case config::ColorSourceKind::Fixed:
            return faded(platform::toQColor(vtbackend::RGBColor(source.rgb)));
        case config::ColorSourceKind::PaletteMid:
            return faded(QGuiApplication::palette().color(QPalette::Mid));
        case config::ColorSourceKind::PaletteWindow:
            return faded(QGuiApplication::palette().color(QPalette::Window));
        case config::ColorSourceKind::PaletteWindowText:
            return faded(QGuiApplication::palette().color(QPalette::WindowText));
        case config::ColorSourceKind::PaletteHighlight: return faded(platform::toQColor(osHighlight().fill));
        case config::ColorSourceKind::PaletteHighlightedText:
            return faded(platform::toQColor(osHighlight().text));
        case config::ColorSourceKind::StatusMarkOnWindow:
            return faded(_provider.statusMarkOn(static_cast<int>(source.status),
                                                QGuiApplication::palette().color(QPalette::Window)));
        case config::ColorSourceKind::Status:
            return faded(_provider.statusColor(source.status, config::StatusInkUse::Text));
        case config::ColorSourceKind::StatusMark:
            return faded(_provider.statusColor(source.status, config::StatusInkUse::Mark));
        case config::ColorSourceKind::Scheme:
            return faded(platform::toQColor(_scheme.bySlot[static_cast<size_t>(source.slot)]));
        case config::ColorSourceKind::SchemeOnBar:
            return faded(platform::toQColor(config::legibleOnBar(_scheme, source.slot)));
        case config::ColorSourceKind::SchemeShade:
            return faded(platform::toQColor(
                config::shadeLightness(_scheme.bySlot[static_cast<size_t>(source.slot)], source.level)));
        case config::ColorSourceKind::Separator:
            return platform::atOpacity(QColor(dark ? Qt::white : Qt::black),
                                       _separatorPercent * source.alphaPercent / 100);
        case config::ColorSourceKind::Tone:
            return platform::atOpacity(
                platform::toQColor(vtbackend::RGBColor(dark ? source.darkRgb : source.rgb)),
                dark ? source.darkAlphaPercent : source.alphaPercent);
        case config::ColorSourceKind::Fill: return kitInk(config::FillAlphas);
        case config::ColorSourceKind::Label: return kitInk(config::LabelAlphas);
        case config::ColorSourceKind::Surface:
            if (auto const* const pair =
                    stepOf(std::span<config::SurfacePair const>(config::SurfaceColors), source.level))
                return faded(platform::toQColor(vtbackend::RGBColor(dark ? pair->dark : pair->light)));
            return { Qt::transparent };
    }
    return { Qt::transparent };
}

ChromeTabStyleTokens::ChromeTabStyleTokens(config::TabStyleTokens tokens,
                                           ChromeColorTokens const& colors,
                                           UiStyleProvider& provider):
    QObject { &provider }, _tokens { tokens }, _colors { colors }, _provider { provider }
{
    connect(&provider, &UiStyleProvider::paletteRevisionChanged, this, &ChromeTabStyleTokens::changed);
}

bool ChromeTabStyleTokens::hasBarGround() const
{
    return _provider.barGround().has_value();
}

QColor ChromeTabStyleTokens::barGround() const
{
    return _provider.barGround().value_or(QColor { Qt::transparent });
}

QColor ChromeTabStyleTokens::resolve(config::TabFill const& fill) const
{
    switch (fill.kind)
    {
        case config::TabFillKind::Auto: break;
        case config::TabFillKind::Accent: return _provider.accentColor();
        case config::TabFillKind::Subtle: return _colors.color(config::ChromeColorRole::TabHoverFill);
        case config::TabFillKind::Custom: return platform::toQColor(fill.color);
    }
    return { Qt::transparent };
}

ChromePaneRoleNames::ChromePaneRoleNames(QObject* parent): QObject { parent }
{
}

ChromeBadgeKindNames::ChromeBadgeKindNames(QObject* parent): QObject { parent }
{
}

ChromePaneEndNames::ChromePaneEndNames(QObject* parent): QObject { parent }
{
}

ChromePaneStateNames::ChromePaneStateNames(QObject* parent): QObject { parent }
{
}

ChromeHeaderOccupantNames::ChromeHeaderOccupantNames(QObject* parent): QObject { parent }
{
}

ChromeSecureInputNames::ChromeSecureInputNames(QObject* parent): QObject { parent }
{
}

ChromeBlockNames::ChromeBlockNames(QObject* parent): QObject { parent }
{
}

ChromeDropZoneNames::ChromeDropZoneNames(QObject* parent): QObject { parent }
{
}

ChromeDropPayloadNames::ChromeDropPayloadNames(QObject* parent): QObject { parent }
{
}

} // namespace contour::window
