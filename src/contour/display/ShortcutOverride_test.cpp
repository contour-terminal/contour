// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the shortcut-override claim: which key chords a menu may take from the terminal. Every
// event is built from a standard key's own binding on this platform, so nothing here hard-codes ⌘Q.

#include <contour/display/ShortcutOverride.hpp>

#include <QtCore/QEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QKeySequence>

#include <catch2/catch_test_macros.hpp>

#include <array>

using contour::command::MenuBarPlacement;
using contour::command::MenuItemRole;
using contour::display::shortcutOverrideClaim;
using contour::display::ShortcutOverrideClaim;
using contour::display::standardKeyFor;

namespace
{

/// The claim on a ShortcutOverride event for the first chord of @p sequence.
/// @param sequence The chord, as a key sequence.
/// @param placement Where the menu bar lives.
/// @return Who the chord belongs to.
ShortcutOverrideClaim claimFor(QKeySequence const& sequence, MenuBarPlacement placement)
{
    auto const combo = sequence[0];
    QKeyEvent event(QEvent::ShortcutOverride, combo.key(), combo.keyboardModifiers());
    return shortcutOverrideClaim(event, placement);
}

/// The standard keys a menu owns.
constexpr auto MenuKeys = std::array { QKeySequence::Quit, QKeySequence::Preferences };

} // namespace

TEST_CASE("ShortcutOverride: the menu-owned keys go to a global menu bar only", "[shortcutoverride]")
{
#if defined(__APPLE__)
    // Where the claim matters, the Menu cases must not pass vacuously.
    REQUIRE_FALSE(QKeySequence(QKeySequence::Quit).isEmpty());
    REQUIRE_FALSE(QKeySequence(QKeySequence::Preferences).isEmpty());
#endif
    for (auto const key: MenuKeys)
    {
        auto const sequence = QKeySequence(key);
        if (sequence.isEmpty())
            continue; // This platform binds no chord to the key.
        CHECK(claimFor(sequence, MenuBarPlacement::Global) == ShortcutOverrideClaim::Menu);
        CHECK(claimFor(sequence, MenuBarPlacement::None) == ShortcutOverrideClaim::Terminal);
    }
}

TEST_CASE("ShortcutOverride: every other chord stays with the terminal under a global menu bar",
          "[shortcutoverride]")
{
    auto const copy = QKeySequence(QKeySequence::Copy);
    if (!copy.isEmpty())
        CHECK(claimFor(copy, MenuBarPlacement::Global) == ShortcutOverrideClaim::Terminal);
    CHECK(claimFor(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_K), MenuBarPlacement::Global)
          == ShortcutOverrideClaim::Terminal);
    CHECK(claimFor(QKeySequence(Qt::Key_A), MenuBarPlacement::Global) == ShortcutOverrideClaim::Terminal);
}

TEST_CASE("ShortcutOverride: the role-to-key table gives Quit and Preferences their standard keys",
          "[shortcutoverride]")
{
    CHECK(standardKeyFor(MenuItemRole::Quit) == QKeySequence::Quit);
    CHECK(standardKeyFor(MenuItemRole::Preferences) == QKeySequence::Preferences);
    CHECK_FALSE(standardKeyFor(MenuItemRole::About).has_value());
    CHECK_FALSE(standardKeyFor(MenuItemRole::None).has_value());
}
