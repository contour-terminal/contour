// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace core::tui
{

/// @brief Bitmask enumeration for keyboard modifier keys.
///
/// Bit layout follows Kitty keyboard protocol:
/// - Bits 0-3: Standard modifiers (Shift, Alt, Ctrl, Super)
/// - Bits 4-5: Hyper, Meta (rarely used, not implemented)
/// - Bits 6-7: Lock keys (CapsLock, NumLock)
enum class Modifier : std::uint8_t
{
    None = 0,
    Shift = 1 << 0,
    Alt = 1 << 1,
    Ctrl = 1 << 2,
    Super = 1 << 3,
    // Hyper = 1 << 4,  // Rarely used
    // Meta = 1 << 5,   // Rarely used
    CapsLock = 1 << 6,
    NumLock = 1 << 7,
};

/// @brief Bitwise OR for combining modifiers.
[[nodiscard]] constexpr auto operator|(Modifier lhs, Modifier rhs) noexcept -> Modifier
{
    return static_cast<Modifier>(static_cast<std::uint8_t>(lhs) | static_cast<std::uint8_t>(rhs));
}

/// @brief Bitwise AND for testing modifiers.
[[nodiscard]] constexpr auto operator&(Modifier lhs, Modifier rhs) noexcept -> Modifier
{
    return static_cast<Modifier>(static_cast<std::uint8_t>(lhs) & static_cast<std::uint8_t>(rhs));
}

/// @brief Bitwise OR assignment for combining modifiers.
constexpr auto operator|=(Modifier& lhs, Modifier rhs) noexcept -> Modifier&
{
    lhs = lhs | rhs;
    return lhs;
}

/// @brief Bitwise NOT for clearing modifier flags (e.g. @c mods & ~Modifier::Shift).
[[nodiscard]] constexpr auto operator~(Modifier value) noexcept -> Modifier
{
    return static_cast<Modifier>(~static_cast<std::uint8_t>(value));
}

/// @brief Bitwise AND assignment for masking modifiers.
constexpr auto operator&=(Modifier& lhs, Modifier rhs) noexcept -> Modifier&
{
    lhs = lhs & rhs;
    return lhs;
}

/// @brief Tests whether a modifier flag is set.
/// @param mods The modifier bitmask to test.
/// @param flag The flag to check for.
/// @return True if the flag is set.
[[nodiscard]] constexpr auto hasModifier(Modifier mods, Modifier flag) noexcept -> bool
{
    return (mods & flag) != Modifier::None;
}

/// @brief Strips lock key modifiers (CapsLock, NumLock) from a modifier bitmask.
///
/// Lock keys are reported by the Kitty keyboard protocol but should be ignored
/// when matching modifier combinations for keybindings and special key detection.
/// @param mods The modifier bitmask to strip lock keys from.
/// @return The modifier bitmask with CapsLock and NumLock bits cleared.
[[nodiscard]] constexpr auto withoutLockKeys(Modifier mods) noexcept -> Modifier
{
    return static_cast<Modifier>(
        static_cast<std::uint8_t>(mods)
        & ~(static_cast<std::uint8_t>(Modifier::CapsLock) | static_cast<std::uint8_t>(Modifier::NumLock)));
}

} // namespace core::tui
