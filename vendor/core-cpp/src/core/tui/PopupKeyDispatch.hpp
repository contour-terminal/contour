// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/InputEvent.hpp>

#include <cstdint>

namespace core::tui
{

/// @brief Navigation actions recognized by popup list components.
enum class PopupNavAction : std::uint8_t
{
    None,        ///< Key not recognized as a navigation action.
    SelectNext,  ///< Move selection down (Down, Ctrl+J).
    SelectPrev,  ///< Move selection up (Up, Ctrl+K).
    PageDown,    ///< Move selection down by one page.
    PageUp,      ///< Move selection up by one page.
    SelectFirst, ///< Jump to first item (Home).
    SelectLast,  ///< Jump to last item (End).
};

/// @brief Classifies a key event as a popup navigation action.
///
/// Recognizes the standard navigation keys shared by all popup components:
/// Up/Ctrl+K (prev), Down/Ctrl+J (next), PageUp/PageDown, Home/End.
///
/// @param key The key event to classify.
/// @return The navigation action, or PopupNavAction::None if not a navigation key.
[[nodiscard]] inline PopupNavAction classifyNavigationKey(KeyEvent const& key) noexcept
{
    auto const mods = withoutLockKeys(key.modifiers);

    if (key.key == KeyCode::Up || (key.codepoint == 'k' && hasModifier(mods, Modifier::Ctrl)))
        return PopupNavAction::SelectPrev;

    if (key.key == KeyCode::Down || (key.codepoint == 'j' && hasModifier(mods, Modifier::Ctrl)))
        return PopupNavAction::SelectNext;

    if (key.key == KeyCode::PageUp)
        return PopupNavAction::PageUp;

    if (key.key == KeyCode::PageDown)
        return PopupNavAction::PageDown;

    if (key.key == KeyCode::Home)
        return PopupNavAction::SelectFirst;

    if (key.key == KeyCode::End)
        return PopupNavAction::SelectLast;

    return PopupNavAction::None;
}

} // namespace core::tui
