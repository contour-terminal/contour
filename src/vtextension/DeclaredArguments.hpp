// SPDX-License-Identifier: Apache-2.0
//
// The one substitution of a declared argv template, shared by everything an extension starts from one: a
// declared command (DeclaredProcesses) and a pane template (DeclaredPanes). Each placeholder takes the value
// of its name, checked against its declared kind; a value stays one argv word, and no shell is involved.
#pragma once

#include <vtpty/DirectoryHandle.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include <vtextension/IHostServices.hpp>
#include <vtextension/Manifest.hpp>
#include <vtextension/RootResolution.hpp>

namespace vtextension
{

/// Why the arguments of a declared template are refused.
enum class ArgumentFault : uint8_t
{
    NotAPlaceholder, ///< A name is passed that the template has no placeholder for, or is passed twice.
    Missing,         ///< A placeholder has no value.
    NotItsKind,      ///< A value is not of its declared kind, or its name is not declared.
    OutsideItsRoot,  ///< A `path:<root>` value does not resolve inside its root.
};

/// Builds the argv of a declared template from the call's arguments.
/// @param argvTemplate The template: literal words and whole-word `{name}` placeholders.
/// @param manifest The extension's manifest, whose `arg` lines declare each placeholder's kind.
/// @param args A value for each placeholder, by name; no other name, and each once.
/// @param directories Where a `path:<root>` value is resolved.
/// @param roots The declared roots, resolved.
/// @return The argv, a `path:<root>` value replaced by the path it resolves to now; or why the arguments are
///         refused.
[[nodiscard]] std::expected<std::vector<std::string>, ArgumentFault> substituteDeclared(
    std::span<std::string const> argvTemplate,
    Manifest const& manifest,
    ConfigMap const& args,
    vtpty::IDirectoryOpener& directories,
    std::span<ResolvedRoot const> roots);

} // namespace vtextension
