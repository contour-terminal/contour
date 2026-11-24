// SPDX-License-Identifier: Apache-2.0
//
// A user's variable set applied to one call of a pane or command that a `pane-env-set`/`command-env-set` line
// binds: the call's set argument names the set, and the call gets the listed variables the set holds -- each
// value re-checked against its shape -- while every other variable keeps the normal inheritance rules.
#pragma once

#include <vtpty/PathStyle.hpp>

#include <expected>
#include <optional>
#include <span>
#include <vector>

#include <vtextension/DeclaredArguments.hpp>
#include <vtextension/IHostServices.hpp>
#include <vtextension/Manifest.hpp>
#include <vtextension/UserSettableEnvironment.hpp>

namespace vtextension
{

/// A call's arguments with the set argument taken out, and the variables the named set gives.
struct SetSelection
{
    ConfigMap arguments; ///< What the template's placeholders are filled from.
    std::vector<EnvironmentEntry>
        environment; ///< The binding's variables the set holds, in the binding's order.
};

/// Applies a pane's or command's `*-env-set` binding to one call.
/// @param binding The pane's or command's binding, if it has one.
/// @param args The call's arguments, the set argument among them if the call names a set.
/// @param sets The user's sets, as checkEnvironmentSets() left them.
/// @param style The path grammar an AbsolutePath value is spelled in.
/// @return The selection; ArgumentFault::NotItsKind when the set argument is not a Name, names no set, or a
///         listed value fails isNormalisedUserValue(), and ArgumentFault::NotAPlaceholder when it is passed
///         twice. Without a binding, or with the set argument left out, the arguments unchanged and no
///         environment (an `env-set` passed to a template without a binding is then refused by
///         substituteDeclared() as NotAPlaceholder).
[[nodiscard]] std::expected<SetSelection, ArgumentFault> selectEnvironmentSet(
    std::optional<EnvironmentSetBinding> const& binding,
    ConfigMap const& args,
    std::span<EnvironmentSet const> sets,
    vtpty::PathStyle style);

/// A pane's or command's own environment layer followed by what its call's set gives. The two never hold the
/// same name: a template's layer holds only SettableVariables, a set only UserSettableVariables, and the two
/// tables are disjoint (a `[settable]` test pins it). Which entry a consumer keeps for a shared name is so
/// never asked.
/// @param declared The template's own `pane-env` or `command-env-for` values.
/// @param selected SetSelection::environment.
/// @return @p declared, then @p selected.
[[nodiscard]] std::vector<EnvironmentEntry> withSetEnvironment(std::span<EnvironmentEntry const> declared,
                                                               std::span<EnvironmentEntry const> selected);

} // namespace vtextension
