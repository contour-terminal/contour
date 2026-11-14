// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The decision behind the macOS native key trace: would a menu item's key equivalent take a key press?
/// Plain values in, an enum out, so it is table-tested on every host; the Cocoa adapter only gathers the
/// values.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace contour::platform
{

/// NSEventModifierFlags bits, as AppKit defines them (stable ABI values).
namespace nsmod
{
    constexpr uint64_t Shift = uint64_t { 1 } << 17;
    constexpr uint64_t Control = Shift << 1;    // 1 << 18
    constexpr uint64_t Option = Shift << 2;     // 1 << 19
    constexpr uint64_t Command = Shift << 3;    // 1 << 20
    constexpr uint64_t NumericPad = Shift << 4; // 1 << 21
    constexpr uint64_t Function = Shift << 6;   // 1 << 23
    /// Device-independent bits a key equivalent is compared on. NumericPad is left out: AppKit sets it on
    /// every arrow-key event and no menu item names it.
    constexpr uint64_t Compared = Shift | Control | Option | Command | Function;
} // namespace nsmod

/// How a menu item's key equivalent relates to a key event.
enum class KeyEquivalentMatch : uint8_t
{
    None = 0,         ///< The item would not take the event.
    Strict,           ///< Same character and same modifiers, Function included.
    IgnoringFunction, ///< Same character and modifiers once the Function flag is ignored on both sides.
};

namespace detail
{
    struct ModifierName
    {
        uint64_t bit;
        std::string_view name;
    };

    constexpr auto ModifierNames = std::array {
        ModifierName { .bit = nsmod::Shift, .name = "shift" },
        ModifierName { .bit = nsmod::Control, .name = "control" },
        ModifierName { .bit = nsmod::Option, .name = "option" },
        ModifierName { .bit = nsmod::Command, .name = "command" },
        ModifierName { .bit = nsmod::Function, .name = "function" },
        ModifierName { .bit = nsmod::NumericPad, .name = "numericPad" },
    };

    [[nodiscard]] constexpr bool isUpperAscii(char32_t c) noexcept
    {
        return c >= U'A' && c <= U'Z';
    }

    [[nodiscard]] constexpr char32_t foldCase(char32_t c) noexcept
    {
        return isUpperAscii(c) ? c - U'A' + U'a' : c;
    }
} // namespace detail

/// Decides whether a menu item's key equivalent matches a key event, by AppKit's rules: characters compare
/// case-insensitively, an uppercase ASCII letter in the item implies Shift, and only the device-independent
/// modifiers count. It approximates AppKit, as a diagnostic needs, and compares the first UTF-16 unit, not a
/// full code point.
/// @param eventChar First UTF-16 unit of the event's charactersIgnoringModifiers.
/// @param eventModifiers The event's NSEventModifierFlags.
/// @param itemChar First UTF-16 unit of the item's keyEquivalent.
/// @param itemModifiers The item's keyEquivalentModifierMask.
/// @return None, Strict, or IgnoringFunction.
[[nodiscard]] inline KeyEquivalentMatch classifyKeyEquivalent(char32_t eventChar,
                                                              uint64_t eventModifiers,
                                                              char32_t itemChar,
                                                              uint64_t itemModifiers) noexcept
{
    if (detail::foldCase(eventChar) != detail::foldCase(itemChar))
        return KeyEquivalentMatch::None;

    auto const eventBits = eventModifiers & nsmod::Compared;
    auto itemBits = itemModifiers & nsmod::Compared;
    if (detail::isUpperAscii(itemChar))
        itemBits |= nsmod::Shift;

    if (eventBits == itemBits)
        return KeyEquivalentMatch::Strict;
    if ((eventBits & ~nsmod::Function) == (itemBits & ~nsmod::Function))
        return KeyEquivalentMatch::IgnoringFunction;
    return KeyEquivalentMatch::None;
}

/// Names the set bits of an NSEventModifierFlags value, e.g. "control|command|function".
/// @param modifiers The flags.
/// @return The names joined by '|', or "none".
[[nodiscard]] inline std::string describeNsModifiers(uint64_t modifiers)
{
    auto result = std::string {};
    for (auto const& entry: detail::ModifierNames)
    {
        if ((modifiers & entry.bit) == 0)
            continue;
        if (!result.empty())
            result += '|';
        result += entry.name;
    }
    return result.empty() ? std::string { "none" } : result;
}

} // namespace contour::platform
