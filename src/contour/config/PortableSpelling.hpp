// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/BindingCollisions.hpp>
#include <contour/config/KeyboardPlatformKind.hpp>
#include <contour/config/ModifierNames.hpp>

#include <vtbackend/input/InputBinding.hpp>
#include <vtbackend/input/InputGenerator.hpp>

#include <algorithm>
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
    return spelling == PrimarySpelling::Canonical ? canonicalModifierSpellings(modifiers, kind)
                                                  : literalModifierSpellings(modifiers);
}

/// The modifiers a row means on @p reader when a configuration written on @p writer spells it @p spelling.
/// @param modifiers The row's modifiers on @p writer.
/// @param writer The keyboard platform the configuration is written for.
/// @param spelling How the row is spelled.
/// @param reader The keyboard platform reading the configuration.
/// @return The union of what each written spelling denotes on @p reader.
[[nodiscard]] inline vtbackend::Modifiers modifiersReadOn(vtbackend::Modifiers modifiers,
                                                          KeyboardPlatformKind writer,
                                                          PrimarySpelling spelling,
                                                          KeyboardPlatformKind reader)
{
    auto result = vtbackend::Modifiers {};
    for (auto const name: modifierSpellings(modifiers, writer, spelling))
        result |= parseModifierSpelling(name, reader).value_or(vtbackend::Modifiers {});
    return result;
}

/// How the writer spells each row of @p rows, written on @p kind.
///
/// `Primary` is what lets one contour.yml work on every machine, but it translates each row on its own: two
/// rows that differ on the host can land on one chord elsewhere -- a Mac's Command+W and Shift+Command+W both
/// read back as Ctrl+Shift+W on the desktop, where the loader folds them into one binding that runs both. So
/// of two rows that would fold on some other platform, one that is spelled with `Primary` is written
/// literally instead -- the later one where both are, so that the earlier keeps its portable spelling --
/// until no two rows fold.
/// @param rows The table, in the order it is written.
/// @param kind The keyboard platform the configuration is written for.
/// @return One spelling per row, in the same order.
template <typename Input>
[[nodiscard]] std::vector<PrimarySpelling> portablePrimarySpellings(
    std::span<vtbackend::InputBinding<Input, ActionList> const> rows, KeyboardPlatformKind kind)
{
    auto result = std::vector<PrimarySpelling>(rows.size(), PrimarySpelling::Canonical);
    auto const folds = [&](std::size_t a, std::size_t b) {
        return canFireTogether(rows[a], rows[b])
               && std::ranges::any_of(AllKeyboardPlatformKinds, [&](KeyboardPlatformKind reader) {
                      return reader != kind
                             && modifiersReadOn(rows[a].modifiers, kind, result[a], reader)
                                    == modifiersReadOn(rows[b].modifiers, kind, result[b], reader);
                  });
    };
    auto const respellable = [&](std::size_t index) {
        return result[index] == PrimarySpelling::Canonical
               && modifierSpellings(rows[index].modifiers, kind, PrimarySpelling::Canonical)
                      != literalModifierSpellings(rows[index].modifiers);
    };
    // Each pass only turns Canonical into Literal, so this ends after at most one pass per row.
    auto changed = true;
    while (changed)
    {
        changed = false;
        for (auto const later: std::views::iota(std::size_t { 0 }, rows.size()))
            for (auto const earlier: std::views::iota(std::size_t { 0 }, later))
            {
                if (!folds(earlier, later))
                    continue;
                auto const victim = respellable(later) ? later : earlier;
                if (respellable(victim))
                {
                    result[victim] = PrimarySpelling::Literal;
                    changed = true;
                }
            }
    }
    return result;
}

} // namespace contour::config
