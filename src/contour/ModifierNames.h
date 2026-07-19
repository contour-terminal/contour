// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/AsciiText.h>

#include <vtbackend/InputGenerator.h>

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>

namespace contour
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
    /// Compares two modifier spellings case-insensitively. @see contour::ascii::fold
    [[nodiscard]] constexpr bool sameSpelling(std::string_view a, std::string_view b) noexcept
    {
        return std::ranges::equal(a, b, [](char x, char y) { return ascii::fold(x) == ascii::fold(y); });
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

} // namespace contour
