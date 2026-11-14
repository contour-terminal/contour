// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The location chip's glue between QML's values and the pure decisions: the state glyphs as a QStringList,
/// the band's minimum width read from the map QML measures into, and the translation of the chip's sentences.

#include <contour/window/LocationChip.hpp>
#include <contour/window/QtString.hpp>
#include <contour/window/TitleBandGeometry.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariant>

#include <string>
#include <string_view>
#include <utility>

namespace contour::window
{

/// @return The terminal-style glyph of every connection state (LocationChipStateRows), so QML can reserve the
///         widest of them.
[[nodiscard]] inline QStringList locationChipStateGlyphList()
{
    auto glyphs = QStringList {};
    for (auto const& row: LocationChipStateRows)
        glyphs.push_back(toQString(row.glyph));
    return glyphs;
}

/// Translates the chip's source sentences with Qt's translator (the chip's own context), as the chip says
/// them to the user and to a screen reader.
struct QtTranslated
{
    /// @param sentence A source-language sentence of window/LocationChip.hpp.
    /// @return Its translation, or the sentence itself where there is none.
    [[nodiscard]] std::string operator()(std::string_view sentence) const
    {
        return QCoreApplication::translate("LocationChip", std::string(sentence).c_str()).toStdString();
    }
};

/// @param parts The widths the band reports by their keys (window::BandWidthParts).
/// @return What the band is at least wide (window::bandMinimumWidth()).
[[nodiscard]] inline double bandMinimumWidthOfParts(QVariantMap const& parts)
{
    return bandMinimumWidthFrom([&](std::string_view key) { return parts.value(toQString(key)).toDouble(); });
}

} // namespace contour::window
