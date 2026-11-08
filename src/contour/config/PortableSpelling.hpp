// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/BindingCollisions.hpp>
#include <contour/config/KeyboardPlatformKind.hpp>
#include <contour/config/ModifierNames.hpp>

#include <vtbackend/input/InputBinding.hpp>
#include <vtbackend/input/InputGenerator.hpp>

#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

namespace contour::config
{

/// How a written binding row spells its modifiers.
enum class PrimarySpelling : uint8_t
{
    Canonical =
        0,   ///< `Primary` where the chord holds the host's Primary set (@see canonicalModifierSpellings).
    Literal, ///< Every modifier by its own name, so the row means the same chord on every platform.
};

/// The spellings a writer emits for @p modifiers on @p kind under @p spelling.
/// @param modifiers The chord's modifiers.
/// @param kind The keyboard platform the configuration is written for.
/// @param spelling Whether `Primary` may be used.
/// @return The spellings, in the order they are written.
[[nodiscard]] inline std::vector<std::string_view> modifierSpellings(vtbackend::Modifiers modifiers,
                                                                     KeyboardPlatformKind kind,
                                                                     PrimarySpelling spelling)
{
    if (spelling == PrimarySpelling::Canonical)
        return canonicalModifierSpellings(modifiers, kind);
    auto result = std::vector<std::string_view> {};
    for (auto const& row: vtbackend::ChordModifierTable)
        if (modifiers.contains(row.modifier))
            result.push_back(row.name);
    return result;
}

/// The modifiers @p spellings mean when a configuration is read on @p kind.
/// @param spellings The written spellings, each one the writer emits (so each parses).
/// @param kind The keyboard platform reading them.
/// @return The union of what each spelling denotes there.
[[nodiscard]] inline vtbackend::Modifiers readBack(std::span<std::string_view const> spellings,
                                                   KeyboardPlatformKind kind)
{
    auto result = vtbackend::Modifiers {};
    for (auto const spelling: spellings)
        result |= parseModifierSpelling(spelling, kind).value_or(vtbackend::Modifiers {});
    return result;
}

/// How the writer spells each row of @p rows, written on @p kind.
///
/// `Primary` is what lets one contour.yml work on every machine, but it is a per-row translation: two rows
/// that differ on the host can land on one chord elsewhere -- Command+W and Shift+Command+W both read back
/// as Ctrl+Shift+W on the desktop, where the loader would merge them and run both. So a row that would land,
/// on some other platform, on the chord an earlier row already holds there is written literally instead,
/// and the earlier row keeps its portable spelling.
/// @param rows The table, in the order it is written.
/// @param kind The keyboard platform the configuration is written for.
/// @return One spelling per row, in the same order.
template <typename Input>
[[nodiscard]] std::vector<PrimarySpelling> portablePrimarySpellings(
    std::span<vtbackend::InputBinding<Input, ActionList> const> rows, KeyboardPlatformKind kind)
{
    auto result = std::vector<PrimarySpelling>(rows.size(), PrimarySpelling::Canonical);
    auto const chordOn = [&](std::size_t index, KeyboardPlatformKind reader) {
        auto const spellings = modifierSpellings(rows[index].modifiers, kind, result[index]);
        return readBack(spellings, reader);
    };
    for (auto const index: std::views::iota(std::size_t { 0 }, rows.size()))
        for (auto const readerIndex: std::views::iota(std::size_t { 0 }, KeyboardPlatformKindCount))
        {
            auto const reader = static_cast<KeyboardPlatformKind>(readerIndex);
            if (reader == kind || result[index] == PrimarySpelling::Literal)
                continue;
            auto const& row = rows[index];
            auto const folds =
                std::ranges::any_of(std::views::iota(std::size_t { 0 }, index), [&](auto earlier) {
                    auto const& other = rows[earlier];
                    return other.input == row.input && other.clickCount == row.clickCount
                           && modesOverlap(other.modes, row.modes)
                           && chordOn(earlier, reader) == chordOn(index, reader);
                });
            if (folds)
                result[index] = PrimarySpelling::Literal;
        }
    return result;
}

} // namespace contour::config
