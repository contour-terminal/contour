// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/KeyboardPlatformKind.hpp>

#include <vtbackend/input/InputGenerator.hpp>

#include <crispy/ASCII.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>
#include <vector>

namespace contour::config
{

/// One spelling contour.yml accepts in a `mods:` entry, and the modifier it denotes.
struct ConfigModifierRow
{
    /// The spelling, as written in the config.
    std::string_view name;
    /// What it means.
    vtbackend::Modifier modifier;
};

/// Every spelling contour.yml accepts in a `mods:` entry.
///
/// Synonyms are simply extra rows, so adding one — or adding a modifier — is adding a row here and
/// nowhere else: the parser, the "expected one of" diagnostic and the checks below are all derived
/// from this table.
///
/// This is the third and widest of three modifier tables, and the only one carrying synonyms. The
/// other two are vtbackend::ChordModifierTable (the wire vocabulary, ordered by bit) and
/// ShortcutModifierTable (the UI's display vocabulary, ordered for rendering). The static_asserts
/// below, and the mirror one in Shortcut.h, are what keep the three from drifting apart — which is
/// what issue #1987 was: the UI taught "Ctrl" and the parser demanded "Control".
///
/// The order is the order the diagnostic lists them in, so a synonym reads next to its sibling.
constexpr inline auto ConfigModifierTable = std::array {
    ConfigModifierRow { .name = "Shift", .modifier = vtbackend::Modifier::Shift },
    ConfigModifierRow { .name = "Alt", .modifier = vtbackend::Modifier::Alt },
    ConfigModifierRow { .name = "Control", .modifier = vtbackend::Modifier::Control },
    ConfigModifierRow { .name = "Ctrl", .modifier = vtbackend::Modifier::Control },
    ConfigModifierRow { .name = "Super", .modifier = vtbackend::Modifier::Super },
    // "Meta" denotes SUPER, not Modifier::Meta: contour.yml has spelled the Windows/Command key
    // "Meta" since before Modifier::Meta existed. Re-pointing it would silently move every existing
    // `mods: [Meta]` binding onto a key most keyboards do not have.
    ConfigModifierRow { .name = "Meta", .modifier = vtbackend::Modifier::Super },
    ConfigModifierRow { .name = "Hyper", .modifier = vtbackend::Modifier::Hyper },
};

/// The one chord modifier with no config spelling of its own, because "Meta" is spent on Super.
///
/// The extended CSIu keyboard protocol reports a real, distinct Meta that therefore cannot be bound.
/// @see ConfigModifierTable's "Meta" row.
constexpr inline auto ModifierWithoutConfigSpelling = vtbackend::Modifier::Meta;

namespace detail
{
    /// Compares two modifier spellings case-insensitively. @see crispy::ascii::fold
    [[nodiscard]] constexpr bool sameSpelling(std::string_view a, std::string_view b) noexcept
    {
        return std::ranges::equal(
            a, b, [](char x, char y) { return crispy::ascii::fold(x) == crispy::ascii::fold(y); });
    }
} // namespace detail

/// Parses one `mods:` entry, case-insensitively.
///
/// @param name The spelling as written in the config.
/// @return The modifier @p name denotes, or nullopt when it is not an accepted spelling.
[[nodiscard]] constexpr std::optional<vtbackend::Modifier> parseModifierName(std::string_view name) noexcept
{
    for (auto const& row: ConfigModifierTable)
        if (detail::sameSpelling(name, row.name))
            return row.modifier;
    return std::nullopt;
}

/// The spelling of the platform-neutral primary modifier in a `mods:` entry.
inline constexpr std::string_view PrimaryModifierName = "Primary";

/// The modifiers the `Primary` spelling stands for on @p kind: the key an application's own chords hang on.
/// Command on macOS, where that is Super (Qt's Control/Meta swap is off); Control+Shift elsewhere, where a
/// bare Control is the terminal application's.
/// @param kind The keyboard platform the configuration is loaded for.
/// @return The concrete modifiers.
[[nodiscard]] constexpr vtbackend::Modifiers primaryModifiers(KeyboardPlatformKind kind) noexcept
{
    if (kind == KeyboardPlatformKind::Mac)
        return vtbackend::Modifiers { vtbackend::Modifier::Super };
    return vtbackend::Modifiers { vtbackend::Modifier::Control, vtbackend::Modifier::Shift };
}

/// Parses one `mods:` entry, `Primary` included, case-insensitively.
///
/// @param name The spelling as written in the config.
/// @param kind The keyboard platform `Primary` resolves against.
/// @return The modifiers @p name denotes, or nullopt when it is not an accepted spelling.
[[nodiscard]] constexpr std::optional<vtbackend::Modifiers> parseModifierSpelling(
    std::string_view name, KeyboardPlatformKind kind) noexcept
{
    if (detail::sameSpelling(name, PrimaryModifierName))
        return primaryModifiers(kind);
    if (auto const modifier = parseModifierName(name))
        return vtbackend::Modifiers { *modifier };
    return std::nullopt;
}

/// The spellings a writer emits for @p modifiers on @p kind: "Primary" first when they contain the Primary
/// set, then each remaining modifier by its canonical name. So a row means the same on every platform it is
/// carried to, and a config written on one OS works on another.
/// @param modifiers The chord's modifiers.
/// @param kind The keyboard platform the configuration is written for.
/// @return The spellings, in the order they are written.
[[nodiscard]] inline std::vector<std::string_view> canonicalModifierSpellings(vtbackend::Modifiers modifiers,
                                                                              KeyboardPlatformKind kind)
{
    auto result = std::vector<std::string_view> {};
    auto const primary = primaryModifiers(kind);
    auto const hasPrimary = modifiers.contains(primary);
    if (hasPrimary)
        result.push_back(PrimaryModifierName);
    auto const rest = hasPrimary ? modifiers.without(primary) : modifiers;
    for (auto const& row: vtbackend::ChordModifierTable)
        if (rest.contains(row.modifier))
            result.push_back(row.name);
    return result;
}

static_assert(std::ranges::all_of(vtbackend::ChordModifierTable,
                                  [](auto const& chord) {
                                      return chord.modifier == ModifierWithoutConfigSpelling
                                             || parseModifierName(chord.name) == chord.modifier;
                                  }),
              "every chord modifier Contour can render must parse back to ITSELF from config: the "
              "config writer emits these names verbatim (YAMLConfigWriter::format(Modifiers) -> "
              "std::formatter<Modifier>), so a modifier added to vtbackend's table but missing here "
              "would be written into the user's contour.yml and then refused on the next load");

static_assert(std::ranges::all_of(ConfigModifierTable,
                                  [](auto const& row) {
                                      return parseModifierName(row.name) == row.modifier;
                                  }),
              "every spelling must parse to its OWN row's modifier: if it does not, an earlier row "
              "claims it first, which means a duplicate spelling is resolving by table ORDER and "
              "reordering these rows would be a silent behaviour change");

} // namespace contour::config
