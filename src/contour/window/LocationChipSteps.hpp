// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The location chip's glue between QML's values and the pure decisions: the fit steps as a QVariantList, the
/// fit and the band's minimum width read from the maps QML measures into, and the translation of the chip's
/// sentences. One place, shared by WindowController and the test doubles standing in for it.

#include <contour/window/LocationChip.hpp>
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

/// @return Each of LocationChipSteps as a map { fit, key }, in elision order: a LocationChipFit's underlying
///         value and the name WindowController::chipFit() takes that step's width by.
[[nodiscard]] inline QVariantList locationChipStepList()
{
    auto steps = QVariantList {};
    for (auto const& step: LocationChipSteps)
        steps.push_back(
            QVariantMap { { QStringLiteral("fit"), static_cast<int>(std::to_underlying(step.fit)) },
                          { QStringLiteral("key"),
                            QString::fromUtf8(step.key.data(), static_cast<qsizetype>(step.key.size())) } });
    return steps;
}

/// @return The terminal-style glyph of every connection state (LocationChipStateRows), so QML can reserve the
///         widest of them.
[[nodiscard]] inline QStringList locationChipStateGlyphList()
{
    auto glyphs = QStringList {};
    for (auto const& row: LocationChipStateRows)
        glyphs.push_back(QString::fromUtf8(row.glyph.data(), static_cast<qsizetype>(row.glyph.size())));
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

/// @param fit A window::LocationChipFit's underlying value, as QML hands it over.
/// @return The enumerator; Full (the default, whole chip) for a value that is none of them.
[[nodiscard]] inline LocationChipFit locationChipFitOf(int fit) noexcept
{
    return locationChipFitFromInt(fit).value_or(LocationChipFit::Full);
}

/// @param widths What each fit step needs by its key (window::LocationChipSteps), and the room as
///               `available`, all in the same unit.
/// @return A LocationChipFit's underlying value: the first step that fits.
[[nodiscard]] inline int locationChipFitOfWidths(QVariantMap const& widths)
{
    auto const widthOf = [&](std::string_view key) {
        return widths.value(QString::fromUtf8(key.data(), static_cast<qsizetype>(key.size()))).toDouble();
    };
    return static_cast<int>(std::to_underlying(
        locationChipFitFrom(widthOf, widths.value(QStringLiteral("available")).toDouble())));
}

/// @param parts The widths the band reports by their keys (window::BandWidthParts).
/// @return What the band is at least wide (window::bandMinimumWidth()).
[[nodiscard]] inline double bandMinimumWidthOfParts(QVariantMap const& parts)
{
    return bandMinimumWidthFrom(
        [&](std::string_view key) { return parts.value(QString::fromUtf8(key)).toDouble(); });
}

} // namespace contour::window
