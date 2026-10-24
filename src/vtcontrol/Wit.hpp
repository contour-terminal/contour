// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <string_view>

namespace vtcontrol
{

/// The WIT package the control verbs are published as. Terminal-neutral: any host may implement it.
inline constexpr std::string_view ControlWitPackage = "terminal-extensions:control@0.1.0";

/// @param name A camelBack or dotted name (`ratioPercent`, `pane.split`, `SplitRequest`).
/// @return Its WIT spelling, kebab-case (`ratio-percent`, `pane-split`, `split-request`).
[[nodiscard]] std::string kebabCase(std::string_view name);

/// Renders the method catalog as the WIT package `terminal-extensions:control@0.1.0`.
///
/// `interface types` declares every record and enum reachable from an `Exposure::Everywhere` row, in catalog
/// order with each record before its first use, plus `control-error`; `interface control` has one func per
/// such row, documented with its summary; `world controller` imports it. A record without fields is never
/// declared, since WIT has no empty record: an empty request is a func without parameters, an empty reply
/// is `_`. Both flatten to nothing, so the Canonical ABI is the same either way.
/// @return The text of `wit/control/world.wit`.
[[nodiscard]] std::string renderControlWit();

} // namespace vtcontrol
