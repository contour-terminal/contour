// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The words the chrome says about a served tab, translated: the strip in its pane's header and the
/// "Served by …" its tab describes itself with. The decision is window::servedChromeOf's; this only phrases
/// it.

#include <contour/config/UiStyle.hpp>
#include <contour/window/ServedChrome.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QString>

namespace contour::window
{

/// @param chrome How the chrome marks a tab (servedChromeOf).
/// @return "Served by {title}", translated, for a served tab; empty for any other.
[[nodiscard]] inline QString servedByText(ServedChrome const& chrome)
{
    if (chrome.marker == TabMarker::None)
        return {};
    return QCoreApplication::translate("ServedChrome", "Served by %1")
        .arg(QString::fromStdString(chrome.title));
}

/// @param chrome How the chrome marks a tab (servedChromeOf).
/// @return The pane header's strip, translated -- `◈ {title} · {extension}, module {digest} · draws this
/// tab`,
///         without the module for an unknown digest -- for a served tab; empty for any other.
[[nodiscard]] inline QString servedStripText(ServedChrome const& chrome)
{
    if (chrome.marker == TabMarker::None)
        return {};
    auto const glyph = QString::fromUtf8(config::ServedTabGlyph);
    auto const title = QString::fromStdString(chrome.title);
    auto const extension = QString::fromStdString(chrome.extension);
    if (chrome.digest.empty())
        return QCoreApplication::translate("ServedChrome", "%1 %2 · %3 · draws this tab")
            .arg(glyph, title, extension);
    return QCoreApplication::translate("ServedChrome", "%1 %2 · %3, module %4 · draws this tab")
        .arg(glyph, title, extension, QString::fromStdString(chrome.digest));
}

} // namespace contour::window
