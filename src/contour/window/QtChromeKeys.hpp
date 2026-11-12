// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Qt's key numbering and modifier flags translated into the chrome's own (ChromeKeys.hpp): the one table
/// the window controller and the test controllers both read, so a key the chrome answers is a key every
/// controller hands it.

#include <contour/window/ChromeKeys.hpp>

#include <Qt>
#include <algorithm>
#include <array>

namespace contour::window
{

/// A Qt key and the chrome key it is.
struct QtChromeKeyRow
{
    Qt::Key qt;    ///< Qt's key.
    ChromeKey key; ///< The chrome's.
};

/// Every Qt key the chrome has a name for; any other is ChromeKey::Other.
inline constexpr auto QtChromeKeyRows = std::array {
    QtChromeKeyRow { .qt = Qt::Key_Left, .key = ChromeKey::Left },
    QtChromeKeyRow { .qt = Qt::Key_Right, .key = ChromeKey::Right },
    QtChromeKeyRow { .qt = Qt::Key_Up, .key = ChromeKey::Up },
    QtChromeKeyRow { .qt = Qt::Key_Down, .key = ChromeKey::Down },
    QtChromeKeyRow { .qt = Qt::Key_Home, .key = ChromeKey::Home },
    QtChromeKeyRow { .qt = Qt::Key_End, .key = ChromeKey::End },
    QtChromeKeyRow { .qt = Qt::Key_Return, .key = ChromeKey::Return },
    QtChromeKeyRow { .qt = Qt::Key_Enter, .key = ChromeKey::Return },
    QtChromeKeyRow { .qt = Qt::Key_Space, .key = ChromeKey::Space },
    QtChromeKeyRow { .qt = Qt::Key_Backspace, .key = ChromeKey::Backspace },
    QtChromeKeyRow { .qt = Qt::Key_Delete, .key = ChromeKey::Delete },
    QtChromeKeyRow { .qt = Qt::Key_Escape, .key = ChromeKey::Escape },
    QtChromeKeyRow { .qt = Qt::Key_Tab, .key = ChromeKey::Tab },
    QtChromeKeyRow { .qt = Qt::Key_Backtab, .key = ChromeKey::Tab },
    QtChromeKeyRow { .qt = Qt::Key_F2, .key = ChromeKey::F2 },
    QtChromeKeyRow { .qt = Qt::Key_F10, .key = ChromeKey::F10 },
    QtChromeKeyRow { .qt = Qt::Key_Menu, .key = ChromeKey::Menu },
};

/// @param qtKey A Qt key code.
/// @return The chrome key it is; ChromeKey::Other for a key the chrome has no name for.
[[nodiscard]] constexpr ChromeKey chromeKeyOf(int qtKey) noexcept
{
    auto const row =
        std::ranges::find(QtChromeKeyRows, qtKey, [](auto const& r) { return static_cast<int>(r.qt); });
    return row != QtChromeKeyRows.end() ? row->key : ChromeKey::Other;
}

/// Qt's keypad flag marks where a key sits, not a chord; the chrome ignores it.
/// @param modifiers The modifier flags a key arrived with.
/// @return The chrome's modifier state.
[[nodiscard]] constexpr ChromeKeyModifier chromeModifierOf(Qt::KeyboardModifiers modifiers) noexcept
{
    modifiers &= ~Qt::KeypadModifier;
    if (modifiers == Qt::NoModifier)
        return ChromeKeyModifier::None;
    if (modifiers == Qt::AltModifier)
        return ChromeKeyModifier::Alt;
    if (modifiers == Qt::ShiftModifier)
        return ChromeKeyModifier::Shift;
    return ChromeKeyModifier::Other;
}

} // namespace contour::window
