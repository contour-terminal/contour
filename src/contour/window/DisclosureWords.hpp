// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a screen reader is told of a control that opens and closes (Qt Quick's Accessible has no expanded
/// state for a button or a tree item): the one translatable pair every such control says, so a language
/// translates it once.

#include <contour/window/TranslateNoop.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace contour::window
{

/// Whether the thing a control opens is on screen.
enum class Disclosure : uint8_t
{
    Collapsed = 0, ///< It is closed, or its children are hidden.
    Expanded,      ///< It is open, or its children show.
};

/// The translation context the disclosure words are collected and translated in.
inline constexpr auto DisclosureContext = std::string_view { "Disclosure" };

/// The words of every disclosure state, indexed by @c Disclosure.
inline constexpr std::array<std::string_view, 2> DisclosureWords {
    QT_TRANSLATE_NOOP("Disclosure", "collapsed"), QT_TRANSLATE_NOOP("Disclosure", "expanded")
};
static_assert(std::to_underlying(Disclosure::Expanded) + 1 == DisclosureWords.size());

/// @param disclosure A disclosure state.
/// @return Its source-language word, to translate in DisclosureContext.
[[nodiscard]] constexpr std::string_view disclosureWord(Disclosure disclosure) noexcept
{
    return DisclosureWords[std::to_underlying(disclosure)];
}

} // namespace contour::window
