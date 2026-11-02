// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/Config.hpp>
#include <contour/config/KeyboardPlatformKind.hpp>
#include <contour/config/ModifierNames.hpp>

#include <vtbackend/input/InputGenerator.hpp>
#include <vtbackend/input/MatchModes.hpp>

#include <cstdint>
#include <optional>

namespace contour::config
{

/// Which keyboard platforms a built-in row exists on.
enum class PlatformScope : uint8_t
{
    All = 0,     ///< Every platform.
    MacOnly,     ///< macOS only.
    DesktopOnly, ///< Every platform but macOS: X11, Wayland and Windows.
};

/// Where a built-in row is consulted: written into a freshly generated configuration, held back for
/// configurations that predate it, or both.
///
/// A new default cannot reach an existing user (a configuration's `input_mapping:` replaces the built-in
/// table wholesale), so a row that should is also a fallback. @see builtinFallbackKeyMappings
enum class RowReach : uint8_t
{
    WrittenAndFallback = 0, ///< In makeDefaultInputMappings() and in the fallback tables.
    WrittenOnly,            ///< Only in makeDefaultInputMappings(): the generated configuration lists it.
    FallbackOnly,           ///< Only in the fallback tables: a configuration that predates it still gets it.
};

/// Which defaults edition a built-in row belongs to.
enum class EditionScope : uint8_t
{
    Both = 0,       ///< Classic and Native2026.
    Native2026Only, ///< The newest edition only.
    ClassicOnly,    ///< The oldest edition only.
};

/// Whether a chord is spelled with the platform-neutral `Primary` modifier.
enum class PrimaryUse : uint8_t
{
    Absent = 0, ///< The chord's modifiers are exactly ChordSpec::extra.
    Present,    ///< The chord's modifiers are the host's Primary set (@see primaryModifiers) plus extra.
};

/// A row's modifiers, before the keyboard platform is known.
struct ChordSpec
{
    /// The modifiers beside the Primary set (all of them, when @ref primary is Absent).
    vtbackend::Modifiers extra {};
    /// Whether the host's Primary set is part of the chord.
    PrimaryUse primary = PrimaryUse::Absent;
};

/// The concrete modifiers @p chord stands for on @p kind.
/// @param chord The row's chord.
/// @param kind The keyboard platform.
/// @return The union of the Primary set (when the chord has it) and the extra modifiers.
[[nodiscard]] constexpr vtbackend::Modifiers resolveChord(ChordSpec chord, KeyboardPlatformKind kind) noexcept
{
    return chord.primary == PrimaryUse::Present ? chord.extra | primaryModifiers(kind) : chord.extra;
}

/// One built-in binding, with the columns that decide where and for whom it exists.
/// @tparam Input The binding's input: a key, a character or a mouse button.
template <typename Input>
struct DefaultBindingRow
{
    PlatformScope scope = PlatformScope::All;                ///< The platforms the row exists on.
    RowReach reach = RowReach::WrittenAndFallback;           ///< Written, fallback, or both.
    EditionScope edition = EditionScope::Both;               ///< The defaults editions the row belongs to.
    vtbackend::MatchModes modes {};                          ///< The terminal modes the row requires.
    ChordSpec chord {};                                      ///< The row's modifiers.
    Input input {};                                          ///< The key, character or button.
    ActionList binding {};                                   ///< What the row does.
    std::optional<uint8_t> clickCount {};                    ///< A mouse row's click count.
    DeclinedInput declined = DeclinedInput::PassThrough;     ///< What a declined press does (fallbacks).
    bool (*enabled)(Config const&) noexcept = alwaysEnabled; ///< Whether the row takes part in matching.
};

/// The modifiers a click follows a hyperlink with in the Native2026 defaults of @p kind: read from the
/// table's own FollowHyperlink row, so there is one statement of it. Command (Super) on macOS, where Qt's
/// Control/Meta swap is off, else Control. The Classic edition's is Control everywhere.
/// @param kind The keyboard platform.
/// @return The modifiers; none when the table has no such row.
[[nodiscard]] vtbackend::Modifiers followHyperlinkModifier(KeyboardPlatformKind kind);

} // namespace contour::config
