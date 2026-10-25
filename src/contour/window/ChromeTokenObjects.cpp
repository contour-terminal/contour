// SPDX-License-Identifier: Apache-2.0
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
    _paneHeaderWeight { tokens.paneHeaderWeight }
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

ChromeColorTokens::ChromeColorTokens(Sources sources,
                                     config::ChromeSchemeColors scheme,
                                     int separatorPercent,
                                     UiStyleProvider& provider):
    QObject { &provider },
    _sources { sources },
    _scheme { scheme },
    _separatorPercent { separatorPercent },
    _provider { provider }
{
    connect(&provider, &UiStyleProvider::paletteRevisionChanged, this, &ChromeColorTokens::changed);
}

QColor ChromeColorTokens::color(config::ChromeColorRole role) const
{
    return resolve(_sources[static_cast<size_t>(role)]);
}

QColor ChromeColorTokens::resolve(config::ChromeColorSource const& source) const
{
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
        case config::ColorSourceKind::PaletteWindow:
            return faded(QGuiApplication::palette().color(QPalette::Window));
        case config::ColorSourceKind::PaletteWindowText:
            return faded(QGuiApplication::palette().color(QPalette::WindowText));
        case config::ColorSourceKind::Status:
            return faded(_provider.statusColor(source.status, config::StatusInkUse::Text));
        case config::ColorSourceKind::StatusMark:
            return faded(_provider.statusColor(source.status, config::StatusInkUse::Mark));
        case config::ColorSourceKind::Scheme:
            return faded(platform::toQColor(_scheme.bySlot[static_cast<size_t>(source.slot)]));
        case config::ColorSourceKind::SchemeOnBar:
            return faded(platform::toQColor(config::inkOnBar(_scheme, source.slot)));
        case config::ColorSourceKind::SchemeShade:
            return faded(platform::toQColor(
                config::shadeLightness(_scheme.bySlot[static_cast<size_t>(source.slot)], source.level)));
        case config::ColorSourceKind::Separator:
            return platform::atOpacity(QColor(dark ? Qt::white : Qt::black),
                                       _separatorPercent * source.alphaPercent / 100);
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

ChromePaneRoleNames::ChromePaneRoleNames(QObject* parent): QObject { parent }
{
}

ChromeBadgeKindNames::ChromeBadgeKindNames(QObject* parent): QObject { parent }
{
}

} // namespace contour::window
