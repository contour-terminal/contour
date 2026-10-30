// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The keys and modifiers a global hotkey may use, and how each platform spells them. Qt-free and
/// dependency-free, so every row is table-tested in contour_test.

#include <contour/config/HotkeyChord.hpp>

#include <vtbackend/input/InputGenerator.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace contour::platform
{

/// One key a global hotkey may end on, with its code on each platform.
struct HotkeyKeyRow
{
    std::variant<vtbackend::Key, char32_t> key {}; ///< The key; letters are listed in lower case.
    std::uint16_t macVirtualKey = 0;               ///< HIToolbox's virtual key code (Events.h, kVK_*).
    std::uint16_t windowsVirtualKey = 0;           ///< WinUser.h's virtual-key code (VK_*).
    std::string_view xdgName {};                   ///< The XKB keysym name the GlobalShortcuts portal takes.
};

/// Every key a global hotkey may end on: A-Z, 0-9, the backquote, Space, Return, Tab, Escape, F1-F20 and
/// the four arrows. Adding a key is adding a row.
inline constexpr auto HotkeyKeyRows = std::array {
    HotkeyKeyRow { .key = U'a', .macVirtualKey = 0x00, .windowsVirtualKey = 0x41, .xdgName = "a" },
    HotkeyKeyRow { .key = U'b', .macVirtualKey = 0x0B, .windowsVirtualKey = 0x42, .xdgName = "b" },
    HotkeyKeyRow { .key = U'c', .macVirtualKey = 0x08, .windowsVirtualKey = 0x43, .xdgName = "c" },
    HotkeyKeyRow { .key = U'd', .macVirtualKey = 0x02, .windowsVirtualKey = 0x44, .xdgName = "d" },
    HotkeyKeyRow { .key = U'e', .macVirtualKey = 0x0E, .windowsVirtualKey = 0x45, .xdgName = "e" },
    HotkeyKeyRow { .key = U'f', .macVirtualKey = 0x03, .windowsVirtualKey = 0x46, .xdgName = "f" },
    HotkeyKeyRow { .key = U'g', .macVirtualKey = 0x05, .windowsVirtualKey = 0x47, .xdgName = "g" },
    HotkeyKeyRow { .key = U'h', .macVirtualKey = 0x04, .windowsVirtualKey = 0x48, .xdgName = "h" },
    HotkeyKeyRow { .key = U'i', .macVirtualKey = 0x22, .windowsVirtualKey = 0x49, .xdgName = "i" },
    HotkeyKeyRow { .key = U'j', .macVirtualKey = 0x26, .windowsVirtualKey = 0x4A, .xdgName = "j" },
    HotkeyKeyRow { .key = U'k', .macVirtualKey = 0x28, .windowsVirtualKey = 0x4B, .xdgName = "k" },
    HotkeyKeyRow { .key = U'l', .macVirtualKey = 0x25, .windowsVirtualKey = 0x4C, .xdgName = "l" },
    HotkeyKeyRow { .key = U'm', .macVirtualKey = 0x2E, .windowsVirtualKey = 0x4D, .xdgName = "m" },
    HotkeyKeyRow { .key = U'n', .macVirtualKey = 0x2D, .windowsVirtualKey = 0x4E, .xdgName = "n" },
    HotkeyKeyRow { .key = U'o', .macVirtualKey = 0x1F, .windowsVirtualKey = 0x4F, .xdgName = "o" },
    HotkeyKeyRow { .key = U'p', .macVirtualKey = 0x23, .windowsVirtualKey = 0x50, .xdgName = "p" },
    HotkeyKeyRow { .key = U'q', .macVirtualKey = 0x0C, .windowsVirtualKey = 0x51, .xdgName = "q" },
    HotkeyKeyRow { .key = U'r', .macVirtualKey = 0x0F, .windowsVirtualKey = 0x52, .xdgName = "r" },
    HotkeyKeyRow { .key = U's', .macVirtualKey = 0x01, .windowsVirtualKey = 0x53, .xdgName = "s" },
    HotkeyKeyRow { .key = U't', .macVirtualKey = 0x11, .windowsVirtualKey = 0x54, .xdgName = "t" },
    HotkeyKeyRow { .key = U'u', .macVirtualKey = 0x20, .windowsVirtualKey = 0x55, .xdgName = "u" },
    HotkeyKeyRow { .key = U'v', .macVirtualKey = 0x09, .windowsVirtualKey = 0x56, .xdgName = "v" },
    HotkeyKeyRow { .key = U'w', .macVirtualKey = 0x0D, .windowsVirtualKey = 0x57, .xdgName = "w" },
    HotkeyKeyRow { .key = U'x', .macVirtualKey = 0x07, .windowsVirtualKey = 0x58, .xdgName = "x" },
    HotkeyKeyRow { .key = U'y', .macVirtualKey = 0x10, .windowsVirtualKey = 0x59, .xdgName = "y" },
    HotkeyKeyRow { .key = U'z', .macVirtualKey = 0x06, .windowsVirtualKey = 0x5A, .xdgName = "z" },
    HotkeyKeyRow { .key = U'0', .macVirtualKey = 0x1D, .windowsVirtualKey = 0x30, .xdgName = "0" },
    HotkeyKeyRow { .key = U'1', .macVirtualKey = 0x12, .windowsVirtualKey = 0x31, .xdgName = "1" },
    HotkeyKeyRow { .key = U'2', .macVirtualKey = 0x13, .windowsVirtualKey = 0x32, .xdgName = "2" },
    HotkeyKeyRow { .key = U'3', .macVirtualKey = 0x14, .windowsVirtualKey = 0x33, .xdgName = "3" },
    HotkeyKeyRow { .key = U'4', .macVirtualKey = 0x15, .windowsVirtualKey = 0x34, .xdgName = "4" },
    HotkeyKeyRow { .key = U'5', .macVirtualKey = 0x17, .windowsVirtualKey = 0x35, .xdgName = "5" },
    HotkeyKeyRow { .key = U'6', .macVirtualKey = 0x16, .windowsVirtualKey = 0x36, .xdgName = "6" },
    HotkeyKeyRow { .key = U'7', .macVirtualKey = 0x1A, .windowsVirtualKey = 0x37, .xdgName = "7" },
    HotkeyKeyRow { .key = U'8', .macVirtualKey = 0x1C, .windowsVirtualKey = 0x38, .xdgName = "8" },
    HotkeyKeyRow { .key = U'9', .macVirtualKey = 0x19, .windowsVirtualKey = 0x39, .xdgName = "9" },
    HotkeyKeyRow { .key = U'`', .macVirtualKey = 0x32, .windowsVirtualKey = 0xC0, .xdgName = "grave" },
    HotkeyKeyRow { .key = U' ', .macVirtualKey = 0x31, .windowsVirtualKey = 0x20, .xdgName = "space" },
    HotkeyKeyRow {
        .key = vtbackend::Key::Enter, .macVirtualKey = 0x24, .windowsVirtualKey = 0x0D, .xdgName = "Return" },
    HotkeyKeyRow {
        .key = vtbackend::Key::Tab, .macVirtualKey = 0x30, .windowsVirtualKey = 0x09, .xdgName = "Tab" },
    HotkeyKeyRow { .key = vtbackend::Key::Escape,
                   .macVirtualKey = 0x35,
                   .windowsVirtualKey = 0x1B,
                   .xdgName = "Escape" },
    HotkeyKeyRow {
        .key = vtbackend::Key::F1, .macVirtualKey = 0x7A, .windowsVirtualKey = 0x70, .xdgName = "F1" },
    HotkeyKeyRow {
        .key = vtbackend::Key::F2, .macVirtualKey = 0x78, .windowsVirtualKey = 0x71, .xdgName = "F2" },
    HotkeyKeyRow {
        .key = vtbackend::Key::F3, .macVirtualKey = 0x63, .windowsVirtualKey = 0x72, .xdgName = "F3" },
    HotkeyKeyRow {
        .key = vtbackend::Key::F4, .macVirtualKey = 0x76, .windowsVirtualKey = 0x73, .xdgName = "F4" },
    HotkeyKeyRow {
        .key = vtbackend::Key::F5, .macVirtualKey = 0x60, .windowsVirtualKey = 0x74, .xdgName = "F5" },
    HotkeyKeyRow {
        .key = vtbackend::Key::F6, .macVirtualKey = 0x61, .windowsVirtualKey = 0x75, .xdgName = "F6" },
    HotkeyKeyRow {
        .key = vtbackend::Key::F7, .macVirtualKey = 0x62, .windowsVirtualKey = 0x76, .xdgName = "F7" },
    HotkeyKeyRow {
        .key = vtbackend::Key::F8, .macVirtualKey = 0x64, .windowsVirtualKey = 0x77, .xdgName = "F8" },
    HotkeyKeyRow {
        .key = vtbackend::Key::F9, .macVirtualKey = 0x65, .windowsVirtualKey = 0x78, .xdgName = "F9" },
    HotkeyKeyRow {
        .key = vtbackend::Key::F10, .macVirtualKey = 0x6D, .windowsVirtualKey = 0x79, .xdgName = "F10" },
    HotkeyKeyRow {
        .key = vtbackend::Key::F11, .macVirtualKey = 0x67, .windowsVirtualKey = 0x7A, .xdgName = "F11" },
    HotkeyKeyRow {
        .key = vtbackend::Key::F12, .macVirtualKey = 0x6F, .windowsVirtualKey = 0x7B, .xdgName = "F12" },
    HotkeyKeyRow {
        .key = vtbackend::Key::F13, .macVirtualKey = 0x69, .windowsVirtualKey = 0x7C, .xdgName = "F13" },
    HotkeyKeyRow {
        .key = vtbackend::Key::F14, .macVirtualKey = 0x6B, .windowsVirtualKey = 0x7D, .xdgName = "F14" },
    HotkeyKeyRow {
        .key = vtbackend::Key::F15, .macVirtualKey = 0x71, .windowsVirtualKey = 0x7E, .xdgName = "F15" },
    HotkeyKeyRow {
        .key = vtbackend::Key::F16, .macVirtualKey = 0x6A, .windowsVirtualKey = 0x7F, .xdgName = "F16" },
    HotkeyKeyRow {
        .key = vtbackend::Key::F17, .macVirtualKey = 0x40, .windowsVirtualKey = 0x80, .xdgName = "F17" },
    HotkeyKeyRow {
        .key = vtbackend::Key::F18, .macVirtualKey = 0x4F, .windowsVirtualKey = 0x81, .xdgName = "F18" },
    HotkeyKeyRow {
        .key = vtbackend::Key::F19, .macVirtualKey = 0x50, .windowsVirtualKey = 0x82, .xdgName = "F19" },
    HotkeyKeyRow {
        .key = vtbackend::Key::F20, .macVirtualKey = 0x5A, .windowsVirtualKey = 0x83, .xdgName = "F20" },
    HotkeyKeyRow { .key = vtbackend::Key::LeftArrow,
                   .macVirtualKey = 0x7B,
                   .windowsVirtualKey = 0x25,
                   .xdgName = "Left" },
    HotkeyKeyRow {
        .key = vtbackend::Key::UpArrow, .macVirtualKey = 0x7E, .windowsVirtualKey = 0x26, .xdgName = "Up" },
    HotkeyKeyRow { .key = vtbackend::Key::RightArrow,
                   .macVirtualKey = 0x7C,
                   .windowsVirtualKey = 0x27,
                   .xdgName = "Right" },
    HotkeyKeyRow { .key = vtbackend::Key::DownArrow,
                   .macVirtualKey = 0x7D,
                   .windowsVirtualKey = 0x28,
                   .xdgName = "Down" },
};

/// Finds a key's row.
/// @param key The key or character of a chord; letters match in either case.
/// @return The row, or nullptr when the key has none.
[[nodiscard]] constexpr HotkeyKeyRow const* hotkeyKeyRow(
    std::variant<vtbackend::Key, char32_t> const& key) noexcept
{
    auto folded = key;
    if (auto* const character = std::get_if<char32_t>(&folded);
        character && *character >= U'A' && *character <= U'Z')
        *character = *character - U'A' + U'a';
    for (auto const& row: HotkeyKeyRows)
        if (row.key == folded)
            return &row;
    return nullptr;
}

/// One modifier a global hotkey may hold, with its mask on each platform.
struct HotkeyModifierRow
{
    vtbackend::Modifier modifier {}; ///< The modifier.
    std::uint32_t macMask = 0;       ///< Carbon's modifier mask (controlKey, optionKey, shiftKey, cmdKey).
    std::uint32_t windowsMask = 0;   ///< RegisterHotKey's MOD_* flag.
    std::string_view xdgName {};     ///< The GlobalShortcuts portal's spelling.
};

/// The modifiers, in the order the portal's trigger lists them. On macOS Control is the control key and
/// Super is Command, under Qt::AA_MacDontSwapCtrlAndMeta.
inline constexpr auto HotkeyModifierRows = std::array {
    HotkeyModifierRow {
        .modifier = vtbackend::Modifier::Control, .macMask = 0x1000, .windowsMask = 0x2, .xdgName = "CTRL" },
    HotkeyModifierRow {
        .modifier = vtbackend::Modifier::Alt, .macMask = 0x0800, .windowsMask = 0x1, .xdgName = "ALT" },
    HotkeyModifierRow {
        .modifier = vtbackend::Modifier::Shift, .macMask = 0x0200, .windowsMask = 0x4, .xdgName = "SHIFT" },
    HotkeyModifierRow {
        .modifier = vtbackend::Modifier::Super, .macMask = 0x0100, .windowsMask = 0x8, .xdgName = "LOGO" },
};

/// Spells a chord as the GlobalShortcuts portal takes a preferred trigger, such as "CTRL+grave".
/// @param chord The chord.
/// @return The trigger, or an empty string when the chord's key has no row.
[[nodiscard]] inline std::string xdgTrigger(config::HotkeyChord const& chord)
{
    auto const* const keyRow = hotkeyKeyRow(chord.key);
    if (keyRow == nullptr)
        return {};
    auto trigger = std::string {};
    for (auto const& modifierRow: HotkeyModifierRows)
    {
        if (!chord.modifiers.contains(modifierRow.modifier))
            continue;
        trigger += modifierRow.xdgName;
        trigger += '+';
    }
    trigger += keyRow->xdgName;
    return trigger;
}

} // namespace contour::platform
