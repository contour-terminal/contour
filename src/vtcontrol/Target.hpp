// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <vtcontrol/ControlError.hpp>
#include <vtcontrol/ObjectRef.hpp>

namespace vtcontrol
{

/// An object a target spec can name.
struct TargetCandidate
{
    ObjectRef ref {};                 ///< Its id.
    std::string label;                ///< Its label, or empty.
    std::optional<uint32_t> position; ///< Its 1-based position, for tabs and windows.
};

/// Resolves what a person typed to one object.
///
/// In order, the first kind of match wins: (1) a full id of `kind`; (2) a case-insensitive label;
/// (3) a decimal 1-based position, for tabs and windows only; (4) an id suffix of at least four hex
/// digits. More than one match of the winning kind is `Ambiguous`, with `candidates` holding each
/// match's full id.
/// @param kind The kind of object wanted.
/// @param spec What the caller wrote.
/// @param candidates The objects of that kind.
/// @return The object, or `NotFound` naming `spec`, or `Ambiguous`.
[[nodiscard]] std::expected<ObjectRef, ControlError> resolveTarget(
    ObjectKind kind, std::string_view spec, std::span<TargetCandidate const> candidates);

} // namespace vtcontrol
