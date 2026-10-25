// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/ColorConversion.hpp>
#include <contour/window/ChromeTokenObjects.hpp>
#include <contour/window/UiStyleProvider.hpp>

#include <QtGui/QGuiApplication>
#include <QtGui/QPalette>

#include <cmath>

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
    _title { sizedFont(font, tokens.title) }
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

ChromeLayoutTokens::ChromeLayoutTokens(config::LayoutTokens tokens, QObject* parent):
    QObject { parent }, _tokens { tokens }
{
}

ChromeColorTokens::ChromeColorTokens(Sources sources,
                                     config::ChromeSchemeColors scheme,
                                     UiStyleProvider& provider):
    QObject { &provider }, _sources { sources }, _scheme { scheme }, _provider { provider }
{
    connect(&provider, &UiStyleProvider::paletteRevisionChanged, this, &ChromeColorTokens::changed);
}

QColor ChromeColorTokens::color(config::ChromeColorRole role) const
{
    auto const& source = _sources[static_cast<size_t>(role)];
    auto const base = [&]() -> QColor {
        switch (source.kind)
        {
            case config::ColorSourceKind::Fixed: return platform::toQColor(vtbackend::RGBColor(source.rgb));
            case config::ColorSourceKind::PaletteWindow:
                return QGuiApplication::palette().color(QPalette::Window);
            case config::ColorSourceKind::PaletteWindowText:
                return QGuiApplication::palette().color(QPalette::WindowText);
            case config::ColorSourceKind::Status:
                return _provider.statusColor(static_cast<int>(source.status));
            case config::ColorSourceKind::Scheme:
                return platform::toQColor(_scheme.bySlot[static_cast<size_t>(source.slot)]);
        }
        return { Qt::transparent };
    }();

    return platform::atOpacity(base, source.alphaPercent);
}

} // namespace contour::window
