// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// xterm's key-modifier resources (XTMODKEYS `CSI > Pp ; Pv m` and XTRMMODKEYS
/// `CSI > Ps n`) as a table, plus the pure decision of what a given request means.
///
/// Split out from the sequence handler because `Pp` is a RESOURCE SELECTOR, not a
/// value, and reading it as one is a mistake the handler is well placed to make.
/// Keeping the mapping here — free of Screen, Terminal and Sequence — is what lets
/// it be tested directly. Shaped after `DECModeNumbers` in primitives.h, the other
/// VT-enum-to-wire-number table with intentional gaps.

#include <array>
#include <cstdint>
#include <optional>
#include <utility>

namespace vtbackend
{

/// The key-modifier resources XTMODKEYS addresses.
/// @see ctlseqs.txt, "Set/reset key modifier options (XTMODKEYS)".
enum class ModifyKeysResource : uint8_t
{
    Keyboard,
    CursorKeys,
    FunctionKeys,
    KeypadKeys,
    OtherKeys,
    ModifierKeys,
    SpecialKeys,
};

/// What the terminal should do about a key-modifier request.
enum class ModifyKeysAction : uint8_t
{
    /// Assign `ModifyKeysRequest::otherKeysValue` to modifyOtherKeys.
    SetOtherKeys,
    /// A resource Contour recognizes but does not track. Accepted, not applied:
    /// Contour always reports modifiers for cursor and function keys, so an app
    /// asking for that is asking for what it already has.
    Ignore,
    /// A resource number xterm does not define (5, or anything past 7).
    Unknown,
};

/// One row per resource: the enum, the number the wire spells it with, and what a
/// request naming it does. Adding a resource Contour starts tracking is changing one
/// row's action — not editing a lookup and a switch elsewhere as well.
///
/// The numbering is NOT contiguous: 5 is reserved for input via xterm's string
/// action and deliberately absent, which is why this is a table and not a cast.
struct ModifyKeysNumbering
{
    ModifyKeysResource resource; ///< The resource.
    uint8_t number;              ///< Its `Pp`/`Ps` number on the wire.
    ModifyKeysAction action;     ///< What a request naming it does.
};

inline constexpr auto ModifyKeysResources = std::to_array<ModifyKeysNumbering>({
    { .resource = ModifyKeysResource::Keyboard, .number = 0, .action = ModifyKeysAction::Ignore },
    { .resource = ModifyKeysResource::CursorKeys, .number = 1, .action = ModifyKeysAction::Ignore },
    { .resource = ModifyKeysResource::FunctionKeys, .number = 2, .action = ModifyKeysAction::Ignore },
    { .resource = ModifyKeysResource::KeypadKeys, .number = 3, .action = ModifyKeysAction::Ignore },
    { .resource = ModifyKeysResource::OtherKeys, .number = 4, .action = ModifyKeysAction::SetOtherKeys },
    { .resource = ModifyKeysResource::ModifierKeys, .number = 6, .action = ModifyKeysAction::Ignore },
    { .resource = ModifyKeysResource::SpecialKeys, .number = 7, .action = ModifyKeysAction::Ignore },
});

/// @param resource The resource to spell.
/// @return @p resource's wire number.
[[nodiscard]] constexpr uint8_t toModifyKeysResourceNum(ModifyKeysResource resource) noexcept
{
    for (auto const& row: ModifyKeysResources)
        if (row.resource == resource)
            return row.number;
    return 0; // unreachable for a valid enumerator
}

/// @param number A wire resource number.
/// @return The resource @p number names, or nullopt if it names none.
[[nodiscard]] constexpr std::optional<ModifyKeysResource> fromModifyKeysResourceNum(int number) noexcept
{
    // std::cmp_equal, not `==`: `number` arrives from a CSI parameter and may be
    // negative, which a promotion-based comparison against the uint8_t table would
    // get wrong in principle (and clang-tidy rejects on sight).
    for (auto const& row: ModifyKeysResources)
        if (std::cmp_equal(row.number, number))
            return row.resource;
    return std::nullopt;
}

/// The modifyOtherKeys level Contour starts at, and returns to on a reset.
///
/// xterm distinguishes "initial value" from the -1 XTRMMODKEYS assigns; Contour
/// cannot, because only level 2 changes how a key is encoded (@see
/// InputGenerator::generate) — so both collapse to the legacy encoding.
constexpr int InitialModifyOtherKeys = 0;

/// A decoded key-modifier request.
struct ModifyKeysRequest
{
    ModifyKeysAction action = ModifyKeysAction::Unknown;
    int otherKeysValue = InitialModifyOtherKeys; ///< Only meaningful for SetOtherKeys.

    bool operator==(ModifyKeysRequest const&) const = default;
};

/// Decodes one XTMODKEYS/XTRMMODKEYS request.
/// @param resource The `Pp`/`Ps` resource selector as it arrived on the wire.
/// @param value The value to assign (XTMODKEYS' `Pv`, or the reset/disable value).
/// @return What the terminal should do.
[[nodiscard]] constexpr ModifyKeysRequest modifyKeysRequest(int resource, int value) noexcept
{
    for (auto const& row: ModifyKeysResources)
        if (std::cmp_equal(row.number, resource))
            return ModifyKeysRequest { .action = row.action, .otherKeysValue = value };
    return ModifyKeysRequest { .action = ModifyKeysAction::Unknown };
}

} // namespace vtbackend
