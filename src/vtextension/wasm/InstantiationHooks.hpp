// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The export names a module may not use, whichever engine runs it: names some engine would run by itself
/// while instantiating -- where no deadline stops it and host functions fail -- or keeps for itself. The host
/// refuses such a module before any engine sees it (CompiledExtension::load), so that every engine refuses
/// the same modules, and at once, rather than one running what another ignores.

#include <algorithm>
#include <array>
#include <string_view>

namespace vtextension::wasm
{

/// The export names refused outright. A WASI reactor's `_initialize` is not among them: the host calls it
/// itself, under the load deadline, and an engine that would call it while instantiating hides it first.
inline constexpr auto RefusedExportNames = std::to_array<std::string_view>({
    // An early Emscripten initializer, which WAMR runs while instantiating any module. No guest exports it.
    "__post_instantiate",
    // The static constructors, which WAMR runs while instantiating. No guest exports it: a Rust guest's
    // constructors run from `_initialize`.
    "__wasm_call_ctors",
    // The name the WAMR backend gives `_initialize` to hide it from WAMR (wamr/HiddenExports.hpp): a module
    // exporting it would be taken for its own initializer.
    "~initialize",
});

/// @param name An export's name, as the module spells it.
/// @return Whether a module exporting it is refused: one of RefusedExportNames, or a name holding a NUL,
/// which
///         an engine reading C strings would take for the name before the NUL (`_initialize\0` for
///         `_initialize`).
[[nodiscard]] constexpr bool isRefusedExportName(std::string_view name) noexcept
{
    return name.contains('\0') || std::ranges::contains(RefusedExportNames, name);
}

} // namespace vtextension::wasm
